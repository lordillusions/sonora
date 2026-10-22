#!/usr/bin/env python3
"""AGT-6a: real HTTP tool-loop acceptance with a loopback provider fixture, never a paid model/key."""
import argparse
import http.server
import json
import subprocess
import tempfile
import threading
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--probe', type=Path, default=Path('build/tools/control_probe'))
options = parser.parse_args()
rounds = {}
violations = []
leaks = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass  # Never log credentials, headers or request bodies.

    def reply(self, status, body, location=None):
        encoded = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(encoded)))
        if location:
            self.send_header('Location', location)
        self.end_headers()
        try:
            self.wfile.write(encoded)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        case = self.path.split('/')[1]
        if case == 'leak':
            leaks.append(True)
            self.reply(500, {})
            return
        length = int(self.headers.get('Content-Length', 0))
        if length > 1024 * 1024:
            violations.append('Request exceeds context budget')
            self.reply(413, {})
            return
        data = json.loads(self.rfile.read(length))
        if 'dummy-fixture-key' in json.dumps(data):
            violations.append('Credential entered model context')
        anthropic = self.path.endswith('/messages')
        expected = self.headers.get('x-api-key') if anthropic else self.headers.get('Authorization')
        if expected != ('dummy-fixture-key' if anthropic else 'Bearer dummy-fixture-key'):
            violations.append('Incorrect authentication framing')
        if anthropic and self.headers.get('anthropic-version') != '2023-06-01':
            violations.append('Missing Anthropic version header')
        if case == 'unauthorized':
            self.reply(401, {'error': {'message': 'dummy-fixture-key must not enter the UI'}})
            return
        if case == 'redirect':
            self.reply(302, {}, f'http://127.0.0.1:{self.server.server_port}/leak/responses')
            return
        if case == 'invalid':
            self.reply(200, b'{bad JSON')
            return
        if case == 'oversized':
            self.reply(200, b' ' * (4 * 1024 * 1024 + 1))
            return
        if case in ('cancel', 'document'):
            time.sleep(.6)
        responses = self.path.endswith('/responses')
        index = rounds.get(case, 0)
        rounds[case] = index + 1
        history = data['input'] if responses else data['messages']
        revision = ''
        if index:
            last = history[-1]
            encoded = last['output'] if responses else last['content'][0]['content'] if anthropic else last['content']
            revision = json.loads(encoded).get('revision', '')
        names = ['sonora_project_inspect', 'sonora_capabilities_get', 'sonora_serum_supersaw']
        name = names[index] if index < len(names) else None
        arguments = {} if index != 2 else dict(root='D#', mode='minor', bars=4, startBeat=0, expectedRevision=revision)
        call_id = f'http-fixture-{index}'
        if len(data['tools']) != 11:
            violations.append('Native tool catalogue mismatch')
        if responses:
            item = dict(type='function_call', call_id=call_id, name=name, arguments=json.dumps(arguments)) if name else dict(
                type='message', role='assistant', content=[dict(type='output_text', text='The requested progression is created.')])
            body = dict(status='completed', output=[item])
        elif anthropic:
            item = dict(type='tool_use', id=call_id, name=name, input=arguments) if name else dict(type='text', text='Created the progression.')
            body = dict(stop_reason='tool_use' if name else 'end_turn', content=[item])
        else:
            message = dict(role='assistant', content='Created the progression.')
            if name:
                message['tool_calls'] = [dict(id=call_id, type='function', function=dict(name=name, arguments=json.dumps(arguments)),
                    extra_content=dict(google=dict(thought_signature='opaque-fixture')))]
            body = dict(choices=[dict(finish_reason='tool_calls' if name else 'stop', message=message)])
        self.reply(200, body)


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
server.daemon_threads = True
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    cases = [
        ('responses', [], 'PASS: native responses'),
        ('chat', ['--chat'], 'PASS: native chat-completions'),
        ('anthropic', ['--anthropic'], 'PASS: native anthropic-messages'),
        ('cancel', ['--agent-cancel'], 'Native stopped-request guard: PASS'),
        ('document', ['--agent-document-change'], 'Native stopped-request guard: PASS'),
        ('unauthorized', ['--agent-error'], 'Provider returned HTTP 401'),
        ('redirect', ['--agent-error'], 'Provider returned HTTP 302'),
        ('invalid', ['--agent-error'], 'malformed or oversized JSON'),
        ('oversized', ['--agent-error'], 'exceeded 4 MiB'),
    ]
    for name, flags, marker in cases:
        command = [str(options.probe.resolve()), '--agent-runner', '--agent-http',
            f'http://127.0.0.1:{server.server_port}/{name}', *flags]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90, text=True)
        log = Path(tempfile.gettempdir()) / f'sonora-native-http-{name}.log'
        # Only a dummy credential exists in this fixture; even that must not appear in native diagnostics.
        assert 'dummy-fixture-key' not in result.stdout, f'{name}: credential leaked to diagnostics'
        log.write_text(result.stdout)
        assert result.returncode == 0 and marker in result.stdout, f'{name}: failed, inspect {log}'
        print(f'PASS {name}', flush=True)
    assert not leaks, 'Authentication followed an HTTP redirect'
    assert not violations, '; '.join(violations)
    print('9 native HTTP cases passed; real Serum composition/save/reload/undo in all three protocols.')
finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=2)
