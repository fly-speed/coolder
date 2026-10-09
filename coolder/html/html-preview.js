'use strict';

const htmlPreview = { version: 0, file: null, project: null, links: [] };
function clearHtmlPreview() {
  ++htmlPreview.version;
  htmlPreview.links = [];
  $('html-preview-frame').removeAttribute('srcdoc');
}
function previewResourcePath(reference, source, root) {
  if (/^(?:[a-z][a-z\d+.-]*:|\/\/)/i.test(reference))
    throw new Error(reference);
  const relative = source.slice(root.length + 1);
  const url = new URL(reference, 'https://preview.invalid/' + relative);
  const path = decodeURIComponent(url.pathname).replace(/^\/+/, '');
  if (!path || path.split('/').some(part => part === '..' || part === '.') || path.includes('\\'))
    throw new Error(reference);
  return root + '/' + path;
}
async function replacePreviewAsync(text, pattern, replace) {
  const matches = [...text.matchAll(pattern)];
  for (let i = matches.length - 1; i >= 0; --i) {
    const m = matches[i];
    text = text.slice(0, m.index) + await replace(m) + text.slice(m.index + m[0].length);
  }
  return text;
}
async function refreshHtmlPreview() {
  clearHtmlPreview();
  const version = htmlPreview.version, warnings = new Set();
  const file = htmlPreview.file, project = htmlPreview.project;
  const markdown = CoolderMarkdown.isMarkdownName(file.path);
  const nonce = document.querySelector('meta[name="preview-script-nonce"]').content;
  uiText($('html-preview-status'), () => t('正在准备预览…'));
  const cache = new Map();
  let bytes = 0;
  function bounded(value) {
    bytes += value.length;
    if (bytes > 16 * 1024 * 1024) throw new Error('Preview exceeds 16 MiB');
    return value;
  }
  async function read(path, binary = false) {
    const key = (binary ? 'asset:' : 'text:') + path;
    if (!cache.has(key)) {
      if (cache.size >= 128) throw new Error('Preview exceeds 128 resources');
      cache.set(key, (async () => {
        if (!binary) {
          const item = sourceFiles.get(path);
          const editing = state.file?.path === path ? codeEditors.editing : item?.editing;
          if (editing) return bounded(editing.model ? editing.model.getValue() :
            (state.file?.path === path ? $('code-fallback').value : item.source.querySelector('#code-fallback').value));
          const data = await projectFileRequest('read', path, undefined, project);
          if (data.truncated) throw new Error(path + ' (truncated)');
          return bounded(data.content);
        }
        const data = await projectFileRequest('preview-asset', path, undefined, project);
        return bounded('data:' + data.mime + ';base64,' + data.base64);
      })());
    }
    return cache.get(key);
  }
  async function asset(reference, source) {
    if (/^data:(?:image|font)\//i.test(reference)) return reference;
    if (reference.startsWith('#')) return reference;
    try { return await read(previewResourcePath(reference, source, project.project_path), true); }
    catch { warnings.add(reference); return 'data:,'; }
  }
  async function css(text, source, chain = []) {
    if (chain.includes(source) || chain.length >= 8) throw new Error(source);
    text = await replacePreviewAsync(text, /@import\s+(?:url\(\s*)?["']([^"']+)["']\s*\)?\s*([^;]*);/gi, async m => {
      try {
        const path = previewResourcePath(m[1], source, project.project_path);
        const imported = await css(await read(path), path, [...chain, source]);
        return m[2].trim() ? '@media ' + m[2] + '{' + imported + '}' : imported;
      } catch { warnings.add(m[1]); return ''; }
    });
    return replacePreviewAsync(text, /url\(\s*(["']?)([^"')]+)\1\s*\)/gi,
      async m => 'url("' + await asset(m[2].trim(), source) + '")');
  }
  try {
    const source = await read(file.path);
    const doc = new DOMParser().parseFromString(markdown ? CoolderMarkdown.render(source) : source, 'text/html');
    if (markdown) {
      const style = doc.createElement('style');
      style.textContent = `
        :root { color-scheme: light; }
        body { max-width: 920px; margin: 0 auto; padding: 28px; color: #243247; background: white;
          font: 16px/1.7 system-ui, sans-serif; overflow-wrap: anywhere; }
        h1,h2,h3,h4,h5,h6 { line-height: 1.3; margin: 1.4em 0 .6em; }
        h1,h2 { border-bottom: 1px solid #dce2eb; padding-bottom: .3em; }
        a { color: #315bc0; } img { max-width: 100%; height: auto; }
        pre { padding: 16px; background: #f3f5f8; overflow-x: auto; border-radius: 8px; }
        code { font-family: ui-monospace, monospace; background: #f3f5f8; padding: .15em .3em; }
        pre code { padding: 0; } blockquote { margin-left: 0; padding-left: 16px; border-left: 4px solid #c5cfdf; color: #536581; }
        table { display: block; max-width: 100%; overflow-x: auto; border-collapse: collapse; }
        th,td { border: 1px solid #c5cfdf; padding: 8px 12px; } th { background: #f3f5f8; }
        .task-list-item { list-style: none; } hr { border: 0; border-top: 1px solid #dce2eb; }
      `;
      doc.head.append(style);
    }
    doc.querySelectorAll('base, meta[http-equiv], iframe, object, embed').forEach(e => e.remove());
    const eventBindings = [];
    for (const el of doc.querySelectorAll('*')) {
      el.removeAttribute('data-coolder-preview-event');
      for (const attribute of [...el.attributes]) {
        if (!markdown && /^on[a-z]+$/i.test(attribute.name)) {
          const id = el.getAttribute('data-coolder-preview-event') || String(eventBindings.length);
          el.setAttribute('data-coolder-preview-event', id);
          eventBindings.push({ id, name: attribute.name.toLowerCase(), code: attribute.value });
        }
        if (/^on/i.test(attribute.name) || ['srcset', 'integrity', 'crossorigin', 'nonce', 'ping', 'action', 'formaction'].includes(attribute.name))
          el.removeAttribute(attribute.name);
      }
      if (el.hasAttribute('style')) el.setAttribute('style', await css(el.getAttribute('style'), file.path));
    }
    for (const link of doc.querySelectorAll('link')) {
      if (link.rel === 'stylesheet') {
        try {
          const path = previewResourcePath(link.getAttribute('href'), file.path, project.project_path);
          const style = doc.createElement('style');
          style.textContent = await css(await read(path), path);
          if (link.media) style.media = link.media;
          link.replaceWith(style);
        } catch { warnings.add(link.getAttribute('href')); link.remove(); }
      } else link.remove();
    }
    for (const style of doc.querySelectorAll('style'))
      style.textContent = await css(style.textContent, file.path);
    for (const img of doc.querySelectorAll('img[src], input[type="image"][src]'))
      img.setAttribute('src', await asset(img.getAttribute('src'), file.path));
    const links = [];
    for (const el of doc.querySelectorAll('[data-coolder-preview-link]'))
      el.removeAttribute('data-coolder-preview-link');
    for (const el of doc.querySelectorAll('a[href], area[href]')) {
      const href = el.getAttribute('href');
      if (href.startsWith('#')) continue;
      try {
        const path = previewResourcePath(href, file.path, project.project_path);
        if (markdown || !/\.html?$/i.test(path)) throw new Error(href);
        const fragment = new URL(href, 'https://preview.invalid/').hash;
        el.setAttribute('data-coolder-preview-link', String(links.length));
        el.removeAttribute('target');
        links.push({ path, fragment });
      } catch { el.removeAttribute('href'); }
    }
    if (!markdown) {
      const bridge = doc.createElement('script');
      bridge.textContent = `
        document.addEventListener('click', function(event) {
          if (event.defaultPrevented || event.button !== 0) return;
          const link = event.target.closest('a, area');
          if (!link) return;
          const index = link.getAttribute('data-coolder-preview-link');
          if (index !== null) {
            event.preventDefault();
            parent.postMessage({ type: 'coolder-preview-link', version: ${version}, index: Number(index) }, '*');
          }
        });
        window.addEventListener('DOMContentLoaded', function() {
          const fragment = ${JSON.stringify(file.fragment || '')};
          if (fragment) {
            try { document.getElementById(decodeURIComponent(fragment.slice(1)))?.scrollIntoView(); } catch (_) {}
          }
        });
      `;
      doc.body.append(bridge);
    }
    for (const binding of eventBindings) {
      const script = doc.createElement('script');
      script.textContent = '(function () { const element = document.querySelector(' +
        JSON.stringify('[data-coolder-preview-event="' + binding.id + '"]') +
        '); if (element) element[' + JSON.stringify(binding.name) +
        '] = function (event) {\n' + binding.code + '\n}; })();';
      doc.body.append(script);
    }
    for (const script of doc.querySelectorAll('script')) {
      if (markdown) { script.remove(); continue; }
      if (script.type && !['text/javascript', 'application/javascript'].includes(script.type)) {
        if (script.type === 'module') { warnings.add('JavaScript modules'); script.remove(); }
        continue;
      }
      try {
        const content = script.hasAttribute('src')
          ? await read(previewResourcePath(script.getAttribute('src'), file.path, project.project_path))
          : script.textContent;
        const encoded = btoa(Array.from(new TextEncoder().encode(content), b => String.fromCharCode(b)).join(''));
        script.textContent = '';
        script.setAttribute('nonce', nonce);
        script.setAttribute('src', 'data:text/javascript;base64,' + encoded);
      } catch { warnings.add(script.getAttribute('src')); script.remove(); }
    }
    const policy = doc.createElement('meta');
    policy.httpEquiv = 'Content-Security-Policy';
    policy.content = "default-src 'none'; script-src 'nonce-" + nonce + "'; style-src 'unsafe-inline'; img-src data:; font-src data:; connect-src 'none'; form-action 'none'; base-uri 'none'";
    doc.head.prepend(policy);
    if (version !== htmlPreview.version) return;
    htmlPreview.links = links;
    $('html-preview-frame').setAttribute('sandbox', markdown ? '' : 'allow-scripts');
    $('html-preview-frame').srcdoc = '<!doctype html>\n' + doc.documentElement.outerHTML;
    uiText($('html-preview-status'), () => warnings.size
      ? t('部分资源无法预览：') + [...warnings].join(', ') : file.path);
  } catch (error) {
    if (version === htmlPreview.version) plainText($('html-preview-status'), error.message);
  }
}
// File panels are cloned when tabs open; delegate to their stable parent.
$('source-pane').addEventListener('click', event => {
  if (!event.target.closest('#html-preview-open')) return;
  htmlPreview.minimized = false;
  $('html-preview-restore').hidden = true;
  htmlPreview.file = state.file;
  htmlPreview.project = state.project;
  const markdown = CoolderMarkdown.isMarkdownName(state.file.path);
  uiText($('html-preview-title'), () => t(markdown ? 'Markdown 预览' : '网页预览'));
  $('html-preview-frame').removeAttribute('data-i18n-title');
  $('html-preview-frame').title = t(markdown ? 'Markdown 预览' : '网页预览');
  $('html-preview-dialog').showModal();
  refreshHtmlPreview();
});
$('html-preview-refresh').onclick = refreshHtmlPreview;


function setHtmlPreviewMaximized(maximized) {
  $('html-preview-dialog').classList.toggle('maximized', maximized);
  $('html-preview-maximize').setAttribute('aria-pressed', String(maximized));
  const button = $('html-preview-maximize');
  const label = maximized ? '还原' : '最大化';
  for (const attr of ['title', 'aria-label']) {
    button.setAttribute('data-i18n-' + attr, label);
    button.setAttribute(attr, t(label));
  }
}
$('html-preview-maximize').onclick = () => {
  setHtmlPreviewMaximized(!$('html-preview-dialog').classList.contains('maximized'));
};
$('html-preview-dialog').addEventListener('close', () => {
  if (htmlPreview.minimized) return;
  clearHtmlPreview();
  setHtmlPreviewMaximized(false);
  exitHtmlPreviewFullscreen();
});

$('html-preview-dialog').querySelector('.dialog-title').addEventListener('dblclick', event => {
  if (!event.target.closest('button')) $('html-preview-maximize').click();
});

window.addEventListener('message', event => {
  if (event.source !== $('html-preview-frame').contentWindow || !$('html-preview-dialog').open) return;
  const data = event.data;
  if (!data || data.type !== 'coolder-preview-link' || data.version !== htmlPreview.version || !Number.isInteger(data.index)) return;
  const target = htmlPreview.links[data.index];
  if (!target) return;
  htmlPreview.file = target;
  refreshHtmlPreview();
});

function renderHtmlPreviewFullscreen() {
  const full = Boolean(htmlPreview.fullscreen && document.fullscreenElement);
  if (!document.fullscreenElement) htmlPreview.fullscreen = false;
  $('html-preview-dialog').classList.toggle('preview-fullscreen', full);
  const button = $('html-preview-fullscreen');
  button.setAttribute('aria-pressed', String(full));
  for (const attr of ['title', 'aria-label']) {
    const label = full ? '退出全屏' : '全屏';
    button.setAttribute('data-i18n-' + attr, label);
    button.setAttribute(attr, t(label));
  }
}
async function exitHtmlPreviewFullscreen() {
  if (htmlPreview.fullscreen && document.fullscreenElement) await document.exitFullscreen();
  htmlPreview.fullscreen = false;
  renderHtmlPreviewFullscreen();
}
$('html-preview-fullscreen').onclick = task(async () => {
  if (htmlPreview.fullscreen) return exitHtmlPreviewFullscreen();
  if (!document.fullscreenEnabled || !document.documentElement.requestFullscreen)
    throw new Error(t('当前浏览器不支持全屏，请使用最大化。'));
  // Dialog elements cannot request fullscreen themselves.
  htmlPreview.fullscreen = true;
  try { await document.documentElement.requestFullscreen(); }
  catch (error) { htmlPreview.fullscreen = false; throw error; }
  renderHtmlPreviewFullscreen();
});
document.addEventListener('fullscreenchange', renderHtmlPreviewFullscreen);
$('html-preview-minimize').onclick = task(async () => {
  await exitHtmlPreviewFullscreen();
  htmlPreview.minimized = true;
  $('html-preview-restore-name').textContent = htmlPreview.file.path;
  $('html-preview-restore').hidden = false;
  $('html-preview-dialog').close();
  $('html-preview-restore').focus();
});
$('html-preview-restore').onclick = () => {
  htmlPreview.minimized = false;
  $('html-preview-restore').hidden = true;
  $('html-preview-dialog').showModal();
};
