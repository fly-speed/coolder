'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../html/app.js'), 'utf8');
class Element {
  constructor(text) {
    this.text = typeof text === 'function' ? text() : text;
    this.children = [];
    this.isConnected = true;
    this.classList = { toggle() {} };
  }
  append(...items) {
    for (const item of items) {
      item.parent = this;
      this.children.push(item);
    }
  }
  setAttribute() {}
  get firstElementChild() {
    return this.children[0];
  }
  remove() {
    this.parent.children.splice(this.parent.children.indexOf(this), 1);
  }
}
const messages = new Element();
const state = { run: 'first' };
const ctx = vm.createContext({
  state,
  $: () => messages,
  t: s => s,
  node: (_tag, text) => new Element(text)
});
vm.runInContext(
  source.slice(source.indexOf('function runProgressStage('), source.indexOf('function renderRun(')),
  ctx
);
const show = ctx.renderRunProgress;
show({ status: 'running', phase: 'model_call' });
show({ status: 'running', phase: 'model_stream' });
assert.equal(state.progressView.list.children.length, 1);
show({ status: 'running', phase: 'tool_call', current_tool: 'workspace.validate' });
assert.equal(state.progressView.list.children.at(-1).children[0].text, '正在运行项目验证');
show({ status: 'running', phase: 'model_call', completed_tool_calls: 1 });
const length = state.progressView.list.children.length;
show({ status: 'running', phase: 'model_call', completed_tool_calls: 1 });
assert.equal(state.progressView.list.children.length, length);
show({ status: 'completed', changes: [{ review_status: 'pending' }] });
assert.equal(state.progressView.list.children.at(-1).children[0].text, '成果已生成，等待确认写入');
show({ status: 'failed', error: '{"secret":"never render raw payload"}' });
assert.equal(
  state.progressView.list.children.at(-1).children[0].text,
  '任务失败，请查看回复中的原因'
);
state.run = 'second';
show({ status: 'running', phase: 'queued' });
assert.equal(messages.children.length, 2);
for (let i = 1; i < 100; i++)
  show({ status: 'running', phase: 'model_call', completed_tool_calls: i });
assert.equal(state.progressView.list.children.length, 60);
assert.equal(ctx.runProgressStage({ status: 'running', phase: 'model_retry' })[0], 'retry');
assert.equal(ctx.runProgressStage({ status: 'running', phase: 'paused' })[0], 'paused');
console.log(
  'Run progress passed: stage mapping, repeated polls, pending review, safe errors, separate runs and bounded history.'
);
