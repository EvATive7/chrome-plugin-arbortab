// Unit tests for the pure tree model: node --test extension/test
import test from 'node:test';
import assert from 'node:assert/strict';
import * as tree from '../src/background/tree.js';

function build(order, parents = {}) {
  const state = tree.createWindowState(order);
  for (const [tabId, parentId] of Object.entries(parents)) {
    state.nodes.set(Number(tabId), { parentId, collapsed: false });
  }
  return state;
}

test('depthOf follows the parent chain', () => {
  const state = build([1, 2, 3, 4], { 2: 1, 3: 2 });
  assert.equal(tree.depthOf(state, 1), 0);
  assert.equal(tree.depthOf(state, 2), 1);
  assert.equal(tree.depthOf(state, 3), 2);
  assert.equal(tree.depthOf(state, 4), 0);
});

test('setParent refuses cycles', () => {
  const state = build([1, 2, 3], { 2: 1, 3: 2 });
  assert.equal(tree.setParent(state, 1, 3), false);
  assert.equal(tree.setParent(state, 1, 1), false);
  assert.equal(tree.parentOf(state, 1), null);
});

test('subtreeIds stays in strip order and includes the root', () => {
  const state = build([1, 2, 3, 4, 5], { 2: 1, 3: 1, 4: 2 });
  assert.deepEqual(tree.subtreeIds(state, 1), [1, 2, 3, 4]);
  assert.deepEqual(tree.descendantsOf(state, 1), [2, 3, 4]);
  assert.deepEqual(tree.subtreeIds(state, 5), [5]);
});

test('collapsed ancestors hide descendants', () => {
  const state = build([1, 2, 3], { 2: 1, 3: 2 });
  tree.setCollapsed(state, 1, true);
  assert.equal(tree.isHidden(state, 2), true);
  assert.equal(tree.isHidden(state, 3), true);
  assert.equal(tree.isHidden(state, 1), false);
  assert.equal(tree.hiddenCount(state, 1), 2);
  const expanded = tree.expandAncestors(state, 3);
  assert.deepEqual(expanded, [1]);
  assert.equal(tree.isHidden(state, 3), false);
});

test('normalize keeps subtrees contiguous after their parent', () => {
  const state = build([3, 1, 2, 4], { 2: 1, 3: 2 });
  tree.normalize(state);
  assert.deepEqual(state.order, [1, 2, 3, 4]);
});

test('normalize treats orphans as roots', () => {
  const state = build([1, 2, 3], { 2: 99 });
  tree.normalize(state);
  assert.deepEqual(state.order, [1, 2, 3]);
  assert.equal(tree.parentOf(state, 2), 99);
  tree.reconcile(state, [1, 2, 3]);
  assert.equal(tree.parentOf(state, 2), null);
});

test('placeAfterSubtree moves a tab to the end of the anchor subtree', () => {
  const state = build([1, 5, 2, 3], { 2: 1, 3: 1 });
  tree.placeAfterSubtree(state, 5, 1);
  assert.deepEqual(state.order, [1, 2, 3, 5]);
});

test('inferParentFromIndex derives the parent from the neighbour below', () => {
  const state = build([1, 2, 3], { 2: 1 });
  // Dropping "3" at index 1 puts it right after 1 and before 2, which is depth 1 under 1.
  assert.equal(tree.inferParentFromIndex(state, 3, 1), tree.parentOf(state, 2));
  assert.equal(tree.inferParentFromIndex(state, 3, 0), tree.parentOf(state, 1));
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

test('reconcile drops dead tabs and adopts unknown ones', () => {
  const state = build([1, 2, 3], { 2: 1, 3: 1 });
  tree.reconcile(state, [1, 3, 4]);
  assert.equal(state.nodes.has(2), false);
  assert.deepEqual(state.order, [1, 3, 4]);
});
