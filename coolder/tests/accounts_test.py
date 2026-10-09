"""One-time administrator setup, durable storage, and legacy-data regressions."""

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


@contextmanager
def server(data, *options):
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    with tempfile.NamedTemporaryFile(mode='w+') as log:
        process = subprocess.Popen(
            [
                sys.argv[1],
                '--data',
                str(data),
                '--port',
                str(port),
                '--html',
                str(ROOT / 'html'),
                '--log-file',
                log.name,
                *options,
            ],
            stdout=log,
            stderr=log,
        )

        def request(path, body=None, cookie='', expected=200):
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            connection.request(
                'GET' if body is None else 'POST',
                '/api/v1/auth/' + path,
                None if body is None else json.dumps(body),
                {'Content-Type': 'application/json', 'Cookie': cookie},
            )
            response = connection.getresponse()
            payload = json.loads(response.read())
            token = response.getheader('Set-Cookie', '').split(';')[0]
            code = response.status
            connection.close()
            if expected is not None:
                assert code == expected, (path, code, payload)
            return payload, token, code

        try:
            for _ in range(100):
                if process.poll() is not None:
                    log.seek(0)
                    raise AssertionError(log.read())
                try:
                    request('status')
                    break
                except OSError:
                    time.sleep(0.05)
            else:
                raise AssertionError('server not ready')
            yield request
        finally:
            process.terminate()
            process.wait(timeout=10)


with tempfile.TemporaryDirectory(prefix='coolder-accounts-') as temp:
    root = Path(temp)
    fresh = root / 'fresh'
    with server(fresh) as request:
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(
                pool.map(
                    lambda username: request(
                        'register',
                        {'username': username, 'password': 'bootstrap-test-password'},
                        expected=None,
                    ),
                    ['first_admin', 'second_admin'],
                )
            )
        assert sorted(r[2] for r in results) == [200, 409]
        cookie = next(r[1] for r in results if r[2] == 200)
        listing, _, _ = request('users', cookie=cookie)
        assert len(listing['users']) == 1 and listing['users'][0]['admin']
        admin = listing['users'][0]['username']
        request('users/update', {'username': admin, 'enabled': False}, cookie, 400)
        for _ in range(10):
            request('login', {'username': admin, 'password': 'wrong'}, expected=401)
        request('login', {'username': admin, 'password': 'wrong'}, expected=429)
    assert not (fresh / 'access-token').exists()
    with server(fresh) as request:
        status, _, _ = request('status')
        assert status['initialized'] and not status['authenticated']
        request('users', cookie=cookie, expected=401)
        _, cookie, _ = request(
            'login', {'username': admin, 'password': 'bootstrap-test-password'}
        )
        request(
            'users/create',
            {'username': 'developer', 'password': 'developer-password'},
            cookie,
        )
        # UI preferences are independent of role and cannot target another account.
        request('preferences', expected=401)
        defaults, _, _ = request('preferences', cookie=cookie)
        assert (defaults['language'], defaults['theme'], defaults['font_size']) == (
            'zh',
            'default',
            'md',
        )
        request(
            'preferences',
            {'language': 'ja', 'theme': 'green', 'font_size': 'lg'},
            cookie,
        )
        _, user_cookie, _ = request(
            'login', {'username': 'developer', 'password': 'developer-password'}
        )
        user_defaults, _, _ = request('preferences', cookie=user_cookie)
        assert user_defaults == defaults
        request(
            'preferences',
            {'username': admin, 'language': 'ko', 'theme': 'ocean', 'font_size': 'sm'},
            user_cookie,
        )
        for invalid in [
            {'language': '../auth/accounts'},
            {'language': 'template'},
            {'language': 'missing'},
            {'theme': 'unknown'},
            {'font_size': 'huge'},
            {'language': 12},
            {'theme': None},
        ]:
            request('preferences', invalid, user_cookie, 400)
        prefs, _, _ = request('preferences', cookie=cookie)
        assert prefs['language'] == 'ja' and prefs['theme'] == 'green'
        prefs, _, _ = request('preferences', {'font_size': 'md'}, user_cookie)
        assert prefs['language'] == 'ko' and prefs['theme'] == 'ocean'
        request(
            'password',
            {'current_password': 'wrong-password', 'password': 'changed-password'},
            user_cookie,
            403,
        )
        _, user_cookie, _ = request(
            'password',
            {'current_password': 'developer-password', 'password': 'changed-password'},
            user_cookie,
        )
        assert request('preferences', cookie=user_cookie)[0] == prefs
    with server(fresh) as request:
        for username, password, language, theme, size in [
            (admin, 'bootstrap-test-password', 'ja', 'green', 'lg'),
            ('developer', 'changed-password', 'ko', 'ocean', 'md'),
        ]:
            _, cookie, _ = request(
                'login', {'username': username, 'password': password}
            )
            prefs, _, _ = request('preferences', cookie=cookie)
            assert (prefs['language'], prefs['theme'], prefs['font_size']) == (
                language,
                theme,
                size,
            )
    for path in (fresh / 'auth').glob('ui-*.v1'):
        assert path.stat().st_mode & 0o077 == 0
    assert (fresh / 'auth/accounts.v1').stat().st_mode & 0o077 == 0

    legacy = root / 'legacy'
    legacy.mkdir()
    (legacy / 'access-token').write_text('old-token-only-for-bootstrap')
    (legacy / 'workspace').mkdir()
    (legacy / 'workspace/README.md').write_text('existing project')
    with server(legacy) as request:
        status, _, _ = request('status')
        assert not status['initialized']
        assert 'bootstrap_token_required' not in status
        body = {'username': 'owner', 'password': 'migration-password'}
        _, cookie, _ = request('register', body)
        request('register', body, expected=409)
        status, _, _ = request('status')
        assert status['initialized']
        request(
            'users', cookie='coolder_session=old-token-only-for-bootstrap', expected=401
        )
        request('logout', {}, cookie)
        request('users', cookie=cookie, expected=401)
    assert (legacy / 'workspace/README.md').read_text() == 'existing project'
    with server(legacy) as request:
        status, _, _ = request('status')
        assert status['initialized']
        request('register', body, expected=409)
        _, cookie, _ = request('login', body)
        listing, _, _ = request('users', cookie=cookie)
        assert len(listing['users']) == 1 and listing['users'][0]['admin']

    # A damaged account database must never reopen public administrator setup.
    (legacy / 'auth/accounts.v1').write_text('corrupt database')
    error_log = root / 'startup-errors.log'
    failed = subprocess.run(
        [sys.argv[1], '--data', str(legacy), '--html', str(ROOT / 'html'),
         '--log-file', str(error_log)],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert failed.returncode != 0 and 'invalid account store' in error_log.read_text()
    overlap = root / 'overlap'
    failed = subprocess.run(
        [
            sys.argv[1],
            '--data',
            str(overlap),
            '--workspace',
            str(overlap / 'users'),
            '--log-file',
            str(error_log),
            '--html',
            str(ROOT / 'html'),
        ],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert (
        failed.returncode != 0 and 'overlaps private account storage' in error_log.read_text()
    )

print(
    'PASS: bootstrap race, login throttling, session expiry on restart, migration, corrupt storage, private-root boundaries'
)
