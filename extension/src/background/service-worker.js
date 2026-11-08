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
    payload[windowId] = {
      order: state.order,
      nodes: [...state.nodes].map(([tabId, node]) => [tabId, node.parentId, node.collapsed]),
    };
  }
  return payload;
}

function restoreWindows(payload) {
  windows = new Map();
  for (const [windowId, data] of Object.entries(payload || {})) {
    const state = tree.createWindowState(data.order || []);
    for (const [tabId, parentId, collapsed] of data.nodes || []) {
      state.nodes.set(Number(tabId), { parentId: parentId ?? null, collapsed: Boolean(collapsed) });
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
    if (!isTreeTab(tab)) {
      // Pinned tabs and grouped tabs always live at depth 0 with no parent.
      const node = state.nodes.get(tab.id);
      if (node && node.parentId != null) {
        node.parentId = null;
      }
    }
  }
  tree.normalize(state);
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
    tree.setParent(state, tab.id, opener.id);
    tree.placeAfterSubtree(state, tab.id, opener.id);
    await applyOrder(tab.windowId, state);
  }
  bumpState();
  await saveSession();
}

async function applyOrder(windowId, state) {
  const tabIds = state.order;
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
  state.nodes.delete(tabId);
  for (const node of state.nodes.values()) {
    if (node.parentId === tabId) {
      node.parentId = null;
    }
  }
  state.order = state.order.filter((id) => id !== tabId);
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
  if (!state.order.includes(tabId)) {
    return;
  }
  const fromIndex = state.order.indexOf(tabId);
  let target = moveInfo.toIndex;
  if (fromIndex !== -1 && fromIndex < target) {
    target -= 1;
  }
  state.order.splice(fromIndex, 1);
  state.order.splice(Math.min(target, state.order.length), 0, tabId);
  const parentId = tree.inferParentFromIndex(state, tabId, target);
  tree.setParent(state, tabId, parentId);
  tree.normalize(state);
  bumpState();
  await saveSession();
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
  const moved = source ? tree.subtreeIds(source, tabId) : [tabId];
  const target = stateFor(attachInfo.newWindowId);
  for (const id of moved) {
    const node = source && source.nodes.get(id);
    target.nodes.set(id, { parentId: node ? node.parentId : null, collapsed: node ? node.collapsed : false });
    if (!target.order.includes(id)) {
      target.order.push(id);
    }
    if (source) {
      source.nodes.delete(id);
      source.order = source.order.filter((existing) => existing !== id);
    }
  }
  tree.normalize(target);
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
    // Leaving the tree scope promotes children one level (PLAN.md section 3).
    for (const childId of tree.childIdsOf(state, tabId)) {
      tree.setParent(state, childId, node.parentId);
    }
    node.parentId = null;
    node.collapsed = false;
    tree.normalize(state);
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
      dragActive = true;
      break;
    case 'dragEnd':
      dragActive = false;
      break;
    case 'dropOnto':
      await dropOnto(message.targetTabId);
      break;
    default:
      break;
  }
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

async function dropOnto(targetTabId) {
  const target = await chrome.tabs.get(targetTabId).catch(() => null);
  if (!target) {
    return;
  }
  const [activeTab] = await chrome.tabs.query({ active: true, windowId: target.windowId });
  if (!activeTab || activeTab.id === targetTabId) {
    return;
  }
  const state = stateFor(target.windowId);
  const subtree = tree.subtreeIds(state, activeTab.id);
  tree.setParent(state, activeTab.id, targetTabId);
  for (const id of subtree) {
    if (!state.order.includes(id)) {
      state.order.push(id);
    }
  }
  tree.normalize(state);
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
  tree.setParent(state, tab.id, tree.parentOf(state, parent));
  tree.normalize(state);
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
  const index = state.order.indexOf(tab.id);
  const previous = state.order[index - 1];
  if (previous == null) {
    return;
  }
  tree.setParent(state, tab.id, previous);
  tree.normalize(state);
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
  closeTree: async (tabId) => {
    const tab = await chrome.tabs.get(tabId).catch(() => null);
    if (tab) {
      await closeSubtree(tabId, tab.windowId);
    }
  },
  toggleCollapsed,
};

void ensureInit();
