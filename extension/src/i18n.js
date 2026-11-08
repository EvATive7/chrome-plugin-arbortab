// Shared helpers for Chrome extension localization.

export function msg(name, substitutions) {
  return chrome.i18n.getMessage(name, substitutions) || name;
}

export function localizeDocument(root = document) {
  root.documentElement.lang = chrome.i18n.getUILanguage().replace('_', '-');
  for (const element of root.querySelectorAll('[data-i18n]')) {
    element.textContent = msg(element.dataset.i18n);
  }
  for (const element of root.querySelectorAll('[data-i18n-title]')) {
    element.title = msg(element.dataset.i18nTitle);
  }
}
