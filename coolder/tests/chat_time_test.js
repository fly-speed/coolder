'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../html/app.js'), 'utf8');
const i18n = { language: 'zh' };
const context = vm.createContext({ i18n });
vm.runInContext(
  source.slice(source.indexOf('function messageTime('), source.indexOf('function message(')),
  context
);
const seconds = 1791262800;
assert.equal(context.messageTime(seconds).getTime(), seconds * 1000);
assert.equal(context.messageTime(seconds * 1000).getTime(), seconds * 1000);
assert.equal(context.messageTime(String(seconds)).getTime(), seconds * 1000);
assert.equal(context.messageTime('2026-10-06T05:00:00Z').getTime(), seconds * 1000);
for (const value of [undefined, null, '', 0, -1, 'invalid', Infinity])
  assert.equal(context.messageTime(value), null);
for (const language of ['zh', 'en', 'ja', 'ko']) {
  i18n.language = language;
  assert.match(context.formatMessageTime(context.messageTime(seconds)), /2026/);
}
console.log(
  'Message timestamps: seconds, milliseconds, ISO, missing/invalid history and four languages passed.'
);
