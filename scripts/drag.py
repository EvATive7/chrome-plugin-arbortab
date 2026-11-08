"""Real-mouse drag checks for M5 (F4 drop detection, F5 drop end, F6 highlight).

`HandleTabDragInContainer` only runs while a tab is actually being dragged with the mouse, so
the detection itself cannot be reached through the extension's test hooks. This script drives
the Win32 cursor instead: the coordinates come from the native layout snapshot, calibrated
against a screenshot of the sandbox window.

    uv run --with websocket-client --with pillow --with psutil python scripts/drag.py
"""
from __future__ import annotations

import argparse
import ctypes
import json
import sys
import time
import urllib.request
from ctypes import wintypes
from pathlib import Path

from PIL import Image, ImageGrab

sys.path.insert(0, str(Path(__file__).resolve().parent))

import click as win  # noqa: E402  (scripts/click.py: window helpers)
from e2e import Session, browser_ws, call_worker, extension_state, report, service_worker_session  # noqa: E402

CDP_PORT = 9334
ORIGIN = "chrome-extension://ohpbcfelmjohmnbcidneadeodlbeleno"
PORTS = (47821, 47822, 47823, 47824, 47825)
ARTIFACTS = Path(__file__).resolve().parent.parent / "artifacts"

user32 = ctypes.WinDLL("user32", use_last_error=True)


# ----------------------------------------------------------------------------------------
# Native endpoint (layout snapshot + drag diagnostics)
# ----------------------------------------------------------------------------------------
def native(message: dict) -> dict:
    import websocket  # noqa: PLC0415

    last = None
    for port in PORTS:
        try:
            ws = websocket.create_connection(f"ws://127.0.0.1:{port}/arbortab", timeout=10,
                                             origin=ORIGIN, suppress_origin=False)
            break
        except Exception as exc:  # noqa: BLE001
            last = exc
    else:
        raise SystemExit(f"cannot reach the native endpoint: {last}")
    try:
        ws.recv()  # hello
        ws.send(json.dumps(message))
        deadline = time.time() + 20
        while time.time() < deadline:
            reply = json.loads(ws.recv())
            if reply.get("type") == message["type"]:
                return reply
        raise SystemExit(f"no reply to {message['type']}")
    finally:
        ws.close()


def layout_snapshot() -> tuple[list[dict], dict]:
    reply = native({"type": "debug.layout"})
    tabs = [entry for entry in (reply.get("layouts") or []) if entry.get("isTab")]
    return tabs, reply.get("stats", {})


# ----------------------------------------------------------------------------------------
# Extension state
# ----------------------------------------------------------------------------------------
def ui_state(worker: Session, browser: Session) -> dict:
    state = extension_state(worker, browser)
    window = next(iter(state["windows"].values()))
    order = [int(tab) for tab in window["order"]]
    parents = {int(tab): int(parent) for tab, parent in window.get("parents", [])}
    collapsed = {int(tab) for tab in window.get("collapsed", [])}
    nodes = {tab: (parents.get(tab), tab in collapsed) for tab in order}
    return {"order": order, "nodes": nodes}


def setup_tabs(worker: Session, browser: Session, count: int = 4) -> list[int]:
    """Replaces the window contents with `count` top level tabs and returns their ids."""
    urls = ["https://example.com/", "https://example.org/", "https://example.net/",
            "https://www.iana.org/help/example-domains", "https://example.com/?2"]
    created = []
    for url in urls[:count]:
        tab = call_worker(worker, f"chrome.tabs.create({{url:{url!r}, active:false}}).then(t => t.id)",
                          browser)
        created.append(int(tab))
        time.sleep(0.4)
    keep = set(created)
    for tab in call_worker(worker, "chrome.tabs.query({}).then(ts => ts.map(t => t.id))", browser):
        if int(tab) not in keep:
            call_worker(worker, f"chrome.tabs.remove({int(tab)}).catch(() => null)", browser)
    time.sleep(0.6)
    # Every kept tab is top level: no opener was passed to chrome.tabs.create.
    for tab in created[1:]:
        call_worker(worker, f"chrome.tabs.update({tab}, {{active:false}})", browser)
    call_worker(worker, f"chrome.tabs.update({created[0]}, {{active:true}})", browser)
    time.sleep(1.5)
    return created


# ----------------------------------------------------------------------------------------
# Geometry: layout snapshot is in DIPs, the screen is in physical pixels
# ----------------------------------------------------------------------------------------
def main_window() -> tuple[int, tuple[int, int, int, int]]:
    """The largest visible top level window of the sandbox browser (popups come and go)."""
    import subprocess  # noqa: PLC0415

    raw = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
         "Where-Object { $_.ExecutablePath -like '*_test*portable*' -and "
         "$_.CommandLine -notlike '*--type=*' } | ForEach-Object { $_.ProcessId }"],
        capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    pids = [int(value) for value in raw.split() if value.strip().isdigit()]
    if not pids:
        raise SystemExit("sandbox Chrome is not running")
    pid = pids[0]

    found: list[tuple[int, tuple[int, int, int, int]]] = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_proc(hwnd, _lparam):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        rect = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        width, height = rect.right - rect.left, rect.bottom - rect.top
        if width > 600 and height > 400:
            found.append((int(hwnd), (rect.left, rect.top, width, height)))
        return True

    user32.EnumWindows(enum_proc, 0)
    if not found:
        raise SystemExit(f"no visible window for pid {pid}")
    handle, rect = max(found, key=lambda item: item[1][2] * item[1][3])
    return handle, rect


def window_rect() -> tuple[int, int, int, int]:
    return main_window()[1]


def grab(rect: tuple[int, int, int, int]):
    left, top, width, height = rect
    return ImageGrab.grab(bbox=(left, top, left + width, top + height), all_screens=True).convert("RGB")


def calibrate(rect, tabs: list[dict]) -> dict:
    """Finds the physical origin of the tab strip by locating the active (first) tab.

    The first tab is activated before calibrating, so the topmost bright band of the strip is
    that tab; its layout y is 0 (the container origin) and its layout x is known.
    """
    handle, _rect = main_window()
    scale = ctypes.windll.user32.GetDpiForWindow(ctypes.c_void_p(handle)) / 96.0
    image = grab(rect)
    width = rect[2]
    bands: list[tuple[int, int]] = []
    start = None
    for y in range(60, rect[3]):
        count = sum(1 for x in range(20, min(width, 500), 4) if image.getpixel((x, y))[0] >= 50)
        if count > 60:
            if start is None:
                start = y
        elif start is not None:
            bands.append((start, y - 1))
            start = None
    # The strip has thin bright separators; the active tab is the first band tall enough to be
    # a row.
    tab_bands = [band for band in bands if band[1] - band[0] >= 40]
    if not tab_bands:
        raise SystemExit(f"could not locate the active tab row in the screenshot: {bands}")
    band_start, band_end = tab_bands[0]
    sample_y = (band_start + band_end) // 2
    # The window border and the strip divider are thin bright columns; the tab is the longest
    # bright run.
    runs: list[tuple[int, int]] = []
    start = None
    for x in range(0, width):
        bright = image.getpixel((x, sample_y))[0] >= 55
        if bright and start is None:
            start = x
        elif not bright and start is not None:
            runs.append((start, x - 1))
            start = None
    if not runs:
        raise SystemExit("could not measure the active tab's horizontal extent")
    left_edge = max(runs, key=lambda run: run[1] - run[0])[0]
    origin = {
        "scale": scale,
        "offset_x": left_edge - tabs[0]["x"] * scale,
        "offset_y": band_start - tabs[0]["y"] * scale,
        "band": (band_start, band_end),
    }
    print(f"calibrated: scale={scale} offsets=({origin['offset_x']}, {origin['offset_y']}) "
          f"active band={origin['band']}")
    return origin


def screen_point(rect, origin: dict, tab: dict, band: str = "middle") -> tuple[int, int]:
    scale = origin["scale"]
    height = tab["h"] * scale
    if band == "top":
        local_y = tab["y"] * scale + height * 0.18
    elif band == "bottom":
        local_y = tab["y"] * scale + height * 0.82
    else:
        local_y = tab["y"] * scale + height * 0.5
    return (rect[0] + origin["offset_x"] + (tab["x"] + tab["w"] / 2) * scale,
            rect[1] + origin["offset_y"] + local_y)


# ----------------------------------------------------------------------------------------
# Synthetic mouse drag
# ----------------------------------------------------------------------------------------
def move_to(x: float, y: float) -> None:
    user32.SetCursorPos(int(round(x)), int(round(y)))


def drag(start: tuple[float, float], end: tuple[float, float], *, esc: bool = False,
         hold_shot: Path | None = None, hold_seconds: float = 0.6,
         hold_probe=None) -> dict | None:
    win.focus_window()
    move_to(*start)
    time.sleep(0.25)
    user32.mouse_event(0x0002, 0, 0, 0, 0)  # MOUSEEVENTF_LEFTDOWN
    time.sleep(0.25)

    steps = 14
    for index in range(1, steps + 1):
        fraction = index / steps
        move_to(start[0] + (end[0] - start[0]) * fraction,
                start[1] + (end[1] - start[1]) * fraction)
        time.sleep(0.05)
    # A couple of tiny moves: Chrome only re-evaluates the drop target on a real move.
    for _ in range(3):
        move_to(end[0], end[1] + 1)
        time.sleep(0.05)
        move_to(end[0], end[1] - 1)
        time.sleep(0.05)

    time.sleep(hold_seconds)
    held: dict | None = None
    if hold_shot is not None or hold_probe is not None:
        held = {"stats": None, "probe": None}
        if hold_shot is not None:
            left, top, width, height = window_rect()
            ARTIFACTS.mkdir(parents=True, exist_ok=True)
            grab((left, top, width, height)).save(hold_shot)
        _tabs, held["stats"] = layout_snapshot()
        if hold_probe is not None:
            held["probe"] = hold_probe()

    if esc:
        user32.keybd_event(0x1B, 0, 0, 0)  # VK_ESCAPE
        time.sleep(0.05)
        user32.keybd_event(0x1B, 0, 2, 0)
        time.sleep(0.35)
    user32.mouse_event(0x0004, 0, 0, 0, 0)  # MOUSEEVENTF_LEFTUP
    time.sleep(1.6)
    return held


def highlight_pixels(path: Path, point: tuple[int, int], rect) -> list[tuple[int, int, int]]:
    image = Image.open(path).convert("RGB")
    samples = []
    for delta in (-8, 0, 8):
        x = point[0] - rect[0] + delta
        y = point[1] - rect[1]
        if 0 <= x < image.width and 0 <= y < image.height:
            samples.append(image.getpixel((x, y)))
    return samples


# ----------------------------------------------------------------------------------------
# Scenarios
# ----------------------------------------------------------------------------------------
def by_id(tabs: list[dict]) -> dict[int, dict]:
    return {int(tab["tabId"]): tab for tab in tabs}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", help="comma separated scenario ids (1-4)")
    args = parser.parse_args()
    wanted = {int(x) for x in args.only.split(",")} if args.only else {1, 2, 3, 4}

    browser = Session(browser_ws())
    browser.call("Target.setDiscoverTargets", {"discover": True})
    worker = service_worker_session(browser)

    tabs = setup_tabs(worker, browser, 4)
    snapshot, stats = layout_snapshot()
    if {int(t["tabId"]) for t in snapshot} != set(tabs):
        print(f"WARNING: snapshot {[t['tabId'] for t in snapshot]} != expected {tabs}")
    rect = window_rect()
    origin = calibrate(rect, snapshot)

    def current() -> dict:
        return ui_state(worker, browser)

    def tab_entry(state: dict, tab: int) -> dict:
        snapshot_now, _ = layout_snapshot()
        return by_id(snapshot_now)[tab]

    results: dict[int, bool] = {}

    # --- 1. drop on the middle band nests the dragged tab -------------------------------
    if 1 in wanted:
        a, b, c, d = tabs
        state = current()
        shot = ARTIFACTS / "m5-live-middle.png"
        start = screen_point(rect, origin, tab_entry(state, c))
        end = screen_point(rect, origin, tab_entry(state, b), "middle")
        print(f"drag {c} -> {b} (middle) {start} -> {end}")
        held = drag(start, end, hold_shot=shot)
        ok_hold = bool(held) and held["stats"]["dragging"] and held["stats"]["dropTarget"] == b
        results[1] = report("holding over the middle band targets that tab", ok_hold,
                            f"drag={held}")
        pixels = highlight_pixels(shot, end, rect)
        tinted = [px for px in pixels if px[2] - px[0] > 15]
        results[1] = report("the drop target is highlighted", bool(tinted),
                            f"samples={pixels}") and results[1]
        time.sleep(1.2)
        state = current()
        results[1] = report("dropped tab became a child of the target",
                            state["nodes"].get(c, (None, None))[0] == b,
                            f"nodes={state['nodes']}") and results[1]
        results[1] = report("no temporary collapse was left behind",
                            not any(collapsed for _parent, collapsed in state["nodes"].values())) \
            and results[1]

    # --- 2. drop on the top quarter uses the native reorder and stays top level ---------
    if 2 in wanted:
        state = current()
        order_before = state["order"]
        a, b, c, d = tabs
        start = screen_point(rect, origin, tab_entry(state, d))
        end = screen_point(rect, origin, tab_entry(state, a), "top")
        print(f"drag {d} -> {a} (top quarter) {start} -> {end}")
        drag(start, end, hold_shot=ARTIFACTS / "m5-live-edge.png")
        time.sleep(1.2)
        state = current()
        order_after = state["order"]
        moved = order_after.index(d) < order_after.index(a)
        results[2] = report("the edge drop reordered the tab natively", moved,
                            f"{order_before} -> {order_after}")
        results[2] = report("the edge drop did not nest the tab",
                            state["nodes"].get(d, (None, None))[0] is None,
                            f"nodes={state['nodes']}") and results[2]

    # --- 3. Esc cancels the drag without side effects ----------------------------------
    if 3 in wanted:
        state = current()
        before = {"order": state["order"], "nodes": dict(state["nodes"])}
        # b is the parent created by scenario 1 (or the second tab otherwise).
        parent = next((tab for tab, (p, _c) in state["nodes"].items()
                       if any(pp == tab for pp, _cc in state["nodes"].values())), tabs[1])
        target = tabs[3]
        start = screen_point(rect, origin, tab_entry(state, parent))
        end = screen_point(rect, origin, tab_entry(state, target), "middle")
        print(f"drag {parent} -> {target} then Esc {start} -> {end}")
        held = drag(start, end, esc=True, hold_shot=ARTIFACTS / "m5-live-esc.png")
        ok_hold = bool(held) and held["stats"]["dropTarget"] == target
        state = current()
        results[3] = report("the drag targeted the tab before Esc", ok_hold, f"stats={held}")
        results[3] = report("Esc left the tree untouched",
                            state["order"] == before["order"]
                            and state["nodes"] == before["nodes"],
                            f"{before['order']} -> {state['order']}") and results[3]

    # --- 4. dragging a parent carries its subtree --------------------------------------
    if 4 in wanted:
        state = current()
        parent = next((tab for tab, (p, _c) in state["nodes"].items()
                       if any(pp == tab for pp, _cc in state["nodes"].values())), None)
        if parent is None:
            results[4] = report("a parent tab exists to drag", False, f"nodes={state['nodes']}")
        else:
            children = [tab for tab, (p, _c) in state["nodes"].items() if p == parent]
            target = next(tab for tab in tabs if tab != parent and tab not in children)
            start = screen_point(rect, origin, tab_entry(state, parent))
            end = screen_point(rect, origin, tab_entry(state, target), "middle")
            print(f"drag parent {parent} -> {target} {start} -> {end}")
            held = drag(start, end, hold_shot=ARTIFACTS / "m5-live-parent.png",
                        hold_probe=current)
            during = (held or {}).get("probe") or {}
            results[4] = report("the dragged parent hides its subtree while held",
                                during.get("nodes", {}).get(parent, (None, None))[1] is True,
                                f"nodes={during.get('nodes')}, stats={(held or {}).get('stats')}")
            time.sleep(1.2)
            after = current()
            results[4] = report("the temporary collapse is undone after the drop",
                                after["nodes"].get(parent, (None, None))[1] is False) \
                and results[4]
            results[4] = report("the parent became a child of the target",
                                after["nodes"].get(parent, (None, None))[0] == target,
                                f"nodes={after['nodes']}") and results[4]
            order = after["order"]
            contiguous = all(order.index(child) > order.index(parent) for child in children)
            results[4] = report("the subtree followed its parent", contiguous,
                                f"order={order} children={children}") and results[4]

    stats = layout_snapshot()[1]
    print(f"native stats: {json.dumps(stats)}")
    failed = [key for key, ok in results.items() if not ok]
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
