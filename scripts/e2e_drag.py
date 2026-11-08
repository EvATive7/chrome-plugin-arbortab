"""Verifies the drop protocol of M5 without synthetic mouse input.

The native drop detection (F4) needs a real drag, but everything downstream of it runs through
the same extension entry points the protocol uses (`arbortab.__testDrop`).

    uv run --with websocket-client python scripts/e2e_drag.py
"""
from __future__ import annotations

import sys
import time

from e2e import Session, browser_ws, call_worker, extension_state, report, service_worker_session


def main() -> int:
    browser = Session(browser_ws())
    browser.call("Target.setDiscoverTargets", {"discover": True})
    worker = None
    for _ in range(8):
        try:
            worker = service_worker_session(browser)
            break
        except SystemExit:
            # Wake the worker with a tab event: an idle MV3 worker stops on its own.
            browser.call("Target.createTarget", {"url": "about:blank"})
            time.sleep(1.5)
    if worker is None:
        raise SystemExit("the ArborTab service worker did not start")

    # A dedicated parent with two children plus unrelated tabs, so the test does not depend on
    # whatever tree the sandbox already has.
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

    def snapshot():
        current = extension_state(worker, browser)
        window = next(iter(current["windows"].values()))
        return window, {int(t): (p, c) for t, p, c in window["nodes"]}

    def activate(tab_id):
        call_worker(worker, f"chrome.tabs.update({tab_id}, {{active:true}})", browser)
        time.sleep(0.9)

    window, nodes = snapshot()
    report("the new parent has two children",
           [t for t, (p, _c) in nodes.items() if p == parent] == [first, second],
           f"children={[t for t, (p, _c) in nodes.items() if p == parent]}")
    report("the outsider is top level", nodes.get(outsider, (None, None))[0] is None)

    # Dragging a parent hides its subtree for the duration of the drag (PLAN.md section 3).
    activate(parent)
    call_worker(worker, "arbortab.__testDragBegin()", browser)
    time.sleep(1.2)
    window, nodes = snapshot()
    report("dragging a parent temporarily collapses it", nodes[parent][1] is True)
    call_worker(worker, "arbortab.__testDragEnd()", browser)
    time.sleep(1.2)
    window, nodes = snapshot()
    report("the temporary collapse is undone on drag end", nodes[parent][1] is False)

    # Middle drop: the outsider becomes the last child of the parent.
    activate(outsider)
    call_worker(worker, f"arbortab.__testDrop({parent}, 'nest')", browser)
    time.sleep(2.5)
    window, nodes = snapshot()
    order = window["order"]
    report("a middle drop nests the tab under the target",
           nodes.get(outsider, (None, None))[0] == parent,
           f"parent={nodes.get(outsider, (None, None))[0]}")
    report("the nested tab sits after the target's existing subtree",
           order.index(outsider) > max(order.index(c) for c in (first, second)), str(order))

    # Edge drop: a sibling reorder that keeps the parent.
    # Dropping the outsider before the parent: it stays top level and moves in front of it.
    activate(outsider)
    call_worker(worker, f"arbortab.__testDrop({parent}, 'before')", browser)
    time.sleep(2.0)
    window, nodes = snapshot()
    order = window["order"]
    report("an edge drop keeps the tab top level",
           nodes.get(outsider, (None, None))[0] is None,
           f"parent={nodes.get(outsider, (None, None))[0]}")
    report("the tab moved in front of the anchor", order.index(outsider) < order.index(parent),
           str(order))

    # Dropping after the parent's subtree keeps the subtree contiguous.
    activate(outsider)
    call_worker(worker, f"arbortab.__testDrop({parent}, 'after')", browser)
    time.sleep(2.0)
    window, nodes = snapshot()
    order = window["order"]
    subtree = [parent, first, second]
    report("an after-drop lands after the whole anchor subtree",
           order.index(outsider) > max(order.index(t) for t in subtree), str(order))
    report("the anchor subtree stays contiguous",
           order[order.index(parent):order.index(parent) + 3] == subtree, str(order))

    # A parent dropped onto its own descendant is refused (no cycles).
    activate(parent)
    call_worker(worker, f"arbortab.__testDrop({second}, 'nest')", browser)
    time.sleep(1.5)
    window, nodes = snapshot()
    report("dropping a parent onto its descendant is refused",
           nodes.get(parent, (None, None))[0] is None
           and nodes.get(second, (None, None))[0] == parent,
           f"nodes={nodes}")

    report("browser still healthy", bool(extension_state(worker, browser).get("connected")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
