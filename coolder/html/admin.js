'use strict';

let identity = null;
let needsSetup = false;

async function authStatus() {
  const status = await request('/api/v1/auth/status');
  identity = status.authenticated ? status : null;
  needsSetup = !status.initialized;
  $('admin-settings').hidden = !identity?.admin;
  $('password-settings').hidden = !identity;
  $('logout').hidden = !identity;
  uiText($('login-title'), () =>
    needsSetup ? t('欢迎使用 coolder · 创建管理员') : t('登录 coolder')
  );
  uiText($('login-description'), () =>
    needsSetup
      ? t('首次使用，请创建管理员账户。随后可在管理控制台创建普通用户。密码至少 10 字节。')
      : t('使用管理员为你创建的账户登录。')
  );
  $('confirm-label').hidden = !needsSetup;
  const form = $('login-form');
  form.elements.confirm.required = needsSetup;
  form.elements.password.autocomplete = needsSetup ? 'new-password' : 'current-password';
  if (!identity && !$('login-dialog').open) $('login-dialog').showModal();
  return status;
}

$('login-form').onsubmit = async e => {
  e.preventDefault();
  const submit = e.target.querySelector('button');
  submit.disabled = true;
  try {
    const data = Object.fromEntries(new FormData(e.target));
    if (needsSetup && data.password !== data.confirm) throw new Error(t('两次输入的密码不一致'));
    await request(needsSetup ? '/api/v1/auth/register' : '/api/v1/auth/login', data);
    e.target.reset();
    $('login-dialog').close();
    plainText($('login-error'), '');
    await init();
  } catch (error) {
    plainText($('login-error'), error.message);
    await authStatus().catch(() => {});
  } finally {
    submit.disabled = false;
  }
};

$('logout').onclick = task(async () => {
  await request('/api/v1/auth/logout', {});
  location.reload();
});

async function users() {
  const data = await request('/api/v1/auth/users');
  $('user-list').replaceChildren();
  for (const user of data.users) {
    const row = node('div', undefined, 'user-row');
    row.append(
      node('strong', user.username),
      node('span', () =>
        user.admin ? t('管理员') : user.enabled ? t('普通用户 · 已启用') : t('普通用户 · 已停用')
      )
    );
    if (!user.admin) {
      row.append(
        button(
          () => (user.enabled ? t('停用') : t('启用')),
          async () => {
            await request('/api/v1/auth/users/update', {
              username: user.username,
              enabled: !user.enabled
            });
            await users();
          }
        ),
        button(
          () => t('重置密码'),
          () => {
            const form = $('user-reset-form');
            form.reset();
            form.hidden = false;
            form.elements.username.value = user.username;
            form.elements.password.focus();
          }
        )
      );
    }
    $('user-list').append(row);
  }
}

const adminTabs = [
  ['users-tab', 'users-panel'],
  ['policy-tab', 'policy-panel'],
  ['settings', 'providers-panel']
];
let adminPolicyLoaded = false;
let adminProvidersLoaded = false;

function selectAdminTab(id) {
  for (const [tab, panel] of adminTabs) {
    const selected = id === tab;
    $(panel).hidden = !selected;
    $(tab).setAttribute('aria-selected', String(selected));
    $(tab).tabIndex = selected ? 0 : -1;
  }
  $('admin-content').classList.toggle('providers-view', id === 'settings');
  $('admin-content').scrollTop = 0;
}

$('admin-settings').onclick = task(async () => {
  await authStatus();
  if (!identity?.admin) throw new Error(t('需要管理员权限'));
  if (!$('admin-dialog').open && $('admin-restore').hidden) {
    await users();
    adminPolicyLoaded = false;
    adminProvidersLoaded = false;
    selectAdminTab('users-tab');
  }
  openAdminWindow();
});
$('users-tab').onclick = task(async () => {
  selectAdminTab('users-tab');
  await users();
});
$('settings').onclick = task(async () => {
  selectAdminTab('settings');
  if (!adminProvidersLoaded) {
    await providers();
    selectProvider();
    adminProvidersLoaded = true;
  }
});
for (const [id] of adminTabs) {
  $(id).addEventListener('keydown', event => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
    event.preventDefault();
    const index = adminTabs.findIndex(([tab]) => tab === id);
    const next =
      event.key === 'Home'
        ? 0
        : event.key === 'End'
          ? adminTabs.length - 1
          : (index + (event.key === 'ArrowRight' ? 1 : -1) + adminTabs.length) % adminTabs.length;
    $(adminTabs[next][0]).focus();
    $(adminTabs[next][0]).click();
  });
}

$('user-create-form').onsubmit = task(async () => {
  const form = $('user-create-form');
  await request('/api/v1/auth/users/create', Object.fromEntries(new FormData(form)));
  form.reset();
  await users();
  notice(t('普通用户已创建'));
});
$('user-reset-form').onsubmit = task(async () => {
  const form = $('user-reset-form');
  await request('/api/v1/auth/users/update', Object.fromEntries(new FormData(form)));
  form.reset();
  form.hidden = true;
  notice(t('密码已重置，该用户需重新登录'));
});
$('password-settings').onclick = task(async () => {
  await openPersonalSettings();
  $('password-form').reset();
  $('password-dialog').showModal();
});
$('password-form').onsubmit = task(async () => {
  const data = Object.fromEntries(new FormData($('password-form')));
  if (data.password !== data.confirm) throw new Error(t('两次输入的密码不一致'));
  await request('/api/v1/auth/password', data);
  $('password-form').reset();
  $('password-dialog').close();
  notice(t('密码已更新，其他登录会话已退出'));
});

function clearWorkspace() {
  clearComposerAttachments();
  closeAllSourceFiles(true);
  hideAdminWindow();
  resetRun();
  state.project = null;
  $('project-settings').disabled = true;
  state.projects = [];
  state.providers = [];
  state.session = '';
  state.file = null;
  state.folder = '';
  for (const id of [
    'messages',
    'projects',
    'sessions',
    'files',
    'changes',
    'tools',
    'provider',
    'provider-list',
    'detail-content',
    'user-list'
  ])
    $(id).replaceChildren();
  for (const id of ['provider-form', 'user-create-form', 'user-reset-form', 'password-form'])
    $(id).reset();
  $('user-reset-form').hidden = true;
  uiText($('project-title'), () => t('开始一个项目'));
  uiText($('project-path'), () => t('创建项目或导入已有目录'));
  uiText($('file-title'), () => t('文件预览'));
  uiText($('file-content'), () => t('选择文件查看内容'));
  $('edit-file').disabled = true;
  $('prompt').value = '';
}

async function init() {
  try {
    const status = await authStatus();
    if (!identity) return;
    await loadPersonalSettings();
    clearWorkspace();
    uiText(
      $('connection'),
      () => `${identity.username} · ${identity.admin ? t('管理员') : t('普通用户')}`
    );
    if (!status.ai_agent_allowed) {
      $('send').disabled = true;
      notice(t('AI 编程权限已关闭，请联系管理员'));
      return;
    }
    await Promise.all([providers(), projects()]);
    if (state.projects.length) await selectProject(state.projects[0]);
  } catch (error) {
    if (!$('login-dialog').open) notice(error);
  }
}

init();

// Adapted from webcool/html/js/admin-ai-policy.js; field names and limits match webcool.
(() => {
  const api = { adminAiPolicy: '/api/v1/admin/ai-policy' };
  const fetchJson = (url, options) =>
    request(url, options?.body ? JSON.parse(options.body) : undefined);
  const showStatus = message => notice(message);
  const adminAiPolicySubmit = $('admin-ai-policy-submit');
  // Administrator-only controls shared by the current coding agent and future
  // agent types. The server remains authoritative; browser values are untrusted.
  var adminAiFields = {
    read_chunk_kib: document.getElementById('admin-ai-read-chunk'),
    read_file_limit_kib: document.getElementById('admin-ai-read-file-limit'),
    enabled: document.getElementById('admin-ai-enabled'),
    allow_admin: document.getElementById('admin-ai-allow-admin'),
    allow_users: document.getElementById('admin-ai-allow-users'),
    allow_browser_debug: document.getElementById('admin-ai-allow-browser-debug'),
    allow_build_network: document.getElementById('admin-ai-allow-build-network'),
    allow_users_shared_projects: document.getElementById('admin-ai-allow-users-shared-projects'),
    allow_users_local_projects: document.getElementById('admin-ai-allow-users-local-projects'),
    language_tools: document.getElementById('admin-ai-language-tools'),
    node_executable_path: document.getElementById('admin-ai-node-executable-path'),
    python_executable_path: document.getElementById('admin-ai-python-executable-path'),
    javac_executable_path: document.getElementById('admin-ai-javac-executable-path'),
    java_executable_path: document.getElementById('admin-ai-java-executable-path'),
    go_executable_path: document.getElementById('admin-ai-go-executable-path'),
    cargo_executable_path: document.getElementById('admin-ai-cargo-executable-path'),
    make_executable_path: document.getElementById('admin-ai-make-executable-path'),
    swift_executable_path: document.getElementById('admin-ai-swift-executable-path'),
    dotnet_executable_path: document.getElementById('admin-ai-dotnet-executable-path'),
    kotlinc_executable_path: document.getElementById('admin-ai-kotlinc-executable-path'),
    php_executable_path: document.getElementById('admin-ai-php-executable-path'),
    dmd_executable_path: document.getElementById('admin-ai-dmd-executable-path'),
    sensitive_paths: document.getElementById('admin-ai-sensitive-paths'),
    max_active_runs_per_user: document.getElementById('admin-ai-max-runs'),
    max_output_tokens: document.getElementById('admin-ai-max-tokens'),
    quick_mode_output_tokens: document.getElementById('admin-ai-quick-max-tokens'),
    provider_connect_timeout_seconds: document.getElementById('admin-ai-provider-connect-timeout'),
    provider_stream_timeout_seconds: document.getElementById('admin-ai-provider-stream-timeout'),
    quick_mode_tool_calls: document.getElementById('admin-ai-quick-tool-calls'),
    standard_mode_tool_calls: document.getElementById('admin-ai-standard-tool-calls'),
    large_mode_tool_calls: document.getElementById('admin-ai-large-tool-calls'),
    browser_debug_report_threshold: document.getElementById(
      'admin-ai-browser-debug-report-threshold'
    ),
    max_no_progress_tool_calls: document.getElementById('admin-ai-max-no-progress-tool-calls'),
    tool_context_compaction_kib: document.getElementById('admin-ai-tool-context-compaction'),
    sandbox_timeout_ms: document.getElementById('admin-ai-timeout'),
    sandbox_cpu_seconds: document.getElementById('admin-ai-cpu'),
    sandbox_memory_mib: document.getElementById('admin-ai-memory'),
    sandbox_process_count: document.getElementById('admin-ai-processes'),
    sandbox_output_kib: document.getElementById('admin-ai-output')
  };
  var adminAiExecutableKeys = [
    'node_executable_path',
    'python_executable_path',
    'javac_executable_path',
    'java_executable_path',
    'go_executable_path',
    'cargo_executable_path',
    'make_executable_path',
    'swift_executable_path',
    'dotnet_executable_path',
    'kotlinc_executable_path',
    'php_executable_path',
    'dmd_executable_path'
  ];
  var adminAiLanguageToolCheckboxes = Array.prototype.slice.call(
    document.querySelectorAll('[data-admin-ai-language-tool]')
  );
  var adminAiSavedValues = {};
  var adminAiSaveButtons = {};
  var adminAiFieldSaving = {};
  var adminAiPolicySaving = false;

  function syncCurrentAiAgentAccess(data) {
    if (!identity) return;
    identity.ai_agent_allowed =
      data.enabled !== false &&
      (identity.admin ? data.allow_admin !== false : data.allow_users !== false);
    $('send').disabled = !identity.ai_agent_allowed;
  }

  function normalizedAdminAiLanguageTools(value) {
    const selected = {};
    String(value || '')
      .split(',')
      .forEach(function (item) {
        const key = item.trim().toLowerCase();
        if (key) selected[key] = true;
      });
    return adminAiLanguageToolCheckboxes
      .map(function (checkbox) {
        return checkbox.value;
      })
      .filter(function (key) {
        return selected[key];
      })
      .join(',');
  }

  function selectedAdminAiLanguageTools() {
    return adminAiLanguageToolCheckboxes
      .filter(function (checkbox) {
        return checkbox.checked;
      })
      .map(function (checkbox) {
        return checkbox.value;
      })
      .join(',');
  }

  function syncAdminAiLanguageToolCheckboxes(value) {
    const normalized = normalizedAdminAiLanguageTools(value);
    const selected = {};
    normalized.split(',').forEach(function (key) {
      if (key) selected[key] = true;
    });
    adminAiLanguageToolCheckboxes.forEach(function (checkbox) {
      checkbox.checked = !!selected[checkbox.value];
    });
    if (adminAiFields.language_tools) {
      adminAiFields.language_tools.value = selectedAdminAiLanguageTools();
    }
  }

  function adminAiFieldValue(field) {
    if (field === adminAiFields.language_tools) return selectedAdminAiLanguageTools();
    return field.type === 'checkbox'
      ? field.checked
      : field.type === 'number'
        ? Number(field.value)
        : field.value.trim();
  }

  function adminAiServerFieldValue(field, value) {
    if (field === adminAiFields.language_tools) return normalizedAdminAiLanguageTools(value);
    if (field === adminAiFields.allow_build_network) return value === true;
    if (field.type === 'checkbox') return value !== false;
    if (field.type === 'number') return Number(value);
    return value == null ? '' : String(value).trim();
  }

  function adminAiValuesEqual(left, right) {
    return typeof left === typeof right && left === right;
  }

  function adminAiFieldDirty(key) {
    const field = adminAiFields[key];
    return (
      !!field &&
      !field.disabled &&
      Object.prototype.hasOwnProperty.call(adminAiSavedValues, key) &&
      !adminAiValuesEqual(adminAiFieldValue(field), adminAiSavedValues[key])
    );
  }

  function updateAdminAiPolicyDirtyState() {
    let anyDirty = false;
    Object.keys(adminAiFields).forEach(function (key) {
      const dirty = adminAiFieldDirty(key);
      anyDirty = anyDirty || dirty;
      const button = adminAiSaveButtons[key];
      if (button) button.disabled = !dirty || adminAiPolicySaving || !!adminAiFieldSaving[key];
    });
    if (adminAiPolicySubmit) {
      adminAiPolicySubmit.disabled =
        !anyDirty ||
        adminAiPolicySaving ||
        Object.keys(adminAiFieldSaving).some(function (key) {
          return adminAiFieldSaving[key];
        });
    }
  }

  function renderAdminAiExecutableDetection(key, data) {
    const field = adminAiFields[key];
    const detection = document.getElementById(
      'admin-ai-' + key.replace('_executable_path', '').replace(/_/g, '-') + '-detection'
    );
    const detected = data[key + '_detected'] || '';
    if (field) field.placeholder = detected;
    if (detection)
      uiText(detection, () =>
        detected ? t('当前检测到：') + detected : t('尚未检测到，请填写可执行文件的绝对路径。')
      );
  }

  function renderAdminAiBrowserAvailability(data) {
    adminAiFields.allow_browser_debug.disabled = data.browser_debug_available !== true;
    uiText($('admin-ai-browser-debug-detection'), () =>
      data.browser_debug_available
        ? t('已检测到浏览器调试环境，可设置是否允许智能体使用。')
        : data.browser_debug_unavailable_reason || t('浏览器调试暂不可用。')
    );
  }

  function renderAdminAiPolicyField(key, data, updateField) {
    const field = adminAiFields[key];
    if (!field || data[key] == null) return;
    const savedValue = adminAiServerFieldValue(field, data[key]);
    adminAiSavedValues[key] = savedValue;
    if (updateField !== false) {
      if (field === adminAiFields.language_tools) syncAdminAiLanguageToolCheckboxes(savedValue);
      else if (field.type === 'checkbox') field.checked = savedValue;
      else field.value = savedValue;
    }
    if (adminAiExecutableKeys.indexOf(key) !== -1) {
      renderAdminAiExecutableDetection(key, data);
    }
  }

  async function saveAdminAiPolicyField(key, button) {
    const field = adminAiFields[key];
    if (!field || field.disabled || !button || !field.reportValidity()) return;
    const body = {};
    const submittedValue = adminAiFieldValue(field);
    body[key] = submittedValue;
    adminAiFieldSaving[key] = true;
    updateAdminAiPolicyDirtyState();
    try {
      const data = await fetchJson(api.adminAiPolicy, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      });
      // Keep other unsaved form edits intact. A per-item save must only
      // reconcile the field submitted by this button.
      // If the user typed again while this request was in flight, retain that
      // newer edit while advancing the saved baseline to the server response.
      renderAdminAiPolicyField(
        key,
        data,
        adminAiValuesEqual(adminAiFieldValue(field), submittedValue)
      );
      renderAdminAiBrowserAvailability(data);
      syncCurrentAiAgentAccess(data);
      showStatus(t('AI智能体设置项已保存'), 'ok');
    } catch (err) {
      showStatus(t('保存AI智能体策略失败：') + err.message, 'err');
    } finally {
      adminAiFieldSaving[key] = false;
      updateAdminAiPolicyDirtyState();
    }
  }

  // Add a compact save action beside every policy input. Creating these from
  // the authoritative field map keeps new policy settings from silently
  // missing the per-item workflow in future releases.
  Object.keys(adminAiFields).forEach(function (key) {
    const field = adminAiFields[key];
    const container =
      field && field.closest
        ? field.closest('[data-admin-ai-field-container]') || field.closest('label')
        : null;
    if (!container || container.querySelector('.admin-ai-field-save')) return;
    container.classList.add('admin-ai-policy-field');
    if (field.type === 'checkbox') {
      container.classList.add('admin-ai-policy-field-checkbox');
    }
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'admin-ai-field-save';
    uiText(button, () => t('保存此项'));
    button.title = t('保存此设置项');
    button.setAttribute(
      'aria-label',
      t('保存：') +
        (
          container.querySelector('span, strong')?.textContent || container.firstChild.textContent
        ).trim()
    );
    button.disabled = true;
    adminAiSaveButtons[key] = button;
    button.addEventListener('click', function () {
      saveAdminAiPolicyField(key, button);
    });
    container.appendChild(button);
    if (field.type !== 'hidden') {
      field.addEventListener('input', updateAdminAiPolicyDirtyState);
      field.addEventListener('change', updateAdminAiPolicyDirtyState);
    }
  });

  adminAiLanguageToolCheckboxes.forEach(function (checkbox) {
    checkbox.addEventListener('change', function () {
      if (adminAiFields.language_tools) {
        adminAiFields.language_tools.value = selectedAdminAiLanguageTools();
      }
      updateAdminAiPolicyDirtyState();
    });
  });

  function renderAdminAiPolicy(data) {
    renderAdminAiBrowserAvailability(data);
    Object.keys(adminAiFields).forEach(function (key) {
      renderAdminAiPolicyField(key, data, true);
    });
    adminAiExecutableKeys.forEach(function (key) {
      renderAdminAiExecutableDetection(key, data);
    });
    updateAdminAiPolicyDirtyState();
  }

  async function saveAdminAiPolicy() {
    adminAiPolicySaving = true;
    updateAdminAiPolicyDirtyState();
    const body = {};
    Object.keys(adminAiFields).forEach(function (key) {
      const field = adminAiFields[key];
      if (!field || field.disabled || !adminAiFieldDirty(key)) return;
      body[key] = adminAiFieldValue(field);
    });
    try {
      const data = await fetchJson(api.adminAiPolicy, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      });
      Object.keys(body).forEach(function (key) {
        renderAdminAiPolicyField(
          key,
          data,
          adminAiValuesEqual(adminAiFieldValue(adminAiFields[key]), body[key])
        );
      });
      renderAdminAiBrowserAvailability(data);
      syncCurrentAiAgentAccess(data);
      showStatus(t('AI智能体策略已保存'), 'ok');
    } catch (err) {
      showStatus(t('保存AI智能体策略失败：') + err.message, 'err');
    } finally {
      adminAiPolicySaving = false;
      updateAdminAiPolicyDirtyState();
    }
  }

  for (const [key, field] of Object.entries(adminAiFields)) {
    field.name = key;
    if (field.type === 'number') field.required = true;
  }
  $('policy-tab').onclick = task(async () => {
    selectAdminTab('policy-tab');
    if (!adminPolicyLoaded) {
      renderAdminAiPolicy(await request(api.adminAiPolicy));
      adminPolicyLoaded = true;
    }
  });
  $('policy-form').onsubmit = task(saveAdminAiPolicy);
})();

const adminWindow = { bounds: null, maximized: false, minimized: false, drag: null };

function fitAdminWindow() {
  const margin = 8;
  const width = Math.max(0, window.innerWidth - margin * 2);
  const height = Math.max(0, window.innerHeight - margin * 2);
  const initialWidth = Math.min(860, width);
  const initialHeight = Math.min(720, Math.max(240, height - 64));
  adminWindow.bounds ??= {
    width: initialWidth,
    height: initialHeight,
    left: (window.innerWidth - initialWidth) / 2,
    top: (window.innerHeight - initialHeight) / 2
  };
  const b = adminWindow.bounds;
  b.width = Math.min(b.width, width);
  b.height = Math.min(b.height, height);
  b.left = Math.max(margin, Math.min(b.left, window.innerWidth - b.width - margin));
  b.top = Math.max(margin, Math.min(b.top, window.innerHeight - b.height - margin));
  const rect = adminWindow.maximized ? { left: margin, top: margin, width, height } : b;
  for (const key of ['left', 'top', 'width', 'height'])
    $('admin-dialog').style[key] = rect[key] + 'px';
  $('admin-maximize').setAttribute('aria-pressed', String(adminWindow.maximized));
  $('admin-maximize').title = adminWindow.maximized ? t('还原') : t('最大化');
  $('admin-maximize').setAttribute(
    'aria-label',
    adminWindow.maximized ? t('还原管理控制台') : t('最大化管理控制台')
  );
}

function openAdminWindow() {
  adminWindow.minimized = false;
  $('admin-restore').hidden = true;
  fitAdminWindow();
  if (!$('admin-dialog').open) $('admin-dialog').showModal();
}

function hideAdminWindow() {
  adminWindow.minimized = false;
  $('admin-restore').hidden = true;
  $('admin-dialog').close();
}

function stopAdminDrag() {
  const drag = adminWindow.drag;
  adminWindow.drag = null;
  $('admin-titlebar').classList.remove('dragging');
  if (drag && $('admin-titlebar').hasPointerCapture(drag.id))
    $('admin-titlebar').releasePointerCapture(drag.id);
}

$('admin-minimize').onclick = () => {
  stopAdminDrag();
  adminWindow.minimized = true;
  $('admin-dialog').close();
  $('admin-restore').hidden = false;
  $('admin-restore').focus();
};
$('admin-restore').onclick = openAdminWindow;
$('admin-maximize').onclick = () => {
  stopAdminDrag();
  adminWindow.maximized = !adminWindow.maximized;
  fitAdminWindow();
};
$('admin-titlebar').addEventListener('dblclick', event => {
  if (!event.target.closest('button')) $('admin-maximize').click();
});
$('admin-titlebar').addEventListener('pointerdown', event => {
  if (event.button !== 0 || event.target.closest('button') || adminWindow.maximized) return;
  event.preventDefault();
  const b = adminWindow.bounds;
  adminWindow.drag = {
    id: event.pointerId,
    x: event.clientX,
    y: event.clientY,
    left: b.left,
    top: b.top
  };
  $('admin-titlebar').setPointerCapture(event.pointerId);
  $('admin-titlebar').classList.add('dragging');
});
$('admin-titlebar').addEventListener('pointermove', event => {
  const d = adminWindow.drag;
  if (!d || d.id !== event.pointerId) return;
  adminWindow.bounds.left = d.left + event.clientX - d.x;
  adminWindow.bounds.top = d.top + event.clientY - d.y;
  fitAdminWindow();
});
for (const event of ['pointerup', 'pointercancel', 'lostpointercapture'])
  $('admin-titlebar').addEventListener(event, stopAdminDrag);
$('admin-dialog').addEventListener('close', () => {
  stopAdminDrag();
  if (!adminWindow.minimized) $('admin-restore').hidden = true;
});
window.addEventListener('blur', stopAdminDrag);
window.addEventListener('resize', () => {
  stopAdminDrag();
  fitAdminWindow();
});
