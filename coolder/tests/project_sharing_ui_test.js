'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../html/app.js'), 'utf8');
const calls = [];
const owner = { id: 'p1', owner_id: 'alice', project_path: 'sample', shared: false };
const participant = { id: 'p2', owner_id: 'bob', project_path: 'sample', shared: true };
let resolveRead;
const context = {
  URLSearchParams, state: { project: participant }, apiRoot: '/api/v1/ai', t: s => s,
  request: async (url, body) => {
    calls.push({ url, body });
    if (url.includes('/read?')) return new Promise(resolve => { resolveRead = resolve; });
    return { entries: [{ path: 'README.md', directory: false }] };
  }
};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('function projectFileRequest('), source.indexOf('\nfunction button(')), context);
(async () => {
  const listing = await context.api('/workspace/list?path=sample');
  assert.equal(listing.entries[0].path, 'sample/README.md');
  assert.match(calls[0].url, /owner_id=bob&project_id=p2&path=$/);
  await assert.rejects(context.api('/workspace/read?path=sample-other/secret'), /当前项目/);
  await assert.rejects(context.api('/runs/start', {}), /拥有者/);
  await assert.rejects(context.api('/workspace/patch/apply', {}), /拥有者/);
  const reading = context.api('/workspace/read?path=sample%2FREADME.md');
  context.state.project = owner;
  resolveRead({ path: 'README.md', content: 'shared content' });
  assert.equal((await reading).path, 'sample/README.md');
  await context.projectFileRequest('save', 'sample/README.md', { sha256: 'baseline', content: 'edit' }, participant);
  assert.deepEqual(JSON.parse(JSON.stringify(calls.at(-1).body)), {
    sha256: 'baseline', content: 'edit', owner_id: 'bob', project_id: 'p2', path: 'README.md'
  });
  await context.api('/runs/start', { project_id: 'p1' });
  assert.equal(calls.at(-1).url, '/api/v1/ai/runs/start');
  console.log('Project sharing UI routing passed: project identity, path boundaries, captured edit target and shared AI guard.');
})().catch(error => { console.error(error); process.exitCode = 1; });
