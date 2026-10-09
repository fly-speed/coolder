'use strict';

const $ = id => document.getElementById(id),
  apiRoot = '/api/v1/ai';

const state = {
  providers: [],
  projects: [],
  project: null,
  session: '',
  run: '',
  snapshot: null,
  progressView: null,
  closedDiffs: new Set(),
  reviewFile: null,
  file: null,
  fileSelection: 0,
  folder: '',
  poll: 0,
  epoch: 0
};

function node(tag, text, cls) {
  const e = document.createElement(tag);
  if (typeof text === 'function') uiText(e, text);
  else if (text !== undefined) plainText(e, text);
  if (cls) e.className = cls;
  return e;
}

function notice(error) {
  const dialog = document.querySelector('dialog[open]');
  if (dialog) {
    let alert = dialog.querySelector('.dialog-notice');
    if (!alert) {
      alert = node('p', undefined, 'dialog-notice error');
      alert.setAttribute('role', 'alert');
      const title = dialog.querySelector('.dialog-title');
      if (title) title.after(alert);
      else dialog.prepend(alert);
    }
    alert.classList.toggle('error', error instanceof Error);
    plainText(alert, error.message || String(error));
  }
  plainText($('notice'), error.message || String(error));
  $('notice').classList.toggle('error', error instanceof Error);
  $('notice').title = error.message || String(error);
  $('notice').hidden = false;
  clearTimeout(notice.timer);
  notice.timer = setTimeout(() => ($('notice').hidden = true), 9000);
}

new MutationObserver(() => {
  $('phase').title = $('phase').textContent;
}).observe($('phase'), { childList: true, characterData: true, subtree: true });

function task(fn) {
  return async e => {
    if (e) e.preventDefault();
    try {
      await fn(e);
    } catch (error) {
      notice(error);
    }
  };
}

async function request(path, body) {
  const response = await fetch(path, {
    method: body === undefined ? 'GET' : 'POST',
    credentials: 'same-origin',
    headers: body === undefined ? {} : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body)
  });
  let data;
  try {
    data = await response.json();
  } catch {
    throw new Error(t('服务返回无效响应'));
  }
  if (response.status === 401) {
    clearWorkspace();
    resetPersonalSettings();
    $('password-settings').hidden = true;
    $('logout').hidden = true;
    uiText($('connection'), () => t('请登录'));
    $('admin-settings').hidden = true;
    for (const dialog of document.querySelectorAll('dialog[open]')) dialog.close();
    $('login-dialog').showModal();
  }
  if (!response.ok || data.ok === false) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
}

function projectFileRequest(operation, path, body, project = state.project) {
  const relative = path === project.project_path ? '' : path.slice(project.project_path.length + 1);
  if (path !== project.project_path && !path.startsWith(project.project_path + '/'))
    throw new Error(t('文件不属于当前项目'));
  const identity = { owner_id: project.owner_id, project_id: project.id };
  const url = '/api/v1/collaborate/' + operation;
  return body === undefined
    ? request(url + '?' + new URLSearchParams({ ...identity, path: relative }))
    : request(url, { ...body, ...identity, path: relative });
}

async function api(path, body) {
  if (state.project?.owner_id && /^\/workspace\/(read|list)\?/.test(path)) {
    const project = state.project;
    const [operation, query] = path.slice('/workspace/'.length).split('?');
    const data = await projectFileRequest(operation, new URLSearchParams(query).get('path'), undefined, project);
    const fullPath = relative => relative ? project.project_path + '/' + relative : project.project_path;
    if (data.entries) data.entries.forEach(entry => { entry.path = fullPath(entry.path); });
    if (data.path !== undefined) data.path = fullPath(data.path);
    return data;
  }
  if (state.project?.shared && /^\/(runs|sessions|sandbox|projects\/(plan|tasks|workflow|index)|workspace\/(patch|change-set))/.test(path))
    throw new Error(t('共享项目的 AI 会话由项目拥有者管理'));
  return request(apiRoot + path, body);
}

function button(text, fn) {
  const e = node('button', text);
  e.type = 'button';
  e.onclick = task(fn);
  return e;
}

// Only unwrap known assistant/provider envelopes. JSON code examples stay intact.
function assistantDisplayText(value, streaming = false, depth = 0) {
  const pending = () =>
    t(streaming ? '正在整理模型回复…' : '模型回复中没有可显示的正文，请查看运行状态。');
  if (depth > 8) return pending();
  const read = item => assistantDisplayText(item, streaming, depth + 1);
  const join = items =>
    [...new Set(items.filter(item => typeof item === 'string' && item.trim()))].join('\n\n');
  if (typeof value === 'string') {
    const original = value;
    let text = value.trim();
    const fenced = text.match(/^```(?:json)?\s*\n([\s\S]*?)\n```$/i);
    if (fenced) text = fenced[1].trim();
    else if (/^```json\s*\n/i.test(text)) text = text.replace(/^```json\s*\n/i, '').trim();
    if (!text) return '';
    if (text[0] !== '{' && text[0] !== '[' && text[0] !== '"') return original;
    try {
      const parsed = JSON.parse(text);
      if (typeof parsed === 'string') return read(parsed);
      const result = assistantEnvelopeText(parsed, read, join);
      return result === null ? original : result || pending();
    } catch {
      // Incomplete streaming envelopes are protocol fragments, not chat prose.
      if (
        /^\{\s*"(?:type|object|choices|output|completion_summary|text|content|error)"\s*:/.test(
          text
        ) ||
        /"(?:object|type)"\s*:\s*"(?:chat\.completion|response|final|message)/.test(text) ||
        /"(?:choices|completion_summary)"\s*:/.test(text) ||
        (streaming && /^(?:\{|\[|\{\s*"[\w]*"?)\s*$/.test(text))
      )
        return pending();
      return original;
    }
  }
  if (value && typeof value === 'object') {
    return assistantEnvelopeText(value, read, join) || pending();
  }
  return '';
}

function assistantEnvelopeText(value, read, join) {
  if (!value || typeof value !== 'object') return null;
  if (Array.isArray(value)) {
    if (
      !value.every(
        item => item && ['text', 'output_text', 'message', 'refusal'].includes(item.type)
      )
    )
      return null;
    return join(value.map(read));
  }
  if (Array.isArray(value.choices)) {
    return join(
      value.choices.map(choice =>
        read(choice.message?.content || choice.delta?.content || choice.text || '')
      )
    );
  }
  if (value.object === 'response' || Array.isArray(value.output)) {
    return join([
      read(value.output_text || ''),
      ...(value.output || [])
        .filter(item => item.type === 'message')
        .map(item => read(item.content))
    ]);
  }
  if (value.type === 'message' && Array.isArray(value.content)) {
    return join(
      value.content.filter(item => ['text', 'output_text', 'refusal'].includes(item.type)).map(read)
    );
  }
  if (['text', 'output_text'].includes(value.type)) return read(value.text || '');
  if (value.type === 'refusal') return read(value.refusal || value.text || '');
  if (value.error)
    return read(typeof value.error === 'string' ? value.error : value.error.message || '');
  if (
    ['final', 'answer', 'response'].includes(value.type) ||
    'completion_summary' in value ||
    ('text' in value && ('changes' in value || 'tool_calls' in value))
  ) {
    const parts = [
      read(value.completion_summary || ''),
      read(value.text || value.summary || value.message || '')
    ];
    for (const key of ['warnings', 'next_steps']) {
      const item = value[key];
      if (typeof item === 'string') parts.push(read(item));
      else if (Array.isArray(item))
        parts.push(join(item.filter(entry => typeof entry === 'string').map(read)));
    }
    return join(parts);
  }
  if (value.role === 'assistant' && value.content !== undefined) return read(value.content);
  // Non-prose protocol objects must never expose tool arguments or usage metadata.
  if (
    [
      'tool_call',
      'tool_calls',
      'function_call',
      'tool_use',
      'thinking',
      'reasoning',
      'usage'
    ].includes(value.type) ||
    value.object === 'chat.completion.chunk'
  )
    return '';
  return null;
}

function messageTime(value) {
  if (value === undefined || value === null || value === '') return null;
  const numeric = Number(value);
  const date = Number.isFinite(numeric)
    ? new Date(numeric < 1e12 ? numeric * 1000 : numeric)
    : new Date(value);
  return Number.isFinite(date.getTime()) && date.getTime() > 0 ? date : null;
}
function formatMessageTime(date) {
  return new Intl.DateTimeFormat(i18n.language, {
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false
  }).format(date);
}
function message(role, text, createdAt) {
  const e = node('div', undefined, 'message ' + role);
  const author = node('strong', () => (role === 'user' ? t('你') : 'COOLDER'));
  if (role === 'user') {
    const header = node('div', undefined, 'message-header');
    const date = messageTime(createdAt);
    const time = node('time', () => (date ? formatMessageTime(date) : t('时间未知')));
    if (date) time.dateTime = date.toISOString();
    header.append(author, time);
    e.append(header);
    e.dataset.requestText = text || '';
  } else e.append(author);
  e.append(document.createTextNode(role === 'assistant' ? assistantDisplayText(text) : text || ''));
  $('messages').append(e);
  return e;
}

const requestIndex = { entries: [], frame: 0 };
function updateRequestIndexPosition() {
  cancelAnimationFrame(requestIndex.frame);
  requestIndex.frame = requestAnimationFrame(() => {
    const top = $('messages').getBoundingClientRect().top + 60;
    let active = requestIndex.entries[0];
    for (const entry of requestIndex.entries) {
      if (entry.message.getBoundingClientRect().top <= top) active = entry;
    }
    const scroller = $('messages');
    if (
      scroller.scrollHeight > scroller.clientHeight &&
      scroller.scrollTop + scroller.clientHeight >= scroller.scrollHeight - 2
    ) {
      active = requestIndex.entries[requestIndex.entries.length - 1];
    }
    for (const entry of requestIndex.entries) {
      entry.button.classList.toggle('active', entry === active);
      if (entry === active) entry.button.setAttribute('aria-current', 'true');
      else entry.button.removeAttribute('aria-current');
    }
  });
}
function rebuildRequestIndex() {
  const rail = $('request-index');
  rail.replaceChildren();
  $('request-index-preview').hidden = true;
  requestIndex.entries = [];
  for (const message of $('messages').querySelectorAll('.message.user')) {
    const summary = (message.dataset.requestText || '').replace(/\s+/g, ' ').trim();
    const marker = button('−', () => {
      $('request-index-preview').hidden = true;
      const scroller = $('messages');
      scroller.scrollTo({
        top:
          scroller.scrollTop +
          message.getBoundingClientRect().top -
          scroller.getBoundingClientRect().top -
          16,
        behavior: 'smooth'
      });
    });
    marker.setAttribute('aria-label', summary.slice(0, 100));
    const showPreview = () => {
      const preview = $('request-index-preview');
      preview.replaceChildren(
        node('small', message.querySelector('time')?.textContent || ''),
        node('p', summary.slice(0, 240))
      );
      preview.hidden = false;
      const area = rail.parentElement.getBoundingClientRect();
      preview.style.top =
        Math.max(
          4,
          Math.min(
            marker.getBoundingClientRect().top - area.top,
            area.height - preview.offsetHeight - 4
          )
        ) + 'px';
    };
    marker.onmouseenter = marker.onfocus = showPreview;
    marker.onmouseleave = marker.onblur = () => ($('request-index-preview').hidden = true);
    rail.append(marker);
    requestIndex.entries.push({ message, button: marker });
  }
  rail.hidden = !requestIndex.entries.length;
  updateRequestIndexPosition();
}
new MutationObserver(rebuildRequestIndex).observe($('messages'), { childList: true });
$('messages').addEventListener('scroll', updateRequestIndexPosition, { passive: true });
new ResizeObserver(updateRequestIndexPosition).observe($('messages'));

function selectBodyTab(name) {
  for (const tab of ['conversation', 'source', 'review']) {
    const selected = tab === name;
    $(tab + '-pane').hidden = !selected;
    $(tab + '-tab').classList.toggle('active', selected);
    $(tab + '-tab').setAttribute('aria-selected', String(selected));
    $(tab + '-tab').tabIndex = selected ? 0 : -1;
  }
  if (name === 'source') requestAnimationFrame(() => codeEditors.viewer?.layout());
  if (name === 'review' && state.reviewFile?.startsWith('manual:')) {
    activateSourceFile(state.reviewFile.slice(7));
    renderReviewFileTabs();
  }
  if (name === 'review') requestAnimationFrame(() => codeEditors.editing?.diff?.layout());
}
for (const name of ['conversation', 'source', 'review']) {
  $(name + '-tab').onclick = () => selectBodyTab(name);
  $(name + '-tab').onkeydown = event => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
    event.preventDefault();
    const tabs = ['conversation', 'source', 'review'];
    const next =
      event.key === 'Home'
        ? tabs[0]
        : event.key === 'End'
          ? tabs[2]
          : tabs[(tabs.indexOf(name) + (event.key === 'ArrowRight' ? 1 : 2)) % tabs.length];
    selectBodyTab(next);
    $(next + '-tab').focus();
  };
}

function resetRun() {
  selectBodyTab('conversation');
  uiText($('changes-tab'), () => t('变更'));
  uiText($('review-tab'), () => t('差异确认'));
  clearTimeout(state.poll);
  state.epoch++;
  state.run = '';
  state.snapshot = null;
  state.progressView = null;
  state.closedDiffs.clear();
  state.reviewFile = null;
  renderReviewFileTabs();
  $('pause').disabled = true;
  $('cancel').disabled = true;
  $('send').disabled = false;
  $('recover').hidden = true;
  uiText($('phase'), () => t('就绪'));
  $('changes').replaceChildren();
  $('tools').replaceChildren();
}

async function providers() {
  const data = await api('/providers');
  state.providers = data.providers || [];
  const previous = $('provider').value;
  $('provider').replaceChildren();
  for (const p of state.providers) {
    const o = node('option', p.name + ' · ' + p.model);
    o.value = p.id;
    if (p.enabled) $('provider').append(o);
  }
  if (state.providers.some(p => p.id === previous)) $('provider').value = previous;
  else {
    const preferred = state.providers.find(p => p.is_default && p.enabled);
    if (preferred) $('provider').value = preferred.id;
  }
  if (!$('provider').options.length) {
    const o = node('option', () =>
      identity?.admin ? t('请在管理控制台配置模型') : t('暂无可用模型，请联系管理员')
    );
    o.value = '';
    $('provider').append(o);
  }
  renderProviderList();
}

async function projects() {
  const data = await request('/api/v1/collaborate/projects');
  state.projects = (data.projects || []).map(p => ({
    ...p,
    id: p.project_id,
    project_path: p.path
  }));
  $('projects').replaceChildren();
  for (const p of state.projects) {
    const b = button(p.title || p.project_path, () => selectProject(p));
    b.append(node('small', p.project_path));
    b.append(node('small', () => `${p.owner} · ${t(p.permission === 'owner' ? '拥有者' : p.permission === 'write' ? '读写' : '只读')}`));
    if (state.project && state.project.id === p.id && state.project.owner_id === p.owner_id) b.classList.add('active');
    $('projects').append(b);
  }
  if (!state.projects.length)
    $('projects').append(node('small', () => t('点击 ＋ 创建或导入项目')));
}

async function selectProject(p) {
  if (!closeAllSourceFiles()) return;
  clearComposerAttachments();
  resetRun();
  state.project = p;
  fileTree.root = null;
  $('project-settings').disabled = false;
  for (const id of ['send', 'plan', 'history']) $(id).disabled = !!p.shared;
  state.session = '';
  state.file = null;
  clearCodeViewer();
  $('edit-file').disabled = true;
  uiText($('file-title'), () => t('文件预览'));
  uiText($('file-content'), () => t('选择文件查看内容'));
  plainText($('project-title'), p.title || p.project_path);
  plainText($('project-path'), p.project_path);
  $('project-title').title = p.project_path;
  $('messages').replaceChildren();
  message('assistant', p.shared ? t('共享项目：可浏览文件，具有读写权限时可编辑保存。AI 会话由项目拥有者管理。') : t('项目已就绪。你希望实现什么？'));
  await Promise.all([sessions(), files(p.project_path), projects()]);
}

async function sessions() {
  if (!state.project) return;
  if (state.project.shared) { $('sessions').replaceChildren(); return; }
  const projectId = state.project.id;
  const epoch = state.epoch;
  const data = await api('/sessions?project_id=' + encodeURIComponent(projectId));
  if (epoch !== state.epoch) return;
  $('sessions').replaceChildren();
  for (const s of data.sessions || []) {
    const b = button(
      () => s.title || t('编程会话'),
      () => selectSession(s)
    );
    if (state.session === s.session_id) b.classList.add('active');
    $('sessions').append(b);
  }
}

async function selectSession(s) {
  clearComposerAttachments();
  resetRun();
  state.session = s.session_id;
  const data = await api(
    '/sessions?project_id=' +
      encodeURIComponent(state.project.id) +
      '&session_id=' +
      encodeURIComponent(s.session_id)
  );
  const item = (data.sessions || []).find(x => x.session_id === s.session_id);
  $('messages').replaceChildren();
  for (const m of item?.messages || [])
    message(m.role, m.content || m.text || '', m.sent_at ?? m.created_at);
  await sessions();
  if (item?.last_run_id) {
    state.run = item.last_run_id;
    await poll(state.epoch);
  }
}

const fileTree = { root: null, generation: 0, expanded: new Set() };

async function openTreeFile(entry, pinned = false) {
  if (sourceFiles.has(entry.path)) {
    if (pinned) pinSourceFile(entry.path);
    activateSourceFile(entry.path);
    selectBodyTab('source');
    return;
  }
  if (codeEditors.editing?.saving) return;
  const epoch = state.epoch,
    selection = ++state.fileSelection;
  const data = await api('/workspace/read?path=' + encodeURIComponent(entry.path));
  if (epoch !== state.epoch || selection !== state.fileSelection) return;
  createSourceFile(data, !pinned);
  uiText($('file-title'), () => entry.path + (data.truncated ? t('（内容已截断）') : ''));
  plainText($('file-content'), data.content);
  state.file = data;
  selectBodyTab('source');
  showCodeFile(data);
  renderSourceFileTabs();
  $('edit-file').disabled = data.truncated || state.project?.permission === 'read';
  for (const item of $('files').querySelectorAll('[role="treeitem"]'))
    item.setAttribute('aria-selected', String(item.dataset.path === entry.path));
}

async function files(path = state.folder) {
  const root = path || state.project?.project_path || '';
  if (fileTree.root !== root) {
    fileTree.root = root;
    fileTree.expanded.clear();
  }
  state.folder = root;
  plainText($('folder'), root || '/');
  const generation = ++fileTree.generation,
    project = state.project;
  const current = () => generation === fileTree.generation && state.project === project;
  const tree = $('files');
  const focused = tree.contains(document.activeElement)
    ? document.activeElement.dataset.path
    : null;
  const scroll = tree.scrollTop;
  tree.replaceChildren();
  async function load(parent, directory) {
    const data = await api('/workspace/list?path=' + encodeURIComponent(directory));
    if (!current() || !parent.isConnected) return;
    parent.replaceChildren();
    const entries = (data.entries || []).sort(
      (a, b) =>
        Number(b.directory) - Number(a.directory) ||
        a.path.localeCompare(b.path, undefined, { numeric: true })
    );
    if (!entries.length) {
      const empty = node('li', () => t('空文件夹'), 'tree-empty');
      empty.setAttribute('role', 'none');
      parent.append(empty);
    }
    for (const entry of entries) {
      if (!current() || !parent.isConnected) return;
      const item = node('li', undefined, 'tree-item');
      item.setAttribute('role', 'treeitem');
      item.setAttribute('aria-selected', String(state.file?.path === entry.path));
      item.dataset.path = entry.path;
      item.tabIndex = -1;
      const row = node('div', undefined, 'tree-row');
      row.title = entry.path;
      const arrow = node('span', entry.directory ? '▸' : '', 'tree-arrow');
      arrow.setAttribute('aria-hidden', 'true');
      const icon = node(
        'span',
        entry.directory ? '▰' : '≡',
        entry.directory ? 'tree-folder' : 'tree-file'
      );
      icon.setAttribute('aria-hidden', 'true');
      row.append(arrow, icon, node('span', entry.path.split('/').pop(), 'tree-name'));
      item.append(row);
      parent.append(item);
      let group = null,
        loaded = false,
        loading = false;
      if (entry.directory) {
        item.setAttribute('aria-expanded', 'false');
        group = node('ul', undefined, 'tree-children');
        group.setAttribute('role', 'group');
        group.hidden = true;
        item.append(group);
      }
      const expand = async open => {
        item.setAttribute('aria-expanded', String(open));
        group.hidden = !open;
        arrow.textContent = open ? '▾' : '▸';
        if (open) fileTree.expanded.add(entry.path);
        else fileTree.expanded.delete(entry.path);
        if (!open || loaded || loading) return;
        loading = true;
        item.setAttribute('aria-busy', 'true');
        try {
          await load(group, entry.path);
          loaded = true;
        } catch (error) {
          item.setAttribute('aria-expanded', 'false');
          group.hidden = true;
          arrow.textContent = '▸';
          fileTree.expanded.delete(entry.path);
          throw error;
        } finally {
          loading = false;
          item.removeAttribute('aria-busy');
        }
      };
      row.onclick = task(async () => {
        focusTreeItem(item);
        if (entry.directory) await expand(group.hidden);
        else await openTreeFile(entry);
      });
      if (!entry.directory)
        row.ondblclick = task(async event => {
          event.stopPropagation();
          await openTreeFile(entry, true);
        });
      item.onkeydown = task(async event => {
        if (event.target !== item) return;
        const items = [...tree.querySelectorAll('[role="treeitem"]')].filter(
          n => n.getClientRects().length
        );
        const index = items.indexOf(item);
        if (
          ['ArrowDown', 'ArrowUp', 'Home', 'End', 'ArrowRight', 'ArrowLeft', 'Enter', ' '].includes(
            event.key
          )
        )
          event.preventDefault();
        if (event.key === 'ArrowDown') focusTreeItem(items[Math.min(index + 1, items.length - 1)]);
        else if (event.key === 'ArrowUp') focusTreeItem(items[Math.max(index - 1, 0)]);
        else if (event.key === 'Home') focusTreeItem(items[0]);
        else if (event.key === 'End') focusTreeItem(items[items.length - 1]);
        else if (event.key === 'ArrowRight' && entry.directory) {
          if (group.hidden) await expand(true);
          else focusTreeItem(group.querySelector('[role="treeitem"]'));
        } else if (event.key === 'ArrowLeft') {
          if (entry.directory && !group.hidden) await expand(false);
          else focusTreeItem(item.parentElement.closest('[role="treeitem"]'));
        } else if (event.key === 'Enter' || event.key === ' ') {
          if (entry.directory) await expand(group.hidden);
          else await openTreeFile(entry);
        }
      });
      if (entry.directory && fileTree.expanded.has(entry.path)) {
        try {
          await expand(true);
        } catch (error) {
          if (current()) notice(error);
        }
      }
    }
  }
  await load(tree, root);
  if (!current()) return;
  const items = [...tree.querySelectorAll('[role="treeitem"]')];
  const target = items.find(item => item.dataset.path === focused) || items[0];
  if (target) {
    target.tabIndex = 0;
    if (focused) focusTreeItem(target);
  }
  tree.scrollTop = scroll;
}
function focusTreeItem(item) {
  if (!item) return;
  for (const sibling of $('files').querySelectorAll('[role="treeitem"]')) sibling.tabIndex = -1;
  item.tabIndex = 0;
  item.focus();
}

function runProgressStage(s) {
  if (s.status === 'failed') return ['failed', '任务失败，请查看回复中的原因'];
  if (s.status === 'cancelled' || s.status === 'canceled') return ['cancelled', '任务已取消'];
  if (s.status === 'completed') {
    const pending = (s.changes || []).filter(c => c.review_status === 'pending').length;
    return pending
      ? ['pending:' + pending, '成果已生成，等待确认写入', String(pending)]
      : ['completed', '任务处理结束，请查看最终回复'];
  }
  if (s.phase === 'paused' || s.paused) return ['paused', '任务已暂停'];
  if (s.cancel_requested) return ['cancelling', '正在取消任务'];
  if (s.pause_requested) return ['pausing', '正在暂停任务'];
  if (s.phase === 'queued') return ['queued', '任务已排队，等待执行'];
  if (s.phase === 'model_retry') return ['retry', '模型请求正在重试'];
  if (s.phase === 'tool_call') {
    const tool = String(s.current_tool || '');
    const label = /validate|test/.test(tool)
      ? '正在运行项目验证'
      : /execute|command|shell/.test(tool)
        ? '正在执行命令'
        : /propose|stage|create|write|mkdir/.test(tool)
          ? '正在准备文件修改'
          : /read|list|search|find/.test(tool)
            ? '正在查看项目文件'
            : '正在执行工具';
    return ['tool:' + tool + ':' + (s.completed_tool_calls || 0), label, tool.slice(0, 80)];
  }
  if (s.phase === 'staged_change')
    return ['staged:' + (s.staged_change_version || 0), '文件修改已暂存，尚未写入正式目录'];
  if (s.phase === 'model_call' || s.phase === 'model_stream' || s.provider_response_pending)
    return ['model:' + (s.completed_tool_calls || 0), '正在等待或接收模型回复'];
  return null;
}
function renderRunProgress(s) {
  if (!state.run) return;
  let view = state.progressView;
  if (!view || view.run !== state.run || !view.root.isConnected) {
    const root = node('details', undefined, 'run-progress');
    root.open = true;
    const summary = node('summary', () => t('任务进展'));
    const list = node('ol');
    list.setAttribute('aria-live', 'polite');
    root.append(summary, list);
    $('messages').append(root);
    view = state.progressView = { run: state.run, root, list, stage: '', completed: 0, staged: 0 };
  }
  const append = (label, detail = '') => {
    const follow = view.list.scrollHeight - view.list.scrollTop - view.list.clientHeight < 24;
    const row = node('li');
    row.append(node('span', () => t(label)));
    if (detail) row.append(node('small', detail));
    view.list.append(row);
    // Keep progress lightweight during long tool loops.
    while (view.list.children.length > 60) view.list.firstElementChild.remove();
    if (follow) view.list.scrollTop = view.list.scrollHeight;
  };
  const count = Number(s.completed_tool_calls) || 0;
  if (count > view.completed) {
    append('工具执行进度', String(count));
    view.completed = count;
  }
  const staged = Number(s.staged_change_version) || 0;
  if (staged > view.staged) {
    append('文件修改已暂存，尚未写入正式目录');
    view.staged = staged;
  }
  const stage = runProgressStage(s);
  if (stage && stage[0] !== view.stage) {
    append(stage[1], stage[2]);
    view.stage = stage[0];
  }
  view.root.classList.toggle('finished', s.status !== 'running');
}

function renderRun(s) {
  const messages = $('messages');
  const follow = messages.scrollHeight - messages.scrollTop - messages.clientHeight < 64;
  state.snapshot = s;
  plainText($('phase'), [s.status, s.phase, s.current_tool].filter(Boolean).join(' · '));
  const active = s.status === 'running';
  $('send').disabled = active;
  $('pause').disabled = !active;
  $('cancel').disabled = !active;
  uiText($('pause'), () => (s.pause_requested || s.phase === 'paused' ? t('继续') : t('暂停')));
  $('recover').hidden = active || !s.recovery_available;
  if (s.session_id) state.session = s.session_id;
  renderRunProgress(s);
  let live = $('live-reply');
  if (!live) {
    live = message('assistant', '');
    live.id = 'live-reply';
  }
  live.replaceChildren(
    node('strong', 'COOLDER'),
    document.createTextNode(
      assistantDisplayText(
        s.completion_summary || s.text || s.streamed_text || s.error,
        s.status === 'running'
      ) || t('正在处理任务…')
    )
  );
  $('tools').replaceChildren();
  for (const t of Array.isArray(s.tool_calls) ? s.tool_calls : [])
    $('tools').append(
      node('div', (t.ok ? '✓ ' : '! ') + t.name + ' ' + (t.path || t.query || ''), 'tool')
    );
  renderChanges(s);
  if (s.error) $('phase').textContent += ' · ' + s.error;
  if (follow) messages.scrollTop = messages.scrollHeight;
}

function canCloseDiff(change) {
  return change.review_status === 'accepted' || change.review_status === 'rejected';
}

function diffViewKey(change) {
  return JSON.stringify([state.run, change.path, change.generation, change.draft_hash]);
}

function renderChanges(s) {
  renderReviewFileTabs(s);
  $('changes').replaceChildren();
  const pending = (s.changes || []).filter(change => change.review_status === 'pending');
  if (pending.length) {
    const summary = node('div', undefined, 'pending-review-summary');
    summary.append(node('p', () => t('生成成果尚未写入正式目录，请接受变更后使用。')));
    const accept = button(
      () => t('全部接受并写入项目'),
      async () => {
        accept.disabled = true;
        const run = state.run,
          session = state.session,
          epoch = state.epoch;
        try {
          await api('/runs/result/review', {
            run_id: run,
            session_id: session,
            reviews: pending.map(change => ({
              path: change.path,
              generation: change.generation,
              draft_hash: change.draft_hash,
              decision: 'accepted'
            }))
          });
          if (epoch !== state.epoch || run !== state.run) return;
          await poll(epoch);
          await files();
          notice(t('已写入正式项目目录'));
        } finally {
          accept.disabled = false;
        }
      }
    );
    accept.disabled = s.status === 'running';
    summary.append(accept);
    $('changes').append(summary);
    if (s.status === 'completed') uiText($('phase'), () => t('生成完成 · 待写入项目'));
  }
  uiText($('changes-tab'), () => t('变更') + (pending.length ? ' (' + pending.length + ')' : ''));
  uiText(
    $('review-tab'),
    () => t('差异确认') + (pending.length ? ' (' + pending.length + ')' : '')
  );
  const reviewed = (s.changes || []).filter(canCloseDiff);
  const hidden = (s.changes || []).filter(c => state.closedDiffs.has(diffViewKey(c)));
  const visible = reviewed.filter(c => !state.closedDiffs.has(diffViewKey(c)));
  if (reviewed.length || hidden.length) {
    const toolbar = node('div', undefined, 'diff-view-toolbar');
    const closeAll = button(
      () => t('关闭全部已审核差异'),
      () => {
        for (const c of visible) state.closedDiffs.add(diffViewKey(c));
        renderChanges(state.snapshot);
      }
    );
    closeAll.disabled = !visible.length;
    toolbar.append(closeAll);
    if (hidden.length) {
      toolbar.append(
        button(
          () => t('显示已关闭差异') + ' (' + hidden.length + ')',
          () => {
            for (const c of hidden) state.closedDiffs.delete(diffViewKey(c));
            renderChanges(state.snapshot);
          }
        )
      );
    }
    $('changes').append(toolbar);
  }
  let displayed = 0;
  for (const c of s.changes || []) {
    if (state.closedDiffs.has(diffViewKey(c))) continue;
    displayed++;
    if (state.reviewFile !== diffViewKey(c)) continue;
    const wrap = node('div', undefined, 'change');
    const header = node('div', undefined, 'change-header');
    header.append(
      node(
        'h4',
        `${c.path} · ${c.review_status || 'pending'} · +${c.added_lines || 0} −${c.removed_lines || 0}`
      )
    );
    if (canCloseDiff(c)) {
      header.append(
        button(
          () => t('关闭差异'),
          () => {
            state.closedDiffs.add(diffViewKey(c));
            renderChanges(state.snapshot);
          }
        )
      );
    }
    wrap.append(header, node('pre', c.diff || c.content || c.reason || ''));
    const actions = node('div', undefined, 'actions');
    for (const [label, decision] of [
      [() => t('接受'), 'accepted'],
      [() => t('拒绝'), 'rejected']
    ]) {
      const b = button(label, async () => {
        await api('/runs/result/review', {
          run_id: state.run,
          session_id: state.session,
          reviews: [{ path: c.path, generation: c.generation, draft_hash: c.draft_hash, decision }]
        });
        await poll(state.epoch);
        await files();
      });
      b.disabled = c.review_status !== 'pending';
      actions.append(b);
    }
    wrap.append(actions);
    $('changes').append(wrap);
  }
  if (!displayed && hidden.length) {
    $('changes').append(node('p', () => t('差异视图已关闭，修改及审核状态仍保留。'), 'muted'));
  }
}

async function poll(epoch) {
  clearTimeout(state.poll);
  if (!state.run) return;
  const id = state.run;
  try {
    const s = await api(
      '/runs/status?id=' +
        encodeURIComponent(id) +
        '&session_id=' +
        encodeURIComponent(state.session)
    );
    if (epoch !== state.epoch || id !== state.run) return;
    renderRun(s);
    if (s.status === 'running') {
      state.poll = setTimeout(() => poll(epoch), 1000);
    } else {
      await Promise.all([sessions(), files()]);
    }
  } catch (error) {
    notice(error);
    if (epoch === state.epoch && !$('login-dialog').open)
      state.poll = setTimeout(() => poll(epoch), 4000);
  }
}

async function start(resume = false) {
  if (!state.project) throw new Error(t('请先选择项目'));
  if (!$('provider').value) throw new Error(t('请先配置模型'));
  const prompt =
    $('prompt').value.trim() ||
    (!resume && composerAttachments.items.length ? t('请根据附件完成任务') : '');
  if (!resume && !prompt) throw new Error(t('请描述编程任务'));
  if (state.submitting) return;
  const submittedAt = Date.now();
  state.submitting = true;
  const epoch = state.epoch;
  $('send').disabled = true;
  let prepared = null;
  const attachmentNames = resume ? [] : composerAttachments.items.map(item => item.name);
  renderComposerAttachments();
  try {
    prepared = resume
      ? { attachments: [], attachment_draft: '' }
      : await prepareComposerAttachments();
    if (epoch !== state.epoch) return;
    const data = await api('/runs/start', {
      attachments: prepared.attachments,
      attachment_draft: prepared.attachment_draft,
      agent_id: 'coding',
      provider_id: $('provider').value,
      prompt: prompt || t('继续完成上次任务'),
      path: state.project.project_path,
      project_id: state.project.id,
      session_id: state.session,
      remember_session: true,
      resume_after_restart: true,
      resume_from_progress: resume,
      execution_mode: $('mode').value,
      ui_language: 'zh'
    });
    prepared = null;
    if (epoch !== state.epoch) {
      notice(t('任务已在原项目后台启动，可从运行记录查看'));
      return;
    }
    $('live-reply')?.removeAttribute('id');
    message(
      'user',
      (prompt || t('继续完成上次任务')) +
        (attachmentNames.length ? '\n' + t('附件：') + attachmentNames.join(', ') : ''),
      submittedAt
    );
    if (!resume) clearComposerAttachments();
    state.run = data.run_id;
    state.session = data.session_id || state.session;
    $('prompt').value = '';
    $('messages').scrollTop = $('messages').scrollHeight;
    await poll(state.epoch);
  } catch (e) {
    $('send').disabled = false;
    throw e;
  } finally {
    await discardComposerDraft(prepared?.attachment_draft);
    state.submitting = false;
    renderComposerAttachments();
    if (epoch === state.epoch) $('send').disabled = state.snapshot?.status === 'running';
  }
}

$('toggle-composer').onclick = () => {
  const collapsed = !$('composer-content').hidden;
  $('composer-content').hidden = collapsed;
  $('composer').classList.toggle('collapsed', collapsed);
  const toggle = $('toggle-composer');
  toggle.setAttribute('aria-expanded', String(!collapsed));
  const label = collapsed ? '展开输入框' : '收起输入框';
  toggle.dataset.i18nTitle = label;
  toggle.dataset.i18nAriaLabel = label;
  toggle.title = t(label);
  toggle.setAttribute('aria-label', t(label));
  if (!collapsed) $('prompt').focus();
};

$('prompt-form').onsubmit = task(() => start());

$('recover').onclick = task(() => start(true));

let promptComposing = false;
let promptCompositionEndedAt = 0;
$('prompt').addEventListener('compositionstart', () => {
  promptComposing = true;
});
$('prompt').addEventListener('compositionend', () => {
  promptComposing = false;
  promptCompositionEndedAt = Date.now();
});
$('prompt').onkeydown = e => {
  if (
    e.key !== 'Enter' ||
    e.isComposing ||
    e.keyCode === 229 ||
    promptComposing ||
    Date.now() - promptCompositionEndedAt < 50
  )
    return;
  if (!$('enter-to-send').checked || e.shiftKey || e.ctrlKey || e.metaKey || e.altKey) return;
  e.preventDefault();
  if (e.repeat || $('send').disabled || state.submitting) return;
  $('prompt-form').requestSubmit($('send'));
};
document.addEventListener('pointerdown', e => {
  if (!$('send-options').contains(e.target)) $('send-options').open = false;
});
$('send-options').addEventListener('keydown', e => {
  if (e.key === 'Escape') {
    e.preventDefault();
    $('send-options').open = false;
    $('send-options').querySelector('summary').focus();
  }
});

$('pause').onclick = task(async () => {
  await api('/runs/pause', {
    run_id: state.run,
    paused: !(state.snapshot?.pause_requested || state.snapshot?.phase === 'paused')
  });
  await poll(state.epoch);
});

$('cancel').onclick = task(async () => {
  await api('/runs/cancel', { run_id: state.run });
  await poll(state.epoch);
});

$('new-session').onclick = task(async () => {
  clearComposerAttachments();
  resetRun();
  state.session = '';
  $('messages').replaceChildren();
  message('assistant', t('新会话已就绪。'));
  await sessions();
});

$('new-project').onclick = () => $('project-dialog').showModal();
$('welcome-create-project').onclick = () => $('new-project').click();

let projectDirectory = '', projectDirectoryParent = '', directoryRequest = 0;

async function browseProjectDirectory(path = '') {
  const requestId = ++directoryRequest;
  $('project-directory-select').disabled = true;
  $('project-directory-up').disabled = true;
  $('project-directory-list').replaceChildren();
  $('project-directory-list').setAttribute('aria-busy', 'true');
  uiText($('project-directory-status'), () => t('正在读取目录…'));
  $('project-directory-status').classList.remove('error');
  try {
    const data = await api('/projects/directories?path=' + encodeURIComponent(path));
    if (requestId !== directoryRequest) return;
    projectDirectory = data.path;
    projectDirectoryParent = data.parent;
    $('project-directory-path').value = data.path;
    $('project-directory-up').disabled = !data.parent || data.parent === data.path;
    $('project-directory-select').disabled = false;
    for (const entry of data.entries) {
      const button = node('button', '📁 ' + entry.name);
      button.type = 'button';
      button.onclick = () => browseProjectDirectory(entry.path);
      $('project-directory-list').append(button);
    }
    uiText($('project-directory-status'), () => data.truncated
      ? t('目录较多，仅显示部分内容；可输入完整路径打开目录。')
      : data.entries.length ? '' : t('此目录下没有子目录。'));
  } catch (error) {
    if (requestId !== directoryRequest) return;
    uiText($('project-directory-status'), () => t('无法读取目录，请检查路径和访问权限。'));
    $('project-directory-status').classList.add('error');
  } finally {
    if (requestId === directoryRequest)
      $('project-directory-list').setAttribute('aria-busy', 'false');
  }
}

$('project-browse').onclick = () => {
  $('project-directory-dialog').showModal();
  // Start from the last successfully browsed directory, or the workspace.
  $('project-directory-path').value = projectDirectory;
  browseProjectDirectory(projectDirectory);
};
$('project-directory-dialog').addEventListener('close', () => ++directoryRequest);
$('project-directory-go').onsubmit = e => {
  e.preventDefault();
  browseProjectDirectory($('project-directory-path').value.trim());
};
$('project-directory-up').onclick = () => browseProjectDirectory(projectDirectoryParent);
$('project-directory-select').onclick = () => {
  $('new-project-path').value = projectDirectory;
  $('project-directory-dialog').close();
  $('new-project-path').focus();
};

$('project-form').onsubmit = task(async () => {
  const f = new FormData($('project-form')),
    operation = f.get('operation');
  const data = await api(
    operation === 'create' ? '/workspace/project/create' : '/projects/' + operation,
    {
      path: f.get('path'),
      language: f.get('language'),
      platform: 'cross-platform',
      confirm: true
    }
  );
  $('project-dialog').close();
  await projects();
  const p =
    state.projects.find(p => p.project_path === f.get('path')) ||
    state.projects.find(p => p.id === data.project_id);
  if (p) await selectProject(p);
});

// Coding presets copied from webcool/html/js/ai-settings.js.
// Image-generation providers are not coding-agent providers.
const providerPresets = {
  deepseek: {
    name: 'DeepSeek',
    baseUrl: 'https://api.deepseek.com',
    models: ['deepseek-v4-flash', 'deepseek-v4-pro', 'deepseek-v4-flash-vision-exp']
  },
  kimi: {
    name: 'Kimi',
    baseUrl: 'https://api.moonshot.cn/v1',
    models: ['kimi-k3', 'kimi-k2.7-code', 'kimi-k2.7-code-highspeed', 'kimi-k2.6']
  },
  qwen: {
    name: t('千问'),
    baseUrl: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
    protocol: 'openai_responses',
    models: ['qwen3.7-plus', 'qwen3.8-max', 'qwen3.8-flash']
  }
};

function providerVendor(provider) {
  try {
    const host = new URL(provider?.base_url).hostname;
    if (host === 'api.deepseek.com') return 'deepseek';
    if (['api.moonshot.cn', 'api.moonshot.ai', 'api.kimi.com'].includes(host)) return 'kimi';
    if (
      ['dashscope.aliyuncs.com', 'dashscope-intl.aliyuncs.com'].includes(host) ||
      host.endsWith('.maas.aliyuncs.com')
    )
      return 'qwen';
  } catch {
    /* Unsaved/custom URLs need no preset. */
  }
  return 'custom';
}

function syncProviderModels() {
  const preset = providerPresets[$('provider-vendor').value];
  const select = $('provider-model-preset');
  select.replaceChildren();
  for (const model of preset?.models || []) {
    const option = node('option', model);
    option.value = model;
    select.append(option);
  }
  const custom = node('option', () => t('自定义模型'));
  custom.value = '';
  select.append(custom);
  const model = $('provider-form').elements.model.value;
  select.value = preset?.models.includes(model) ? model : '';
  $('provider-model-options').hidden = !preset;
}

function presetProtocol(preset, model) {
  return preset.protocol || (model === 'kimi-k3' ? 'openai_responses' : 'openai_chat');
}

$('provider-vendor').onchange = function () {
  const key = this.value;
  const preset = providerPresets[key];
  if (preset) {
    const form = $('provider-form');
    form.reset();
    form.elements.id.value = '';
    form.elements.api_key.value = '';
    this.value = key;
    form.elements.name.value = preset.name;
    form.elements.base_url.value = preset.baseUrl;
    form.elements.model.value = preset.models[0];
    form.elements.protocol.value = presetProtocol(preset, preset.models[0]);
  }
  syncProviderModels();
  renderProviderList();
};
$('provider-model-preset').onchange = function () {
  const form = $('provider-form');
  if (this.value) {
    form.elements.model.value = this.value;
    form.elements.protocol.value = presetProtocol(
      providerPresets[$('provider-vendor').value],
      this.value
    );
  } else {
    form.elements.model.focus();
  }
};
$('provider-form').elements.model.addEventListener('input', syncProviderModels);

function renderProviderList() {
  const selected = $('provider-form').elements.id.value;
  $('provider-list').replaceChildren();
  $('provider-empty').hidden = state.providers.length > 0;
  uiText($('provider-form-title'), () => (selected ? t('编辑模型') : t('新增模型')));
  $('delete-provider').disabled = !selected;
  $('test-provider').disabled = !selected;
  for (const provider of state.providers) {
    const row = button(undefined, () => selectProvider(provider.id));
    row.className = 'provider-list-item';
    row.classList.toggle('active', provider.id === selected);
    row.setAttribute('aria-pressed', String(provider.id === selected));
    row.append(node('strong', provider.name), node('small', provider.model));
    row.append(
      node('small', () =>
        [provider.enabled ? t('已启用') : t('已停用'), provider.is_default ? t('默认模型') : '']
          .filter(Boolean)
          .join(' · ')
      )
    );
    $('provider-list').append(row);
  }
}

function selectProvider(id = '') {
  const form = $('provider-form');
  const p = state.providers.find(p => p.id === id);
  for (const name of ['id', 'name', 'protocol', 'base_url', 'model'])
    form.elements[name].value = p?.[name] || (name === 'protocol' ? 'openai_compatible' : '');
  form.elements.api_key.value = '';
  form.elements.enabled.checked = p ? p.enabled : true;
  for (const name of ['allow_file_content', 'is_default'])
    form.elements[name].checked = !!p?.[name];
  $('provider-vendor').value = providerVendor(p);
  syncProviderModels();
  renderProviderList();
}

$('new-provider').onclick = () => {
  selectProvider();
  $('provider-form').elements.name.focus();
};

$('provider-form').onsubmit = task(async () => {
  const form = $('provider-form');
  const data = Object.fromEntries(new FormData(form));
  data.enabled = form.elements.enabled.checked;
  data.allow_file_content = form.elements.allow_file_content.checked;
  data.is_default = form.elements.is_default.checked;
  // Match webcool's Responses compatibility flags for Qwen and stateless vendors.
  if (data.protocol === 'openai_responses') {
    const stateless = /^(deepseek|kimi)/i.test(data.model);
    const qwen = !stateless && (/^qwen/i.test(data.model) || providerVendor(data) === 'qwen');
    if (stateless || qwen) {
      data.responses_store = qwen;
      data.responses_background = false;
      data.responses_compact = false;
      if (qwen) {
        data.responses_strict_tools = false;
        data.responses_reasoning_summary = 'none';
        data.responses_cache_ttl = 'none';
      }
    }
  }
  const saved = await api('/providers/save', data);
  form.elements.api_key.value = '';
  await providers();
  selectProvider(saved.provider.id);
  notice(t('配置已保存'));
});

$('test-provider').onclick = task(async () => {
  const id = $('provider-form').elements.id.value;
  if (!id) throw new Error(t('请先保存模型配置'));
  const data = await api('/providers/test', { id });
  notice(data.message || t('连接测试完成'));
});

$('delete-provider').onclick = task(async () => {
  const id = $('provider-form').elements.id.value;
  if (!id) return;
  if (!confirm(t('删除此模型配置？'))) return;
  await api('/providers/delete', { id });
  $('provider-form').reset();
  await providers();
  selectProvider();
});

$('refresh-files').onclick = task(() => files());

$('collapse-files').onclick = task(() => {
  fileTree.expanded.clear();
  return files();
});

for (const name of ['files', 'changes', 'tools'])
  $(name + '-tab').onclick = () => {
    if (name === 'changes') {
      state.reviewFile = null;
      renderChanges(state.snapshot || { changes: [] });
      selectBodyTab('review');
      return;
    }
    for (const n of ['files', 'tools']) {
      $(n + '-pane').hidden = n !== name;
      $(n + '-tab').classList.toggle('active', n === name);
    }
  };

function projectDetail(title, subtitle, action, project = state.project) {
  uiText($('detail-title'), () => t(title));
  uiText($('detail-subtitle'), () => t(subtitle));
  const icon = $(action).querySelector('svg');
  $('detail-icon').replaceChildren(...(icon ? [icon.cloneNode(true)] : []));
  $('detail-dialog').querySelector('.dialog-notice')?.remove();
  const content = $('detail-content');
  content.replaceChildren();
  if (project) {
    const context = node('div', undefined, 'project-dialog-context');
    context.append(node('strong', project.title || project.project_path), node('small', project.project_path));
    content.append(context);
  }
  return content;
}

function detailSection(title) {
  const section = node('section', undefined, 'project-dialog-section');
  section.append(node('h3', () => t(title)));
  return section;
}

function detailField(title, control) {
  const label = node('label', undefined, 'project-dialog-field');
  label.append(node('span', () => t(title)), control);
  return label;
}

const projectStatusLabels = {
  pending: '待开始', running: '进行中', in_progress: '进行中',
  completed: '已完成', failed: '失败', blocked: '已阻塞', cancelled: '已取消', paused: '已暂停'
};

$('history').onclick = task(async () => {
  const project = state.project;
  const data = await api('/runs');
  if (state.project !== project) return;
  const content = projectDetail('运行记录', '查看项目的执行状态与历史结果', 'history', project);
  const list = node('div', undefined, 'project-run-list');
  for (const r of data.runs || []) {
    if (project && r.path !== project.project_path && r.project_path !== project.project_path) continue;
    const row = button('', async () => {
      $('detail-dialog').close();
      resetRun();
      state.run = r.run_id || r.id;
      await poll(state.epoch);
    });
    row.className = 'project-run-row';
    const info = node('span', undefined, 'project-run-info');
    info.append(node('strong', r.model || r.agent_id || r.run_id || r.id));
    const timestamp = r.started_at ? new Date(Number(r.started_at) * 1000) : null;
    info.append(node('small', timestamp && !Number.isNaN(timestamp.getTime()) ? timestamp.toLocaleString() : r.run_id || r.id));
    const status = node('span', () => t(projectStatusLabels[r.status] || r.status), 'project-role');
    status.dataset.status = r.status;
    row.title = r.run_id || r.id;
    row.append(info, status, node('span', '›', 'project-run-arrow'));
    list.append(row);
  }
  if (!list.children.length) list.append(node('p', () => t('暂无运行记录'), 'project-dialog-empty'));
  content.append(list);
  $('detail-dialog').showModal();
});

$('plan').onclick = task(async () => {
  if (!state.project) throw new Error(t('请先选择项目'));
  const project = state.project;
  const data = await api('/projects?id=' + encodeURIComponent(project.id));
  if (state.project !== project) return;
  const p = data.project || data;
  const content = projectDetail('项目计划', '设定项目目标，规划任务与进度', 'plan', project);
  const planning = detailSection('项目目标');
  const goal = node('textarea');
  goal.value = p.goal || '';
  goal.rows = 4;
  goal.setAttribute('aria-label', t('项目目标'));
  const scale = node('select');
  for (const [value, label] of [['quick', '小型项目'], ['standard', '中型项目'], ['large', '大型项目']]) {
    const option = node('option', () => t(label));
    option.value = value;
    scale.append(option);
  }
  const actions = node('div', undefined, 'project-plan-actions');
  const generate = button(() => t('生成并保存任务计划'), async () => {
    generate.disabled = true;
    try {
      const proposal = await api('/projects/plan/propose', { project_id: project.id, goal: goal.value, scale: scale.value });
      await api('/projects/plan', {
        project_id: project.id, plan_version: p.plan_version,
        goal: proposal.goal, modules: proposal.modules, tasks: proposal.tasks
      });
      await projects();
      $('detail-dialog').close();
      $('plan').click();
    } finally { generate.disabled = false; }
  });
  generate.className = 'primary';
  actions.append(detailField('项目规模', scale), generate);
  planning.append(goal, actions);
  const tasks = detailSection('任务进度');
  const completed = (p.tasks || []).filter(item => item.status === 'completed').length;
  const progress = node('progress');
  progress.max = Math.max(1, (p.tasks || []).length);
  progress.value = completed;
  progress.setAttribute('aria-label', t('任务进度'));
  const progressRow = node('div', undefined, 'project-plan-progress');
  progressRow.append(progress, node('small', `${completed} / ${(p.tasks || []).length}`));
  tasks.append(progressRow);
  for (const item of p.tasks || []) {
    const row = node('div', undefined, 'project-task-row');
    row.append(node('strong', item.title));
    const status = node('select');
    status.setAttribute('aria-label', item.title + ' · ' + t('任务进度'));
    for (const value of ['pending', 'in_progress', 'completed', 'blocked', 'failed']) {
      const option = node('option', () => t(projectStatusLabels[value]));
      option.value = value;
      status.append(option);
    }
    status.value = item.status;
    status.onchange = task(async () => {
      status.disabled = true;
      try {
        await api('/projects/tasks/status', { project_id: project.id, task_id: item.id, status: status.value, plan_version: p.plan_version });
        $('detail-dialog').close();
        $('plan').click();
      } catch (error) { status.value = item.status; throw error; }
      finally { status.disabled = false; }
    });
    row.append(status);
    tasks.append(row);
  }
  if (!(p.tasks || []).length) tasks.append(node('p', () => t('暂无任务，填写项目目标后生成计划'), 'project-dialog-empty'));
  content.append(planning, tasks);
  $('detail-dialog').showModal();
});

$('export-session').onclick = task(async () => {
  if (!state.session) throw new Error(t('请先选择一个已有会话'));
  const project = state.project;
  const saved = await api('/sessions/save', { session_id: state.session });
  if (state.project !== project) return;
  const content = projectDetail('导出会话', '会话已保存到项目目录', 'export-session', project);
  const result = detailSection('导出位置');
  result.append(node('code', saved.path, 'project-export-path'));
  content.append(result);
  $('detail-dialog').showModal();
  await files();
});

const projectMenu = document.querySelector('.body-project-menu');
projectMenu.addEventListener('click', event => {
  if (event.target.closest('button')) projectMenu.open = false;
});
document.addEventListener('click', event => {
  if (!projectMenu.contains(event.target)) projectMenu.open = false;
});
projectMenu.addEventListener('keydown', event => {
  if (event.key === 'Escape') {
    projectMenu.open = false;
    projectMenu.querySelector('summary').focus();
  }
});

for (const b of document.querySelectorAll('[data-close]'))
  b.onclick = () => b.closest('dialog').close();

$('login-dialog').addEventListener('cancel', e => e.preventDefault());

// Expand the body without recreating its conversation or Monaco editors.
(() => {
  const layout = $('coding-layout');
  const maximize = $('body-maximize');
  const fullscreen = $('body-fullscreen');
  function render() {
    const full = document.fullscreenElement === document.documentElement;
    document.documentElement.classList.toggle('body-fullscreen', full);
    maximize.disabled = full;
    maximize.setAttribute('aria-pressed', String(layout.classList.contains('body-maximized')));
    fullscreen.setAttribute('aria-pressed', String(full));
    for (const [button, label] of [
      [maximize, layout.classList.contains('body-maximized') ? '还原窗口' : '最大化'],
      [fullscreen, full ? '退出全屏' : '全屏']
    ]) {
      button.dataset.i18nTitle = label;
      button.setAttribute('data-i18n-aria-label', label);
      button.title = t(label);
      button.setAttribute('aria-label', t(label));
    }
    requestAnimationFrame(() => {
      codeEditors.viewer?.layout();
      codeEditors.editing?.diff?.layout();
      codeEditors.editing?.editor?.layout();
    });
  }
  maximize.onclick = () => {
    layout.classList.toggle('body-maximized');
    render();
  };
  fullscreen.onclick = task(async () => {
    if (document.fullscreenElement === document.documentElement) {
      await document.exitFullscreen();
    } else {
      if (!document.fullscreenEnabled || !document.documentElement.requestFullscreen)
        throw new Error(t('当前浏览器不支持全屏，请使用最大化。'));
      await document.documentElement.requestFullscreen();
    }
  });
  document.addEventListener('fullscreenchange', render);
  document.addEventListener('DOMContentLoaded', render, { once: true });
})();

// Keep the coding panes independently resizable without changing their contents.
(() => {
  const layout = $('coding-layout');
  const left = $('left-splitter');
  const right = $('right-splitter');
  const mobile = matchMedia('(max-width: 620px)');
  const minimum = { left: 160, center: 320, right: 240 };
  const storageKey = 'coolder.pane-widths.v1';
  let widths = null;
  let drag = null;
  try {
    const saved = JSON.parse(localStorage.getItem(storageKey));
    if (saved && Number.isFinite(saved.left) && Number.isFinite(saved.right)) widths = saved;
  } catch {
    // Resizing also works when browser storage is unavailable.
  }

  const collapsed = side => layout.classList.contains(side + '-collapsed');
  const occupied = side => (collapsed(side) ? 34 : widths[side]);
  for (const side of ['left', 'right']) {
    const toggle = close => {
      finish();
      layout.classList.toggle(side + '-collapsed', close);
      for (const action of ['collapse', 'expand'])
        $(action + '-' + side).setAttribute('aria-expanded', String(!close));
      render();
      $(close ? 'expand-' + side : 'collapse-' + side).focus();
    };
    $('collapse-' + side).onclick = () => toggle(true);
    $('expand-' + side).onclick = () => toggle(false);
  }

  function available() {
    return layout.clientWidth - 12;
  }

  function render() {
    if (
      mobile.matches ||
      layout.classList.contains('body-maximized') ||
      document.documentElement.classList.contains('body-fullscreen')
    )
      return;
    const total = available();
    widths ??= { left: 230, right: total * 0.34 };
    if (!collapsed('left'))
      widths.left = Math.max(
        minimum.left,
        Math.min(widths.left, total - minimum.center - (collapsed('right') ? 34 : minimum.right))
      );
    if (!collapsed('right'))
      widths.right = Math.max(
        minimum.right,
        Math.min(widths.right, total - minimum.center - occupied('left'))
      );
    layout.style.setProperty('--project-width', widths.left + 'px');
    layout.style.setProperty('--inspector-width', widths.right + 'px');
    for (const [handle, side, other] of [
      [left, 'left', 'right'],
      [right, 'right', 'left']
    ]) {
      handle.setAttribute('aria-valuemin', minimum[side]);
      handle.setAttribute('aria-valuemax', Math.floor(total - minimum.center - occupied(other)));
      handle.setAttribute('aria-valuenow', Math.round(widths[side]));
      handle.setAttribute('aria-valuetext', Math.round(widths[side]) + t(' 像素'));
    }
  }

  function save() {
    try {
      localStorage.setItem(storageKey, JSON.stringify(widths));
    } catch {
      // Browser privacy settings must not interrupt a drag.
    }
  }

  function resize(side, value) {
    const other = side === 'left' ? 'right' : 'left';
    widths[side] = Math.max(
      minimum[side],
      Math.min(value, available() - minimum.center - occupied(other))
    );
    render();
  }

  function finish() {
    if (!drag) return;
    const { handle, pointerId } = drag;
    drag = null;
    handle.classList.remove('dragging');
    document.body.classList.remove('resizing-panes');
    if (handle.hasPointerCapture(pointerId)) handle.releasePointerCapture(pointerId);
    save();
  }

  for (const [handle, side] of [
    [left, 'left'],
    [right, 'right']
  ]) {
    handle.addEventListener('pointerdown', event => {
      if (event.button !== 0 || mobile.matches || drag) return;
      event.preventDefault();
      handle.focus();
      drag = { handle, pointerId: event.pointerId, x: event.clientX, width: widths[side] };
      handle.setPointerCapture(event.pointerId);
      handle.classList.add('dragging');
      document.body.classList.add('resizing-panes');
    });
    handle.addEventListener('pointermove', event => {
      if (!drag || drag.handle !== handle || drag.pointerId !== event.pointerId) return;
      resize(side, drag.width + (event.clientX - drag.x) * (side === 'left' ? 1 : -1));
    });
    for (const event of ['pointerup', 'pointercancel', 'lostpointercapture'])
      handle.addEventListener(event, finish);
    handle.addEventListener('dblclick', () => {
      widths = null;
      render();
      save();
    });
    handle.addEventListener('keydown', event => {
      if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
      event.preventDefault();
      const delta = (event.key === 'ArrowRight' ? 1 : -1) * (event.shiftKey ? 40 : 10);
      const value =
        event.key === 'Home'
          ? minimum[side]
          : event.key === 'End'
            ? available()
            : widths[side] + delta * (side === 'left' ? 1 : -1);
      resize(side, value);
      save();
    });
  }
  window.addEventListener('blur', finish);
  mobile.addEventListener('change', () => {
    finish();
    render();
  });
  new ResizeObserver(() => {
    finish();
    render();
  }).observe(layout);
  render();
})();


$('project-settings').onclick = task(async () => {
  const project = state.project;
  if (!project) return;
  const identity = { owner_id: project.owner_id, project_id: project.id };
  const endpoint = '/api/v1/collaborate/settings';
  const data = await request(endpoint + '?' + new URLSearchParams(identity));
  if (state.project !== project) return;
  project.permission = data.permission;
  $('edit-file').disabled = !state.file || state.file.truncated || data.permission === 'read';
  const content = projectDetail('项目设置', '管理项目成员与访问权限', 'project-settings', project);
  const members = detailSection('项目成员');
  const labels = { owner: '拥有者', read: '只读', write: '读写' };
  const update = async (username, permission) => {
    await request(endpoint, { ...identity, username, permission });
    notice(t('项目成员已更新'));
    $('project-settings').click();
  };
  for (const member of data.members) {
    const row = node('div', undefined, 'project-member');
    const avatar = node('span', member.username.slice(0, 1).toUpperCase(), 'project-member-avatar');
    avatar.setAttribute('aria-hidden', 'true');
    const identity = node('div', undefined, 'project-member-identity');
    identity.append(node('strong', member.username), node('small', () => t(labels[member.permission])));
    row.append(avatar, identity);
    if (data.permission === 'owner' && member.permission !== 'owner') {
      const permission = node('select');
      permission.setAttribute('aria-label', member.username + ' ' + t('权限'));
      for (const value of ['read', 'write']) {
        const option = node('option', () => t(labels[value]));
        option.value = value;
        permission.append(option);
      }
      permission.value = member.permission;
      const controls = node('div', undefined, 'project-member-controls');
      const remove = button(() => t('移除成员'), () => update(member.username, 'remove'));
      remove.className = 'project-member-remove';
      controls.append(permission, button(() => t('保存权限'), () => update(member.username, permission.value)), remove);
      row.append(controls);
    } else {
      row.append(node('span', () => t(labels[member.permission]), 'project-role'));
    }
    members.append(row);
  }
  content.append(members);
  if (data.permission === 'owner') {
    const form = node('form', undefined, 'project-invite-form');
    const username = node('select');
    username.required = true;
    username.size = 5;
    username.className = 'project-user-list';
    username.setAttribute('aria-label', t('选择用户'));
    const search = node('input');
    search.type = 'search';
    search.placeholder = t('搜索用户名');
    search.setAttribute('aria-label', t('搜索用户名'));
    search.autocomplete = 'off';
    const picker = node('div', undefined, 'project-user-picker');
    picker.append(detailField('搜索用户名', search), detailField('选择用户', username));
    const permission = node('select');
    permission.setAttribute('aria-label', t('权限'));
    for (const value of ['read', 'write']) {
      const option = node('option', () => t(labels[value]));
      option.value = value;
      permission.append(option);
    }
    const submit = node('button', () => t('添加参与者'));
    submit.type = 'submit';
    submit.className = 'primary';
    const controls = node('div', undefined, 'project-invite-controls');
    controls.append(detailField('权限', permission), submit);
    form.append(picker, controls);
    const renderUsers = () => {
      const selected = username.value;
      const query = search.value.trim().toLocaleLowerCase();
      username.replaceChildren();
      const prompt = node('option', () => t('请选择用户'));
      prompt.value = '';
      prompt.disabled = true;
      username.append(prompt);
      const users = (data.users || []).filter(user => user.username.toLocaleLowerCase().includes(query));
      for (const user of users) {
        const reason = user.permission === 'owner' ? '拥有者' : !user.enabled ? '已停用' : user.permission ? '已加入项目' : '';
        const option = node('option', () => reason ? `${user.username} · ${t(reason)}` : user.username);
        option.value = user.username;
        option.disabled = !!reason;
        username.append(option);
      }
      if (!users.length) {
        const empty = node('option', () => t('没有匹配的用户'));
        empty.disabled = true;
        empty.value = '';
        username.append(empty);
      }
      const preserved = Array.from(username.options).some(option => option.value === selected && !option.disabled);
      username.value = preserved ? selected : '';
      submit.disabled = !username.value;
    };
    search.oninput = renderUsers;
    username.onchange = () => { submit.disabled = !username.value; };
    renderUsers();
    form.onsubmit = task(async () => {
      submit.disabled = true;
      try { await update(username.value.trim(), permission.value); }
      finally { submit.disabled = !username.value; }
    });
    const invite = detailSection('添加参与者');
    invite.append(node('p', () => t('选择用户并设置权限。已加入项目或已停用的用户不可重复添加。'), 'project-dialog-hint'), form);
    content.append(invite);
  }
  $('detail-dialog').showModal();
});
