'use strict';

// Catalogs contain plain text only. Never translate user content, source code or model output.
const i18n = {
  language: 'zh',
  catalog: {},
  locales: [],
  bindings: new Map(),
  ready: null,
  async load(language) {
    if (!/^[A-Za-z0-9-]{1,24}$/.test(language)) throw new Error('Invalid language');
    const response = await fetch('/i18n/' + language + '.json');
    if (!response.ok) throw new Error(t('无法加载语言文件'));
    const catalog = await response.json();
    if (!catalog || Array.isArray(catalog) || typeof catalog !== 'object')
      throw new Error(t('无法加载语言文件'));
    return catalog;
  },
  apply(language, catalog) {
    this.language = language;
    this.catalog = catalog;
    document.documentElement.lang = language;
    for (const element of document.querySelectorAll('[data-i18n]'))
      element.textContent = t(element.dataset.i18n);
    for (const attr of ['title', 'placeholder', 'aria-label'])
      for (const element of document.querySelectorAll('[data-i18n-' + attr + ']'))
        element.setAttribute(attr, t(element.getAttribute('data-i18n-' + attr)));
    for (const [element, render] of this.bindings) {
      if (element.isConnected) element.textContent = render();
      else this.bindings.delete(element);
    }
  }
};

function t(key) {
  const value = Object.prototype.hasOwnProperty.call(i18n.catalog, key) && i18n.catalog[key];
  return typeof value === 'string' && value ? value : key;
}

function uiText(element, render) {
  if (i18n.bindings.size > 500)
    for (const bound of i18n.bindings.keys()) if (!bound.isConnected) i18n.bindings.delete(bound);
  delete element.dataset.i18n;
  i18n.bindings.set(element, render);
  element.textContent = render();
}

for (const element of document.querySelectorAll('option, title')) {
  if (/[\u4e00-\u9fff]/.test(element.textContent))
    element.dataset.i18n = element.textContent.trim();
}

i18n.ready = fetch('/i18n/manifest.json')
  .then(response => {
    if (!response.ok) throw new Error('Language catalog unavailable');
    return response.json();
  })
  .then(locales => {
    i18n.locales = locales.filter(item => /^[A-Za-z0-9-]{1,24}$/.test(item.code));
  });
// Keep a failed manifest load observable when personal settings are opened.
i18n.ready.catch(() => {});

function plainText(element, value) {
  i18n.bindings.delete(element);
  delete element.dataset.i18n;
  element.textContent = value;
}
