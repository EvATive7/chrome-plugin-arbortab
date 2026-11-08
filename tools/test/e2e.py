"""End-to-end checks for the ArborTab extension in the sandbox Chrome (start it with
tools/dev/dev.py first).

    uv run --with websocket-client python tools/test/e2e.py tree       # open / collapse / close
    uv run --with websocket-client python tools/test/e2e.py drop       # drop protocol
    uv run --with websocket-client python tools/test/e2e.py tree-visual

The native drop detection needs a real mouse drag, which cannot be automated reliably; the
`drop` scenario drives everything downstream of it through the extension's test hooks
(`arbortab.__testDragBegin` / `__testDrop` / `__testDragEnd`).
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


def window_tree(worker: Session, browser: Session | None = None) -> tuple[list[int], dict]:
    """First window as (pre-order tab ids, {tab: (parent or None, collapsed)})."""
    window = next(iter(extension_state(worker, browser)["windows"].values()))
    order = [int(tab) for tab in window["order"]]
    parents = {int(tab): int(parent) for tab, parent in window.get("parents", [])}
    collapsed = {int(tab) for tab in window.get("collapsed", [])}
    return order, {tab: (parents.get(tab), tab in collapsed) for tab in order}


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


def report(name: str, ok: bool, detail: str = "") -> bool:
    print(f"{'PASS' if ok else 'FAIL'} {name}{' — ' + detail if detail else ''}")
    return ok


def connect() -> tuple[Session, Session]:
    browser = Session(browser_ws())
    browser.call("Target.setDiscoverTargets", {"discover": True})
    for _ in range(8):
        try:
            return browser, service_worker_session(browser)
        except SystemExit:
            # Wake the worker with a tab event: an idle MV3 worker stops on its own.
            browser.call("Target.createTarget", {"url": "about:blank"})
            time.sleep(1.5)
    raise SystemExit("the ArborTab service worker did not start")


def tree_scenario() -> int:
    browser, worker = connect()
    state = extension_state(worker, browser)
    ok = report("extension connected to the native module", bool(state.get("connected")),
                json.dumps(state.get("features")))
    ok &= report("native layout feature enabled", bool(state.get("features", {}).get("layout")))
    native = state.get("native") or {}
    ok &= report("protocols are compatible", state.get("updateNeeded") is None,
                 f"module range {native.get('protocolMin')}..{native.get('protocolMax')}")
    ok &= report("offsets are ready", (native.get("offsets") or {}).get("status") == "ready",
                 json.dumps(native.get("offsets")))

    order_before, _ = window_tree(worker, browser)
    parent = order_before[0]

    # chrome.tabs.create with openerTabId is exactly what a link-opened tab produces.
    call_worker(
        worker,
        f"chrome.tabs.create({{url:'https://example.org/', openerTabId:{parent}, active:false}})",
        browser)
    time.sleep(2.0)

    order, nodes = window_tree(worker, browser)
    added = [tab for tab in order if tab not in order_before]
    ok &= report("new tab was added to the tree", len(order) == len(order_before) + 1,
                 f"{len(order_before)} -> {len(order)}")
    if not added:
        report("new tab is a child of its opener", False, "no new tab id found")
        return 1
    child = added[-1]
    ok &= report("new tab is a child of its opener", nodes.get(child, (None, False))[0] == parent,
                 f"child={child} parent={nodes.get(child, (None, None))[0]} expected={parent}")

    call_worker(worker, f"arbortab.toggleCollapsed({parent})", browser)
    time.sleep(0.8)
    _, nodes = window_tree(worker, browser)
    ok &= report("collapsing the parent collapses the subtree", nodes[parent][1] is True)

    call_worker(worker, f"chrome.tabs.update({child}, {{active:true}})", browser)
    time.sleep(1.2)
    _, nodes = window_tree(worker, browser)
    ok &= report("activating a hidden child expands its ancestors", nodes[parent][1] is False)

    # Keep one unrelated tab alive: closing every tab of the last window quits the browser.
    call_worker(worker, "chrome.tabs.create({url:'about:blank', active:false})", browser)
    time.sleep(0.8)
    order_before, _ = window_tree(worker, browser)
    call_worker(worker, f"arbortab.closeTree({parent})", browser)
    time.sleep(1.8)
    remaining, _ = window_tree(worker, browser)
    ok &= report("closing the parent removed the subtree",
                 child not in remaining and parent not in remaining,
                 f"{order_before} -> {remaining}")
    ok &= report("browser survived the whole scenario",
                 bool(extension_state(worker, browser).get("connected")))
    return 0 if ok else 1


def drop_scenario() -> int:
    browser, worker = connect()

    # A dedicated parent with two children plus an unrelated tab, so the test does not depend
    # on whatever tree the sandbox already has.
    parent = call_worker(
        worker, "chrome.tabs.create({url:'https://example.com/', active:false}).then(t => t.id)",
        browser)
    time.sleep(1.2)
    for url in ("https://example.org/", "https://www.iana.org/help/example-domains"):
        call_worker(worker, f"chrome.tabs.create({{url:'{url}', openerTabId:{parent}, active:false}})",
                    browser)
        time.sleep(1.2)
    first, second = call_worker(
        worker,
        f"chrome.tabs.get({parent})"
        f".then(anchor => chrome.tabs.query({{windowId: anchor.windowId}}))"
        f".then(ts => ts.filter(t => t.openerTabId === {parent}).sort((a,b) => a.index-b.index)"
        f".map(t => t.id))",
        browser)
    outsider = call_worker(
        worker, "chrome.tabs.create({url:'https://example.net/', active:false}).then(t => t.id)",
        browser)
    time.sleep(2.0)

    def parent_of(nodes: dict, tab: int):
        return nodes.get(tab, (None, None))[0]

    def drop(target: int, zone: str, dragged: int, settle: float = 2.0):
        call_worker(worker, f"arbortab.__testDrop({target}, '{zone}', {dragged})", browser)
        time.sleep(settle)
        return window_tree(worker, browser)

    order, nodes = window_tree(worker, browser)
    children = [tab for tab in order if parent_of(nodes, tab) == parent]
    ok = report("the new parent has two children", children == [first, second], f"children={children}")
    ok &= report("the outsider is top level", parent_of(nodes, outsider) is None)

    # Dragging a parent hides its subtree for the duration of the drag.
    call_worker(worker, f"arbortab.__testDragBegin({parent})", browser)
    time.sleep(1.2)
    _, nodes = window_tree(worker, browser)
    ok &= report("dragging a parent temporarily collapses it", nodes[parent][1] is True)
    call_worker(worker, "arbortab.__testDragEnd()", browser)
    time.sleep(1.2)
    _, nodes = window_tree(worker, browser)
    ok &= report("the temporary collapse is undone on drag end", nodes[parent][1] is False)

    # Middle drop: the outsider becomes the last child of the parent.
    order, nodes = drop(parent, "nest", outsider, settle=2.5)
    ok &= report("a middle drop nests the tab under the target",
                 parent_of(nodes, outsider) == parent, f"parent={parent_of(nodes, outsider)}")
    ok &= report("the nested tab sits after the target's existing subtree",
                 order.index(outsider) > max(order.index(c) for c in (first, second)), str(order))

    # Edge drop: a sibling reorder under the anchor's parent (top level here).
    order, nodes = drop(parent, "before", outsider)
    ok &= report("an edge drop keeps the tab top level",
                 parent_of(nodes, outsider) is None, f"parent={parent_of(nodes, outsider)}")
    ok &= report("the tab moved in front of the anchor",
                 order.index(outsider) < order.index(parent), str(order))

    # Dropping after the parent's subtree keeps the subtree contiguous.
    order, nodes = drop(parent, "after", outsider)
    subtree = [parent, first, second]
    ok &= report("an after-drop lands after the whole anchor subtree",
                 order.index(outsider) > max(order.index(t) for t in subtree), str(order))
    ok &= report("the anchor subtree stays contiguous",
                 order[order.index(parent):order.index(parent) + 3] == subtree, str(order))

    # A parent dropped onto its own descendant is refused (no cycles).
    order, nodes = drop(second, "nest", parent, settle=1.5)
    ok &= report("dropping a parent onto its descendant is refused",
                 parent_of(nodes, parent) is None and parent_of(nodes, second) == parent,
                 f"nodes={nodes}")

    ok &= report("browser still healthy", bool(extension_state(worker, browser).get("connected")))
    return 0 if ok else 1


def tree_visual() -> int:
    """Builds a small tree so the strip can be inspected (and screenshotted) by hand."""
    browser, worker = connect()
    order, _ = window_tree(worker, browser)
    parent = order[0]
    for url in ("https://example.org/", "https://www.iana.org/help/example-domains"):
        call_worker(worker, f"chrome.tabs.create({{url:'{url}', openerTabId:{parent}, active:false}})",
                    browser)
        time.sleep(1.2)
    time.sleep(1.0)
    print(json.dumps(extension_state(worker, browser)["windows"], indent=1))
    return 0


SCENARIOS = {"tree": tree_scenario, "drop": drop_scenario, "tree-visual": tree_visual}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", choices=list(SCENARIOS))
    return SCENARIOS[parser.parse_args().scenario]()


if __name__ == "__main__":
    sys.exit(main())
