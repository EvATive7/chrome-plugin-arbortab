// ArborTab service worker: keeps the tree model in sync with chrome.tabs and pushes the
// display state to the native module (PLAN.md sections 4.2/4.3).

import * as tree from './tree.js';
import { NativeLink } from './native-link.js';

const DEFAULT_CONFIG = {
  indent: 16,
  confirmThreshold: 0, // close descendants without asking (0 = never ask)
  newTabButtonIsChild: false,
  showCount: true,
  highlightColor: '#663D7EFF',
};

const SESSION_KEY = 'arbortab.windows';

/** @type {Map<number, ReturnType<typeof tree.createWindowState>>} */
let windows = new Map();
let config = { ...DEFAULT_CONFIG };
let dragActive = false;
/** Tab that was collapsed only for the duration of a drag (PLAN.md §3: drag a parent alone). */
let dragTemporarilyCollapsed = null;
let stateVersion = 0;
let lastSentVersion = -1;
let sendTimer = null;
let initialized = null;

const link = new NativeLink({
  onMessage: handleNativeMessage,
  onStatus: (status) => {
    if (status.connected && status.features) {
      // After a (re)connect the native module has no state at all: resend everything.
      lastSentVersion = -1;
      scheduleSend();
    }
  },
  onHello: (message) => {
    link.nativeVersion = message.nativeVersion;
    lastSentVersion = -1;
    scheduleSend();
  },
});

// ---------------------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------------------

function serializeWindows() {
  const payload = {};
  for (const [windowId, state] of windows) {
    const order = tree.preorder(state);
    payload[windowId] = {
      order,
      parents: order
        .map((tabId) => [tabId, tree.parentOf(state, tabId)])
        .filter(([, parentId]) => parentId != null),
      collapsed: order.filter((tabId) => tree.isCollapsed(state, tabId)),
    };
  }
  return payload;
}

function restoreWindows(payload) {
  windows = new Map();
  for (const [windowId, data] of Object.entries(payload || {})) {
    const order = data.order || [];
    const state = tree.createWindowState(order);
    for (const [tabId, parentId] of data.parents || []) {
      tree.attachAsLastChild(state, Number(tabId), parentId);
    }
    // attachAsLastChild appends; the stored pre-order restores the sibling positions.
    tree.adoptOrder(state, order);
    for (const tabId of data.collapsed || []) {
      tree.setCollapsed(state, Number(tabId), true);
    }
    windows.set(Number(windowId), state);
  }
}

async function saveSession() {
  try {
    await chrome.storage.session.set({ [SESSION_KEY]: serializeWindows() });
  } catch {
    // storage.session is best effort; the tree rebuilds from tabs.* events.
  }
}

async function loadSession() {
  try {
    const stored = await chrome.storage.session.get(SESSION_KEY);
    if (stored && stored[SESSION_KEY]) {
      restoreWindows(stored[SESSION_KEY]);
    }
  } catch {
    // ignore
  }
}

async function loadConfig() {
  const stored = await chrome.storage.sync.get('config');
  config = { ...DEFAULT_CONFIG, ...(stored?.config || {}) };
}

// ---------------------------------------------------------------------------------------
// Window state helpers
// ---------------------------------------------------------------------------------------

function stateFor(windowId) {
  let state = windows.get(windowId);
  if (!state) {
    state = tree.createWindowState([]);
    windows.set(windowId, state);
  }
  return state;
}

function isTreeTab(tab) {
  return Boolean(tab) && !tab.pinned && (tab.groupId === undefined || tab.groupId === -1);
}

async function syncWindow(windowId) {
  const tabs = await chrome.tabs.query({ windowId });
  tabs.sort((a, b) => a.index - b.index);
  const state = stateFor(windowId);
  const liveIds = tabs.map((tab) => tab.id);
  tree.reconcile(state, liveIds);
  for (const tab of tabs) {
    if (!isTreeTab(tab) && tree.parentOf(state, tab.id) != null) {
      // Pinned tabs and grouped tabs always live at depth 0 with no parent.
      tree.attachAsLastChild(state, tab.id, null);
    }
  }
  tree.adoptOrder(state, liveIds);
  return state;
}

// ---------------------------------------------------------------------------------------
// Native state push
// ---------------------------------------------------------------------------------------

function bumpState() {
  stateVersion += 1;
  scheduleSend();
}

function scheduleSend() {
  if (sendTimer) {
    return;
  }
  sendTimer = setTimeout(() => {
    sendTimer = null;
    void sendState();
  }, 16);
}

async function sendState() {
  if (!link.isConnected) {
    return;
  }
  lastSentVersion = stateVersion;
  const allTabs = {};
  const windowTabs = [];
  for (const [windowId, state] of windows) {
    const tabs = await chrome.tabs.query({ windowId });
    if (!tabs.length) {
      continue;
    }
    const byIndex = tabs.sort((a, b) => a.index - b.index);
    const display = tree.displayState(state, byIndex.map((tab) => tab.id));
    for (const tabId of display.windowTabs) {
      windowTabs.push(tabId);
      allTabs[tabId] = display.tabs[tabId];
    }
  }
  link.send({
    type: 'state',
    windowTabs,
    tabs: allTabs,
    config: {
      indent: config.indent,
      showCount: config.showCount,
      highlightColor: config.highlightColor,
    },
  });
  // The native module keeps its own copy; remember what it has so we can skip identical sends.
  lastSentVersion = stateVersion;
}

// ---------------------------------------------------------------------------------------
// Tab event handlers
// ---------------------------------------------------------------------------------------

async function onCreated(createdTab) {
  await ensureInit();
  // Chrome 156 does not include openerTabId in the onCreated payload, so the tab is re-read;
  // the opener is what decides whether this tab joins the tree (PLAN.md section 3).
  const tab = (await chrome.tabs.get(createdTab.id).catch(() => null)) || createdTab;
  if (!tab.windowId) {
    return;
  }
  const state = stateFor(tab.windowId);
  const siblings = await chrome.tabs.query({ windowId: tab.windowId });
  tree.reconcile(state, siblings.map((candidate) => candidate.id));
  const opener = tab.openerTabId != null
    ? siblings.find((candidate) => candidate.id === tab.openerTabId)
    : null;
  if (isTreeTab(tab) && opener && isTreeTab(opener)) {
    tree.attachAsLastChild(state, tab.id, opener.id);
    await applyOrder(tab.windowId, state);
  }
  bumpState();
  await saveSession();
}

async function applyOrder(windowId, state) {
  const tabIds = tree.preorder(state);
  for (let index = 0; index < tabIds.length; index += 1) {
    const tabId = tabIds[index];
    try {
      const current = await chrome.tabs.get(tabId);
      if (current.windowId !== windowId) {
        continue;
      }
      if (current.index !== index) {
        await chrome.tabs.move(tabId, { index });
      }
    } catch {
      // The tab disappeared mid-flight; the next reconcile fixes the order.
    }
  }
}

async function onRemoved(tabId, removeInfo) {
  await ensureInit();
  if (removeInfo.isWindowClosing) {
    windows.delete(removeInfo.windowId);
    return;
  }
  const state = stateFor(removeInfo.windowId);
  tree.removeTab(state, tabId);
  bumpState();
  await saveSession();
}

async function closeSubtree(tabId, windowId) {
  const state = stateFor(windowId);
  const descendants = tree.descendantsOf(state, tabId);
  if (!descendants.length) {
    await chrome.tabs.remove(tabId);
    return;
  }
  if (config.confirmThreshold > 0 && descendants.length >= config.confirmThreshold) {
    const confirmed = await confirmClose(tabId, descendants.length);
    if (!confirmed) {
      return;
    }
  }
  await chrome.tabs.remove(descendants);
  await chrome.tabs.remove(tabId);
}

function confirmClose(tabId, count) {
  // Service workers cannot open dialogs; a small confirmation window keeps it simple.
  return chrome.windows
    .create({
      type: 'popup',
      width: 360,
      height: 160,
      url: chrome.runtime.getURL(`src/options/confirm.html?tab=${tabId}&count=${count}`),
    })
    .then(() => false)
    .catch(() => false);
}

async function onMoved(tabId, moveInfo) {
  await ensureInit();
  if (dragActive) {
    return; // the drag handlers decide the parent when the drop finishes
  }
  const state = stateFor(moveInfo.windowId);
  if (!tree.preorder(state).includes(tabId)) {
    return;
  }
  // Someone reordered tabs outside the drop protocol: keep every parent relation and
  // re-derive the sibling order from the strip (PLAN.md section 4.3).
  const tabs = await chrome.tabs.query({ windowId: moveInfo.windowId });
  tabs.sort((a, b) => a.index - b.index);
  const changed = tree.adoptOrder(state, tabs.map((tab) => tab.id));
  if (changed) {
    bumpState();
    await saveSession();
  }
  await applyOrder(moveInfo.windowId, state);
}

async function onActivated(activeInfo) {
  await ensureInit();
  const state = stateFor(activeInfo.windowId);
  const expanded = tree.expandAncestors(state, activeInfo.tabId);
  if (expanded.length) {
    bumpState();
    await saveSession();
  }
}

async function onAttached(tabId, attachInfo) {
  await ensureInit();
  const source = windows.get(attachInfo.fromWindowId) || null;
  const target = stateFor(attachInfo.newWindowId);
  // The whole subtree travels: its internal relations move verbatim.
  const moved = source ? tree.detachSubtree(source, tabId) : { nodes: new Map() };
  tree.attachSubtreeAsRoot(target, tabId, moved.nodes);
  await syncWindow(attachInfo.newWindowId);
  if (source) {
    await syncWindow(attachInfo.fromWindowId);
  }
  await applyOrder(attachInfo.newWindowId, target);
  bumpState();
  await saveSession();
}

async function onDetached(tabId, detachInfo) {
  await ensureInit();
  const state = stateFor(detachInfo.oldWindowId);
  if (!state.nodes.has(tabId)) {
    return;
  }
  bumpState();
}

async function onUpdated(tabId, changeInfo, tab) {
  await ensureInit();
  if (!('pinned' in changeInfo) && !('groupId' in changeInfo)) {
    return;
  }
  const state = stateFor(tab.windowId);
  const node = state.nodes.get(tabId);
  if (!node) {
    return;
  }
  if (!isTreeTab(tab)) {
    // Leaving the tree scope (pinned / grouped) promotes the children one level.
    const parent = tree.parentOf(state, tabId);
    const children = [...node.children];
    const list = tree.siblingsOf(state, parent);
    const index = list.indexOf(tabId);
    node.children = [];
    node.collapsed = false;
    tree.detach(state, tabId);
    const target = tree.siblingsOf(state, parent);
    target.splice(index < 0 ? target.length : index + 1, 0, ...children);
    tree.attachAsLastChild(state, tabId, null);
    const tabs = await chrome.tabs.query({ windowId: tab.windowId });
    tabs.sort((a, b) => a.index - b.index);
    tree.adoptOrder(state, tabs.map((candidate) => candidate.id));
    bumpState();
    await saveSession();
  }
}

// ---------------------------------------------------------------------------------------
// Native messages
// ---------------------------------------------------------------------------------------

async function handleNativeMessage(message) {
  switch (message.type) {
    case 'twistyClick':
      await toggleCollapsed(message.tabId);
      break;
    case 'dragBegin':
      await onDragBegin();
      break;
    case 'dragEnd':
      await onDragEnd();
      break;
    case 'dropOnto':
      await dropOnto(message.targetTabId, message.zone);
      break;
    default:
      break;
  }
}

// While a parent is dragged only the parent itself should move, so its subtree is hidden.
async function onDragBegin() {
  dragActive = true;
  const tab = await activeTab();
  if (!tab) {
    return;
  }
  const state = stateFor(tab.windowId);
  const children = tree.childIdsOf(state, tab.id);
  if (!children.length) {
    return;
  }
  if (!tree.isCollapsed(state, tab.id)) {
    dragTemporarilyCollapsed = tab.id;
    tree.setCollapsed(state, tab.id, true);
    bumpState();
  }
}

async function onDragEnd() {
  dragActive = false;
  if (dragTemporarilyCollapsed == null) {
    return;
  }
  const tab = await chrome.tabs.get(dragTemporarilyCollapsed).catch(() => null);
  const tabId = dragTemporarilyCollapsed;
  dragTemporarilyCollapsed = null;
  if (!tab) {
    return;
  }
  const state = stateFor(tab.windowId);
  tree.setCollapsed(state, tabId, false);
  bumpState();
  await saveSession();
}

async function toggleCollapsed(tabId) {
  const target = await chrome.tabs.get(tabId).catch(() => null);
  if (!target) {
    return;
  }
  const state = stateFor(target.windowId);
  if (!tree.childIdsOf(state, tabId).length) {
    return;
  }
  tree.setCollapsed(state, tabId, !tree.isCollapsed(state, tabId));
  bumpState();
  await saveSession();
}

async function dropOnto(targetTabId, zone = 'nest') {
  const target = await chrome.tabs.get(targetTabId).catch(() => null);
  if (!target) {
    return;
  }
  const [activeTab] = await chrome.tabs.query({ active: true, windowId: target.windowId });
  if (!activeTab || activeTab.id === targetTabId) {
    return;
  }
  const state = stateFor(target.windowId);
  if (tree.subtreeIds(state, activeTab.id).includes(targetTabId)) {
    return; // dropping a parent onto its own descendant would create a cycle
  }
  if (zone === 'before' || zone === 'after') {
    // Edge drop: a sibling reorder, the parent does not change.
    tree.insertSibling(state, activeTab.id, targetTabId, zone);
  } else {
    // Middle drop: become the target's last child (PLAN.md section 3).
    tree.attachAsLastChild(state, activeTab.id, targetTabId);
  }
  await applyOrder(target.windowId, state);
  bumpState();
  await saveSession();
}

// ---------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------

async function activeTab() {
  const [tab] = await chrome.tabs.query({ active: true, lastFocusedWindow: true });
  return tab || null;
}

async function commandToggleCollapsed() {
  const tab = await activeTab();
  if (tab) {
    await toggleCollapsed(tab.id);
  }
}

async function commandCloseTree() {
  const tab = await activeTab();
  if (tab) {
    await closeSubtree(tab.id, tab.windowId);
  }
}

async function commandOutdent() {
  const tab = await activeTab();
  if (!tab) {
    return;
  }
  const state = stateFor(tab.windowId);
  const parent = tree.parentOf(state, tab.id);
  if (parent == null) {
    return;
  }
  // Promote one level: become the parent's next sibling.
  tree.insertSibling(state, tab.id, parent, 'after');
  await applyOrder(tab.windowId, state);
  bumpState();
  await saveSession();
}

async function commandIndent() {
  const tab = await activeTab();
  if (!tab) {
    return;
  }
  const state = stateFor(tab.windowId);
  const siblings = tree.siblingsOf(state, tree.parentOf(state, tab.id));
  const index = siblings.indexOf(tab.id);
  const previous = index > 0 ? siblings[index - 1] : null;
  if (previous == null) {
    return;
  }
  tree.attachAsLastChild(state, tab.id, previous);
  await applyOrder(tab.windowId, state);
  bumpState();
  await saveSession();
}

// ---------------------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------------------

async function init() {
  await loadConfig();
  await loadSession();
  const tabs = await chrome.tabs.query({});
  const windowsSeen = new Set();
  for (const tab of tabs) {
    windowsSeen.add(tab.windowId);
  }
  for (const windowId of windowsSeen) {
    await syncWindow(windowId);
  }
  for (const windowId of [...windows.keys()]) {
    if (!windowsSeen.has(windowId)) {
      windows.delete(windowId);
    }
  }
  link.start();
  scheduleSend();
}

function ensureInit() {
  if (!initialized) {
    initialized = init();
  }
  return initialized;
}

chrome.tabs.onCreated.addListener((tab) => void onCreated(tab));
chrome.tabs.onRemoved.addListener((tabId, info) => void onRemoved(tabId, info));
chrome.tabs.onMoved.addListener((tabId, info) => void onMoved(tabId, info));
chrome.tabs.onActivated.addListener((info) => void onActivated(info));
chrome.tabs.onAttached.addListener((tabId, info) => void onAttached(tabId, info));
chrome.tabs.onDetached.addListener((tabId, info) => void onDetached(tabId, info));
chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => void onUpdated(tabId, changeInfo, tab));

chrome.commands.onCommand.addListener((command) => {
  void ensureInit().then(async () => {
    switch (command) {
      case 'toggle-collapsed':
        await commandToggleCollapsed();
        break;
      case 'close-tree':
        await commandCloseTree();
        break;
      case 'outdent':
        await commandOutdent();
        break;
      case 'indent':
        await commandIndent();
        break;
      default:
        break;
    }
  });
});

chrome.storage.onChanged.addListener((changes, area) => {
  if (area === 'sync' && changes.config) {
    config = { ...DEFAULT_CONFIG, ...(changes.config.newValue || {}) };
    bumpState();
  }
});

// The options page asks for the live native status.
chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.type !== 'arbortab.status') {
    return undefined;
  }
  void ensureInit().then(() => {
    sendResponse({
      connected: link.isConnected,
      features: link.features,
      nativeVersion: link.nativeVersion,
      port: link.port,
      windows: windows.size,
    });
  });
  return true;
});

chrome.runtime.onInstalled.addListener(() => void ensureInit());
chrome.runtime.onStartup.addListener(() => void ensureInit());

// Exposed for the extension's own options page and for sandbox tests.
globalThis.arbortab = {
  getState: () => ({ version: stateVersion, config, windows: serializeWindows(), connected: link.isConnected, features: link.features }),
  // Sandbox test hook: exercises the same path as a native dropOnto event.
  __testDrop: (targetTabId, zone) => dropOnto(targetTabId, zone),
  __testDragBegin: onDragBegin,
  __testDragEnd: onDragEnd,
  closeTree: async (tabId) => {
    const tab = await chrome.tabs.get(tabId).catch(() => null);
    if (tab) {
      await closeSubtree(tabId, tab.windowId);
    }
  },
  toggleCollapsed,
};

void ensureInit();
