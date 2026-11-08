// Settings page. Every change is saved right away; the service worker picks it up through
// chrome.storage.onChanged and pushes it to the native module.

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
  accentAuto: $('accentAuto'),
  accentColor: $('accentColor'),
  newTabButtonIsChild: $('newTabButtonIsChild'),
  confirmThreshold: $('confirmThreshold'),
};

let config = { ...DEFAULTS };
let saveTimer = null;

function withDefaults(stored) {
  const merged = { ...DEFAULTS, ...(stored || {}) };
  if (LEGACY_HIGHLIGHT_COLORS.has(String(merged.highlightColor).toUpperCase())) {
    merged.highlightColor = 'auto';
  }
  return merged;
}

/** '#AARRGGBB' -> '#rrggbb' for <input type=color>. */
function toCssColor(argb) {
  return /^#[0-9a-f]{8}$/i.test(argb) ? `#${argb.slice(3).toLowerCase()}` : FALLBACK_ACCENT;
}

function render() {
  fields.indent.value = config.indent;
  $('indent-value').textContent = `${config.indent} px`;
  fields.showGuides.checked = config.showGuides;
  fields.showCount.checked = config.showCount;
  fields.animations.checked = config.animations;
  const auto = config.highlightColor === 'auto';
  fields.accentAuto.checked = auto;
  fields.accentColor.disabled = auto;
  if (!auto) {
    fields.accentColor.value = toCssColor(config.highlightColor);
  }
  fields.newTabButtonIsChild.checked = config.newTabButtonIsChild;
  fields.confirmThreshold.value = config.confirmThreshold;
  renderPreview();
}

const PREVIEW_TABS = [
  { depth: 0, title: 'Project notes' },
  { depth: 1, title: 'Design review', active: true },
  { depth: 2, title: 'Mockups — round 2' },
  { depth: 1, title: 'Issue tracker', badge: 4 },
  { depth: 0, title: 'Inbox', drop: true },
];

function renderPreview() {
  const preview = $('preview');
  preview.style.setProperty('--indent', `${config.indent}px`);
  preview.style.setProperty(
    '--drop',
    config.highlightColor === 'auto' ? 'var(--accent)' : toCssColor(config.highlightColor),
  );
  preview.classList.toggle('no-guides', !config.showGuides);
  preview.classList.toggle('no-count', !config.showCount);
  preview.replaceChildren(
    ...PREVIEW_TABS.map((tab) => {
      const row = document.createElement('div');
      row.className = `tab${tab.active ? ' active' : ''}${tab.drop ? ' drop' : ''}`;
      row.style.setProperty('--depth', tab.depth);
      // Guides sit in the middle of each indent step to the left of the row.
      for (let level = 0; level < tab.depth; level += 1) {
        const guide = document.createElement('span');
        guide.className = 'guide';
        guide.style.left = `calc(${level - tab.depth} * var(--indent) + var(--indent) / 2)`;
        row.append(guide);
      }
      const icon = document.createElement('span');
      icon.className = 'icon';
      const title = document.createElement('span');
      title.className = 'title';
      title.textContent = tab.title;
      row.append(icon, title);
      if (tab.badge) {
        const badge = document.createElement('span');
        badge.className = 'badge';
        badge.textContent = tab.badge;
        row.append(badge);
      }
      return row;
    }),
  );
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
fields.accentAuto.addEventListener('change', () =>
  update({
    highlightColor: fields.accentAuto.checked
      ? 'auto'
      : `#FF${fields.accentColor.value.slice(1).toUpperCase()}`,
  }),
);
fields.accentColor.addEventListener('input', () =>
  update({ highlightColor: `#FF${fields.accentColor.value.slice(1).toUpperCase()}` }),
);
fields.confirmThreshold.addEventListener('change', () =>
  update({ confirmThreshold: Math.max(0, Number(fields.confirmThreshold.value) || 0) }),
);

async function reportNativeStatus() {
  const status = $('native-status');
  const detail = $('native-detail');
  try {
    const reply = await chrome.runtime.sendMessage({ type: 'arbortab.status' });
    if (reply?.connected) {
      const features = Object.entries(reply.features || {})
        .map(([name, enabled]) => `${name} ${enabled ? 'on' : 'off'}`)
        .join(' · ');
      status.textContent = 'Connected';
      status.className = 'status ok';
      detail.textContent = `Native ${reply.nativeVersion || '?'} — ${features}`;
    } else {
      status.textContent = 'Not connected';
      status.className = 'status bad';
      detail.textContent =
        'version.dll is not installed or has no offsets for this Chrome build; tabs stay unchanged.';
    }
  } catch {
    status.textContent = 'Unknown';
    status.className = 'status';
    detail.textContent = 'The background worker did not answer.';
  }
}

async function load() {
  const stored = await chrome.storage.sync.get('config');
  config = withDefaults(stored?.config);
  render();
  void reportNativeStatus();
}

void load();
