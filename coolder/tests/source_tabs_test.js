'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../html/code-editor.js'), 'utf8');
const elements = new Map();
function panel(id, value = '') {
  const fallback = { value };
  return {
    id,
    querySelector: () => fallback,
    querySelectorAll: () => [],
    replaceWith(next) {
      elements.set(id, next);
    }
  };
}
let disposed = 0;
function file(name, dirty, fallback = false) {
  const original = 'original-' + name;
  const value = dirty ? 'draft-' + name : original;
  const model = { getValue: () => value, dispose: () => disposed++ };
  return {
    file: { path: name, content: original },
    source: panel('source-file-panel', value),
    review: panel('code-review'),
    viewer: { dispose: () => disposed++, layout() {} },
    viewModel: model,
    editing: {
      original,
      model: fallback ? null : model,
      file: { path: name },
      patch: name + '-patch'
    }
  };
}
const first = file('first.js', true),
  second = file('second.js', true, true);
const sourceFiles = new Map([
  ['first.js', first],
  ['second.js', second]
]);
const state = { file: first.file };
const codeEditors = {
  viewer: first.viewer,
  viewModel: first.viewModel,
  editing: first.editing,
  sequence: 0
};
elements.set('source-file-panel', first.source);
elements.set('code-review', first.review);
let confirms = 0,
  allowClose = false;
const context = vm.createContext({
  state,
  codeEditors,
  sourceFiles,
  $: id => elements.get(id) || { querySelectorAll: () => [] },
  t: x => x,
  uiText() {},
  codeTheme() {},
  requestAnimationFrame() {},
  renderSourceFileTabs() {},
  renderReviewFileTabs() {},
  renderChanges() {},
  showCodeFile() {},
  sourceTemplate: {},
  reviewTemplate: {},
  freshFilePanel: () => panel('source-file-panel'),
  confirm: () => {
    confirms++;
    return allowClose;
  }
});
vm.runInContext(
  source.slice(source.indexOf('function rememberSourceFile('), source.indexOf('function fileTab(')),
  context
);
assert.equal(context.activateSourceFile('second.js'), true);
assert.equal(codeEditors.editing, second.editing);
assert.equal(first.viewModel.getValue(), 'draft-first.js');
assert.equal(context.anySourceDirty(), true);
assert.equal(context.activateSourceFile('first.js'), true);
assert.equal(codeEditors.editing, first.editing);
assert.equal(codeEditors.viewModel, first.viewModel); // Keep the original undo model.
assert.equal(elements.get('source-file-panel'), first.source);
context.closeSourceFile('second.js'); // Inactive fallback drafts also need confirmation.
assert.equal(confirms, 1);
assert.equal(sourceFiles.size, 2);
assert.equal(context.closeAllSourceFiles(), false);
assert.equal(sourceFiles.size, 2);
allowClose = true;
context.closeSourceFile('first.js');
assert.equal(state.file.path, 'second.js');
assert.equal(sourceFiles.size, 1);
assert.equal(disposed, 2);
second.editing.saving = true;
assert.equal(context.closeAllSourceFiles(), false);
second.editing.saving = false;
assert.equal(context.closeAllSourceFiles(), true);
assert.equal(sourceFiles.size, 0);
assert.equal(state.file, null);
assert.equal(disposed, 4);
console.log(
  'Source tabs passed: independent drafts/models, fallback drafts, close confirmation, adjacent selection, saving guard and cleanup.'
);

const keep = file('keep.js', true),
  clean = file('clean.js', false),
  dirty = file('dirty.js', true);
sourceFiles.set('keep.js', keep);
sourceFiles.set('clean.js', clean);
sourceFiles.set('dirty.js', dirty);
context.mountSourceFile(keep);
allowClose = false;
const before = confirms;
assert.equal(context.closeSourceFiles(['clean.js']), true);
assert.equal(confirms, before); // Dirty retained tab does not trigger confirmation.
assert.equal(context.closeSourceFiles(['keep.js', 'dirty.js']), false);
assert.equal(sourceFiles.size, 2); // Batch cancellation is atomic.
allowClose = true;
assert.equal(context.closeSourceFiles(['dirty.js']), true);
assert.equal(state.file.path, 'keep.js');
assert.equal(sourceFiles.size, 1);
assert.equal(keep.viewModel.getValue(), 'draft-keep.js');
console.log('Batch close preserves the excluded draft and cancels atomically.');

context.closeAllSourceFiles(true);
context.createSourceFile({ path: 'preview-a.js', content: 'a' }, true);
context.createSourceFile({ path: 'preview-b.js', content: 'b' }, true);
assert.deepEqual([...sourceFiles.keys()], ['preview-b.js']);
assert.equal(state.file.path, 'preview-b.js');
context.pinSourceFile('preview-b.js');
context.createSourceFile({ path: 'preview-c.js', content: 'c' }, true);
assert.deepEqual([...sourceFiles.keys()], ['preview-b.js', 'preview-c.js']);
context.createSourceFile({ path: 'fixed-d.js', content: 'd' }, false);
context.createSourceFile({ path: 'preview-e.js', content: 'e' }, true);
assert.deepEqual([...sourceFiles.keys()], ['preview-b.js', 'fixed-d.js', 'preview-e.js']);
codeEditors.editing = { file: state.file, original: 'e', model: { getValue: () => 'draft e' } };
context.createSourceFile({ path: 'preview-f.js', content: 'f' }, true);
assert.ok(sourceFiles.has('preview-e.js'));
assert.equal(sourceFiles.get('preview-e.js').preview, false);
assert.equal(sourceFiles.get('preview-e.js').editing.model.getValue(), 'draft e');
assert.equal([...sourceFiles.values()].filter(item => item.preview).length, 1);
console.log(
  'Preview tabs passed: replace single-click preview, keep pinned files, preserve edited drafts.'
);
