"""Run typed/JSON provider tests for any language using a temporary local HTTP fixture."""
import os
import http.server
import json
import subprocess
import sys
import tempfile
import threading

class Fixture(http.server.BaseHTTPRequestHandler):
    errors = []

    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if request['messages'][-1]['content'] != '你好😀':
            self.errors.append(request)
        result = json.dumps({
            'choices': [{'message': {'role': 'assistant', 'content': 'native😀'}}],
            'usage': {'prompt_tokens': 3, 'completion_tokens': 2},
        }).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(result)))
        self.end_headers()
        self.wfile.write(result)

fixture = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
thread = threading.Thread(target=fixture.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory() as root:
        env = dict(os.environ, LIBAI_TEST_PROVIDER_URL=f'http://127.0.0.1:{fixture.server_port}/v1')
        command = sys.argv[1:]
        if len(command) == 1:
            command += [env['LIBAI_TEST_PROVIDER_URL'], root]
        subprocess.run(command, env=env, check=True, timeout=60)
        assert not Fixture.errors, Fixture.errors
        print('local provider fixture passed')
finally:
    fixture.shutdown()
    thread.join()
    fixture.server_close()
