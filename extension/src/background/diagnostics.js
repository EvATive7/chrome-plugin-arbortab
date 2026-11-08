// In-memory event log for the settings page's debug export. It lives only as long as the
// service worker, and never records URLs or titles: only ids, versions and states.

const LIMIT = 200;
const events = [];

export function record(event, data) {
  events.push({ at: new Date().toISOString(), event, ...(data ? { data } : {}) });
  if (events.length > LIMIT) {
    events.splice(0, events.length - LIMIT);
  }
}

export function recentEvents() {
  return events.slice();
}

/** Shape of the tree without any tab content: counts per window. */
export function treeSummary(serializedWindows) {
  return Object.values(serializedWindows).map((window) => {
    const parents = new Map(window.parents);
    const depth = (tabId) => {
      let level = 0;
      for (let id = parents.get(tabId); id != null; id = parents.get(id)) {
        level += 1;
      }
      return level;
    };
    return {
      tabs: window.order.length,
      children: parents.size,
      collapsed: window.collapsed.length,
      maxDepth: Math.max(0, ...window.order.map(depth)),
    };
  });
}
