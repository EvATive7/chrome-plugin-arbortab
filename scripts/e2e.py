"""End-to-end checks for the ArborTab extension in the sandbox Chrome (PLAN.md section 10).

    uv run --with websocket-client python scripts/e2e.py m3
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request

import websocket

CDP_PORT = 9334


def http(path: str):
    with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}{path}", timeout=10) as response:
        return json.load(response)


def browser_ws():
    return websocket.create_connection(http("/json/version")["webSocketDebuggerUrl"], timeout=30)


class Session:
    def __init__(self, ws, session_id: str | None = None):
        self.ws = ws
        self.session_id = session_id
        self.next_id = 0

    def call(self, method: str, params: dict | None = None, timeout: float = 30.0):
        self.next_id += 1
        message = {"id": self.next_id, "method": method, "params": params or {}}
        if self.session_id:
            message["sessionId"] = self.session_id
        self.ws.send(json.dumps(message))
        deadline = time.time() + timeout
        while time.time() < deadline:
            data = json.loads(self.ws.recv())
            if data.get("id") == self.next_id:
                if "error" in data:
                    raise RuntimeError(f"{method}: {data['error']}")
                return data.get("result", {})
        raise TimeoutError(method)

    def eval(self, expression: str, await_promise: bool = True):
        result = self.call("Runtime.evaluate", {
            "expression": expression,
            "returnByValue": True,
            "awaitPromise": await_promise,
        })
        if result.get("exceptionDetails"):
            raise RuntimeError(f"evaluate failed: {result['exceptionDetails']}")
        return result.get("result", {}).get("value")


EXTENSION_PATH = "src/background/service-worker.js"


def service_worker_session(browser: Session) -> Session:
    """Attach to the ArborTab service worker target (other extensions have workers too)."""
    targets = browser.call("Target.getTargets")["targetInfos"]
    workers = [t for t in targets
               if t["type"] == "service_worker" and EXTENSION_PATH in t.get("url", "")]
    if not workers:
        raise SystemExit("the ArborTab service worker is not running")
    target_id = workers[0]["targetId"]
    attached = browser.call("Target.attachToTarget", {"targetId": target_id, "flatten": True})
    return Session(browser.ws, attached["sessionId"])


def page_session(browser: Session, url_part: str = "") -> Session:
    targets = browser.call("Target.getTargets")["targetInfos"]
    pages = [t for t in targets if t["type"] == "page" and url_part in t.get("url", "")]
    if not pages:
        pages = [t for t in targets if t["type"] == "page"]
    attached = browser.call("Target.attachToTarget", {"targetId": pages[0]["targetId"], "flatten": True})
    return Session(browser.ws, attached["sessionId"])


def open_link_in_child(page: Session) -> str:
    """Clicks a target=_blank link so Chrome records openerTabId, then returns the new target id."""
    before = {t["targetId"] for t in page.call("Target.getTargets")["targetInfos"]}
    page.eval("""
        (() => {
          const link = document.createElement('a');
          link.id = 'arbortab-e2e';
          link.href = 'https://example.org/';
          link.target = '_blank';
          link.textContent = 'arbortab e2e';
          link.style.cssText = 'position:fixed;left:20px;top:20px;z-index:2147483647;'
            + 'font-size:24px;background:#fff;color:#000;padding:12px;';
          document.body.appendChild(link);
          return link.getBoundingClientRect().toJSON();
        })()
    """)
    box = page.eval("document.getElementById('arbortab-e2e').getBoundingClientRect().toJSON()")
    x = box["x"] + box["width"] / 2
    y = box["y"] + box["height"] / 2
    page.call("Input.dispatchMouseEvent", {"type": "mousePressed", "x": x, "y": y,
                                           "button": "left", "clickCount": 1})
    page.call("Input.dispatchMouseEvent", {"type": "mouseReleased", "x": x, "y": y,
                                           "button": "left", "clickCount": 1})
    deadline = time.time() + 10
    while time.time() < deadline:
        after = {t["targetId"] for t in page.call("Target.getTargets")["targetInfos"]}
        new = after - before
        if new:
            return next(iter(new))
        time.sleep(0.2)
    raise SystemExit("clicking the link did not open a new tab")


def extension_state(worker: Session, browser: Session | None = None) -> dict:
    """Reads the service worker state, re-attaching if the worker was restarted."""
    last = None
    for attempt in range(6):
        try:
            return worker.eval("arbortab.getState()")
        except Exception as exc:  # noqa: BLE001
            last = exc
            time.sleep(0.8)
            if browser is not None and attempt >= 2:
                worker.__dict__.update(service_worker_session(browser).__dict__)
    raise RuntimeError(f"cannot read the extension state: {last}")


def call_worker(worker: Session, expression: str, browser: Session | None = None):
    last = None
    for attempt in range(6):
        try:
            return worker.eval(expression)
        except Exception as exc:  # noqa: BLE001
            last = exc
            time.sleep(0.8)
            if browser is not None and attempt >= 2:
                worker.__dict__.update(service_worker_session(browser).__dict__)
    raise RuntimeError(f"extension call failed: {expression}: {last}")


def close_target(browser: Session, target_id: str) -> None:
    browser.call("Target.closeTarget", {"targetId": target_id})


def report(name: str, ok: bool, detail: str = "") -> bool:
    print(f"{'PASS' if ok else 'FAIL'} {name}{' — ' + detail if detail else ''}")
    return ok


def m3() -> int:
    browser = Session(browser_ws())
    browser.call("Target.setDiscoverTargets", {"discover": True})
    worker = service_worker_session(browser)
    state = extension_state(worker, browser)
    report("extension connected to the native module", bool(state.get("connected")),
           json.dumps(state.get("features")))
    report("native layout feature enabled", bool(state.get("features", {}).get("layout")))

    window = next(iter(state["windows"].values()))
    order_before = list(window["order"])
    tabs_before = len(order_before)
    parent = order_before[0]

    # chrome.tabs.create with openerTabId is exactly what a link-opened tab produces.
    call_worker(
        worker,
        f"chrome.tabs.create({{url:'https://example.org/', openerTabId:{parent}, active:false}})",
        browser)
    time.sleep(2.0)

    state = extension_state(worker, browser)
    window = next(iter(state["windows"].values()))
    nodes = {int(tab_id): (p, c) for tab_id, p, c in window["nodes"]}
    added = [tab for tab in window["order"] if tab not in order_before]
    report("new tab was added to the tree", len(window["order"]) == tabs_before + 1,
           f"{tabs_before} -> {len(window['order'])}")
    if not added:
        report("new tab is a child of its opener", False, "no new tab id found")
        return 1
    child = added[-1]
    report("new tab is a child of its opener", nodes.get(child, (None, False))[0] == parent,
           f"child={child} parent={nodes.get(child, (None, None))[0]} expected={parent}")

    call_worker(worker, f"arbortab.toggleCollapsed({parent})", browser)
    time.sleep(0.8)
    state = extension_state(worker, browser)
    window = next(iter(state["windows"].values()))
    nodes = {int(tab_id): (p, c) for tab_id, p, c in window["nodes"]}
    report("collapsing the parent collapses the subtree", nodes[parent][1] is True)

    call_worker(worker, f"chrome.tabs.update({child}, {{active:true}})", browser)
    time.sleep(1.2)
    state = extension_state(worker, browser)
    window = next(iter(state["windows"].values()))
    nodes = {int(tab_id): (p, c) for tab_id, p, c in window["nodes"]}
    report("activating a hidden child expands its ancestors", nodes[parent][1] is False)

    # Keep one unrelated tab alive: closing every tab of the last window quits the browser.
    call_worker(worker, "chrome.tabs.create({url:'about:blank', active:false})", browser)
    time.sleep(0.8)
    state = extension_state(worker, browser)
    window = next(iter(state["windows"].values()))
    order_before = list(window["order"])
    call_worker(worker, f"arbortab.closeTree({parent})", browser)
    time.sleep(1.8)
    state = extension_state(worker, browser)
    window = next(iter(state["windows"].values()))
    remaining = window["order"]
    report("closing the parent removed the subtree",
           child not in remaining and parent not in remaining,
           f"{order_before} -> {remaining}")
    report("browser survived the whole scenario", bool(state.get("connected")), "")
    return 0


def m3_visual() -> int:
    """Builds a small tree so the strip can be inspected (and screenshotted) by hand."""
    browser = Session(browser_ws())
    browser.call("Target.setDiscoverTargets", {"discover": True})
    worker = service_worker_session(browser)
    state = extension_state(worker, browser)
    window = next(iter(state["windows"].values()))
    parent = window["order"][0]
    for url in ("https://example.org/", "https://www.iana.org/help/example-domains"):
        call_worker(worker, f"chrome.tabs.create({{url:'{url}', openerTabId:{parent}, active:false}})",
                    browser)
        time.sleep(1.2)
    time.sleep(1.0)
    state = extension_state(worker, browser)
    print(json.dumps(state["windows"], indent=1))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", choices=["m3", "m3-visual"])
    args = parser.parse_args()
    return m3() if args.scenario == "m3" else m3_visual()


if __name__ == "__main__":
    sys.exit(main())
