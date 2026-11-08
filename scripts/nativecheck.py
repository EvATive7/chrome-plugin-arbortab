"""Drive the ArborTab native endpoint directly (no extension needed).

Used for milestone verification in the sandbox Chrome:
    uv run --with websocket-client python scripts/nativecheck.py --depths 0,1,1 --collapsed 0
    uv run --with websocket-client python scripts/nativecheck.py --layout
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request

PORTS = (47821, 47822, 47823, 47824, 47825)
ORIGIN = "chrome-extension://egjpjafmlomkpnncliaigklbdbflface"


def connect():
    import websocket  # noqa: PLC0415

    last = None
    for port in PORTS:
        try:
            return websocket.create_connection(
                f"ws://127.0.0.1:{port}/arbortab", timeout=10,
                origin=ORIGIN, suppress_origin=False)
        except Exception as exc:  # noqa: BLE001
            last = exc
    raise SystemExit(f"cannot reach the native endpoint: {last}")


def tab_ids(port: int = 9334) -> list[dict]:
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=5) as response:
        targets = json.load(response)
    return [t for t in targets if t["type"] == "page"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--depths", help="comma separated depth per tab (page targets)")
    parser.add_argument("--hidden", default="", help="comma separated 0/1 per tab")
    parser.add_argument("--collapsed", default="", help="comma separated 0/1 (has collapsed subtree)")
    parser.add_argument("--children", default="", help="comma separated 0/1 (hasChildren)")
    parser.add_argument("--indent", type=int, default=40)
    parser.add_argument("--layout", action="store_true", help="print the last layout snapshot")
    parser.add_argument("--hold", type=float, default=0.0, help="keep the socket open for N seconds")
    args = parser.parse_args()

    ws = connect()
    hello = json.loads(ws.recv())
    print("hello:", json.dumps(hello, ensure_ascii=False))
    if not hello.get("features", {}).get("layout"):
        print("WARNING: the native layout feature is not enabled", file=sys.stderr)

    if args.layout:
        ws.send(json.dumps({"type": "debug.layout"}))
        for _ in range(20):
            reply = json.loads(ws.recv())
            if reply.get("type") == "debug.layout":
                print("layout:", json.dumps(reply.get("layouts"), indent=1, ensure_ascii=False))
                break
        ws.close()
        return 0

    pages = tab_ids()
    if not pages:
        raise SystemExit("no page targets; is the sandbox Chrome running with --remote-debugging-port?")

    def series(text: str, default: int) -> list[int]:
        if not text:
            return [default] * len(pages)
        values = [int(x) for x in text.split(",")]
        return values + [values[-1]] * (len(pages) - len(values))

    depths = series(args.depths, 0)
    hidden = series(args.hidden, 0)
    collapsed = series(args.collapsed, 0)
    children = series(args.children, 0)

    # Tab ids are not exposed by /json/list, so the native side reports them through a layout
    # snapshot. A state update forces a relayout, which is what fills the snapshot.
    snapshot: list[dict] = []
    for _attempt in range(8):
        ws.send(json.dumps({"type": "state", "windowTabs": [], "tabs": {}, "config": {}}))
        ws.send(json.dumps({"type": "debug.layout"}))
        for _ in range(20):
            message = json.loads(ws.recv())
            if message.get("type") == "debug.layout":
                snapshot = [e for e in (message.get("layouts") or []) if e.get("isTab")]
                break
            time.sleep(0.02)
        if snapshot:
            break
        time.sleep(0.4)
    if not snapshot:
        print("no layout snapshot yet (open/close a tab to force a layout pass)", file=sys.stderr)
        ws.close()
        return 1

    ids = [entry["tabId"] for entry in snapshot]
    print("tab ids:", ids)

    tabs = {}
    for index, tab_id in enumerate(ids):
        tabs[str(tab_id)] = {
            "depth": depths[index] if index < len(depths) else 0,
            "hidden": bool(hidden[index]) if index < len(hidden) else False,
            "hasChildren": bool(children[index]) if index < len(children) else False,
            "collapsed": bool(collapsed[index]) if index < len(collapsed) else False,
            "hiddenCount": 0,
        }
    ws.send(json.dumps({
        "type": "state",
        "windowTabs": ids,
        "tabs": tabs,
        "config": {"indent": args.indent, "showCount": True, "highlightColor": "#663D7EFF"},
    }))
    print("state sent")
    if args.hold:
        time.sleep(args.hold)
    ws.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
