# M3 verification notes (2026-11-08)

Sandbox: `C:\Users\evative7\Desktop\Playground\_test\portable` (Chrome 156.0.8078.80),
profile `prof`, CDP on 9334, extension loaded via `Extensions.loadUnpacked`.

## Automated checks (`scripts/e2e.py m3`)

| check | result |
|---|---|
| extension connected to the native module | PASS |
| native layout feature enabled | PASS |
| new tab was added to the tree | PASS |
| new tab is a child of its opener | PASS |
| collapsing the parent collapses the subtree | PASS |
| activating a hidden child expands its ancestors | PASS |
| closing the parent removed the subtree | PASS |
| browser survived the whole scenario | PASS |

Visual: `artifacts/m3-tree-indent.png` (parent + two indented children),
`artifacts/m3-collapsed.png` (children hidden after collapsing the parent).

## What this milestone changed on the native side

- `core/state`: flat `tabId -> display state` table, last writer wins, per-client ownership
  for pruning on disconnect.
- `core/chrome_api`: TabView detection through the vtable whitelist, tab id via
  `TabView::GetTabInterface` → `TabModel::GetContents` (vtable slot 3) → `SessionTabHelper::IdForTab`.
- `core/ui_thread`: message-only window on the browser UI thread; host views are recorded from
  the layout hook (via `LayoutManagerBase::host_view_`, offset `0x38`) with a 60 s TTL and an SEH
  guard around every call.
- `features/layout`: F1 rewrite of `views::ProposedLayout` (indent, hide, reflow, host height).
- `link/ws_server`: RFC 6455 server, origin check against `config.ini: extensions`, `hello` /
  `state` / `ping` / `debug.layout`.

## Known gaps carried into M4/M5

- Collapse marker on the parent tab is not drawn yet (F3, M4).
- Clicking the favicon does not toggle collapse yet (F2, M4).
- Drag & drop nesting is not implemented yet (F4–F6, M5).
- `chrome://extensions` shortcuts use Alt+Shift+Comma / W / Up / Down (Chrome rejects some
  letter combinations such as `Alt+Shift+KeyW`).
