'use strict';

// Runtime and workers are copied from webcool's vendored Monaco 0.55.1.
const codeEditors = { runtime: null, viewer: null, viewModel: null, sequence: 0, editing: null };
function loadMonaco() {
  if (codeEditors.runtime) return codeEditors.runtime;
  const root = '/vendor/monaco-editor/vs';
  const locale = { zh: 'zh-cn', ja: 'ja', ko: 'ko' }[i18n.language] || 'en';
  window.__webcoolMonacoAmdConfig = {
    paths: {
      vs: root,
      'vs/nls.messages.ja': root + '/nls.messages.ja.js',
      'vs/nls.messages.ko': root + '/nls.messages.ko.js'
    },
    'vs/nls': { availableLanguages: { '*': locale } }
  };
  const workerEnvironment = {
    getWorker(_moduleId, label) {
      const workers = {
        json: 'json.worker-DKiEKt88.js',
        css: 'css.worker-HnVq6Ewq.js',
        scss: 'css.worker-HnVq6Ewq.js',
        less: 'css.worker-HnVq6Ewq.js',
        html: 'html.worker-B51mlPHg.js',
        handlebars: 'html.worker-B51mlPHg.js',
        razor: 'html.worker-B51mlPHg.js',
        typescript: 'ts.worker-CMbG-7ft.js',
        javascript: 'ts.worker-CMbG-7ft.js'
      };
      return new Worker(root + '/assets/' + (workers[label] || 'editor.worker-Be8ye1pW.js'));
    }
  };
  codeEditors.runtime = new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = root + '/monaco.bundle.js';
    script.onload = () =>
      window.require(
        ['vs/editor/editor.main'],
        () => {
          // The bundled AMD runtime installs its own blob worker factory. Override
          // it after initialization so workers remain same-origin static assets.
          window.MonacoEnvironment = workerEnvironment;
          resolve(window.monaco);
        },
        reject
      );
    script.onerror = () => {
      script.remove();
      reject(new Error(t('代码编辑器加载失败，已使用文本模式')));
    };
    document.head.append(script);
  }).catch(error => {
    codeEditors.runtime = null;
    throw error;
  });
  return codeEditors.runtime;
}
function codeOptions() {
  return {
    automaticLayout: true,
    fontSize: parseFloat(getComputedStyle(document.documentElement).fontSize),
    fontFamily: 'Menlo, Monaco, Consolas, monospace',
    scrollBeyondLastLine: false,
    minimap: { enabled: true },
    tabSize: 4,
    insertSpaces: true,
    folding: true,
    bracketPairColorization: { enabled: true }
  };
}
function codeModel(monaco, text, path) {
  const uri = monaco.Uri.parse(
    'inmemory://coolder/' +
      ++codeEditors.sequence +
      '/' +
      path.split('/').map(encodeURIComponent).join('/')
  );
  return monaco.editor.createModel(text, undefined, uri);
}
function codeTheme() {
  if (!window.monaco?.editor) return;
  window.monaco.editor.setTheme(
    document.documentElement.dataset.uiTheme === 'default' ||
      !document.documentElement.dataset.uiTheme
      ? 'vs-dark'
      : 'vs'
  );
  const options = { fontSize: codeOptions().fontSize };
  codeEditors.viewer?.updateOptions(options);
  codeEditors.editing?.editor?.updateOptions(options);
  codeEditors.editing?.diff?.updateOptions(options);
}
new MutationObserver(codeTheme).observe(document.documentElement, {
  attributes: true,
  attributeFilter: ['data-ui-theme', 'data-font-size']
});
function clearCodeViewer() {
  ++codeEditors.sequence;
  codeEditors.viewer?.dispose();
  codeEditors.viewModel?.dispose();
  codeEditors.viewer = null;
  codeEditors.viewModel = null;
  $('code-viewer').hidden = true;
  $('file-content').hidden = false;
}
async function showCodeFile(file) {
  clearCodeViewer();
  const selection = codeEditors.sequence;
  try {
    const monaco = await loadMonaco();
    if (selection !== codeEditors.sequence || state.file !== file || codeEditors.editing) return;
    $('code-viewer').hidden = false;
    codeEditors.viewModel = codeModel(monaco, file.content, file.path);
    codeEditors.viewer = monaco.editor.create($('code-viewer'), {
      ...codeOptions(),
      model: codeEditors.viewModel,
      readOnly: true,
      domReadOnly: true,
      minimap: { enabled: false },
      ariaLabel: t('文件预览')
    });
    codeTheme();
    $('file-content').hidden = true;
  } catch (error) {
    if (selection === codeEditors.sequence)
      notice(new Error(t('代码编辑器加载失败，已使用文本模式')));
  }
}
function codeValue(editing) {
  return editing.model ? editing.model.getValue(undefined, true) : $('code-fallback').value;
}
function codeDirty() {
  const e = codeEditors.editing;
  return e && codeValue(e) !== e.original;
}
function disposeCodeEditing() {
  const e = codeEditors.editing;
  codeEditors.editing = null;
  if (e) {
    e.change?.dispose();
    e.diff?.dispose();
    e.originalModel?.dispose();
  }
  if (e && e.model === codeEditors.viewModel && e.model.getValue() !== e.original)
    e.model.setValue(e.original);
  codeEditors.viewer?.updateOptions({
    readOnly: true,
    domReadOnly: true,
    ariaLabel: t('文件预览')
  });
  $('code-inline-toolbar').hidden = true;
  $('code-fallback').hidden = true;
  $('edit-file').hidden = false;
  uiText($('code-mode'), () => t('只读'));
  $('code-viewer').hidden = !codeEditors.viewer;
  $('file-content').hidden = !!codeEditors.viewer;
  $('code-review').hidden = true;
  $('changes-pane').hidden = false;
  $('code-fallback').value = '';
  $('code-diff-host').hidden = true;
  $('code-diff-fallback').hidden = true;
  renderSourceFileTabs();
  renderReviewFileTabs();
}
function closeCodeEditing() {
  if (codeEditors.editing?.saving) return false;
  if (codeDirty() && !confirm(t('存在未保存的修改，确定关闭？'))) return false;
  disposeCodeEditing();
  return true;
}
function invalidateCodePatch(e) {
  e.patch = '';
  renderSourceFileTabs();
  renderReviewFileTabs();
  $('code-save').disabled = true;
  uiText($('code-status'), () => t('修改后请先预览差异，再确认保存。'));
}
$('edit-file').onclick = task(async () => {
  if (!state.file || state.file.truncated || state.project?.permission === 'read' || codeEditors.editing?.saving) return;
  if (codeEditors.editing?.file.path === state.file.path) {
    selectBodyTab('source');
    requestAnimationFrame(() => codeEditors.editing?.editor?.layout());
    return;
  }
  if (codeDirty() && !confirm(t('存在未保存的修改，确定关闭？'))) return;
  disposeCodeEditing();
  const file = state.file;
  const e = { file, project: state.project, original: file.content, patch: '', saving: false };
  codeEditors.editing = e;
  pinSourceFile(file.path);
  $('code-inline-toolbar').hidden = false;
  $('edit-file').hidden = true;
  uiText($('code-mode'), () => t('编辑中'));
  $('file-content').hidden = true;
  $('code-fallback').readOnly = false;
  $('code-viewer').hidden = true;
  $('code-fallback').hidden = false;
  $('code-fallback').value = file.content;
  $('code-preview').disabled = false;
  $('code-find').disabled = true;
  invalidateCodePatch(e);
  selectBodyTab('source');
  try {
    const monaco = await loadMonaco();
    if (codeEditors.editing !== e) return;
    // Reuse the read-only viewer and model so selection and scroll stay intact.
    if (!codeEditors.viewer) {
      codeEditors.viewModel = codeModel(monaco, $('code-fallback').value, file.path);
      codeEditors.viewer = monaco.editor.create($('code-viewer'), {
        ...codeOptions(),
        model: codeEditors.viewModel
      });
    } else if ($('code-fallback').value !== file.content) {
      codeEditors.viewModel.setValue($('code-fallback').value);
    }
    e.model = codeEditors.viewModel;
    e.editor = codeEditors.viewer;
    $('code-viewer').hidden = false;
    e.editor.updateOptions({
      readOnly: false,
      domReadOnly: false,
      ariaLabel: t('文件内容'),
      wordWrap: $('code-wrap').getAttribute('aria-pressed') === 'true' ? 'on' : 'off'
    });
    e.change = e.model.onDidChangeContent(() => invalidateCodePatch(e));
    if (!e.editor.coolderSaveCommand) {
      e.editor.coolderSaveCommand = e.editor.addCommand(
        monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS,
        () => {
          if (codeEditors.editing) $('code-preview').click();
        }
      );
    }
    $('code-fallback').hidden = true;
    $('code-find').disabled = false;
    codeTheme();
    e.editor.focus();
  } catch (error) {
    if (codeEditors.editing === e) notice(new Error(t('代码编辑器加载失败，已使用文本模式')));
  }
});
$('code-fallback').oninput = () => {
  if (codeEditors.editing) invalidateCodePatch(codeEditors.editing);
};
$('code-preview').onclick = task(async () => {
  const e = codeEditors.editing;
  if (!e || e.saving) return;
  const content = codeValue(e);
  const version = (e.previewVersion || 0) + 1;
  e.previewVersion = version;
  invalidateCodePatch(e);
  const collaborative = !!e.project?.owner_id;
  const latest = collaborative ? await projectFileRequest('read', e.file.path, undefined, e.project) : null;
  const result = collaborative
    ? { original_sha256: latest.sha256, patch_id: latest.sha256 }
    : await api('/workspace/patch/preview', { path: e.file.path, content });
  const hash = [
    ...new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(e.original)))
  ]
    .map(b => b.toString(16).padStart(2, '0'))
    .join('');
  if (codeEditors.editing !== e || version !== e.previewVersion || content !== codeValue(e)) return;
  if (result.original_sha256 !== hash)
    throw new Error(t('文件已被其他任务修改，请重新打开后编辑。'));
  e.pendingContent = content;
  e.patch = result.patch_id;
  state.reviewFile = 'manual:' + e.file.path;
  renderReviewFileTabs();
  plainText($('review-title'), e.file.path);
  $('code-review').hidden = false;
  $('changes-pane').hidden = true;
  selectBodyTab('review');
  $('code-save').disabled = false;
  if (e.editor) {
    const monaco = window.monaco;
    if (!e.diff) {
      e.originalModel = codeModel(monaco, e.original, e.file.path);
      $('code-diff-host').hidden = false;
      e.diff = monaco.editor.createDiffEditor($('code-diff-host'), {
        ...codeOptions(),
        readOnly: true,
        originalEditable: false,
        renderSideBySide: true,
        minimap: { enabled: false }
      });
      e.diff.setModel({ original: e.originalModel, modified: e.model });
    }
  } else {
    $('code-diff-fallback').hidden = false;
    plainText($('code-diff-fallback'), result.diff);
  }
  uiText($('code-status'), () => t('差异已生成，确认后保存。'));
});
$('code-back').onclick = () => {
  const e = codeEditors.editing;
  if (!e || e.saving) return;
  selectBodyTab('source');
  requestAnimationFrame(() => {
    e.editor?.layout();
    e.editor?.focus();
  });
};
$('code-save').onclick = task(async () => {
  const e = codeEditors.editing;
  if (!e?.patch || e.saving) return;
  e.saving = true;
  $('code-save').disabled = true;
  $('code-preview').disabled = true;
  e.editor?.updateOptions({ readOnly: true });
  $('code-fallback').readOnly = true;
  try {
    if (e.project?.owner_id) {
      await projectFileRequest('save', e.file.path, { sha256: e.patch, content: e.pendingContent }, e.project);
    } else {
      await api('/workspace/patch/apply', { patch_id: e.patch });
    }
    if (codeEditors.editing !== e) return;
    const updated = await api('/workspace/read?path=' + encodeURIComponent(e.file.path));
    if (codeEditors.editing !== e) return;
    e.original = updated.content;
    disposeCodeEditing();
    state.file = updated;
    plainText($('file-title'), updated.path);
    plainText($('file-content'), updated.content);
    if (!codeEditors.viewer) showCodeFile(updated);
    selectBodyTab('source');
    notice(t('文件已保存'));
  } finally {
    e.saving = false;
    e.patch = '';
    if (codeEditors.editing === e) e.editor?.updateOptions({ readOnly: false });
    $('code-fallback').readOnly = false;
    if (codeEditors.editing === e) {
      $('code-preview').disabled = false;
      invalidateCodePatch(e);
    }
  }
});
$('code-find').onclick = () =>
  codeEditors.editing?.editor?.getAction('editor.action.startFindReplaceAction')?.run();
$('code-wrap').onclick = () => {
  const enabled = $('code-wrap').getAttribute('aria-pressed') !== 'true';
  $('code-wrap').setAttribute('aria-pressed', String(enabled));
  codeEditors.editing?.editor?.updateOptions({ wordWrap: enabled ? 'on' : 'off' });
};
$('close-code').onclick = closeCodeEditing;
window.addEventListener('beforeunload', event => {
  if (anySourceDirty()) {
    event.preventDefault();
    event.returnValue = '';
  }
});

// Keep each file's DOM and Monaco instances alive when switching tabs. This
// preserves undo history, selection, scroll position and unsaved edits.
const sourceFiles = new Map();
const sourceTemplate = $('source-file-panel').cloneNode(true);
const reviewTemplate = $('code-review').cloneNode(true);
const fileHandlers = new Map();
for (const root of [$('source-file-panel'), $('code-review')]) {
  for (const el of root.querySelectorAll('[id]')) {
    fileHandlers.set(el.id, { onclick: el.onclick, oninput: el.oninput });
  }
}
function freshFilePanel(template) {
  const panel = template.cloneNode(true);
  for (const el of panel.querySelectorAll('[id]')) {
    Object.assign(el, fileHandlers.get(el.id) || {});
  }
  return panel;
}
function rememberSourceFile() {
  const file = state.file;
  if (!file || !sourceFiles.has(file.path)) return;
  Object.assign(sourceFiles.get(file.path), {
    file,
    viewer: codeEditors.viewer,
    viewModel: codeEditors.viewModel,
    editing: codeEditors.editing,
    source: $('source-file-panel'),
    review: $('code-review')
  });
}
function pinSourceFile(path) {
  const item = sourceFiles.get(path);
  if (!item) return;
  item.preview = false;
  renderSourceFileTabs();
}
function createSourceFile(file, preview = false) {
  rememberSourceFile();
  if (preview) {
    for (const [path, existing] of sourceFiles) {
      if (!existing.preview) continue;
      if (existing.editing) {
        existing.preview = false;
        continue;
      }
      sourceFiles.delete(path);
      disposeSourceFile(existing);
    }
  }
  const item = {
    file,
    preview,
    viewer: null,
    viewModel: null,
    editing: null,
    source: freshFilePanel(sourceTemplate),
    review: freshFilePanel(reviewTemplate)
  };
  sourceFiles.set(file.path, item);
  mountSourceFile(item);
}
function mountSourceFile(item) {
  ++codeEditors.sequence;
  $('source-file-panel').replaceWith(item.source);
  $('code-review').replaceWith(item.review);
  state.file = item.file;
  uiText($('source-tab'), () => t('源文件'));
  codeEditors.viewer = item.viewer;
  codeEditors.viewModel = item.viewModel;
  codeEditors.editing = item.editing;
  uiText($('code-mode'), () => t(codeEditors.editing ? '编辑中' : '只读'));
  for (const entry of $('files').querySelectorAll('[role="treeitem"]')) {
    entry.setAttribute('aria-selected', String(entry.dataset.path === item.file?.path));
  }
  renderSourceFileTabs();
  for (const root of [item.source, item.review]) {
    for (const el of root.querySelectorAll('[data-i18n]')) el.textContent = t(el.dataset.i18n);
  }
  codeTheme();
  requestAnimationFrame(() => {
    codeEditors.viewer?.layout();
    codeEditors.editing?.diff?.layout();
  });
}
function activateSourceFile(path) {
  ++state.fileSelection;
  if (state.file?.path === path) return true;
  if (codeEditors.editing?.saving) return false;
  const item = sourceFiles.get(path);
  if (!item) return false;
  rememberSourceFile();
  mountSourceFile(item);
  if (!item.viewer && !item.editing) showCodeFile(item.file);
  renderReviewFileTabs();
  return true;
}
function anySourceDirty() {
  rememberSourceFile();
  return [...sourceFiles.values()].some(item => {
    const e = item.editing;
    return (
      e &&
      (e.model
        ? e.model.getValue(undefined, true)
        : item.source.querySelector('#code-fallback').value) !== e.original
    );
  });
}
function disposeSourceFile(item) {
  const e = item.editing;
  e?.change?.dispose();
  e?.diff?.dispose();
  e?.originalModel?.dispose();
  item.viewer?.dispose();
  item.viewModel?.dispose();
}
function closeSourceFile(path) {
  return closeSourceFiles([path]);
}
function closeAllSourceFiles(force = false) {
  return closeSourceFiles([...sourceFiles.keys()], force);
}
function closeSourceFiles(paths, force = false) {
  rememberSourceFile();
  const items = paths.map(path => sourceFiles.get(path)).filter(Boolean);
  if (!force && (codeEditors.editing?.saving || items.some(item => item.editing?.saving)))
    return false;
  const dirty = items.some(item => {
    const e = item.editing;
    return (
      e &&
      (e.model
        ? e.model.getValue(undefined, true)
        : item.source.querySelector('#code-fallback').value) !== e.original
    );
  });
  // Confirm once before disposing anything, so cancellation leaves every tab intact.
  if (!force && dirty && !confirm(t('存在未保存的修改，确定关闭？'))) return false;
  ++state.fileSelection;
  for (const item of items) {
    sourceFiles.delete(item.file.path);
    disposeSourceFile(item);
  }
  if (!state.file || !sourceFiles.has(state.file.path)) {
    const next = [...sourceFiles.values()].pop();
    mountSourceFile(
      next || {
        file: null,
        viewer: null,
        viewModel: null,
        editing: null,
        source: freshFilePanel(sourceTemplate),
        review: freshFilePanel(reviewTemplate)
      }
    );
  }
  renderSourceFileTabs();
  renderChanges(state.snapshot || { changes: [] });
  return true;
}
function closeReviewFiles(keys) {
  rememberSourceFile();
  const manual = [...sourceFiles.values()].filter(item =>
    keys.includes('manual:' + item.file.path)
  );
  if (manual.some(item => item.editing?.saving)) return false;
  for (const c of state.snapshot?.changes || []) {
    const key = diffViewKey(c);
    if (keys.includes(key)) state.closedDiffs.add(key);
  }
  for (const item of manual) if (item.editing) item.editing.patch = '';
  renderChanges(state.snapshot || { changes: [] });
  return true;
}
function reviewFileKeys() {
  rememberSourceFile();
  return [
    ...(state.snapshot?.changes || []).map(diffViewKey).filter(key => !state.closedDiffs.has(key)),
    ...[...sourceFiles.values()]
      .filter(item => item.editing?.patch)
      .map(item => 'manual:' + item.file.path)
  ];
}
let fileTabMenu = null;
function hideFileTabMenu(restoreFocus = false) {
  if (!fileTabMenu) return;
  const { menu, trigger, dismiss, keydown } = fileTabMenu;
  fileTabMenu = null;
  document.removeEventListener('pointerdown', dismiss, true);
  document.removeEventListener('keydown', keydown, true);
  window.removeEventListener('resize', dismiss);
  document.removeEventListener('scroll', dismiss, true);
  menu.remove();
  if (restoreFocus && trigger.isConnected) trigger.focus();
}
function showFileTabMenu(event, trigger, kind, key) {
  event.preventDefault();
  hideFileTabMenu();
  const epoch = state.epoch;
  const keys = kind === 'source' ? [...sourceFiles.keys()] : reviewFileKeys();
  const close = kind === 'source' ? closeSourceFiles : closeReviewFiles;
  const menu = node('div', undefined, 'file-tab-menu');
  menu.setAttribute('role', 'menu');
  menu.setAttribute('aria-label', t('文件标签菜单'));
  const entries = [
    ['关闭所有', keys],
    ['关闭其它', keys.filter(value => value !== key)],
    ['关闭', [key]]
  ];
  const buttons = entries.map(([label, targets]) => {
    const item = button(
      () => t(label),
      () => {
        hideFileTabMenu();
        if (epoch === state.epoch) close(targets);
      }
    );
    item.setAttribute('role', 'menuitem');
    item.disabled = !targets.length;
    menu.append(item);
    return item;
  });
  const dismiss = e => {
    if (e.type === 'pointerdown' && menu.contains(e.target)) return;
    hideFileTabMenu();
  };
  const keydown = e => {
    if (e.key === 'Escape' || e.key === 'Tab') {
      if (e.key === 'Escape') e.preventDefault();
      hideFileTabMenu(true);
      return;
    }
    if (!['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(e.key)) return;
    e.preventDefault();
    const enabled = buttons.filter(item => !item.disabled);
    const index = enabled.indexOf(document.activeElement);
    const next =
      e.key === 'Home'
        ? 0
        : e.key === 'End'
          ? enabled.length - 1
          : (index + (e.key === 'ArrowUp' ? -1 : 1) + enabled.length) % enabled.length;
    enabled[next]?.focus();
  };
  document.body.append(menu);
  const bounds = trigger.getBoundingClientRect();
  const keyboard = event.type === 'keydown';
  const x = keyboard ? bounds.left : event.clientX;
  const y = keyboard ? bounds.bottom : event.clientY;
  menu.style.left = Math.max(4, Math.min(x, window.innerWidth - menu.offsetWidth - 4)) + 'px';
  menu.style.top = Math.max(4, Math.min(y, window.innerHeight - menu.offsetHeight - 4)) + 'px';
  fileTabMenu = { menu, trigger, dismiss, keydown };
  document.addEventListener('pointerdown', dismiss, true);
  document.addEventListener('keydown', keydown, true);
  window.addEventListener('resize', dismiss);
  document.addEventListener('scroll', dismiss, true);
  buttons[0].focus();
}
function fileTab(bar, path, active, choose, close, suffix = '', key = path) {
  const item = node('div', undefined, 'file-tab-item');
  const tab = button(path.split('/').pop() + suffix, choose);
  tab.title = path;
  tab.setAttribute('role', 'tab');
  tab.setAttribute('aria-selected', String(active));
  const kind = bar.id === 'source-file-tabs' ? 'source' : 'review';
  item.oncontextmenu = event => showFileTabMenu(event, tab, kind, key);
  tab.onkeydown = event => {
    if (event.key === 'ContextMenu' || (event.shiftKey && event.key === 'F10')) {
      showFileTabMenu(event, tab, kind, key);
    }
  };
  item.append(tab);
  if (close) {
    const end = button('×', close);
    end.title = t('关闭') + ' ' + path;
    end.setAttribute('aria-label', end.title);
    item.append(end);
  }
  bar.append(item);
  return tab;
}
function renderSourceFileTabs() {
  const bar = $('source-file-tabs');
  bar.replaceChildren();
  for (const [path, item] of sourceFiles) {
    const e = state.file?.path === path ? codeEditors.editing : item.editing;
    const tab = fileTab(
      bar,
      path,
      state.file?.path === path,
      () => activateSourceFile(path),
      () => closeSourceFile(path),
      e ? ' •' : ''
    );
    tab.classList.toggle('preview-tab', !!item.preview);
    tab.ondblclick = () => pinSourceFile(path);
  }
}
function renderReviewFileTabs(snapshot = state.snapshot || { changes: [] }) {
  rememberSourceFile();
  const bar = $('review-file-tabs');
  bar.replaceChildren();
  const changes = (snapshot.changes || []).filter(c => !state.closedDiffs.has(diffViewKey(c)));
  const manual = [...sourceFiles.values()].filter(item => item.editing?.patch);
  const keys = [...changes.map(diffViewKey), ...manual.map(item => 'manual:' + item.file.path)];
  if (!keys.includes(state.reviewFile)) state.reviewFile = keys[0] || null;
  for (const c of changes) {
    const key = diffViewKey(c);
    fileTab(
      bar,
      c.path,
      state.reviewFile === key,
      () => {
        state.reviewFile = key;
        renderChanges(state.snapshot || { changes: [] });
      },
      canCloseDiff(c)
        ? () => {
            state.closedDiffs.add(key);
            renderChanges(state.snapshot || { changes: [] });
          }
        : null,
      c.review_status === 'pending' ? ' •' : '',
      key
    );
  }
  for (const item of manual) {
    const key = 'manual:' + item.file.path;
    fileTab(
      bar,
      item.file.path,
      state.reviewFile === key,
      () => {
        const previous = state.reviewFile;
        state.reviewFile = key;
        if (!activateSourceFile(item.file.path)) {
          state.reviewFile = previous;
          return;
        }
        renderReviewFileTabs();
      },
      () => {
        if (item.editing.saving) return;
        item.editing.patch = '';
        renderReviewFileTabs();
      },
      ' · ' + t('编辑中'),
      key
    );
  }
  const isManual = state.reviewFile?.startsWith('manual:');
  if (isManual && !$('review-pane').hidden && state.file?.path !== state.reviewFile.slice(7)) {
    activateSourceFile(state.reviewFile.slice(7));
  }
  $('code-review').hidden = !isManual || state.file?.path !== state.reviewFile.slice(7);
  $('changes-pane').hidden = !!isManual;
  requestAnimationFrame(() => codeEditors.editing?.diff?.layout());
}
