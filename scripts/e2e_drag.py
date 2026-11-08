"""Verifies the drag & drop half of M5 without synthetic mouse input.

The native drop detection (F4) needs a real drag, but everything downstream of it can be
exercised through the same extension entry points the protocol uses.
"""
from __future__ import annotations

import json
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

    # The extension must have been loaded by a freshly started sandbox browser: reloading an
    # unpacked extension through chrome.runtime.reload() leaves the worker stopped here.
    # Build a dedicated parent with two children plus an unrelated tab, so the test does not
    # depend on whatever tree the sandbox already has.
    parent = call_worker(
        worker, "chrome.tabs.create({url:'https://example.com/', active:false}).then(t => t.id)",
        browser)
    time.sleep(1.2)
    for url in ("https://example.org/", "https://www.iana.org/help/example-domains"):
        call_worker(worker, f"chrome.tabs.create({{url:'{url}', openerTabId:{parent}, active:false}})",
                    browser)
        time.sleep(1.2)
    outsider = call_worker(
        worker, "chrome.tabs.create({url:'https://example.net/', active:false}).then(t => t.id)",
        browser)
    time.sleep(2.0)

    def snapshot():
        current = extension_state(worker, browser)
        win = next(iter(current["windows"].values()))
        return win, {int(t): (p, c) for t, p, c in win["nodes"]}

    window, nodes = snapshot()
    children = [t for t, (p, _c) in nodes.items() if p == parent]
    report("the new parent has two children", len(children) == 2, str(children))
    report("the outsider is top level", nodes.get(outsider, (None, None))[0] is None)

    # Dragging a parent hides its subtree for the duration of the drag (PLAN.md section 3).
    call_worker(worker, f"chrome.tabs.update({parent}, {{active:true}})", browser)
    time.sleep(0.8)
    call_worker(worker, "arbortab.__testDragBegin()", browser)
    time.sleep(1.2)
    window, nodes = snapshot()
    report("dragging a parent temporarily collapses it", nodes[parent][1] is True)
    call_worker(worker, "arbortab.__testDragEnd()", browser)
    time.sleep(1.2)
    window, nodes = snapshot()
    report("the temporary collapse is undone on drag end", nodes[parent][1] is False)

    # Dropping the outsider onto the parent nests it as the last child of that subtree.
    call_worker(worker, f"chrome.tabs.update({outsider}, {{active:true}})", browser)
    time.sleep(0.8)
    call_worker(worker, f"arbortab.__testDrop({parent})", browser)
    time.sleep(2.5)
    window, nodes = snapshot()
    order = window["order"]
    report("dropped tab became a child of the target",
           nodes.get(outsider, (None, None))[0] == parent,
           f"parent={nodes.get(outsider, (None, None))[0]}")
    report("dropped tab sits after the target's existing subtree",
           order.index(outsider) > max(order.index(c) for c in children), str(order))
    report("browser still healthy", bool(extension_state(worker, browser).get("connected")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
