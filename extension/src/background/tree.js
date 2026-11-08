// Pure tree model for ArborTab. No chrome.* calls, so it can be unit tested under node.
//
// The tree is ordered: every node owns an ordered list of children, and the window owns an
// ordered list of roots. The real tab strip order is the pre-order walk of that tree, so it is
// derived, never stored. The only two mutations are "re-parent" and "reorder among siblings".

export function createWindowState(liveTabIds = []) {
  return { roots: [...liveTabIds], nodes: new Map() };
}

function nodeOf(state, tabId) {
  let node = state.nodes.get(tabId);
  if (!node) {
    node = { children: [], collapsed: false };
    state.nodes.set(tabId, node);
  }
  return node;
}

/** Sibling list that owns `tabId`: the window roots or a node's children. */
export function siblingsOf(state, parentId) {
  return parentId == null ? state.roots : nodeOf(state, parentId).children;
}

export function parentOf(state, tabId) {
  if (state.roots.includes(tabId)) {
    return null;
  }
  for (const [id, node] of state.nodes) {
    if (node.children.includes(tabId)) {
      return id;
    }
  }
  return null;
}

export function childIdsOf(state, parentId) {
  return [...siblingsOf(state, parentId)];
}

/** The real strip order implied by the tree. */
export function preorder(state) {
  const result = [];
  const seen = new Set();
  const walk = (tabId) => {
    if (seen.has(tabId)) {
      return;
    }
    seen.add(tabId);
    result.push(tabId);
    for (const childId of childIdsOf(state, tabId)) {
      walk(childId);
    }
  };
  for (const root of state.roots) {
    walk(root);
  }
  return result;
}

/** Subtree in pre-order, root first (i.e. contiguous in the strip). */
export function subtreeIds(state, rootId) {
  if (rootId == null) {
    return [];
  }
  const result = [];
  const seen = new Set();
  const walk = (tabId) => {
    if (seen.has(tabId)) {
      return;
    }
    seen.add(tabId);
    result.push(tabId);
    for (const childId of childIdsOf(state, tabId)) {
      walk(childId);
    }
  };
  walk(rootId);
  return result;
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

// Ancestors that are currently collapsed, outermost first.
export function collapsedAncestors(state, tabId) {
  return ancestorIds(state, tabId).filter((id) => isCollapsed(state, id)).reverse();
}

export function isHidden(state, tabId) {
  return collapsedAncestors(state, tabId).length > 0;
}

export function expandAncestors(state, tabId) {
  const collapsed = collapsedAncestors(state, tabId);
  for (const id of collapsed) {
    setCollapsed(state, id, false);
  }
  return collapsed;
}

export function hiddenCount(state, tabId) {
  const descendants = descendantsOf(state, tabId);
  if (isCollapsed(state, tabId)) {
    return descendants.length;
  }
  return descendants.filter((id) => isHidden(state, id)).length;
}

// ---------------------------------------------------------------------------------------
// Mutations: detach, re-parent, reorder among siblings
// ---------------------------------------------------------------------------------------

/** Removes `tabId` from its parent's child list; the tab keeps its own children. */
export function detach(state, tabId) {
  state.roots = state.roots.filter((id) => id !== tabId);
  for (const node of state.nodes.values()) {
    if (node.children.includes(tabId)) {
      node.children = node.children.filter((id) => id !== tabId);
    }
  }
}

/** Re-parents `tabId` as the last child of `parentId` (null = last root). */
export function attachAsLastChild(state, tabId, parentId) {
  const parent = parentId ?? null;
  if (tabId === parent) {
    return false;
  }
  if (parent != null && subtreeIds(state, tabId).includes(parent)) {
    return false;
  }
  detach(state, tabId);
  siblingsOf(state, parent).push(tabId);
  return true;
}

/** Reorders `tabId` next to `anchorId` among the anchor's siblings; parent unchanged. */
export function insertSibling(state, tabId, anchorId, where = 'after') {
  if (tabId === anchorId) {
    return false;
  }
  if (subtreeIds(state, tabId).includes(anchorId)) {
    return false;
  }
  const parent = parentOf(state, anchorId);
  detach(state, tabId);
  const list = siblingsOf(state, parent);
  const index = list.indexOf(anchorId);
  const at = index < 0 ? list.length : (where === 'before' ? index : index + 1);
  list.splice(at, 0, tabId);
  return true;
}

/** Removes a tab, splicing its children into its slot (they are promoted one level). */
export function removeTab(state, tabId) {
  const parent = parentOf(state, tabId);
  const list = siblingsOf(state, parent);
  const index = list.indexOf(tabId);
  const children = [...(state.nodes.get(tabId)?.children || [])];
  detach(state, tabId);
  state.nodes.delete(tabId);
  const target = siblingsOf(state, parent);
  target.splice(index < 0 ? target.length : index, 0, ...children);
}

/** Detaches a whole subtree from the window and returns its ids plus its node records. */
export function detachSubtree(state, tabId) {
  const ids = subtreeIds(state, tabId);
  const nodes = new Map();
  for (const id of ids) {
    if (state.nodes.has(id)) {
      nodes.set(id, state.nodes.get(id));
      state.nodes.delete(id);
    }
  }
  detach(state, tabId);
  return { ids, nodes };
}

/** Adds a detached subtree back as the last root of the window. */
export function attachSubtreeAsRoot(state, tabId, nodes) {
  for (const [id, node] of nodes) {
    state.nodes.set(id, node);
  }
  if (!state.roots.includes(tabId)) {
    state.roots.push(tabId);
  }
}

/**
 * Re-derives sibling order from the real strip order without changing any parent relation.
 * Used when Chrome reorders tabs behind our back (native drag we did not intercept).
 */
export function adoptOrder(state, liveTabIds) {
  const live = new Set(liveTabIds);
  const rank = new Map(liveTabIds.map((id, index) => [id, index]));
  for (const id of [...state.nodes.keys()]) {
    if (!live.has(id)) {
      state.nodes.delete(id);
    }
  }
  const seen = new Set();
  const sortList = (list) => list
    .filter((id) => live.has(id) && !seen.has(id))
    .map((id) => {
      seen.add(id);
      return id;
    })
    .sort((a, b) => rank.get(a) - rank.get(b));
  state.roots = sortList(state.roots);
  for (const node of state.nodes.values()) {
    node.children = sortList(node.children);
  }
  for (const id of liveTabIds) {
    if (!seen.has(id)) {
      seen.add(id);
      state.roots.push(id);
    }
  }
  const order = preorder(state);
  return order.length !== liveTabIds.length || order.some((id, index) => id !== liveTabIds[index]);
}

/** Drops tabs that no longer exist and adopts unknown ones. */
export function reconcile(state, liveTabIds) {
  return adoptOrder(state, liveTabIds);
}

// Full display state for the native module.
export function displayState(state, tabIds = preorder(state)) {
  const tabs = {};
  for (const tabId of tabIds) {
    const node = state.nodes.get(tabId);
    const children = childIdsOf(state, tabId);
    tabs[tabId] = {
      depth: depthOf(state, tabId),
      hidden: isHidden(state, tabId),
      hasChildren: children.length > 0,
      collapsed: node ? node.collapsed : false,
      hiddenCount: hiddenCount(state, tabId),
    };
  }
  return { windowTabs: [...tabIds], tabs };
}
