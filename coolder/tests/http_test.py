"""Hermetic integration test: real ACL/libai server and a local model fixture.
No external provider, credentials, user projects, or running services are used.
"""

import base64
import http.client
import http.server
import json
import os
import re
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[1]
requests = []
mode = 'read'


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append(payload)
        if mode == 'slow':
            time.sleep(3)
        tools = [m for m in payload.get('messages', []) if m.get('role') == 'tool']
        if not tools and mode == 'create':
            message = {
                'role': 'assistant',
                'content': None,
                'tool_calls': [
                    {
                        'id': 'call_create',
                        'type': 'function',
                        'function': {
                            'name': 'workspace_propose',
                            'arguments': json.dumps(
                                {'path': 'notes.md', 'content': '# coolder smoke\n'}
                            ),
                        },
                    }
                ],
            }
            for index, (name, arguments) in enumerate(
                [
                    ('workspace_propose_mkdir', {'path': 'generated'}),
                    ('workspace_propose_mkdir', {'path': 'generated/nested'}),
                    (
                        'workspace_propose',
                        {
                            'path': 'generated/nested/result.md',
                            'content': '# nested result\n',
                        },
                    ),
                ]
            ):
                message['tool_calls'].append(
                    {
                        'id': 'call_nested_' + str(index),
                        'type': 'function',
                        'function': {'name': name, 'arguments': json.dumps(arguments)},
                    }
                )
            finish = 'tool_calls'
        elif not tools:
            message = {
                'role': 'assistant',
                'content': None,
                'tool_calls': [
                    {
                        'id': 'call_read',
                        'type': 'function',
                        'function': {
                            'name': 'workspace_read',
                            'arguments': json.dumps({'path': 'README.md'}),
                        },
                    }
                ],
            }
            finish = 'tool_calls'
        else:
            message = {
                'role': 'assistant',
                'content': json.dumps(
                    {
                        'type': 'final',
                        'text': 'coolder fixture finished reading the project.',
                        'completion_summary': 'coolder fixture complete',
                        'changes': [],
                    }
                ),
            }
            finish = 'stop'
        result = {
            'id': 'fixture',
            'object': 'chat.completion',
            'choices': [{'index': 0, 'message': message, 'finish_reason': finish}],
            'usage': {'prompt_tokens': 20, 'completion_tokens': 10, 'total_tokens': 30},
        }
        data = json.dumps(result).encode()
        try:
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except OSError:
            pass


provider = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Provider)
threading.Thread(target=provider.serve_forever, daemon=True).start()
with tempfile.TemporaryDirectory(prefix='coolder-test-') as temp:
    data = Path(temp) / 'data'
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    cookie = ''

    def call(path, body=None, status=200, auth=True, headers=None):
        conn = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        h = {'Content-Type': 'application/json'}
        if auth and cookie:
            h['Cookie'] = cookie
        h.update(headers or {})
        conn.request(
            'GET' if body is None else 'POST',
            path,
            None if body is None else json.dumps(body),
            h,
        )
        res = conn.getresponse()
        raw = res.read()
        response_headers = dict(res.getheaders())
        conn.close()
        assert res.status == status, (path, res.status, raw[:2000])
        return (
            json.loads(raw)
            if 'application/json' in response_headers.get('Content-Type', '')
            else raw
        ), response_headers

    log = open(Path(temp) / 'server.log', 'w+')
    command = [
        sys.argv[1],
        '--port',
        str(port),
        '--data',
        str(data),
        '--html',
        str(ROOT / 'html'),
        '--log-file',
        str(Path(temp) / 'server.log'),
    ]
    process = subprocess.Popen(command, stdout=log, stderr=log)
    try:
        for _ in range(100):
            if process.poll() is not None:
                raise AssertionError('server exited')
            try:
                call('/', auth=False)
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise AssertionError('server not ready')
        page, _ = call('/', auth=False)
        page = page.decode('utf-8')
        assert re.findall(r'<script[^>]+src="([^"]+)"', page) == ['/coolder.js']
        bundled, _ = call('/coolder.js', auth=False)
        bundled = bundled.decode('utf-8')
        sources = [
            'i18n.js',
            'app.js',
            'personal.js',
            'code-editor.js',
            'attachments.js',
            'admin.js',
        ]
        positions = [bundled.index('// ---- ' + name + ' ----') for name in sources]
        assert positions == sorted(positions)
        for name in sources:
            assert (ROOT / 'html' / name).read_text() in bundled, name
        # All editor resources are local, typed correctly, and confined to the vendor tree.
        for asset, mime in [
            ('/coolder.js', 'text/javascript'),
            ('/code-editor.js', 'text/javascript'),
            ('/attachments.js', 'text/javascript'),
            ('/vendor/monaco-editor/vs/monaco.bundle.js', 'text/javascript'),
            ('/vendor/monaco-editor/vs/editor/editor.main.css', 'text/css'),
            (
                '/vendor/monaco-editor/vs/assets/ts.worker-CMbG-7ft.js',
                'text/javascript',
            ),
        ]:
            payload, headers = call(asset, auth=False)
            assert payload and mime in headers['Content-Type']
        for asset in [
            '/vendor/monaco-editor/../package.json',
            '/vendor/monaco-editor/vs/../../../../server/auth.cpp',
            '/vendor/monaco-editor/package.json',
            '/vendor/monaco-editor/vs/missing.js',
        ]:
            call(asset, status=404, auth=False)
        call('/api/v1/ai/projects', status=401, auth=False)
        status, _ = call('/api/v1/auth/status', auth=False)
        assert not status['initialized'] and not status['authenticated']
        credentials = {'username': 'admin', 'password': 'admin-test-password'}
        _, h = call('/api/v1/auth/register', credentials, auth=False)
        cookie = h['Set-Cookie'].split(';')[0]
        call('/api/v1/auth/register', credentials, status=409, auth=False)
        call(
            '/api/v1/auth/login',
            {**credentials, 'password': 'wrong'},
            status=401,
            auth=False,
        )
        assert 'HttpOnly' in h['Set-Cookie'] and 'SameSite=Strict' in h['Set-Cookie']
        call('/api/health')
        call('/api/health', status=403, headers={'Host': 'attacker.invalid'})
        call(
            '/api/v1/ai/projects/ensure',
            {},
            status=403,
            headers={'Origin': 'https://attacker.invalid'},
        )
        call('/api/v1/ai/workspace/read?path=../access-token', status=400)
        call(
            '/api/v1/ai/workspace/project/create',
            {'path': '../escape', 'confirm': True},
            status=400,
        )
        call(
            '/api/v1/ai/workspace/project/create',
            {'path': 'shared', 'storage_scope': 'shared', 'confirm': True},
            status=403,
        )
        saved, _ = call(
            '/api/v1/ai/providers/save',
            {
                'name': 'fixture',
                'protocol': 'openai_compatible',
                'base_url': f'http://127.0.0.1:{provider.server_port}/v1',
                'model': 'fixture',
                'api_key': 'fake-test-key',
                'enabled': True,
                'allow_file_content': True,
                'is_default': True,
            },
        )
        provider_id = saved['provider']['id']
        listing, _ = call('/api/v1/ai/providers')
        assert 'fake-test-key' not in json.dumps(listing)
        call(
            '/api/v1/ai/workspace/project/create',
            {
                'path': 'sample',
                'language': 'python',
                'platform': 'cross-platform',
                'confirm': True,
            },
        )
        project_list, _ = call('/api/v1/ai/projects')
        project = project_list['projects'][0]
        assert project['path'] == 'sample'
        details, _ = call('/api/v1/ai/projects?id=' + project['project_id'])
        proposal, _ = call(
            '/api/v1/ai/projects/plan/propose',
            {
                'project_id': project['project_id'],
                'goal': 'Build a calculator',
                'scale': 'quick',
            },
        )
        plan = {
            'project_id': project['project_id'],
            'plan_version': details['plan_version'],
            'goal': proposal['goal'],
            'modules': proposal['modules'],
            'tasks': proposal['tasks'],
        }
        call('/api/v1/ai/projects/plan', plan)
        call('/api/v1/ai/projects/plan', plan, status=409)
        call('/api/v1/ai/workspace/list?path=sample')
        call('/api/v1/ai/workspace/read?path=sample/README.md')
        if os.name != 'nt':
            (data / 'workspace/sample/escape').symlink_to(data / 'auth/accounts.v1')
            call('/api/v1/ai/workspace/read?path=sample/escape', status=400)
            (data / 'workspace/sample/escape').unlink()

        # Attachments use a private user draft, and invalid batches leave no files.
        def attachment(name, content):
            return {'name': name, 'base64': base64.b64encode(content).decode()}

        text_file = attachment(
            'requirements.txt', b'attachment-fixture: use a blue header'
        )
        png = base64.b64decode(
            'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII='
        )
        image_file = attachment('screenshot.png', png)
        call('/api/v1/ai/attachments', {'files': [text_file]}, status=401, auth=False)
        drafts = data / 'workspace/.webcool_agent/attachments'
        for invalid in [
            [],
            [attachment('../escape.txt', b'test')],
            [{'name': 'bad.txt', 'base64': '%%%%'}],
            [attachment('binary.bin', b'abc\x00def')],
            [attachment('large.txt', b'x' * (32 * 1024 + 1))],
            [text_file] * 9,
            [attachment('large.png', b'x' * (8 * 1024 * 1024 + 1))],
        ]:
            call('/api/v1/ai/attachments', {'files': invalid}, status=400)
            assert not list(drafts.iterdir()), list(drafts.iterdir())
        discarded, _ = call('/api/v1/ai/attachments', {'files': [text_file, text_file]})
        assert len(set(discarded['attachments'])) == 2
        call(
            '/api/v1/ai/attachments/discard',
            {'attachment_draft': '../sample'},
            status=400,
        )
        call(
            '/api/v1/ai/attachments/discard',
            {'attachment_draft': discarded['attachment_draft']},
        )
        assert not list(drafts.iterdir())
        attached, _ = call('/api/v1/ai/attachments', {'files': [text_file, image_file]})
        assert all(
            (data / 'workspace' / path).is_file() for path in attached['attachments']
        )
        private_draft, _ = call('/api/v1/ai/attachments', {'files': [text_file]})
        started, _ = call(
            '/api/v1/ai/runs/start',
            {
                **attached,
                'agent_id': 'coding',
                'provider_id': provider_id,
                'path': 'sample',
                'project_id': project['project_id'],
                'prompt': 'Read README.md and summarize the existing project. Do not change any file.',
                'remember_session': True,
                'resume_after_restart': True,
            },
            status=202,
        )

        def wait_run(run_id):
            for _ in range(160):
                result, _ = call('/api/v1/ai/runs/status?id=' + run_id)
                if result['status'] != 'running':
                    return result
                time.sleep(0.1)
            raise AssertionError('run timed out')

        result = wait_run(started['run_id'])
        assert result['status'] == 'completed', result
        assert not (data / 'workspace' / attached['attachment_draft']).exists()
        assert 'attachment-fixture' in json.dumps(requests)
        assert 'data:image/png;base64,' in json.dumps(requests)

        assert requests and any(
            m.get('role') == 'tool' for p in requests for m in p.get('messages', [])
        ), requests
        assert result['tool_calls'] and result['tool_calls'][0]['ok'], result
        session_list, _ = call(
            '/api/v1/ai/sessions?project_id=' + project['project_id']
        )
        assert session_list['sessions'], session_list
        user_times = [
            m['sent_at']
            for session in session_list['sessions']
            for m in session.get('messages', [])
            if m['role'] == 'user'
        ]
        assert user_times and all(
            0 < value <= time.time() for value in user_times
        ), user_times
        admin_cookie = cookie
        admin_run = started['run_id']
        call(
            '/api/v1/auth/users/create',
            {'username': 'alice', 'password': 'alice-test-password'},
        )
        call(
            '/api/v1/auth/users/create',
            {'username': 'bob', 'password': 'bob-test-password'},
        )
        call(
            '/api/v1/auth/users/create',
            {'username': 'alice', 'password': 'alice-test-password'},
            status=409,
        )
        call(
            '/api/v1/auth/users/create',
            {'username': '../escape', 'password': 'alice-test-password'},
            status=400,
        )
        call(
            '/api/v1/auth/users/create',
            {'username': 'root2', 'password': 'alice-test-password', 'admin': True},
            status=400,
        )
        users, _ = call('/api/v1/auth/users')
        assert len(users['users']) == 3
        assert 'digest' not in json.dumps(users) and 'salt' not in json.dumps(users)
        _, h = call(
            '/api/v1/auth/login',
            {'username': 'alice', 'password': 'alice-test-password'},
            auth=False,
        )
        cookie = h['Set-Cookie'].split(';')[0]
        alice_cookie = cookie
        status, _ = call('/api/v1/auth/status')
        assert status['username'] == 'alice' and not status['admin']
        for path, body in [
            ('/api/v1/auth/users', None),
            (
                '/api/v1/auth/users/create',
                {'username': 'intruder', 'password': 'strong-password'},
            ),
            (
                '/api/v1/auth/users/update',
                {'username': 'admin', 'password': 'strong-password'},
            ),
            ('/api/v1/admin/ai-policy', None),
            ('/api/v1/admin/ai-policy', {'enabled': False}),
            ('/api/v1/ai/providers/save', {'id': provider_id}),
            ('/api/v1/ai/providers/delete', {'id': provider_id}),
            ('/api/v1/ai/providers/test', {'id': provider_id}),
        ]:
            call(path, body, status=403)
        visible, _ = call('/api/v1/ai/providers')
        assert visible['providers'][0]['id'] == provider_id
        empty, _ = call('/api/v1/ai/projects')
        assert not empty['projects']
        call('/api/v1/ai/runs/status?id=' + admin_run, status=404)
        call('/api/v1/ai/projects?id=' + project['project_id'], status=404)
        call(
            '/api/v1/ai/workspace/project/create',
            {
                'path': 'sample',
                'language': 'python',
                'platform': 'cross-platform',
                'confirm': True,
            },
        )
        alice_projects, _ = call('/api/v1/ai/projects')
        alice_project = alice_projects['projects'][0]
        assert alice_project['project_id'] != project['project_id']
        # Alice cannot consume or delete the administrator's draft.
        call(
            '/api/v1/ai/attachments/discard',
            {'attachment_draft': private_draft['attachment_draft']},
        )
        assert (data / 'workspace' / private_draft['attachments'][0]).exists()
        call(
            '/api/v1/ai/runs/start',
            {
                'agent_id': 'coding',
                'provider_id': provider_id,
                'path': 'sample',
                'project_id': alice_project['project_id'],
                'prompt': 'Read the attachment',
                **private_draft,
            },
            status=400,
        )
        alice_draft, _ = call('/api/v1/ai/attachments', {'files': [text_file]})
        assert not (data / 'workspace' / alice_draft['attachment_draft']).exists()
        call(
            '/api/v1/ai/attachments/discard',
            {'attachment_draft': alice_draft['attachment_draft']},
        )
        alice_started, _ = call(
            '/api/v1/ai/runs/start',
            {
                'agent_id': 'coding',
                'provider_id': provider_id,
                'path': 'sample',
                'project_id': alice_project['project_id'],
                'prompt': 'Read README.md',
                'remember_session': True,
            },
            status=202,
        )
        assert wait_run(alice_started['run_id'])['status'] == 'completed'
        _, h = call(
            '/api/v1/auth/login',
            {'username': 'bob', 'password': 'bob-test-password'},
            auth=False,
        )
        cookie = h['Set-Cookie'].split(';')[0]
        empty, _ = call('/api/v1/ai/projects')
        assert not empty['projects']
        call('/api/v1/ai/runs/status?id=' + alice_started['run_id'], status=404)
        # Independent accounts can run concurrently against the same provider.
        mode = 'slow'
        cookie = admin_cookie
        concurrent_admin, _ = call(
            '/api/v1/ai/runs/start',
            {
                'agent_id': 'coding',
                'provider_id': provider_id,
                'path': 'sample',
                'project_id': project['project_id'],
                'prompt': 'Read README.md',
            },
            status=202,
        )
        cookie = alice_cookie
        concurrent_alice, _ = call(
            '/api/v1/ai/runs/start',
            {
                'agent_id': 'coding',
                'provider_id': provider_id,
                'path': 'sample',
                'project_id': alice_project['project_id'],
                'prompt': 'Read README.md',
            },
            status=202,
        )
        call(
            '/api/v1/ai/runs/cancel', {'run_id': concurrent_admin['run_id']}, status=404
        )
        call(
            '/api/v1/ai/runs/cancel', {'run_id': concurrent_alice['run_id']}, status=202
        )
        assert wait_run(concurrent_alice['run_id'])['status'] == 'cancelled'
        cookie = admin_cookie
        active, _ = call('/api/v1/ai/runs/status?id=' + concurrent_admin['run_id'])
        assert active['status'] == 'running'
        call(
            '/api/v1/ai/runs/cancel', {'run_id': concurrent_admin['run_id']}, status=202
        )
        assert wait_run(concurrent_admin['run_id'])['status'] == 'cancelled'
        mode = 'read'
        cookie = admin_cookie
        call(
            '/api/v1/admin/ai-policy',
            {'allow_users': False, 'max_active_runs_per_user': 3},
        )
        cookie = alice_cookie
        call('/api/v1/ai/projects', status=403)
        cookie = admin_cookie
        call('/api/v1/admin/ai-policy', {'enabled': False})
        call('/api/v1/ai/providers')
        call('/api/v1/admin/ai-policy', {'allow_users': True, 'enabled': True})
        call('/api/v1/auth/users/update', {'username': 'alice', 'enabled': False})
        cookie = alice_cookie
        call('/api/health', status=401)
        call(
            '/api/v1/auth/login',
            {'username': 'alice', 'password': 'alice-test-password'},
            status=401,
            auth=False,
        )
        cookie = admin_cookie
        call(
            '/api/v1/auth/users/update',
            {'username': 'alice', 'enabled': True, 'password': 'alice-new-password'},
        )
        call(
            '/api/v1/auth/login',
            {'username': 'alice', 'password': 'alice-test-password'},
            status=401,
            auth=False,
        )
        _, h = call(
            '/api/v1/auth/login',
            {'username': 'alice', 'password': 'alice-new-password'},
            auth=False,
        )
        cookie = h['Set-Cookie'].split(';')[0]
        preserved, _ = call('/api/v1/ai/projects')
        assert preserved['projects'][0]['project_id'] == alice_project['project_id']
        call(
            '/api/v1/auth/password',
            {'current_password': 'wrong', 'password': 'alice-third-password'},
            status=403,
        )
        _, h = call(
            '/api/v1/auth/password',
            {
                'current_password': 'alice-new-password',
                'password': 'alice-third-password',
            },
        )
        call('/api/health', status=401)
        cookie = h['Set-Cookie'].split(';')[0]
        call('/api/health')
        call('/api/logout', {})
        call('/api/health', status=401)
        cookie = admin_cookie
        assert 'alice-test-password' not in (data / 'auth/accounts.v1').read_text()
        # Exercise model writes and generation-specific review through the real runtime.
        mode = 'create'
        created, _ = call(
            '/api/v1/ai/runs/start',
            {
                'agent_id': 'coding',
                'provider_id': provider_id,
                'path': 'sample',
                'prompt': 'Create notes.md with exactly "# coolder smoke\\n". This is documentation only.',
                'remember_session': True,
            },
            status=202,
        )
        generated = wait_run(created['run_id'])
        assert generated['status'] == 'completed', generated
        changes = generated.get('changes', [])
        assert changes, (generated, requests[-1].get('messages', [])[-2:])
        # Completion retains drafts until explicit acceptance; batch acceptance
        # must create nested directories and files in the formal project atomically.
        assert not (data / 'workspace/sample/notes.md').exists()
        assert not (data / 'workspace/sample/generated').exists()
        assert any(
            c['path'].endswith('generated/nested/result.md') for c in changes
        ), changes
        reviews = [
            {
                'path': change['path'],
                'generation': change['generation'],
                'draft_hash': change['draft_hash'],
                'decision': 'accepted',
            }
            for change in changes
            if change['review_status'] == 'pending'
        ]
        stale_reviews = [dict(review) for review in reviews]
        stale_reviews[-1]['draft_hash'] = 'stale'
        call(
            '/api/v1/ai/runs/result/review',
            {'run_id': created['run_id'], 'reviews': stale_reviews},
            status=409,
        )
        assert not (data / 'workspace/sample/notes.md').exists()
        assert not (data / 'workspace/sample/generated').exists()
        call(
            '/api/v1/ai/runs/result/review',
            {'run_id': created['run_id'], 'reviews': reviews},
        )
        assert (
            data / 'workspace/sample/generated/nested/result.md'
        ).read_text() == '# nested result\n'
        reviewed, _ = call('/api/v1/ai/runs/status?id=' + created['run_id'])
        assert all(c['review_status'] == 'accepted' for c in reviewed['changes'])
        content, _ = call('/api/v1/ai/workspace/read?path=sample/notes.md')
        assert 'coolder smoke' in content['content']
        preview, _ = call(
            '/api/v1/ai/workspace/patch/preview',
            {'path': 'sample/notes.md', 'content': '# edited in browser\n'},
        )
        call('/api/v1/ai/workspace/patch/apply', {'patch_id': preview['patch_id']})
        content, _ = call('/api/v1/ai/workspace/read?path=sample/notes.md')
        assert content['content'] == '# edited in browser\n'
        stale, _ = call(
            '/api/v1/ai/workspace/patch/preview',
            {'path': 'sample/notes.md', 'content': 'stale edit'},
        )
        (data / 'workspace/sample/notes.md').write_text('external edit')
        call(
            '/api/v1/ai/workspace/patch/apply',
            {'patch_id': stale['patch_id']},
            status=409,
        )
        # Provider I/O must yield so control requests can pause/cancel the worker.
        mode = 'slow'
        started, _ = call(
            '/api/v1/ai/runs/start',
            {
                'agent_id': 'coding',
                'provider_id': provider_id,
                'path': 'sample',
                'prompt': 'Read README.md',
                'remember_session': True,
            },
            status=202,
        )
        call(
            '/api/v1/ai/runs/pause',
            {'run_id': started['run_id'], 'paused': True},
            status=202,
        )
        call(
            '/api/v1/ai/runs/pause',
            {'run_id': started['run_id'], 'paused': False},
            status=202,
        )
        time.sleep(0.1)
        call('/api/v1/ai/runs/cancel', {'run_id': started['run_id']}, status=202)
        cancelled = wait_run(started['run_id'])
        assert cancelled['status'] == 'cancelled', cancelled
        # Policy settings use libai validation, persist, and support partial saves.
        policy_changes = {
            'language_tools': 'go,java,python,javascript,rust,objective-c,swift,csharp,kotlin,php,d',
            'sensitive_paths': 'private,secrets',
            'max_output_tokens': 100000,
            'quick_mode_output_tokens': 32000,
            'provider_connect_timeout_seconds': 45,
            'provider_stream_timeout_seconds': 120,
            'quick_mode_tool_calls': 24,
            'standard_mode_tool_calls': 48,
            'large_mode_tool_calls': 96,
            'max_no_progress_tool_calls': 5,
            'browser_debug_report_threshold': 4,
            'tool_context_compaction_kib': 48,
            'read_chunk_kib': 16,
            'read_file_limit_kib': 512,
            'sandbox_timeout_ms': 120000,
            'sandbox_cpu_seconds': 60,
            'sandbox_memory_mib': 512,
            'sandbox_process_count': 32,
            'sandbox_output_kib': 128,
        }
        for tool in [
            'node',
            'python',
            'javac',
            'java',
            'go',
            'cargo',
            'make',
            'swift',
            'dotnet',
            'kotlinc',
            'php',
            'dmd',
        ]:
            policy_changes[tool + '_executable_path'] = str(Path(temp) / 'tools' / tool)
        changed, _ = call('/api/v1/admin/ai-policy', policy_changes)
        assert all(changed[key] == value for key, value in policy_changes.items())
        policy_changes['sensitive_paths'] = 'private,secrets,credentials'
        partial, _ = call(
            '/api/v1/admin/ai-policy',
            {'sensitive_paths': policy_changes['sensitive_paths']},
        )
        assert all(partial[key] == value for key, value in policy_changes.items())
        for field in [
            'allow_browser_debug',
            'allow_users_shared_projects',
        ]:
            call('/api/v1/admin/ai-policy', {field: True}, status=400)
        call('/api/v1/admin/ai-policy', {'sandbox_memory_mib': 1}, status=400)
        call(
            '/api/v1/admin/ai-policy',
            {'read_chunk_kib': 64, 'read_file_limit_kib': 1},
            status=400,
        )
        unchanged, _ = call('/api/v1/admin/ai-policy')
        assert all(unchanged[key] == value for key, value in policy_changes.items())
        # Static pages work in both the workspace and an external directory.
        for requested, physical in [
            ('static-page', data / 'workspace/static-page'),
            (str(Path(temp) / 'external page'), Path(temp) / 'external page'),
        ]:
            page, _ = call('/api/v1/ai/workspace/project/create', {
                'path': requested, 'language': 'html', 'confirm': True,
            })
            assert page['language'] == 'html' and page['planning_available']
            assert page['plan_seeded']
            assert {p.name for p in physical.iterdir()} == {
                'index.html', 'style.css', 'script.js', 'README.md',
            }
            html = (physical / 'index.html').read_text()
            assert 'href="style.css"' in html and 'src="script.js"' in html
            assert 'id="greeting-button"' in html and 'id="greeting"' in html
            assert 'addEventListener' in (physical / 'script.js').read_text()
            call('/api/v1/ai/workspace/read?path=' + quote(page['path'] + '/index.html'))
            manifest, _ = call('/api/v1/ai/projects?id=' + page['project_id'])
            assert manifest['language'] == 'html'
            assert manifest['modules'][0]['path'] == page['path']
            call('/api/v1/ai/workspace/project/create', {
                'path': requested, 'language': 'html', 'confirm': True,
            }, status=409)
            assert (physical / 'index.html').read_text() == html
        # Absolute paths register bounded external roots, independent of workspace.
        directories, _ = call('/api/v1/ai/projects/directories?path=' + quote(temp))
        assert directories['path'] == str(Path(temp).resolve())
        assert any(entry['name'] == 'data' for entry in directories['entries'])
        call('/api/v1/ai/projects/directories?path=relative', status=400)
        call('/api/v1/ai/projects/directories?path=' + quote(str(Path(temp) / 'missing')), status=400)
        call('/api/v1/ai/projects/directories', status=401, auth=False)
        external = Path(temp) / 'external project'
        created, _ = call('/api/v1/ai/workspace/project/create', {
            'path': str(external), 'language': 'python', 'confirm': True,
        })
        assert created['storage_scope'] == 'local' and created['planning_available']
        assert (external / 'README.md').is_file()
        external_read = '/api/v1/ai/workspace/read?path=' + quote(created['path'] + '/README.md')
        call(external_read)
        preview, _ = call('/api/v1/ai/workspace/patch/preview', {
            'path': created['path'] + '/README.md', 'content': '# external edit\n',
        })
        call('/api/v1/ai/workspace/patch/apply', {'patch_id': preview['patch_id']})
        assert (external / 'README.md').read_text() == '# external edit\n'
        call('/api/v1/ai/workspace/project/create', {
            'path': str(external), 'confirm': True,
        }, status=409)
        existing = Path(temp) / 'existing project'
        existing.mkdir()
        (existing / 'main.py').write_text('print("existing")')
        imported, _ = call('/api/v1/ai/projects/ensure', {'path': str(existing), 'language': 'python', 'platform': 'cross-platform'})
        assert (existing / 'main.py').read_text() == 'print("existing")'
        (existing / '.git').mkdir()
        call('/api/v1/ai/projects/import-git', {'path': str(existing), 'language': 'python', 'platform': 'cross-platform'})
        call('/api/v1/ai/projects/ensure', {'path': str(existing / 'missing')}, status=400)
        if os.name != 'nt':
            (external / 'escape').symlink_to(existing / 'main.py')
            call('/api/v1/ai/workspace/read?path=' + quote(created['path'] + '/escape'), status=400)
        _, h = call('/api/v1/auth/login', {
            'username': 'alice', 'password': 'alice-third-password',
        }, auth=False)
        alice_cookie = h['Set-Cookie'].split(';')[0]
        cookie = alice_cookie
        call('/api/v1/ai/projects/directories', status=403)
        for endpoint in ['workspace/project/create', 'projects/ensure', 'projects/import-git']:
            call('/api/v1/ai/' + endpoint, {'path': str(existing), 'confirm': True}, status=403)
        cookie = admin_cookie
        call('/api/v1/admin/ai-policy', {'allow_users_local_projects': True})
        cookie = alice_cookie
        call('/api/v1/ai/projects/directories?path=' + quote(str(existing)))
        call('/api/v1/ai/projects/ensure', {'path': str(existing), 'language': 'python', 'platform': 'cross-platform'})
        cookie = admin_cookie
        call('/api/v1/admin/ai-policy', {'allow_users_local_projects': False})
        process.terminate()
        process.wait(timeout=10)
        process = subprocess.Popen(command, stdout=log, stderr=log)
        for _ in range(100):
            try:
                call('/api/v1/auth/status', auth=False)
                break
            except OSError:
                time.sleep(0.05)
        call('/api/health', status=401)
        _, h = call('/api/v1/auth/login', credentials, auth=False)
        cookie = h['Set-Cookie'].split(';')[0]
        call(external_read)
        call('/api/v1/ai/projects?id=' + created['project_id'])
        saved_sessions, _ = call(
            '/api/v1/ai/sessions?project_id=' + project['project_id']
        )
        assert saved_sessions['sessions']
        saved_providers, _ = call('/api/v1/ai/providers')
        assert saved_providers['providers'][0]['id'] == provider_id
        policy, _ = call('/api/v1/admin/ai-policy')
        assert policy['max_active_runs_per_user'] == 3
        assert all(policy[key] == value for key, value in policy_changes.items())
        users, _ = call('/api/v1/auth/users')
        assert len(users['users']) == 3
        call('/api/logout', {})
        call('/api/health', status=401)
        print(
            'PASS: authentication, CSRF/Host, workspace boundaries, providers, projects, real libai tool loop, sessions, pause/cancel, restart persistence'
        )
    except Exception:
        log.flush()
        log.seek(0)
        print(log.read()[-7000:])
        raise
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        log.close()
provider.shutdown()
provider.server_close()
