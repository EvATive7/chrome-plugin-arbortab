// ArborTab service worker: keeps the tree model in sync with chrome.tabs and pushes the
// display state to the native module.

import * as tree from './tree.js';
import {
  NativeLink,
  PROTOCOL,
  nativeOutdated,
  nativeUpdatePromptDecision,
} from './native-link.js';
import { record, recentEvents, treeSummary } from './diagnostics.js';

const DEFAULT_CONFIG = {
  indent: 14,
  confirmThreshold: 0, // close descendants without asking (0 = never ask)
  newTabButtonIsChild: false,
  showCount: true,
  showGuides: true,
  animations: true,
  highlightColor: 'auto', // 'auto' follows the browser theme, otherwise '#AARRGGBB'
};

// Colours earlier builds stored as their default; they now follow the theme instead.
const LEGACY_HIGHLIGHT_COLORS = new Set(['#663D7EFF', '#998B5CF6']);

const SESSION_KEY = 'arbortab.windows';
const LEGACY_NATIVE_UPDATE_PROMPTS_KEY = 'arbortab.nativeUpdatePrompts';
const NATIVE_UPDATE_PROMPT_SESSION_KEY = 'arbortab.nativeUpdatePromptShown';
const INITIAL_INSTALL_PENDING_KEY = 'arbortab.initialInstallPending';
const NATIVE_UPDATE_DELAY_MS = 5000;

/** @type {Map<number, ReturnType<typeof tree.createWindowState>>} */
let windows = new Map();
let config = { ...DEFAULT_CONFIG };
let dragActive = false;
/** Tab that was collapsed only for the duration of a drag (a parent is dragged alone). */
let dragTemporarilyCollapsed = null;
/** Window of the current drag: its strip order is re-asserted when the drag ends. */
let dragWindowId = null;
/** Cross-window tab events belonging to an owned drag-out; handled as one tree transaction. */
let dragOutTransfer = null;
let stateVersion = 0;
let lastSentVersion = -1;
let sendTimer = null;
let initialized = null;
let initialInstall = false;
let nativeUpdatePromptScheduled = false;

const link = new NativeLink({
  onMessage: handleNativeMessage,
  onStatus: (status) => {
    if (status.connected && status.features) {
      // After a (re)connect the native module has no state at all: resend everything.
      lastSentVersion = -1;
      scheduleSend();
    }
  },
  onEvent: record,
  onHandshake: (message) => scheduleNativeUpdatePrompt(message.nativeVersion),
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
  config = withDefaults(stored?.config);
}

function withDefaults(stored) {
  const merged = { ...DEFAULT_CONFIG, ...(stored || {}) };
  if (LEGACY_HIGHLIGHT_COLORS.has(String(merged.highlightColor).toUpperCase())) {
    merged.highlightColor = DEFAULT_CONFIG.highlightColor;
  }
  return merged;
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

const NEW_TAB_URLS = ['chrome://newtab', 'chrome://new-tab-page', 'chrome-search://local-ntp'];

/** Ctrl+T / the "+" button. Chrome still sets openerTabId to the active tab for these. */
function isNewTabPage(tab) {
  const url = tab.pendingUrl || tab.url || '';
  return NEW_TAB_URLS.some((prefix) => url.startsWith(prefix));
}

/**
 * Pinned, grouped and split tabs stay outside the tree: Chrome keeps each of those blocks
 * together, so they never get children or a parent, and the extension never moves them.
 */
function isTreeTab(tab) {
  return Boolean(tab) && !tab.pinned && (tab.groupId ?? -1) === -1
    && (tab.splitViewId ?? -1) === -1;
}

function isExtensionRootTab(tab) {
  return (tab?.pendingUrl || tab?.url || '').startsWith(
    chrome.runtime.getURL('src/options/options.html'),
  );
}

async function syncWindow(windowId) {
  const tabs = await chrome.tabs.query({ windowId });
  tabs.sort((a, b) => a.index - b.index);
  const state = stateFor(windowId);
  const liveIds = tabs.map((tab) => tab.id);
  tree.reconcile(state, liveIds);
  for (const tab of tabs) {
    if (!isTreeTab(tab)
      && (tree.parentOf(state, tab.id) != null || tree.childIdsOf(state, tab.id).length)) {
      // Pinned, grouped and split tabs always live at depth 0 with no parent and no children.
      tree.promoteOut(state, tab.id);
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
      const tab = byIndex.find((candidate) => candidate.id === tabId);
      allTabs[tabId] = {
        ...display.tabs[tabId],
        title: tab?.title || '',
        treeTab: isTreeTab(tab),
      };
    }
  }
  link.send({
    type: 'state',
    windowTabs,
    tabs: allTabs,
    config: {
      indent: config.indent,
      showCount: config.showCount,
      showGuides: config.showGuides,
      animations: config.animations,
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
  // the opener is what decides whether this tab joins the tree.
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
  const nestUnderOpener = opener && isTreeTab(tab) && isTreeTab(opener)
    && !isExtensionRootTab(tab)
    && (config.newTabButtonIsChild || !isNewTabPage(tab));
  if (nestUnderOpener) {
    tree.attachAsLastChild(state, tab.id, opener.id);
  }
  // A new root may have been inserted in the middle of a subtree (Chrome places tabs next to
  // their opener): the tree order wins either way.
  await applyOrder(tab.windowId);
  bumpState();
  await saveSession();
}

// ---------------------------------------------------------------------------------------
// Strip order
// ---------------------------------------------------------------------------------------

// More moves than this within the window means something keeps undoing them (Chrome refusing a
// position, or another extension reordering tabs): order enforcement then pauses for a while
// instead of moving tabs back and forth forever.
const ORDER_GUARD_WINDOW_MS = 10000;
const ORDER_GUARD_MAX_MOVES = 150;
const ORDER_GUARD_PAUSE_MS = 30000;
const ORDER_REFUSALS_RECORDED = 10;
const BOUNDARY_SNAP_DELAY_MS = 300;
const MOVE_BUSY_TIMEOUT_MS = 10000;
const MOVE_BUSY_RETRY_MS = 100;

/** Diagnostics for the debug export. */
const orderStats = { requests: 0, moves: 0, refused: 0, movedEvents: 0, pauses: 0 };
/** @type {Map<number, {running: Promise<void> | null, again: boolean, moves: number[], pausedUntil: number}>} */
const orderRuns = new Map();
const boundarySnapTimers = new Map();
const boundarySnapActive = new Set();
const programmaticMoveTabs = new Map();

function orderRunFor(windowId) {
  let run = orderRuns.get(windowId);
  if (!run) {
    run = { running: null, again: false, moves: [], pausedUntil: 0 };
    orderRuns.set(windowId, run);
  }
  return run;
}

/**
 * Moves the window's tree tabs into the tree's pre-order. Runs one pass at a time per window;
 * requests during a pass make it run once more, so every caller sees the latest tree applied.
 */
function applyOrder(windowId) {
  orderStats.requests += 1;
  const run = orderRunFor(windowId);
  if (run.running) {
    run.again = true;
    return run.running;
  }
  run.running = (async () => {
    try {
      do {
        run.again = false;
        await enforceOrder(windowId, run);
      } while (run.again);
    } finally {
      run.running = null;
    }
  })();
  return run.running;
}

function topLevelRoot(state, tabId) {
  const ancestors = tree.ancestorIds(state, tabId);
  return ancestors.length ? ancestors[ancestors.length - 1] : tabId;
}

function isTabsBusyError(error) {
  return /Tabs cannot be edited right now/i.test(String(error));
}

function recordMoveRefused(windowId, wanted, landed, error = null) {
  orderStats.refused += 1;
  if (orderStats.refused <= ORDER_REFUSALS_RECORDED) {
    record('orderMoveRefused', {
      windowId,
      wanted,
      landed,
      ...(error ? { error: String(error) } : {}),
    });
  }
}

async function moveTreeTabWithRetry(windowId, tabId, wantedForTabs, run) {
  const busyDeadline = Date.now() + MOVE_BUSY_TIMEOUT_MS;
  let busyRetryMs = MOVE_BUSY_RETRY_MS;
  let reportedBusy = false;
  while (true) {
    const tabs = await chrome.tabs.query({ windowId });
    tabs.sort((a, b) => a.index - b.index);
    const tab = tabs.find((candidate) => candidate.id === tabId);
    const wanted = wantedForTabs(tabs);
    if (!tab || wanted == null || tab.index === wanted) {
      return true;
    }
    if (!noteMove(windowId, run, tabs)) {
      return false;
    }
    orderStats.moves += 1;
    let moved;
    try {
      const programmaticUntil = Date.now() + 2000;
      programmaticMoveTabs.set(tabId, programmaticUntil);
      setTimeout(() => {
        if (programmaticMoveTabs.get(tabId) === programmaticUntil) {
          programmaticMoveTabs.delete(tabId);
        }
      }, 2100);
      moved = await chrome.tabs.move(tabId, { index: wanted });
    } catch (error) {
      programmaticMoveTabs.delete(tabId);
      if (isTabsBusyError(error) && Date.now() < busyDeadline) {
        if (!reportedBusy) {
          reportedBusy = true;
          record('orderMoveBusyRetry', { windowId, tabId });
        }
        await new Promise((resolve) => setTimeout(resolve, busyRetryMs));
        busyRetryMs = Math.min(1000, busyRetryMs * 2);
        continue;
      }
      if (isTabsBusyError(error)) {
        record('orderMoveBusyTimeout', { windowId, tabId, timeoutMs: MOVE_BUSY_TIMEOUT_MS });
      } else {
        recordMoveRefused(windowId, wanted, null, error);
      }
      return false;
    }
    const landed = Array.isArray(moved) ? moved[0] : moved;
    if (!landed || landed.index !== wanted) {
      recordMoveRefused(windowId, wanted, landed ? landed.index : null);
      return false;
    }
    return true;
  }
}

async function enforceOrder(windowId, run) {
  const state = windows.get(windowId);
  if (!state || Date.now() < run.pausedUntil) {
    return;
  }
  // Tree tabs keep pre-order. Tabs inside one top-level subtree stay adjacent, while pinned,
  // grouped and split blocks may remain between different top-level subtrees.
  for (let step = 0; step < 4 * state.nodes.size + 16; step += 1) {
    const tabs = await chrome.tabs.query({ windowId });
    const byId = new Map(tabs.map((tab) => [tab.id, tab]));
    const order = tree.preorder(state)
      .filter((tabId) => byId.has(tabId) && isTreeTab(byId.get(tabId)));
    let target = null;
    for (let index = 1; index < order.length && !target; index += 1) {
      const tab = byId.get(order[index]);
      const previous = byId.get(order[index - 1]);
      const sameRoot = topLevelRoot(state, tab.id) === topLevelRoot(state, previous.id);
      if ((sameRoot && tab.index !== previous.index + 1)
        || (!sameRoot && tab.index < previous.index)) {
        target = { tabId: tab.id, previousId: previous.id, sameRoot };
      }
    }
    if (!target) {
      return;
    }
    const moved = await moveTreeTabWithRetry(
      windowId,
      target.tabId,
      (currentTabs) => {
        const current = new Map(currentTabs.map((tab) => [tab.id, tab]));
        const tab = current.get(target.tabId);
        const previous = current.get(target.previousId);
        if (!tab || !previous) {
          return null;
        }
        if (!target.sameRoot && tab.index > previous.index) {
          return null;
        }
        return tab.index < previous.index ? previous.index : previous.index + 1;
      },
      run,
    );
    if (!moved) {
      return;
    }
  }
}

function scheduleTopLevelBoundarySnap(windowId) {
  const pending = boundarySnapTimers.get(windowId);
  if (pending) {
    clearTimeout(pending);
  }
  boundarySnapTimers.set(windowId, setTimeout(() => {
    boundarySnapTimers.delete(windowId);
    if (boundarySnapActive.has(windowId)) {
      scheduleTopLevelBoundarySnap(windowId);
      return;
    }
    void snapTopLevelBoundaries(windowId).catch((error) => {
      record('boundarySnapFailed', { windowId, error: String(error) });
    });
  }, BOUNDARY_SNAP_DELAY_MS));
}

async function snapTopLevelBoundaries(windowId) {
  if (boundarySnapActive.has(windowId)) {
    scheduleTopLevelBoundarySnap(windowId);
    return;
  }
  boundarySnapActive.add(windowId);
  try {
    const run = orderRunFor(windowId);
    if (run.running) {
      await run.running;
    }
    const state = await syncWindow(windowId);
    const tabs = await chrome.tabs.query({ windowId });
    tabs.sort((a, b) => a.index - b.index);
    const plans = tree.planTopLevelBoundarySnaps(
      state,
      tabs.map((tab) => tab.id),
      tabs.filter(isTreeTab).map((tab) => tab.id),
    );
    for (const plan of plans) {
      const tabIds = plan.moveTo === 'after-block'
        ? [...plan.tabIds].reverse()
        : plan.tabIds;
      for (const tabId of tabIds) {
        const moved = await moveTreeTabWithRetry(
          windowId,
          tabId,
          (currentTabs) => {
            const byId = new Map(currentTabs.map((tab) => [tab.id, tab]));
            const tab = byId.get(tabId);
            const block = plan.blockIds.map((id) => byId.get(id)).filter(Boolean);
            if (!tab || !block.length) {
              return null;
            }
            if (plan.moveTo === 'after-block') {
              const anchor = block.reduce((right, item) => item.index > right.index ? item : right);
              if (tab.index === anchor.index + 1) {
                return null;
              }
              return tab.index < anchor.index ? anchor.index : anchor.index + 1;
            }
            const anchor = block.reduce((left, item) => item.index < left.index ? item : left);
            if (tab.index + 1 === anchor.index) {
              return null;
            }
            return tab.index < anchor.index ? anchor.index - 1 : anchor.index;
          },
          run,
        );
        if (!moved) {
          return;
        }
      }
    }
    await syncWindow(windowId);
    await applyOrder(windowId);
    bumpState();
    await saveSession();
    if (plans.length) {
      record('boundarySnap', { windowId, plans: plans.length });
    }
  } finally {
    boundarySnapActive.delete(windowId);
  }
}

function noteMove(windowId, run, tabs) {
  const now = Date.now();
  run.moves = run.moves.filter((at) => now - at < ORDER_GUARD_WINDOW_MS);
  run.moves.push(now);
  if (run.moves.length <= ORDER_GUARD_MAX_MOVES) {
    return true;
  }
  run.moves = [];
  run.pausedUntil = now + ORDER_GUARD_PAUSE_MS;
  orderStats.pauses += 1;
  record('orderLoopPaused', {
    windowId,
    moves: ORDER_GUARD_MAX_MOVES,
    withinMs: ORDER_GUARD_WINDOW_MS,
    pauseMs: ORDER_GUARD_PAUSE_MS,
    tabs: tabs.length,
    pinned: tabs.filter((tab) => tab.pinned).length,
    grouped: tabs.filter((tab) => (tab.groupId ?? -1) !== -1).length,
    split: tabs.filter((tab) => (tab.splitViewId ?? -1) !== -1).length,
  });
  return false;
}

async function onRemoved(tabId, removeInfo) {
  await ensureInit();
  if (removeInfo.isWindowClosing) {
    windows.delete(removeInfo.windowId);
    return;
  }
  const state = stateFor(removeInfo.windowId);
  // Closing a tab closes its whole subtree.
  const descendants = tree.descendantsOf(state, tabId);
  tree.detachSubtree(state, tabId);
  bumpState();
  await saveSession();
  if (descendants.length) {
    await chrome.tabs.remove(descendants).catch(() => null);
  }
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
  orderStats.movedEvents += 1;
  const programmaticUntil = programmaticMoveTabs.get(tabId);
  if (programmaticUntil != null) {
    programmaticMoveTabs.delete(tabId);
    if (Date.now() <= programmaticUntil) {
      return;
    }
  }
  if (dragActive || boundarySnapActive.has(moveInfo.windowId)) {
    return; // owned drag/snap handlers decide the final position
  }
  const movedTab = await chrome.tabs.get(tabId).catch(() => null);
  if (!movedTab) {
    return;
  }
  if (!isTreeTab(movedTab)) {
    // Native group/pinned/split drags emit a burst of onMoved events. Wait for the release and
    // then snap the complete foreign block to its nearest top-level subtree boundary.
    scheduleTopLevelBoundarySnap(moveInfo.windowId);
    return;
  }
  const state = stateFor(moveInfo.windowId);
  if (!tree.preorder(state).includes(tabId)) {
    return;
  }
  // Someone reordered a tree tab outside the drop protocol: keep every parent relation and
  // re-derive sibling order from the strip.
  const tabs = await chrome.tabs.query({ windowId: moveInfo.windowId });
  tabs.sort((a, b) => a.index - b.index);
  const changed = tree.adoptOrder(state, tabs.map((tab) => tab.id));
  if (changed) {
    bumpState();
    await saveSession();
  }
  await applyOrder(moveInfo.windowId);
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
  // Capture before the first await: Chrome may resolve windows.create/tabs.move and finish the
  // transaction while this listener is suspended in ensureInit().
  const transfer = dragOutTransfer;
  await ensureInit();
  if (transfer && transfer.tabIds.has(tabId)
    && (transfer.targetWindowId == null || transfer.targetWindowId === attachInfo.newWindowId)) {
    transfer.targetWindowId = attachInfo.newWindowId;
    return;
  }
  const source = windows.get(attachInfo.fromWindowId) || null;
  const target = stateFor(attachInfo.newWindowId);
  // The whole subtree travels: its internal relations move verbatim.
  const moved = source ? tree.detachSubtree(source, tabId) : { nodes: new Map() };
  tree.attachSubtreeAsRoot(target, tabId, moved.nodes);
  await syncWindow(attachInfo.newWindowId);
  if (source) {
    await syncWindow(attachInfo.fromWindowId);
  }
  scheduleTopLevelBoundarySnap(attachInfo.newWindowId);
  bumpState();
  await saveSession();
}

async function onDetached(tabId, detachInfo) {
  const transfer = dragOutTransfer;
  await ensureInit();
  if (transfer
    && transfer.sourceWindowId === detachInfo.oldWindowId
    && transfer.tabIds.has(tabId)) {
    return;
  }
  const state = stateFor(detachInfo.oldWindowId);
  if (!state.nodes.has(tabId)) {
    return;
  }
  bumpState();
}

async function onUpdated(tabId, changeInfo, tab) {
  await ensureInit();
  if ('title' in changeInfo) {
    // The native count badge follows the rendered title, so title changes need a fresh snapshot.
    bumpState();
  }
  if (!('pinned' in changeInfo) && !('groupId' in changeInfo) && !('splitViewId' in changeInfo)) {
    return;
  }
  await syncWindow(tab.windowId);
  scheduleTopLevelBoundarySnap(tab.windowId);
  bumpState();
  await saveSession();
}

// ---------------------------------------------------------------------------------------
// Native messages
// ---------------------------------------------------------------------------------------

// Native events are handled strictly in order: dragBegin, dropOnto/dragOut and dragEnd of one
// drag all touch the same state and must not interleave.
let nativeQueue = Promise.resolve();

function handleNativeMessage(message) {
  nativeQueue = nativeQueue
    .then(() => dispatchNativeMessage(message))
    .catch((error) => {
      console.warn('ArborTab: native message failed', message?.type, error);
      record('nativeMessageFailed', { type: message?.type, error: String(error) });
    });
  return nativeQueue;
}

async function dispatchNativeMessage(message) {
  await ensureInit();
  switch (message.type) {
    case 'twistyClick':
      await toggleCollapsed(message.tabId);
      break;
    case 'dragBegin':
      await onDragBegin(message.draggedTabId);
      break;
    case 'dragEnd':
      await onDragEnd();
      break;
    case 'dropOnto':
      await dropOnto(message.targetTabId, message.zone, message.draggedTabId);
      break;
    case 'dragOut':
      await dragOut(message.draggedTabId, message.windowX, message.windowY);
      break;
    default:
      break;
  }
}

/** The tab being dragged: reported by the native module, else the active tab. */
async function draggedTab(draggedTabId) {
  if (draggedTabId) {
    const tab = await chrome.tabs.get(draggedTabId).catch(() => null);
    if (tab) {
      return tab;
    }
  }
  return activeTab();
}

// While a parent is dragged only the parent itself should move, so its subtree is hidden.
async function onDragBegin(draggedTabId) {
  dragActive = true;
  dragWindowId = null;
  const tab = await draggedTab(draggedTabId);
  if (!tab) {
    return;
  }
  dragWindowId = tab.windowId;
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
  const windowId = dragWindowId;
  dragWindowId = null;
  if (dragTemporarilyCollapsed != null) {
    const tabId = dragTemporarilyCollapsed;
    dragTemporarilyCollapsed = null;
    const tab = await chrome.tabs.get(tabId).catch(() => null);
    if (tab) {
      tree.setCollapsed(stateFor(tab.windowId), tabId, false);
    }
  }
  // The tree is the only authority on order: whatever Chrome did to the strip during the drag
  // is undone here (moves outside a drag keep going through onMoved/adoptOrder).
  if (windowId != null && windows.has(windowId)) {
    await applyOrder(windowId);
  }
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

function clamp(value, minimum, maximum) {
  return Math.min(maximum, Math.max(minimum, value));
}

async function createDragOutWindow(dragged, windowX, windowY) {
  const createData = { tabId: dragged.id };
  const sourceWindow = await chrome.windows.get(dragged.windowId).catch(() => null);
  if (sourceWindow
    && Number.isFinite(sourceWindow.left) && Number.isFinite(sourceWindow.top)
    && Number.isFinite(sourceWindow.width) && Number.isFinite(sourceWindow.height)
    && Number.isFinite(windowX) && Number.isFinite(windowY)) {
    // Native sends browser-window-relative DIPs. Keep the requested top-left within the first
    // half of the known-visible source window; Chrome otherwise rejects bounds that are less
    // than 50% on a visible display.
    const desiredLeft = sourceWindow.left + windowX - 120;
    const desiredTop = sourceWindow.top + windowY - 20;
    createData.left = Math.round(clamp(
      desiredLeft,
      sourceWindow.left,
      sourceWindow.left + Math.max(0, Math.floor(sourceWindow.width / 2)),
    ));
    createData.top = Math.round(clamp(
      desiredTop,
      sourceWindow.top,
      sourceWindow.top + Math.max(0, Math.floor(sourceWindow.height / 2)),
    ));
  }
  try {
    return await chrome.windows.create(createData);
  } catch (error) {
    if (!('left' in createData) && !('top' in createData)) {
      throw error;
    }
    record('dragOutPositionFallback', {
      tabId: dragged.id,
      left: createData.left,
      top: createData.top,
      error: String(error),
    });
    return chrome.windows.create({ tabId: dragged.id });
  }
}

async function dragOut(draggedTabId, windowX, windowY) {
  const dragged = await draggedTab(draggedTabId);
  if (!dragged || !isTreeTab(dragged)) {
    return;
  }
  const sourceWindowId = dragged.windowId;
  const source = windows.get(sourceWindowId);
  if (!source) {
    return;
  }
  const subtree = tree.subtreeIds(source, dragged.id);
  if (!subtree.length) {
    return;
  }
  dragOutTransfer = {
    sourceWindowId,
    targetWindowId: null,
    tabIds: new Set(subtree),
  };
  try {
    const targetWindow = await createDragOutWindow(dragged, windowX, windowY);
    if (targetWindow?.id == null) {
      throw new Error('Chrome did not return the new window id');
    }
    dragOutTransfer.targetWindowId = targetWindow.id;
    const descendants = subtree.slice(1);
    if (descendants.length) {
      // One tabs.move call preserves pre-order and keeps the transfer all-or-nothing in Chrome.
      await chrome.tabs.move(descendants, { windowId: targetWindow.id, index: -1 });
    }

    // Transfer the model only after all physical tabs arrived. onAttached/onDetached ignore this
    // transaction, so no intermediate sync can flatten the subtree.
    const moved = tree.detachSubtree(source, dragged.id);
    const target = stateFor(targetWindow.id);
    tree.attachSubtreeAsRoot(target, dragged.id, moved.nodes);
    await syncWindow(targetWindow.id);
    if (windows.has(sourceWindowId)) {
      await syncWindow(sourceWindowId).catch(() => windows.delete(sourceWindowId));
    }
    await applyOrder(targetWindow.id);
    bumpState();
    await saveSession();
    record('dragOut', { tabId: dragged.id, tabs: subtree.length });
  } finally {
    // Chrome may dispatch onAttached/onDetached after the API promise resolves. Keep the id set
    // briefly so those late events are still recognized as part of this transaction.
    const completedTransfer = dragOutTransfer;
    setTimeout(() => {
      if (dragOutTransfer === completedTransfer) {
        dragOutTransfer = null;
      }
    }, 5000);
  }
}

async function dropOnto(targetTabId, zone = 'nest', draggedTabId = null) {
  const target = await chrome.tabs.get(targetTabId).catch(() => null);
  if (!target) {
    return;
  }
  const dragged = await draggedTab(draggedTabId);
  if (!dragged || dragged.id === targetTabId || dragged.windowId !== target.windowId) {
    return;
  }
  if (!isTreeTab(dragged) || !isTreeTab(target)) {
    return; // pinned, grouped and split tabs stay outside the tree
  }
  const state = stateFor(target.windowId);
  if (tree.subtreeIds(state, dragged.id).includes(targetTabId)) {
    return; // dropping a parent onto its own descendant would create a cycle
  }
  // Every case moves the dragged tab with its whole subtree; descendants keep their parents.
  if (zone === 'end') {
    // Below the last tab: the last top-level tab.
    tree.attachAsLastChild(state, dragged.id, null);
  } else if (zone === 'before' || zone === 'after') {
    // Edge drop: a sibling reorder next to the target, under the target's parent.
    tree.insertSibling(state, dragged.id, targetTabId, zone);
  } else {
    // Drop onto the tab: become the target's last child.
    tree.attachAsLastChild(state, dragged.id, targetTabId);
  }
  await applyOrder(target.windowId);
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
  await applyOrder(tab.windowId);
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
  await applyOrder(tab.windowId);
  bumpState();
  await saveSession();
}

// ---------------------------------------------------------------------------------------
// Native update reminder
// ---------------------------------------------------------------------------------------

async function openNativeUpdateSettings(normalWindows) {
  const normalWindowIds = new Set(normalWindows.map((window) => window.id));
  const optionsUrl = chrome.runtime.getURL('src/options/options.html');
  const targetUrl = `${optionsUrl}?reason=native-update#native`;
  const existing = (await chrome.tabs.query({})).find((tab) =>
    normalWindowIds.has(tab.windowId)
      && (tab.pendingUrl || tab.url || '').startsWith(optionsUrl),
  );
  if (existing) {
    const currentUrl = existing.pendingUrl || existing.url || '';
    if (currentUrl === targetUrl) {
      // Reload so an already-open reminder runs its scroll/highlight entrance again.
      await chrome.tabs.reload(existing.id);
      await chrome.tabs.update(existing.id, { active: true });
    } else {
      await chrome.tabs.update(existing.id, { active: true, url: targetUrl });
    }
    await chrome.windows.update(existing.windowId, { focused: true });
    return;
  }
  const target = normalWindows.find((window) => window.focused) || normalWindows[0];
  // Deliberately omit openerTabId: the reminder must be a root in ArborTab's tree.
  await chrome.tabs.create({ active: true, windowId: target.id, url: targetUrl });
}

function scheduleNativeUpdatePrompt(nativeVersion) {
  const extensionVersion = chrome.runtime.getManifest().version;
  if (nativeUpdatePromptScheduled) {
    return;
  }
  nativeUpdatePromptScheduled = true;
  setTimeout(async () => {
    try {
      const normalWindows = await chrome.windows.getAll({ windowTypes: ['normal'] });
      if (!normalWindows.length) {
        return;
      }
      const [localStored, sessionStored] = await Promise.all([
        chrome.storage.local.get(INITIAL_INSTALL_PENDING_KEY),
        chrome.storage.session.get(NATIVE_UPDATE_PROMPT_SESSION_KEY),
      ]);
      const installPending = initialInstall
        || localStored[INITIAL_INSTALL_PENDING_KEY] === extensionVersion;
      const decision = nativeUpdatePromptDecision({
        nativeVersion,
        extensionVersion,
        shownThisSession: sessionStored[NATIVE_UPDATE_PROMPT_SESSION_KEY] === true,
        initialInstall: installPending,
      });
      if (installPending) {
        // Do not interrupt the installation session. The session flag disappears after Chrome
        // fully exits, so an old module can remind on the next browser run.
        await Promise.all([
          chrome.storage.session.set({ [NATIVE_UPDATE_PROMPT_SESSION_KEY]: true }),
          chrome.storage.local.remove(INITIAL_INSTALL_PENDING_KEY),
        ]);
        initialInstall = false;
        return;
      }
      if (!decision.shouldOpen) {
        return;
      }
      // Persist in session storage before opening: worker restarts and reconnects stay quiet,
      // while a full Chrome restart permits one new reminder.
      await chrome.storage.session.set({ [NATIVE_UPDATE_PROMPT_SESSION_KEY]: true });
      await openNativeUpdateSettings(normalWindows);
      record('nativeUpdatePromptOpened', { key: decision.key });
    } catch (error) {
      record('nativeUpdatePromptFailed', { error: String(error) });
    } finally {
      nativeUpdatePromptScheduled = false;
    }
  }, NATIVE_UPDATE_DELAY_MS);
}

// ---------------------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------------------

async function init() {
  record('started', { version: chrome.runtime.getManifest().version });
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
    config = withDefaults(changes.config.newValue);
    bumpState();
  }
});

function nativeStatus() {
  return {
    connected: link.isConnected,
    features: link.features,
    nativeVersion: link.native?.nativeVersion ?? null,
    offsets: link.native?.offsets ?? null,
    protocol: PROTOCOL,
    protocolMin: link.native?.protocolMin ?? null,
    protocolMax: link.native?.protocolMax ?? null,
    updateNeeded: link.updateNeeded,
    // A newer native release exists for this extension; a hint, not a compatibility check.
    nativeOutdated: nativeOutdated(link.native?.nativeVersion, chrome.runtime.getManifest().version),
    safeMode: link.native?.safeMode ?? null,
    transport: link.transport,
    bridge: link.bridge,
  };
}

/** Everything a bug report needs, without URLs or titles. */
async function debugReport() {
  const nativeReport = await link.request({ type: 'debug.report' }, 'debug.report');
  const manifest = chrome.runtime.getManifest();
  return {
    generatedAt: new Date().toISOString(),
    extension: { id: chrome.runtime.id, version: manifest.version },
    browser: { userAgent: navigator.userAgent, language: chrome.i18n.getUILanguage() },
    native: { ...nativeStatus(), report: nativeReport },
    config,
    tree: treeSummary(serializeWindows()),
    order: {
      ...orderStats,
      pausedWindows: [...orderRuns.values()].filter((run) => Date.now() < run.pausedUntil).length,
    },
    events: recentEvents(),
  };
}

// The options page asks for the live native status and for the debug export.
chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  const handlers = {
    'arbortab.status': nativeStatus,
    'arbortab.debug': debugReport,
    // The user turns the native features back on after safe mode kept them off.
    'arbortab.safeModeReset': () => {
      record('safeModeReset');
      return link.send({ type: 'safeMode.reset' });
    },
  };
  const handler = handlers[message?.type];
  if (!handler) {
    return undefined;
  }
  void ensureInit()
    .then(() => handler())
    .then(sendResponse);
  return true;
});

chrome.runtime.onInstalled.addListener((details) => {
  // Version-pair reminders before 0.5.0 were remembered permanently; they are obsolete now.
  void chrome.storage.local.remove(LEGACY_NATIVE_UPDATE_PROMPTS_KEY);
  if (details.reason === 'install') {
    initialInstall = true;
    void chrome.storage.local.set({
      [INITIAL_INSTALL_PENDING_KEY]: chrome.runtime.getManifest().version,
    });
  }
  void ensureInit();
});
chrome.runtime.onStartup.addListener(() => void ensureInit());

// Exposed for the extension's own options page and for sandbox tests.
globalThis.arbortab = {
  getState: () => ({ version: stateVersion, config, windows: serializeWindows(), connected: link.isConnected,
    features: link.features, native: link.native, updateNeeded: link.updateNeeded,
    order: { ...orderStats } }),
  // Sandbox test hook: exercises the same path as a native dropOnto event.
  __testDrop: (targetTabId, zone, draggedTabId) => dropOnto(targetTabId, zone, draggedTabId),
  __testDragOut: (draggedTabId, windowX, windowY) => dragOut(draggedTabId, windowX, windowY),
  __testToggle: (tabId) => toggleCollapsed(tabId),
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
