# M5 drag & drop notes (2026-11-08)

## Implemented

- **F4 drop detection** (`features/drag.cpp`): detours
  `DraggedTabsContainer::HandleTabDragInContainer`. The dragged rectangle is hit-tested against
  the host view's children: only TabViews count, the dragged view itself is skipped by
  overlap, and a candidate matches when the drag centre falls inside its middle half
  (`[y + h/4, y + 3h/4]`, PLAN.md §3). On a match the original handler is *not* called, so
  Chrome's own "cross the midline and reorder" behaviour is suppressed.
- **F5 drag end**: detours `TabDragHandlerImpl::EndDrag(EndDragReason)`. `kComplete` (0) with a
  drop target sends `dropOnto {targetTabId}`, every other reason counts as cancelled; both
  paths send `dragEnd {cancelled}` and clear the state.
- **F6 highlight**: the existing `TabView::OnPaint` detour fills the drop target with
  `config.highlightColor` (default `#663D7EFF`).
- Extension: `dragBegin` temporarily collapses an expanded parent so only that tab travels
  (restored on `dragEnd`); `dropOnto` reparents the active tab and moves its subtree to the end
  of the target's subtree.

## Verified automatically (`scripts/e2e_drag.py`)

| check | result |
|---|---|
| the new parent has two children | PASS |
| the outsider is top level | PASS |
| dragging a parent temporarily collapses it | PASS |
| the temporary collapse is undone on drag end | PASS |
| dropped tab became a child of the target | PASS |
| dropped tab sits after the target's existing subtree | PASS |
| browser still healthy | PASS |

These exercise everything downstream of the native drop detection through the same entry
points the protocol uses (`arbortab.__testDrop`, `__testDragBegin`, `__testDragEnd`).

## Still needs a manual drag

`HandleTabDragInContainer` only runs while the mouse is really dragging a tab, so the
detection itself (middle-band hit test, highlight, suppressing the native reorder) has to be
checked by hand in the sandbox:

1. drag tab B and hold it over the **middle** of tab A → A must highlight (translucent
   overlay) and no reordering animation should start;
2. release → B (and its subtree) becomes the last child of A;
3. drag over the **top or bottom quarter** of a tab → no highlight, the native reorder happens
   and the extension infers the parent from the drop index;
4. press Esc mid-drag → `dragEnd {cancelled: true}`, nothing changes.
