# M5 notes — drag & drop (rewritten 2026-11-08)

## Model change

The tree is now an **ordered tree**; the strip order is its pre-order walk and is never stored.

```js
state = { roots: [tabId...], nodes: Map<tabId, { children: [tabId...], collapsed }> }
```

Consequences:

- no `order` array, no `normalize()`, no `inferParentFromIndex()` — the old “let Chrome reorder,
  then reverse-engineer the parent from the drop index” path is gone (it is what produced the
  scrambled trees users saw);
- exactly two structural operations, both applied by the extension: re-parent
  (`attachAsLastChild`) and reorder among siblings (`insertSibling`);
- `tabs.onMoved` outside a drag now only re-derives sibling order (`adoptOrder`) and keeps every
  parent relation, then `tabs.move` restores the pre-order.

## Native side (`features/drag.cpp`)

| | |
|---|---|
| F4 detection | detours `DraggedTabsContainer::HandleTabDragInContainer`. Hit-tests the **pointer**
  against the rows the layout hook produced in its last pass (`StripSnapshot`, container
  coordinates, indent and hidden tabs applied). Reading `views::View::children_` of `host_view_`
  returned no children at all, so no drop was ever detected. Zones: middle half = `nest`, top
  quarter = `before`, bottom quarter = `after`, below the last tab = `end`. Inside the tree strip
  the original handler is **never** called: it moves the tabs in the tab model on every move,
  which is the native "other tabs make room" reorder. |
| drag source | `TabView::OnMousePressed` records the pressed tab; it travels as `draggedTabId` in
  `dragBegin` / `dropOnto` (the extension no longer guesses from the active tab). |
| F5 drop end | detours `TabDragHandlerImpl::EndDrag`. `kComplete` + a zone → `dropOnto {draggedTabId,
  targetTabId, zone}`; always `dragEnd {cancelled}`. |
| F6 highlight | `TabView::OnPaint`: translucent violet fill plus frame for `nest`, 3 px insertion line
  at the top/bottom edge for `before` / `after` / `end`. |

`debug.layout` also reports `stats.dropTarget` / `stats.dropZone` so the detection can be observed
without a screenshot.

## Extension side

- `dropOnto(targetTabId, zone)`: `nest` → become the target's last child; `before`/`after` →
  become the target's sibling at that position (parent unchanged). Dropping a parent onto its own
  descendant is refused.
- `dragBegin` temporarily collapses the dragged parent, `dragEnd` restores it.
- `__testDrop(targetTabId, zone)` exercises the same path from the sandbox.

## Verification status

Automated (`node --test extension/test/tree.test.mjs`, `scripts/e2e_drag.py`, `scripts/e2e.py m3`)
covers the tree model and every downstream path of the protocol.

The native detection was verified by manual drags in the sandbox (2026-11-08); synthetic mouse
input does not start a tab drag in this environment. Manual checklist:

1. drag onto the middle of another tab → row highlighted, release → nested as last child;
2. drag onto the top/bottom quarter → insertion line, release → sibling reorder, parent unchanged;
3. Esc mid-drag → nothing changes;
4. drag an expanded parent → subtree hides while dragging, travels with the parent on drop.
