import { localizeDocument, msg } from '../i18n.js';

localizeDocument();

const params = new URLSearchParams(location.search);
const tabId = Number(params.get('tab'));
const count = Number(params.get('count') || 0);
document.getElementById('question').textContent =
  count === 1 ? msg('closeOneQuestion') : msg('closeManyQuestion', String(count));

document.getElementById('yes').addEventListener('click', async () => {
  const tabs = await chrome.tabs.query({}).catch(() => []);
  const subtree = tabs.filter((tab) => tab.openerTabId === tabId);
  const ids = [tabId, ...subtree.map((tab) => tab.id)].filter((id) => Number.isFinite(id));
  await chrome.tabs.remove(ids).catch(() => {});
  window.close();
});

document.getElementById('no').addEventListener('click', () => window.close());
