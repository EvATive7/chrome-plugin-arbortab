const DEFAULTS = {
  indent: 16,
  showCount: true,
  confirmThreshold: 0,
  newTabButtonIsChild: false,
  highlightColor: '#663D7EFF',
};

const fields = {
  indent: document.getElementById('indent'),
  showCount: document.getElementById('showCount'),
  confirmThreshold: document.getElementById('confirmThreshold'),
  newTabButtonIsChild: document.getElementById('newTabButtonIsChild'),
};

async function load() {
  const stored = await chrome.storage.sync.get('config');
  const config = { ...DEFAULTS, ...(stored?.config || {}) };
  fields.indent.value = config.indent;
  fields.showCount.checked = config.showCount;
  fields.confirmThreshold.value = config.confirmThreshold;
  fields.newTabButtonIsChild.checked = config.newTabButtonIsChild;
  reportNativeStatus();
}

async function reportNativeStatus() {
  const status = document.getElementById('native-status');
  const detail = document.getElementById('native-detail');
  try {
    const reply = await chrome.runtime.sendMessage({ type: 'arbortab.status' });
    if (reply?.connected) {
      const features = Object.entries(reply.features || {})
        .map(([name, enabled]) => `${name}: ${enabled ? 'on' : 'off'}`)
        .join(', ');
      status.textContent = `connected (native ${reply.nativeVersion || '?'})`;
      status.className = 'status ok';
      detail.textContent = features;
    } else {
      status.textContent = 'not connected';
      status.className = 'status bad';
      detail.textContent =
        'The native module (version.dll) is not installed or has no offsets for this Chrome build; tabs stay unchanged.';
    }
  } catch {
    status.textContent = 'unknown';
    status.className = 'status';
  }
}

document.getElementById('save').addEventListener('click', async () => {
  const config = {
    ...DEFAULTS,
    indent: Number(fields.indent.value) || DEFAULTS.indent,
    showCount: fields.showCount.checked,
    confirmThreshold: Number(fields.confirmThreshold.value) || 0,
    newTabButtonIsChild: fields.newTabButtonIsChild.checked,
  };
  await chrome.storage.sync.set({ config });
  const saved = document.getElementById('saved');
  saved.textContent = 'saved';
  setTimeout(() => {
    saved.textContent = '';
  }, 1500);
});

void load();
