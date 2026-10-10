'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../html/app.js'), 'utf8');
const context = vm.createContext({ t: value => value });
vm.runInContext(
  source.slice(
    source.indexOf('function assistantDisplayText('),
    source.indexOf('function message(')
  ),
  context
);
const show = context.assistantDisplayText;
assert.equal(show('已完成，运行 npm test。'), '已完成，运行 npm test。');
const final = {
  type: 'final',
  text: '已新增登录页面。',
  completion_summary: '任务完成',
  changes: [{ path: 'x', content: 'hidden source' }],
  usage: { tokens: 10 }
};
assert.equal(show(JSON.stringify(final)), '任务完成\n\n已新增登录页面。');
assert.equal(show('```json\n' + JSON.stringify(final) + '\n```'), show(JSON.stringify(final)));
assert.equal(show(JSON.stringify(JSON.stringify(final))), show(JSON.stringify(final)));
assert.equal(
  show({
    choices: [{ message: { content: JSON.stringify(final) }, finish_reason: 'stop' }],
    usage: { tokens: 1 }
  }),
  show(JSON.stringify(final))
);
assert.equal(
  show({
    type: 'message',
    content: [
      { type: 'thinking', thinking: 'hidden' },
      { type: 'text', text: '可读回答' }
    ]
  }),
  '可读回答'
);
assert.equal(
  show({
    object: 'response',
    output: [
      { type: 'reasoning', summary: 'hidden' },
      { type: 'message', content: [{ type: 'output_text', text: '响应正文' }] }
    ]
  }),
  '响应正文'
);
assert.equal(show({ error: { message: '服务暂时不可用', code: 503 } }), '服务暂时不可用');
assert.equal(
  show({ type: 'final', text: '完成', completion_summary: '完成', next_steps: ['运行测试'] }),
  '完成\n\n运行测试'
);
for (const example of [
  '{"name":"demo","version":1}',
  '这里是示例：\n```json\n{"type":"final","text":"example"}\n```'
])
  assert.equal(show(example), example);
for (const partial of [
  '{"type":"final","text":"生成中',
  '{"choices":[{"delta":{"content":"hello'
]) {
  assert.equal(show(partial, true), partial.includes('生成中') ? '生成中' : '正在整理模型回复…');
  assert.ok(!show(partial).includes('{'));
}
assert.ok(!show({ type: 'tool_call', arguments: { secret: 'hidden' } }).includes('secret'));
assert.equal(
  show(JSON.stringify({ type: 'final', text: '<script>alert(1)</script>' })),
  '<script>alert(1)</script>'
);
console.log('chat display: all regression cases passed');

assert.equal(show('```json\n{"type":"final","text":"partial', true), 'partial');
assert.equal(show('{"id":"abc","object":"chat.completion","choices":[', true), '正在整理模型回复…');
assert.equal(show('{"', true), '正在整理模型回复…');

// Runtime validation messages can precede the model's JSON, including mid-stream.
const prefix = '构建失败已确认为环境依赖问题，与站点改动无关。';
const wrapped = prefix + '\n\n' + JSON.stringify(final) + '\n验证尚未执行。';
assert.equal(show(wrapped), prefix + '\n\n任务完成\n\n已新增登录页面。\n\n验证尚未执行。');
assert.equal(show(prefix + '\n{"type":"final","text":"第一行\\n第二行', true), prefix + '\n\n第一行\n第二行');
assert.equal(show('{"type":"final","text":"已完成","changes":[{"content":"hidden', true), '已完成');
assert.equal(show('{"type":"final","text":"文本\\u4e', true), '文本');
assert.equal(show('{"type":"final","text":"文本\\', true), '文本');
assert.equal(show({text:'可读正文',usage:{tokens:20}}), '可读正文');
assert.equal(show({summary:'概要',metadata:{id:20}}), '概要');
const tool = {type:'tool_call',arguments:{content:JSON.stringify(final)}};
assert.ok(!show(JSON.stringify(tool)).includes('已新增登录页面'));
assert.ok(!show('{"type":"tool_call","arguments":{"text":"secret',true).includes('secret'));
assert.equal(show(JSON.stringify({type:'final',text:'大括号 } 和引号 \" 保留'})), '大括号 } 和引号 \" 保留');
console.log('Mixed prose and partial JSON display cases passed.');

// Every streaming boundary of a mixed response must avoid raw protocol bytes.
const wire = JSON.stringify(final);
for (let length = 1; length <= wire.length; length++) {
  const displayed = show(prefix + '\n' + wire.slice(0, length), true);
  assert.ok(!displayed.includes('{"type"'), displayed);
  assert.ok(!displayed.includes('hidden source'), displayed);
}
