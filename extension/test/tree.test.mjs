// Unit tests for the pure ordered-tree model: node --test extension/test/tree.test.mjs
//
// The model stores only relations: an ordered root list plus an ordered child list per node.
// The strip order is always the pre-order walk, never stored.
import test from 'node:test';
import assert from 'node:assert/strict';
import * as tree from '../src/background/tree.js';

function build(order, parents = {}) {
  const state = tree.createWindowState(order);
  for (const [tabId, parentId] of Object.entries(parents)) {
    tree.attachAsLastChild(state, Number(tabId), parentId);
  }
  tree.adoptOrder(state, order);
  return state;
}

test('preorder is the strip order', () => {
  const state = build([1, 2, 3, 4, 5], { 2: 1, 3: 1, 4: 3 });
  assert.deepEqual(tree.preorder(state), [1, 2, 3, 4, 5]);
  assert.equal(tree.depthOf(state, 1), 0);
  assert.equal(tree.depthOf(state, 2), 1);
  assert.equal(tree.depthOf(state, 4), 2);
});

test('attachAsLastChild appends and refuses cycles', () => {
  const state = build([1, 2, 3], { 2: 1, 3: 2 });
  tree.attachAsLastChild(state, 5, 1);
  assert.deepEqual(tree.childIdsOf(state, 1), [2, 5]);
  assert.equal(tree.parentOf(state, 5), 1);
  assert.equal(tree.attachAsLastChild(state, 1, 3), false);
  assert.equal(tree.attachAsLastChild(state, 1, 1), false);
  assert.equal(tree.parentOf(state, 1), null);
});

test('insertSibling reorders without changing the parent', () => {
  const state = build([1, 2, 3, 4], { 3: 1, 4: 1 });
  // "4" goes before "3": still a child of 1.
  tree.insertSibling(state, 4, 3, 'before');
  assert.deepEqual(tree.childIdsOf(state, 1), [4, 3]);
  assert.equal(tree.parentOf(state, 4), 1);
  // A root moves next to another root.
  tree.insertSibling(state, 2, 1, 'before');
  assert.deepEqual(state.roots, [2, 1]);
  assert.equal(tree.parentOf(state, 2), null);
});

test('subtreeIds is contiguous in strip order', () => {
  const state = build([1, 2, 3, 4, 5], { 2: 1, 3: 1, 4: 2 });
  assert.deepEqual(tree.subtreeIds(state, 1), [1, 2, 4, 3]);
  assert.deepEqual(tree.descendantsOf(state, 1), [2, 4, 3]);
  assert.deepEqual(tree.subtreeIds(state, 5), [5]);
});

test('collapsed ancestors hide descendants', () => {
  const state = build([1, 2, 3], { 2: 1, 3: 2 });
  tree.setCollapsed(state, 1, true);
  assert.equal(tree.isHidden(state, 2), true);
  assert.equal(tree.isHidden(state, 3), true);
  assert.equal(tree.isHidden(state, 1), false);
  assert.equal(tree.hiddenCount(state, 1), 2);
  assert.deepEqual(tree.expandAncestors(state, 3), [1]);
  assert.equal(tree.isHidden(state, 3), false);
});

test('removeTab promotes the children into its slot', () => {
  const state = build([1, 2, 3, 4], { 2: 1, 3: 2, 4: 2 });
  tree.removeTab(state, 2);
  assert.equal(tree.parentOf(state, 3), 1);
  assert.deepEqual(tree.childIdsOf(state, 1), [3, 4]);
  assert.deepEqual(tree.preorder(state), [1, 3, 4]);
});

test('adoptOrder keeps parents and re-derives sibling order', () => {
  const state = build([1, 2, 3, 4], { 3: 1, 4: 1 });
  // Chrome reports the children in the opposite order.
  tree.adoptOrder(state, [1, 4, 3, 2]);
  assert.deepEqual(tree.childIdsOf(state, 1), [4, 3]);
  assert.equal(tree.parentOf(state, 3), 1);
  assert.equal(tree.parentOf(state, 2), null);
  assert.deepEqual(tree.preorder(state), [1, 4, 3, 2]);
});

test('reconcile drops dead tabs and adopts unknown ones', () => {
  const state = build([1, 2, 3], { 2: 1, 3: 1 });
  tree.reconcile(state, [1, 3, 4]);
  assert.equal(state.nodes.has(2), false);
  assert.deepEqual(tree.preorder(state), [1, 3, 4]);
});

test('unknown tabs become roots at their strip position, not at the end', () => {
  // 5 was opened next to 2 (e.g. inside a tab group or beside a split view).
  const state = build([1, 2, 3, 4], { 4: 3 });
  tree.reconcile(state, [1, 2, 5, 3, 4]);
  assert.equal(tree.parentOf(state, 5), null);
  assert.deepEqual(tree.preorder(state), [1, 2, 5, 3, 4]);
});

test('unknown tabs inside a subtree land after that subtree', () => {
  const state = build([1, 2, 3], { 2: 1, 3: 1 });
  tree.reconcile(state, [1, 2, 9, 3]);
  assert.equal(tree.parentOf(state, 9), null);
  assert.deepEqual(tree.preorder(state), [1, 2, 3, 9]);
});

test('promoteOut lifts the children into the slot and makes the tab a root', () => {
  const state = build([1, 2, 3, 4, 5], { 2: 1, 3: 2, 4: 2, 5: 1 });
  tree.promoteOut(state, 2);
  tree.adoptOrder(state, [1, 2, 3, 4, 5]);
  assert.equal(tree.parentOf(state, 2), null);
  assert.deepEqual(tree.childIdsOf(state, 2), []);
  assert.deepEqual(tree.childIdsOf(state, 1), [3, 4, 5]);
  assert.deepEqual(tree.preorder(state), [1, 3, 4, 5, 2]);
});

test('promoteOut on a leaf only detaches it', () => {
  const state = build([1, 2, 3], { 2: 1 });
  tree.promoteOut(state, 2);
  tree.adoptOrder(state, [1, 2, 3]);
  assert.equal(tree.parentOf(state, 2), null);
  assert.deepEqual(tree.preorder(state), [1, 2, 3]);
});

test('detached subtrees can move to another window', () => {
  const source = build([1, 2, 3], { 2: 1, 3: 2 });
  const target = tree.createWindowState([9]);
  const moved = tree.detachSubtree(source, 1);
  tree.attachSubtreeAsRoot(target, 1, moved.nodes);
  assert.deepEqual(tree.preorder(source), []);
  assert.deepEqual(tree.preorder(target), [9, 1, 2, 3]);
  assert.equal(tree.parentOf(target, 3), 2);
});

test('displayState reports depth, hidden flags and child counts', () => {
  const state = build([1, 2, 3], { 2: 1, 3: 1 });
  tree.setCollapsed(state, 1, true);
  const display = tree.displayState(state, [1, 2, 3]);
  assert.deepEqual(display.windowTabs, [1, 2, 3]);
  assert.equal(display.tabs[1].hasChildren, true);
  assert.equal(display.tabs[1].collapsed, true);
  assert.equal(display.tabs[1].hiddenCount, 2);
  assert.equal(display.tabs[2].hidden, true);
  assert.equal(display.tabs[2].depth, 1);
});
