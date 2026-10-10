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
  source.slice(source.indexOf('function usageText('), source.indexOf('function renderRun(')),
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

state.run = 'usage-run';
const events = [{event:'model_request_started'}, {event:'model_response_completed',input_tokens:1234,output_tokens:56,latency_ms:2500}];
const snapshot = {status:'running', phase:'model_call', model_interactions_json:JSON.stringify(events)};
show(snapshot);
const usageRows = () => state.progressView.list.children.filter(row => row.children[0].text === 'AI 交互消耗');
assert.equal(usageRows().length, 1);
assert.match(usageRows()[0].children[1].text, /第 1 次交互/);
assert.match(usageRows()[0].children[1].text, /输入 tokens: 1,234/);
assert.match(usageRows()[0].children[1].text, /输出 tokens: 56/);
assert.match(usageRows()[0].children[1].text, /2.50 秒/);
show(snapshot);
assert.equal(usageRows().length, 1);
events.push({event:'model_request_started'}, {event:'model_request_failed',input_tokens:20,output_tokens:0,latency_ms:100});
show({...snapshot,model_interactions_json:JSON.stringify(events)});
assert.equal(usageRows().length, 2);
assert.match(usageRows()[0].children[1].text, /第 1 次交互/);
assert.match(usageRows()[1].children[1].text, /第 2 次交互/);
const footer = ctx.usageFooter({input_tokens:1254,output_tokens:56,latency_ms:2600,started_at:100,finished_at:108});
assert.match(footer.text, /本次会话消耗/);
assert.match(footer.text, /总耗时: 8.00 秒/);
assert.match(footer.text, /AI 请求耗时: 2.60 秒/);
assert.match(ctx.usageText({input_tokens:0,output_tokens:0,latency_ms:0}), /输入 tokens: 0/);
assert.match(ctx.usageText({}), /未提供/);
console.log('Usage display passed: per-interaction metrics, no poll duplicates, stable ordinals and terminal totals.');

const cached = ctx.usageText({input_tokens:100,cached_input_tokens:70,output_tokens:20,latency_ms:100});
assert.match(cached, /缓存命中输入 tokens: 70/);
assert.match(cached, /缓存未命中输入 tokens: 30/);
const zeroCache = ctx.usageText({input_tokens:100,cached_input_tokens:0,cache_usage_available:true});
assert.match(zeroCache, /缓存命中输入 tokens: 0/);
assert.match(zeroCache, /缓存未命中输入 tokens: 100/);
assert.ok(!ctx.usageText({input_tokens:100,cached_input_tokens:0}).includes('缓存命中'));
assert.ok(!ctx.usageText({input_tokens:100}).includes('缓存命中'));
assert.match(ctx.usageText({input_tokens:10,cached_input_tokens:20}), /缓存未命中输入 tokens: 0/);
console.log('Cache display passed: hits, misses, explicit zero and absent cache details.');
