'use strict';
let vimRuntime;
function loadVimEditor() {
  if (vimRuntime) return vimRuntime;
  vimRuntime = loadMonaco().then(() => new Promise((resolve, reject) => {
    window.require.config({ paths: { 'coolder-monaco-vim': '/vendor/monaco-vim/monaco-vim.umd' } });
    window.require(['coolder-monaco-vim'], runtime => {
      if (!runtime?.initVimMode) { reject(new Error('Vim editor unavailable')); return; }
      const active = cm => codeEditors.editing?.editor === cm.editor;
      runtime.VimMode.Vim.defineEx('write', 'w', cm => {
        if (active(cm) && !codeEditors.editing.saving) $('code-preview').click();
      });
      runtime.VimMode.Vim.defineEx('quit', 'q', cm => {
        if (active(cm)) closeCodeEditing();
      });
      resolve(runtime);
    }, reject);
  })).catch(error => { vimRuntime = null; throw error; });
  return vimRuntime;
}
// Source panels are cloned for each file tab; keep the handler on their parent.
$('source-pane').addEventListener('click', event => {
  if (!event.target.closest('#code-vim')) return;
  return task(async event => {
  const button = event.target.closest('#code-vim');
  if (!button) return;
  const editing = codeEditors.editing;
  if (!editing?.editor || editing.saving) return;
  const panel = button.closest('#source-file-panel');
  const status = panel.querySelector('#code-vim-status');
  button.disabled = true;
  try {
    if (editing.vim) {
      editing.vim.dispose();
      editing.vim = null;
      status.hidden = true;
      button.setAttribute('aria-pressed', 'false');
    } else {
      const runtime = await loadVimEditor();
      if (codeEditors.editing !== editing || !panel.isConnected) return;
      status.hidden = false;
      editing.vim = runtime.initVimMode(editing.editor, status);
      button.setAttribute('aria-pressed', 'true');
    }
    editing.editor.layout();
    editing.editor.focus();
  } finally { button.disabled = false; }
  })(event);
});
