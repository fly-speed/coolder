'use strict';

let personalSettings = { language: 'zh', theme: 'default', font_size: 'md' };

function applyAppearance(settings) {
  document.documentElement.dataset.uiTheme = settings.theme;
  document.documentElement.dataset.fontSize = settings.font_size;
}

async function loadPersonalSettings() {
  const settings = await request('/api/v1/auth/preferences');
  let catalog = {};
  try {
    catalog = await i18n.load(settings.language);
  } catch (error) {
    settings.language = 'zh';
    notice(error);
  }
  personalSettings = settings;
  applyAppearance(settings);
  i18n.apply(settings.language, catalog);
}

function resetPersonalSettings() {
  personalSettings = { language: 'zh', theme: 'default', font_size: 'md' };
  applyAppearance(personalSettings);
  i18n.apply('zh', {});
}

async function openPersonalSettings() {
  await i18n.ready;
  const select = $('personal-language');
  select.replaceChildren();
  for (const locale of i18n.locales) {
    const option = document.createElement('option');
    option.value = locale.code;
    option.textContent = locale.name;
    select.append(option);
  }
  const form = $('preferences-form');
  for (const key of ['language', 'theme', 'font_size'])
    form.elements[key].value = personalSettings[key];
  $('password-dialog').querySelector('.dialog-notice')?.remove();
}

$('preferences-form').onsubmit = task(async () => {
  const form = $('preferences-form');
  const submit = form.querySelector('button');
  submit.disabled = true;
  try {
    const settings = Object.fromEntries(new FormData(form));
    // Load before saving: a broken translation file must not strand this account.
    const catalog = await i18n.load(settings.language);
    personalSettings = await request('/api/v1/auth/preferences', settings);
    applyAppearance(personalSettings);
    i18n.apply(settings.language, catalog);
    notice(t('个人设置已保存'));
  } finally {
    submit.disabled = false;
  }
});
