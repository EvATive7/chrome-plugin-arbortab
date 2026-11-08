"""Sandbox Chrome harness for ArborTab development (PLAN.md section 10).

Never touches the real Chrome installation: everything runs from a copied
portable Chrome tree.

Examples:
    uv run --with websocket-client --with psutil --with pillow python scripts/sandbox.py launch --fresh --tabs 3 --dll research/poc/version.dll
    uv run --with pillow python scripts/sandbox.py shot artifacts/poc/with-hook.png
    uv run --with websocket-client python scripts/sandbox.py new-tab https://example.com
    uv run --with psutil python scripts/sandbox.py kill
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_SANDBOX = Path(r"C:\Users\evative7\Desktop\Playground\_test\portable")
SANDBOX = Path(os.environ.get("ARBORTAB_SANDBOX", DEFAULT_SANDBOX))
APP = SANDBOX / "app"
PROFILE = SANDBOX / "prof"
PORT = int(os.environ.get("ARBORTAB_CDP_PORT", "9334"))


def chrome_exe() -> Path:
    return APP / "chrome.exe"


def ps(cmd: str) -> str:
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command", cmd],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return out.stdout


def sandbox_pids() -> list[int]:
    raw = ps(
        "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
        "Where-Object { $_.ExecutablePath -like '*_test*portable*' } | "
        "ForEach-Object { $_.ProcessId }"
    )
    return [int(line) for line in raw.split() if line.strip().isdigit()]


def kill() -> None:
    pids = sandbox_pids()
    if not pids:
        print("sandbox: no running processes")
        return
    subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
         "Where-Object { $_.ExecutablePath -like '*_test*portable*' } | "
         "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"],
        capture_output=True,
    )
    for _ in range(50):
        if not sandbox_pids():
            print(f"sandbox: stopped {len(pids)} processes")
            return
        time.sleep(0.2)
    print("sandbox: WARNING processes still alive")


def ensure_vertical_tabs(profile: Path) -> None:
    default = profile / "Default"
    default.mkdir(parents=True, exist_ok=True)
    prefs_path = default / "Preferences"
    prefs: dict = {}
    if prefs_path.exists():
        try:
            prefs = json.loads(prefs_path.read_text(encoding="utf-8"))
        except Exception:
            prefs = {}
    vt = prefs.setdefault("vertical_tabs", {})
    vt["enabled"] = True
    vt["collapsed_state"] = False
    vt["uncollapsed_width"] = 240
    prefs_path.write_text(json.dumps(prefs), encoding="utf-8")

    local_state = profile / "Local State"
    if not local_state.exists():
        local_state.write_text(json.dumps({"profile": {"info_cache": {}}}), encoding="utf-8")


def install_dll(dll: Path | None) -> None:
    target = APP / "version.dll"
    if dll is None:
        if target.exists():
            target.unlink()
            print("sandbox: removed version.dll (baseline)")
        return
    shutil.copy2(dll, target)
    print(f"sandbox: installed {dll} -> {target}")


def wait_cdp(timeout: float = 40.0) -> dict:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/version", timeout=2) as r:
                return json.load(r)
        except Exception as exc:  # noqa: BLE001
            last = exc
            time.sleep(0.3)
    raise SystemExit(f"CDP did not come up on port {PORT}: {last}")


def launch(args: argparse.Namespace) -> None:
    kill()
    if args.fresh and PROFILE.exists():
        shutil.rmtree(PROFILE, ignore_errors=True)
    ensure_vertical_tabs(PROFILE)
    install_dll(Path(args.dll) if args.dll else None)

    cmd = [
        str(chrome_exe()),
        f"--user-data-dir={PROFILE}",
        "--no-first-run",
        "--no-default-browser-check",
        f"--remote-debugging-port={PORT}",
        "--enable-unsafe-extension-debugging",
        f"--window-size={args.size}",
        f"--window-position={args.position}",
        "--disable-features=CalculateNativeWinOcclusion",
        *args.extra,
    ]
    urls = args.tabs if isinstance(args.tabs, list) else []
    if not urls:
        urls = ["about:blank"] * max(1, args.count)
    cmd.extend(urls)
    subprocess.Popen(cmd, cwd=str(APP), close_fds=True)
    info = wait_cdp()
    print(f"sandbox: browser={info.get('Browser')} ws={info.get('webSocketDebuggerUrl')}")
    # let the first paint settle
    time.sleep(args.settle)


class CDP:
    """Minimal CDP client (browser level)."""

    def __init__(self, port: int = PORT) -> None:
        import websocket  # noqa: PLC0415

        info = wait_cdp()
        self.ws = websocket.create_connection(
            info["webSocketDebuggerUrl"], timeout=60, max_size=32 * 1024 * 1024
        )
        self.next_id = 0

    def call(self, method: str, params: dict | None = None, timeout: float = 60.0) -> dict:
        self.next_id += 1
        msg_id = self.next_id
        self.ws.send(json.dumps({"id": msg_id, "method": method, "params": params or {}}))
        deadline = time.time() + timeout
        while time.time() < deadline:
            data = json.loads(self.ws.recv())
            if data.get("id") == msg_id:
                if "error" in data:
                    raise RuntimeError(f"{method}: {data['error']}")
                return data.get("result", {})
        raise TimeoutError(method)

    def close(self) -> None:
        self.ws.close()


def window_rect(pid: int) -> tuple[int, int, int, int]:
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    found: list[tuple[int, int, int, int]] = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_proc(hwnd, _lparam):
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        rect = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        width, height = rect.right - rect.left, rect.bottom - rect.top
        if width > 400 and height > 300:
            found.append((rect.left, rect.top, width, height))
        return True

    user32.EnumWindows(enum_proc, 0)
    if not found:
        raise SystemExit(f"no visible window for pid {pid}")
    return found[0]


def main_window_pid() -> int:
    raw = ps(
        "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
        "Where-Object { $_.ExecutablePath -like '*_test*portable*' } | "
        "Where-Object { $_.CommandLine -notlike '*--type=*' } | "
        "ForEach-Object { $_.ProcessId }"
    )
    pids = [int(x) for x in raw.split() if x.strip().isdigit()]
    if not pids:
        raise SystemExit("sandbox: not running")
    return pids[0]


def shot(path: Path) -> None:
    from PIL import ImageGrab  # noqa: PLC0415

    left, top, width, height = window_rect(main_window_pid())
    img = ImageGrab.grab(bbox=(left, top, left + width, top + height), all_screens=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)
    print(f"sandbox: saved {path} ({width}x{height})")


def cmd_tabs(_args: argparse.Namespace) -> None:
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=5) as r:
        targets = json.load(r)
    for t in targets:
        print(f"{t['type']:10s} {t.get('title', '')[:40]:42s} {t.get('url', '')[:60]}")


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_launch = sub.add_parser("launch")
    p_launch.add_argument("--dll", help="copy this dll to app/version.dll first")
    p_launch.add_argument("--fresh", action="store_true", help="wipe the sandbox profile")
    p_launch.add_argument("--count", type=int, default=1, help="number of about:blank tabs")
    p_launch.add_argument("--tabs", nargs="*", help="explicit URLs to open")
    p_launch.add_argument("--size", default="1200,900")
    p_launch.add_argument("--position", default="0,0")
    p_launch.add_argument("--settle", type=float, default=3.0)
    p_launch.add_argument("--extra", nargs="*", default=[])
    p_launch.set_defaults(func=launch)

    p_shot = sub.add_parser("shot")
    p_shot.add_argument("out")
    p_shot.set_defaults(func=lambda a: shot(Path(a.out)))

    p_kill = sub.add_parser("kill")
    p_kill.set_defaults(func=lambda _a: kill())

    p_tabs = sub.add_parser("tabs")
    p_tabs.set_defaults(func=cmd_tabs)

    p_new = sub.add_parser("new-tab")
    p_new.add_argument("url")
    p_new.add_argument("--count", type=int, default=1)

    p_eval = sub.add_parser("eval")
    p_eval.add_argument("expression")

    args = parser.parse_args()
    if args.cmd == "new-tab":
        cdp = CDP()
        for _ in range(args.count):
            res = cdp.call("Target.createTarget", {"url": args.url})
            print("created", res.get("targetId"))
        cdp.close()
        return
    if args.cmd == "eval":
        import websocket  # noqa: PLC0415

        with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=5) as r:
            page = next(t for t in json.load(r) if t["type"] == "page")
        ws = websocket.create_connection(page["webSocketDebuggerUrl"], timeout=60)
        ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate", "params": {
            "expression": args.expression, "returnByValue": True, "awaitPromise": True}}))
        while True:
            data = json.loads(ws.recv())
            if data.get("id") == 1:
                print(json.dumps(data, indent=2, ensure_ascii=False))
                break
        ws.close()
        return
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
