'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../html/app.js'), 'utf8');
let count = 0,
  now = 1000;
const listeners = {};
const controls = {
  prompt: { addEventListener: (name, callback) => (listeners[name] = callback) },
  'enter-to-send': { checked: true },
  send: { disabled: false },
  'prompt-form': {
    requestSubmit: submitter => {
      assert.equal(submitter, controls.send);
      count++;
    }
  }
};
const state = { submitting: false };
vm.runInNewContext(
  source.slice(
    source.indexOf('let promptComposing ='),
    source.indexOf(
      "document.addEventListener('pointerdown', e => {",
      source.indexOf('let promptComposing =')
    )
  ),
  {
    $: id => controls[id],
    state,
    Date: { now: () => now }
  }
);
function press(overrides = {}) {
  let prevented = false;
  controls.prompt.onkeydown({
    key: 'Enter',
    preventDefault: () => (prevented = true),
    ...overrides
  });
  return prevented;
}
assert.equal(press(), true);
assert.equal(count, 1);
controls['enter-to-send'].checked = false;
assert.equal(press(), false);
assert.equal(press({ ctrlKey: true }), false);
assert.equal(count, 1);
controls['enter-to-send'].checked = true;
assert.equal(press({ shiftKey: true }), false);
assert.equal(press({ isComposing: true }), false);
assert.equal(press({ keyCode: 229 }), false);
listeners.compositionstart();
assert.equal(press(), false);
listeners.compositionend();
assert.equal(press(), false);
now += 100;
assert.equal(press(), true);
assert.equal(count, 2);
press({ repeat: true });
controls.send.disabled = true;
press();
controls.send.disabled = false;
state.submitting = true;
press();
assert.equal(count, 2);
console.log(
  'Send keys passed: Enter, newline mode, Shift+Enter, IME, repeated keys and submission guards.'
);
