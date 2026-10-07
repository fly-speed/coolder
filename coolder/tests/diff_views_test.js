'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../html/app.js'), 'utf8');
class Element {
  constructor(tag, text, cls) {
    this.tag = tag;
    this.text = typeof text === 'function' ? text() : text;
    this.className = cls;
    this.children = [];
  }
  setAttribute(name, value) {
    this[name] = value;
  }
  append(...children) {
    this.children.push(...children);
  }
  replaceChildren(...children) {
    this.children = children;
  }
}
const elements = new Map();
const state = { run: 'run-1', snapshot: null, closedDiffs: new Set() };
let requests = 0;
const context = vm.createContext({
  state,
  sourceFiles: new Map(),
  codeEditors: {},
  rememberSourceFile: () => {},
  requestAnimationFrame: () => {},
  $: id => {
    if (!elements.has(id)) elements.set(id, new Element('div'));
    return elements.get(id);
  },
  t: text => text,
  uiText: (el, text) => (el.text = text()),
  node: (tag, text, cls) => new Element(tag, text, cls),
  button: (text, action) => Object.assign(new Element('button', text), { action }),
  api: () => {
    requests++;
    throw new Error('Closing diffs must not call the server');
  }
});
vm.runInContext(
  source.slice(source.indexOf('function canCloseDiff('), source.indexOf('async function poll(')),
  context
);
const editor = fs.readFileSync(path.join(__dirname, '../html/code-editor.js'), 'utf8');
vm.runInContext(editor.slice(editor.indexOf('function closeReviewFiles(')), context);
function tabs() {
  return all(elements.get('review-file-tabs')).filter(el => el.role === 'tab');
}
const change = (path, status) => ({
  path,
  generation: 1,
  draft_hash: path + '-hash',
  review_status: status,
  diff: '+ source'
});
const snapshot = {
  status: 'completed',
  changes: [change('a.js', 'accepted'), change('b.js', 'rejected'), change('c.js', 'pending')]
};
const original = JSON.stringify(snapshot);
function render(s = snapshot) {
  state.snapshot = s;
  context.renderChanges(s);
}
function all(el = elements.get('changes')) {
  return [el, ...el.children.flatMap(all)];
}
function cards() {
  return all().filter(el => el.className === 'change');
}
function click(label, root) {
  const btn = all(root).find(el => el.tag === 'button' && el.text === label);
  assert.ok(btn, label);
  assert.ok(!btn.disabled, label);
  return btn.action();
}
render();
assert.equal(cards().length, 1);
assert.equal(tabs().length, 3);
assert.equal(all().filter(el => el.text === '关闭差异').length, 1);
click('b.js', elements.get('review-file-tabs'));
assert.ok(all(cards()[0]).some(el => String(el.text).includes('b.js')));
click('a.js', elements.get('review-file-tabs'));
click('关闭差异', cards()[0]);
assert.equal(cards().length, 1);
assert.equal(tabs().length, 2);
render(JSON.parse(original)); // Repeated polling must keep this version hidden.
assert.equal(cards().length, 1);
assert.equal(tabs().length, 2);
click('关闭全部已审核差异');
assert.equal(cards().length, 1);
assert.ok(all(cards()[0]).some(el => String(el.text).includes('c.js')));
assert.ok(!all(cards()[0]).some(el => el.text === '关闭差异'));
click('显示已关闭差异 (2)');
assert.equal(cards().length, 1);
assert.equal(tabs().length, 3);
click('关闭全部已审核差异');
const next = JSON.parse(original);
next.changes[0].generation++;
render(next);
assert.equal(cards().length, 1);
assert.equal(tabs().length, 2); // New file generation must appear.
next.changes[1].draft_hash = 'new-hash';
render(next);
assert.equal(cards().length, 1);
assert.equal(tabs().length, 3);
state.run = 'run-2';
render();
assert.equal(cards().length, 1);
assert.equal(tabs().length, 3); // Same paths in another run stay visible.
state.run = 'run-1';
render({ status: 'completed', changes: snapshot.changes.slice(0, 2) });
assert.equal(cards().length, 0);
assert.equal(tabs().length, 0);
assert.ok(all().some(el => el.text === '差异视图已关闭，修改及审核状态仍保留。'));
assert.equal(requests, 0);
assert.equal(JSON.stringify(snapshot), original);
console.log(
  'Diff view tests passed: individual/batch close, restore, polling, pending protection, version/run isolation, no server writes.'
);

state.closedDiffs.clear();
render();
context.closeReviewFiles([context.diffViewKey(snapshot.changes[2])]);
assert.equal(tabs().length, 2);
assert.equal(snapshot.changes[2].review_status, 'pending');
render();
assert.equal(tabs().length, 2); // Pending views also stay hidden across polls.
click('显示已关闭差异 (1)');
assert.equal(tabs().length, 3);
context.closeReviewFiles(context.reviewFileKeys());
assert.equal(tabs().length, 0);
assert.equal(requests, 0);
assert.equal(JSON.stringify(snapshot), original);
console.log('Context close preserves pending review status and supports restoring all diffs.');
