import json
import sys

initialized = False
for line in sys.stdin:
    request = json.loads(line)
    method = request['method']
    if method == 'notifications/initialized':
        initialized = True
        continue
    if 'id' not in request:
        continue
    if method == 'initialize':
        result = {'protocolVersion': '2025-11-25', 'capabilities': {'tools': {}},
                  'serverInfo': {'name': 'cpp-stdio-fixture', 'version': '1'}}
    elif method == 'tools/list':
        assert initialized
        result = {'tools': [{'name': 'echo', 'inputSchema': {'type': 'object'}}]}
    else:
        assert initialized and method == 'tools/call'
        assert request['params']['name'] == 'echo'
        result = {'content': [{'type': 'text', 'text': 'from stdio'}]}
    print(json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'result': result}), flush=True)
