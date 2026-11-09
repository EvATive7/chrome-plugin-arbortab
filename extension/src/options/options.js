// Settings page. Every change is saved right away; the service worker picks it up through
// chrome.storage.onChanged and pushes it to the native module.

import { localizeDocument, msg } from '../i18n.js';

localizeDocument();

const DEFAULTS = {
  indent: 14,
  showCount: true,
  showGuides: true,
  animations: true,
  confirmThreshold: 0,
  newTabButtonIsChild: false,
  highlightColor: 'auto',
};

// Colours earlier builds stored as their default (kept in sync with the service worker).
const LEGACY_HIGHLIGHT_COLORS = new Set(['#663D7EFF', '#998B5CF6']);
const FALLBACK_ACCENT = '#0b57d0';

const $ = (id) => document.getElementById(id);
const fields = {
  indent: $('indent'),
  showGuides: $('showGuides'),
  showCount: $('showCount'),
  animations: $('animations'),
  accentTheme: $('accentTheme'),
  accentCustom: $('accentCustom'),
  accentColor: $('accentColor'),
  newTabButtonIsChild: $('newTabButtonIsChild'),
  confirmThreshold: $('confirmThreshold'),
};

let config = { ...DEFAULTS };
let saveTimer = null;
// Last custom colour, so Theme -> Custom brings back the user's previous pick.
let customColor = null;

function withDefaults(stored) {
  const merged = { ...DEFAULTS, ...(stored || {}) };
  if (LEGACY_HIGHLIGHT_COLORS.has(String(merged.highlightColor).toUpperCase())) {
    merged.highlightColor = 'auto';
  }
  return merged;
}

/** Any CSS colour (e.g. the computed theme accent) -> '#rrggbb'. */
function toHex(cssColor) {
  const probe = document.createElement('canvas').getContext('2d');
  probe.fillStyle = FALLBACK_ACCENT;
  probe.fillStyle = cssColor;
  return probe.fillStyle.startsWith('#') ? probe.fillStyle : FALLBACK_ACCENT;
}

/** '#AARRGGBB' -> '#rrggbb' for <input type=color>. */
function toCssColor(argb) {
  return /^#[0-9a-f]{8}$/i.test(argb) ? `#${argb.slice(3).toLowerCase()}` : FALLBACK_ACCENT;
}

/** The colour the native module uses when the highlight follows the theme. */
function themeAccent() {
  return getComputedStyle(document.documentElement).getPropertyValue('--accent').trim();
}

function render() {
  const min = Number(fields.indent.min);
  const max = Number(fields.indent.max);
  fields.indent.value = config.indent;
  fields.indent.style.setProperty('--fill', `${((config.indent - min) / (max - min)) * 100}%`);
  $('indent-value').textContent = msg('pixels', String(config.indent));
  fields.showGuides.checked = config.showGuides;
  fields.showCount.checked = config.showCount;
  fields.animations.checked = config.animations;
  const auto = config.highlightColor === 'auto';
  fields.accentTheme.setAttribute('aria-pressed', String(auto));
  fields.accentCustom.setAttribute('aria-pressed', String(!auto));
  $('accent-swatch').hidden = auto;
  if (!auto) {
    customColor = toCssColor(config.highlightColor);
    fields.accentColor.value = customColor;
    $('accent-swatch').style.setProperty('--swatch', customColor);
  }
  fields.newTabButtonIsChild.checked = config.newTabButtonIsChild;
  fields.confirmThreshold.value = config.confirmThreshold;
  $('confirmDown').disabled = config.confirmThreshold <= 0;
  $('confirmUp').disabled = config.confirmThreshold >= 99;
  renderPreview();
}

const PREVIEW_TABS = [
  { depth: 0, title: 'previewProjectNotes', favicon: '#e8710a' },
  { depth: 1, title: 'previewDesignReview', favicon: '#a142f4', active: true },
  { depth: 2, title: 'previewMockups', favicon: '#1e8e3e' },
  { depth: 1, title: 'previewIssueTracker', favicon: '#d93025', badge: 4 },
  { depth: 0, title: 'previewInbox', favicon: '#1a73e8', drop: true },
];

const CLOSE_ICON =
  '<svg class="close" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" ' +
  'stroke-linecap="round"><path d="M4.5 4.5l7 7M11.5 4.5l-7 7"/></svg>';

function renderPreview() {
  const strip = $('preview');
  strip.style.setProperty('--indent', `${config.indent}px`);
  strip.style.setProperty(
    '--drop',
    config.highlightColor === 'auto' ? 'var(--accent)' : toCssColor(config.highlightColor),
  );
  strip.classList.toggle('no-guides', !config.showGuides);
  strip.classList.toggle('no-count', !config.showCount);
  const rows = PREVIEW_TABS.map((tab) => {
    const row = document.createElement('div');
    row.className = `tab${tab.active ? ' active' : ''}${tab.drop ? ' drop' : ''}`;
    row.style.setProperty('--depth', tab.depth);
    const favicon = document.createElement('span');
    favicon.className = 'favicon';
    favicon.style.setProperty('--fav', tab.favicon);
    const title = document.createElement('span');
    title.className = 'title';
    title.textContent = msg(tab.title);
    row.append(favicon, title);
    if (tab.badge) {
      const badge = document.createElement('span');
      badge.className = 'badge';
      badge.textContent = tab.badge;
      row.append(badge);
    }
    if (tab.active) {
      row.insertAdjacentHTML('beforeend', CLOSE_ICON);
    }
    return row;
  });
  strip.replaceChildren(...rows);
  placeGuides(strip);
}

/**
 * Same rule as the native painter (decor.cpp, PaintGuides): one continuous line per indent
 * column over each run of deeper rows, centred in that column of the indent gutter; the
 * column holding the active tab's parent is drawn stronger. Positions are computed from the
 * target geometry rather than measured, so the margin-left transition cannot skew them.
 */
function placeGuides(strip) {
  const indent = config.indent;
  const padding = parseFloat(getComputedStyle(strip).paddingLeft) || 0;
  const rowHeight = strip.firstElementChild?.offsetHeight || 0;
  const active = PREVIEW_TABS.findIndex((tab) => tab.active);
  const activeLevel = active >= 0 ? PREVIEW_TABS[active].depth : 0;
  const maxLevel = Math.max(...PREVIEW_TABS.map((tab) => tab.depth));
  for (let column = 0; column < maxLevel; column += 1) {
    let index = 0;
    while (index < PREVIEW_TABS.length) {
      if (PREVIEW_TABS[index].depth <= column) {
        index += 1;
        continue;
      }
      const first = index;
      while (index < PREVIEW_TABS.length && PREVIEW_TABS[index].depth > column) {
        index += 1;
      }
      const top = padding + first * rowHeight + 3;
      const bottom = padding + index * rowHeight - 3;
      const guide = document.createElement('span');
      const strong = active >= first && active < index && column === activeLevel - 1;
      guide.className = `guide${strong ? ' strong' : ''}`;
      guide.style.left = `${padding + column * indent + Math.floor(indent / 2)}px`;
      guide.style.top = `${top}px`;
      guide.style.height = `${bottom - top}px`;
      strip.append(guide);
    }
  }
}

function save() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => void chrome.storage.sync.set({ config }), 150);
}

function update(patch) {
  config = { ...config, ...patch };
  render();
  save();
}

fields.indent.addEventListener('input', () => update({ indent: Number(fields.indent.value) }));
for (const name of ['showGuides', 'showCount', 'animations', 'newTabButtonIsChild']) {
  fields[name].addEventListener('change', () => update({ [name]: fields[name].checked }));
}
const toArgb = (css) => `#FF${css.slice(1).toUpperCase()}`;
fields.accentTheme.addEventListener('click', () => update({ highlightColor: 'auto' }));
fields.accentCustom.addEventListener('click', () => {
  if (config.highlightColor === 'auto') {
    update({ highlightColor: toArgb(customColor || toHex(themeAccent())) });
  }
});
fields.accentColor.addEventListener('input', () =>
  update({ highlightColor: toArgb(fields.accentColor.value) }),
);

const clampThreshold = (value) => Math.min(99, Math.max(0, Math.round(Number(value) || 0)));
fields.confirmThreshold.addEventListener('change', () =>
  update({ confirmThreshold: clampThreshold(fields.confirmThreshold.value) }),
);
$('confirmDown').addEventListener('click', () =>
  update({ confirmThreshold: clampThreshold(config.confirmThreshold - 1) }),
);
$('confirmUp').addEventListener('click', () =>
  update({ confirmThreshold: clampThreshold(config.confirmThreshold + 1) }),
);

const CHECK_ICON =
  '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" ' +
  'stroke-linecap="round" stroke-linejoin="round"><path d="M3.5 8.5l3 3 6-7"/></svg>';
const OFF_ICON =
  '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" ' +
  'stroke-linecap="round"><path d="M4 8h8"/></svg>';

// Downloads install.ps1 from the latest release, which fetches and installs the native package
// (packaging/windows/install.ps1). -NoExit keeps a Win+R window open so the result stays visible.
const INSTALL_COMMAND =
  'powershell -NoExit -NoProfile -ExecutionPolicy Bypass -Command "irm ' +
  'https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"';

$('install-command').textContent = INSTALL_COMMAND;
$('copy-command').addEventListener('click', async () => {
  await navigator.clipboard.writeText(INSTALL_COMMAND);
  const button = $('copy-command');
  button.textContent = msg('copied');
  setTimeout(() => (button.textContent = msg('copyCommand')), 1500);
});

function setStatus(state, label, detail, substitutions) {
  const status = $('native-status');
  status.className = state ? `pill ${state}` : 'pill';
  status.textContent = msg(label);
  $('native-detail').textContent = msg(detail, substitutions);
}

// Pill and explanation for each offsets status the module reports (native: OffsetsStatusName).
const OFFSETS_STATES = {
  resolving: ['wait', 'statusPreparing', 'offsetsResolvingDetail'],
  waiting: ['wait', 'statusWaiting', 'offsetsWaitingDetail'],
  disabled: ['bad', 'statusDisabled', 'offsetsDisabledDetail'],
  error: ['bad', 'statusError', 'offsetsErrorDetail'],
};

// Native module card: status and, when needed, the install command. Details live in About.
function showSetup(visible, safeMode = false) {
  $('native-setup-row').hidden = !visible;
  $('native-safe-row').hidden = !safeMode;
}

$('safe-retry').addEventListener('click', async () => {
  $('safe-retry').disabled = true;
  await chrome.runtime.sendMessage({ type: 'arbortab.safeModeReset' }).catch(() => {});
  setTimeout(() => {
    $('safe-retry').disabled = false;
    void reportNativeStatus();
  }, 800);
});

function renderNativeStatus(reply) {
  if (!reply) {
    showSetup(false);
    setStatus('', 'unknown', 'backgroundNoAnswer');
    return;
  }
  if (!reply.connected) {
    showSetup(true);
    setStatus('bad', 'notConnected', 'nativeUnavailableDetail');
    return;
  }
  if (reply.updateNeeded) {
    showSetup(reply.updateNeeded === 'native');
    setStatus('bad', 'statusUpdateNeeded',
              reply.updateNeeded === 'native' ? 'updateNativeDetail' : 'updateExtensionDetail');
    return;
  }
  // An older native release still works, but may lack fixes: the install command stays in
  // view whatever the other states say, which keep their own pill and explanation.
  const outdated = Boolean(reply.nativeOutdated);
  if (reply.safeMode?.active) {
    showSetup(outdated, true);
    setStatus('bad', 'statusSafeMode', 'safeModeDetail', [String(reply.safeMode.crashes)]);
    return;
  }
  showSetup(outdated);
  const offsets = reply.offsets?.status ?? 'ready';
  if (OFFSETS_STATES[offsets]) {
    const [state, label, detail] = OFFSETS_STATES[offsets];
    setStatus(state, label, detail, [reply.offsets?.chromeVersion || '?']);
    return;
  }
  if (outdated) {
    setStatus('wait', 'statusUpdateAvailable', 'updateAvailableDetail',
              [reply.nativeVersion, chrome.runtime.getManifest().version]);
    return;
  }
  setStatus('ok', 'connected', 'nativeConnectedDetail');
}

const OFFSETS_LABELS = {
  ready: 'offsetsReady',
  resolving: 'statusPreparing',
  waiting: 'statusWaiting',
  disabled: 'statusDisabled',
  error: 'statusError',
};

function renderAbout(reply) {
  $('about-extension').textContent = chrome.runtime.getManifest().version;
  const connected = Boolean(reply?.connected);
  $('about-native').textContent = connected ? reply.nativeVersion || '?' : msg('notConnected');
  const range = connected && reply.protocolMin != null
    ? (reply.protocolMin === reply.protocolMax ? `${reply.protocolMin}` : `${reply.protocolMin}–${reply.protocolMax}`)
    : '—';
  $('about-protocol').textContent = msg('aboutProtocolValue', [String(reply?.protocol ?? '?'), range]);
  $('about-transport-row').hidden = !connected;
  $('about-transport').textContent = msg('transportPipe');
  const offsets = connected ? reply.offsets : null;
  $('about-offsets-row').hidden = !offsets;
  if (offsets) {
    $('about-offsets').textContent = [
      `Chrome ${offsets.chromeVersion || '?'}`,
      `schema ${offsets.schema ?? '?'}`,
      msg(OFFSETS_LABELS[offsets.status] || 'unknown'),
    ].join(' · ');
  }
  $('about-features-row').hidden = !connected || Boolean(reply.updateNeeded);
  $('about-features').replaceChildren(
    ...Object.entries(reply?.features || {}).map(([name, enabled]) => {
      const chip = document.createElement('span');
      chip.className = `chip ${enabled ? 'on' : 'off'}`;
      chip.title = msg(enabled ? 'featureOnTitle' : 'featureOffTitle');
      chip.innerHTML = enabled ? CHECK_ICON : OFF_ICON;
      chip.append(msg(`feature${name[0].toUpperCase()}${name.slice(1)}`));
      return chip;
    }),
  );
}

async function reportNativeStatus() {
  let reply = null;
  try {
    reply = await chrome.runtime.sendMessage({ type: 'arbortab.status' });
  } catch {
    // The service worker did not answer: both views show "unknown".
  }
  renderNativeStatus(reply);
  renderAbout(reply);
}

// Debug export: the service worker's report (no URLs or titles) as pretty JSON.
const NATIVE_LOG = String.raw`%LOCALAPPDATA%\ArborTab\logs\native.log`;
{
  const hint = $('debug-hint');
  const [before, after] = msg('debugInfoHint', ['@@path@@']).split('@@path@@');
  const code = document.createElement('code');
  code.textContent = NATIVE_LOG;
  hint.replaceChildren(before, code, after ?? '');
}

async function debugReport() {
  const report = await chrome.runtime.sendMessage({ type: 'arbortab.debug' });
  return JSON.stringify(report, null, 2);
}

$('debug-copy').addEventListener('click', async () => {
  await navigator.clipboard.writeText(await debugReport());
  const button = $('debug-copy');
  button.textContent = msg('copied');
  setTimeout(() => (button.textContent = msg('copyCommand')), 1500);
});

$('debug-download').addEventListener('click', async () => {
  const blob = new Blob([await debugReport()], { type: 'application/json' });
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = `arbortab-debug-${new Date().toISOString().replace(/[:.]/g, '-')}.json`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 1000);
});

function showNativeUpdateTarget() {
  const params = new URLSearchParams(location.search);
  if (params.get('reason') !== 'native-update') {
    return;
  }
  const heading = $('native');
  const card = $('native-card');
  const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)').matches;
  heading.scrollIntoView({ behavior: reducedMotion ? 'auto' : 'smooth', block: 'start' });
  card.classList.add('native-update-highlight');
  if (reducedMotion) {
    setTimeout(() => card.classList.remove('native-update-highlight'), 2400);
  } else {
    card.addEventListener('animationend', () => {
      card.classList.remove('native-update-highlight');
    }, { once: true });
  }
}

async function load() {
  const stored = await chrome.storage.sync.get('config');
  config = withDefaults(stored?.config);
  render();
  showNativeUpdateTarget();
  void reportNativeStatus();
  // The module's state changes on its own (offsets arrive, Chrome restarts): keep it live.
  setInterval(() => void reportNativeStatus(), 3000);
}

void load();
