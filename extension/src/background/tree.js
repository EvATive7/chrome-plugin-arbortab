// Pure tree model for ArborTab. No chrome.* calls, so it can be unit tested under node.
//
// A window's tabs form a forest. `order` is the real strip order (chrome tab ids, ascending by
// index). Every subtree must be contiguous and start at its root, which `normalize` enforces.

export function createWindowState(order = []) {
  return { order: [...order], nodes: new Map() };
}

function nodeOf(state, tabId) {
  let node = state.nodes.get(tabId);
  if (!node) {
    node = { parentId: null, collapsed: false };
    state.nodes.set(tabId, node);
  }
  return node;
}

export function parentOf(state, tabId) {
  const node = state.nodes.get(tabId);
  return node ? node.parentId : null;
}

export function isCollapsed(state, tabId) {
  const node = state.nodes.get(tabId);
  return node ? node.collapsed : false;
}

export function setCollapsed(state, tabId, collapsed) {
  const node = nodeOf(state, tabId);
  if (node.collapsed === collapsed) {
    return false;
  }
  node.collapsed = collapsed;
  return true;
}

export function setParent(state, tabId, parentId) {
  if (tabId === parentId) {
    return false;
  }
  // Refuse to create a cycle: walk up from the prospective parent.
  let cursor = parentId;
  while (cursor != null) {
    if (cursor === tabId) {
      return false;
    }
    cursor = parentOf(state, cursor);
  }
  const node = nodeOf(state, tabId);
  const nextParent = parentId ?? null;
  if (node.parentId === nextParent) {
    return false;
  }
  node.parentId = nextParent;
  return true;
}

export function childIdsOf(state, parentId) {
  return state.order.filter((tabId) => parentOf(state, tabId) === parentId);
}

// Subtree in strip order, root first. Includes the root itself.
export function subtreeIds(state, rootId) {
  if (rootId == null) {
    return [];
  }
  const result = [];
  const stack = [rootId];
  while (stack.length) {
    const current = stack.pop();
    result.push(current);
    const children = childIdsOf(state, current);
    for (let index = children.length - 1; index >= 0; index -= 1) {
      stack.push(children[index]);
    }
  }
  // Keep the real strip order so callers can rely on it.
  return result.sort((a, b) => state.order.indexOf(a) - state.order.indexOf(b));
}

export function descendantsOf(state, rootId) {
  return subtreeIds(state, rootId).filter((tabId) => tabId !== rootId);
}

export function depthOf(state, tabId) {
  let depth = 0;
  let cursor = parentOf(state, tabId);
  const seen = new Set();
  while (cursor != null && !seen.has(cursor)) {
    seen.add(cursor);
    depth += 1;
    cursor = parentOf(state, cursor);
  }
  return depth;
}

export function ancestorIds(state, tabId) {
  const result = [];
  let cursor = parentOf(state, tabId);
  const seen = new Set();
  while (cursor != null && !seen.has(cursor)) {
    seen.add(cursor);
    result.push(cursor);
    cursor = parentOf(state, cursor);
  }
  return result;
}

// Ancestors that are currently collapsed, outermost first.
export function collapsedAncestors(state, tabId) {
  return ancestorIds(state, tabId).filter((id) => isCollapsed(state, id)).reverse();
}

export function isHidden(state, tabId) {
  return collapsedAncestors(state, tabId).length > 0;
}

export function hiddenCount(state, tabId) {
  let count = 0;
  for (const id of descendantsOf(state, tabId)) {
    count += isHidden(state, id) ? 1 : 0;
  }
  // Descendants hidden directly by this tab's own collapse state.
  if (isCollapsed(state, tabId)) {
    count = descendantsOf(state, tabId).length;
  }
  return count;
}

export function expandAncestors(state, tabId) {
  const collapsed = collapsedAncestors(state, tabId);
  for (const id of collapsed) {
    setCollapsed(state, id, false);
  }
  return collapsed;
}

// Rewrites `order` so that every subtree is contiguous and directly follows its parent.
export function normalize(state) {
  const present = new Set(state.order);
  const roots = [];
  for (const tabId of state.order) {
    const parent = parentOf(state, tabId);
    if (parent == null || !present.has(parent)) {
      roots.push(tabId);
    }
  }
  const result = [];
  const emit = (tabId) => {
    result.push(tabId);
    for (const childId of childIdsOf(state, tabId)) {
      emit(childId);
    }
  };
  for (const root of roots) {
    emit(root);
  }
  const changed = result.length !== state.order.length ||
    result.some((tabId, index) => tabId !== state.order[index]);
  state.order = result;
  return changed;
}

// Drops tabs that no longer exist and converts orphans into roots.
export function reconcile(state, liveTabIds) {
  const live = new Set(liveTabIds);
  let changed = false;
  for (const tabId of [...state.nodes.keys()]) {
    if (!live.has(tabId)) {
      state.nodes.delete(tabId);
      changed = true;
    }
  }
  for (const [tabId, node] of state.nodes) {
    if (node.parentId != null && !live.has(node.parentId)) {
      node.parentId = null;
      changed = true;
    }
  }
  const filtered = state.order.filter((tabId) => live.has(tabId));
  if (filtered.length !== state.order.length) {
    state.order = filtered;
    changed = true;
  }
  for (const tabId of live) {
    if (!state.order.includes(tabId)) {
      state.order.push(tabId);
      changed = true;
    }
  }
  return changed;
}

// Inserts `tabId` directly after the last tab of `anchorId`'s subtree.
export function placeAfterSubtree(state, tabId, anchorId) {
  const subtree = subtreeIds(state, anchorId);
  if (!subtree.length) {
    return false;
  }
  const anchorTail = subtree[subtree.length - 1];
  if (anchorTail === tabId) {
    return false;
  }
  const currentIndex = state.order.indexOf(tabId);
  if (currentIndex === -1) {
    return false;
  }
  state.order.splice(currentIndex, 1);
  const anchorIndex = state.order.indexOf(anchorTail);
  if (anchorIndex === -1) {
    return false;
  }
  state.order.splice(anchorIndex + 1, 0, tabId);
  return true;
}

// Full display state for the native module.
export function displayState(state, tabIds = state.order) {
  const tabs = {};
  let windowTabCount = 0;
  for (const tabId of tabIds) {
    const node = state.nodes.get(tabId);
    const depth = depthOf(state, tabId);
    const children = childIdsOf(state, tabId);
    tabs[tabId] = {
      depth,
      hidden: isHidden(state, tabId),
      hasChildren: children.length > 0,
      collapsed: node ? node.collapsed : false,
      hiddenCount: hiddenCount(state, tabId),
    };
    windowTabCount += 1;
  }
  return { windowTabs: tabIds.slice(0, windowTabCount), tabs };
}

// Determines the parent a tab should get after a native drag & drop between siblings.
// `order` is the strip order before the drag; `tabId` is the moved tab, `index` its new index.
export function inferParentFromIndex(state, tabId, index) {
  const withoutMoved = state.order.filter((id) => id !== tabId);
  const below = withoutMoved[index];
  if (below != null) {
    return parentOf(state, below);
  }
  const above = withoutMoved[index - 1];
  if (above != null) {
    return parentOf(state, above);
  }
  return null;
}
