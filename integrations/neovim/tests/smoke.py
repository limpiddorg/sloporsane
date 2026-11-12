"""Real SOS daemon and local Chat Completions fixture; no live credentials."""
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

root = Path.cwd()
lifecycle = '--lifecycle' in sys.argv
import tempfile
work = root / ('build-ai/neovim/lifecycle' if lifecycle else 'build-ai/neovim/real')
work.mkdir(parents=True, exist_ok=True)
work = Path(tempfile.mkdtemp(prefix='run-', dir=work))
project = work / 'project'
project.mkdir(exist_ok=True)
subprocess.run(['git', 'init', '-q', str(project)], check=True)
if lifecycle:
    (project / 'state.txt').write_text('original\n')
    (project / 'staged.txt').write_text('baseline\n')
    subprocess.run(['git', '-C', str(project), 'add', 'state.txt', 'staged.txt'], check=True)
# Fixture-only commit: the implementation/user branch and index are untouched.
subprocess.run(['git', '-C', str(project), '-c', 'user.name=SOS Test', '-c',
                'user.email=test@localhost', '-c', 'commit.gpgsign=false',
                'commit', '-q', '--allow-empty', '-m', 'fixture'], check=True)
sys.dont_write_bytecode = True
from socket_paths import socket_directory
if lifecycle:
    (project / 'staged.txt').write_text('user staged change\n')
    subprocess.run(['git', '-C', str(project), 'add', 'staged.txt'], check=True)
    index_before = (project / '.git/index').read_bytes()

plan = {'steps': [{'title': 'Native lifecycle fixture', 'objective': 'Exercise preserved corrections',
                  'substeps': [{'title': 'Write state', 'intention': 'Build in isolation',
                                'changes': 'Write state.txt without importing it'}],
                  'validation': ['Inspect controlled fixture state']}]}


class Provider(BaseHTTPRequestHandler):
    builds = 0
    requests = 0
    def log_message(self, *_):
        pass

    def reply(self, value):
        data = json.dumps(value).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.reply({'data': [{'id': 'local-model'}]})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        title = 'Generate a short session title' in str(body['messages'][0].get('content'))
        if lifecycle and not title:
            Provider.requests += 1
            tools = {item['function']['name'] for item in body.get('tools', [])}
            def call(name, arguments, suffix=''):
                return {'id': name + suffix + str(Provider.requests), 'type': 'function',
                        'function': {'name': name, 'arguments': json.dumps(arguments)}}
            calls = []
            if 'complete_implementation_step' in tools:
                Provider.builds += 1
                text = 'built\n' if Provider.builds == 1 else 'corrected\n'
                calls = [call('write', {'filePath': 'state.txt', 'content': text,
                                       'explanation': 'Preserved fixture state'}, str(Provider.builds)),
                         call('report_implementation_progress', {'substep': 0, 'status': 'completed',
                                                                'explanation': 'Fixture boundary captured'}),
                         call('complete_implementation_step', {'explanation': 'Fixture complete',
                              'substeps': [{'explanation': 'Fixture state written'}], 'conclusion': 'Fixture conclusion'})]
                if Provider.builds > 1:
                    calls.insert(0, call('report_implementation_progress', {'substep': 0, 'status': 'correcting',
                                                                         'explanation': 'Correct captured fixture boundary'}, 'repair'))
                    calls.insert(-1, call('report_implementation_progress', {'substep': None, 'status': 'validating',
                                                                           'explanation': 'Validate revised fixture result'}, 'validation'))
            elif body['messages'][-1]['role'] != 'tool':
                if 'submit_objective_plan' in tools:
                    calls = [call('submit_objective_plan', {'plan': 'Build a controlled state fixture and review its correction.'})]
                elif 'submit_implementation_plan' in tools:
                    calls = [call('submit_implementation_plan', plan)]
                elif 'propose_step_rework' in tools:
                    calls = [call('propose_step_rework', {'request': 'Use corrected implementation'})]
            message = {'role': 'assistant', 'content': None if calls else 'Controlled fixture response'}
            if calls:
                message['tool_calls'] = calls
            self.reply({'id': 'lifecycle-' + str(Provider.requests), 'choices': [{'message': message,
                        'finish_reason': 'tool_calls' if calls else 'stop'}],
                        'usage': {'prompt_tokens': 100, 'completion_tokens': 50}})
            return
        self.reply({'id': 'local-test', 'choices': [{'message': {'role': 'assistant',
                    'content': 'Local test' if title else 'Local answer'}, 'finish_reason': 'stop'}],
                    'usage': {'prompt_tokens': 100, 'completion_tokens': 10}})


http = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
threading.Thread(target=http.serve_forever, daemon=True).start()
env = dict(os.environ)
for kind in ('STATE', 'CONFIG', 'DATA', 'CACHE'):
    env[f'XDG_{kind}_HOME'] = str(work / kind.lower())
env.update(SOS_TEST_WORKSPACE=str(project),
           SOS_TEST_URL=f'http://127.0.0.1:{http.server_port}/v1')
# Re-running starts with an empty fixture configuration, never the user's credentials.
import shutil
for name in ('state', 'config', 'data', 'cache', 'sessions'):
    shutil.rmtree(work / name, ignore_errors=True)
with socket_directory() as sockets:
    address = str(sockets / 's')
    env['SOS_TEST_SOCKET'] = address
    process = None
    try:
        with open(work / 'daemon.log', 'wb') as log:
            process = subprocess.Popen([str(root / 'build-ai/bin/sosd'), address, str(work / 'sessions')],
                                       env=env, stdout=log, stderr=log)
            deadline = time.monotonic() + 10
            while not Path(address).exists():
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(f'Daemon did not start; inspect {work / "daemon.log"}')
                time.sleep(0.02)
            subprocess.run(['nvim', '--headless', '-u', 'NONE', '-l',
                             'integrations/neovim/tests/lifecycle.lua' if lifecycle else
                             'integrations/neovim/tests/smoke.lua'], env=env, check=True, timeout=90)
            if lifecycle:
                assert (project / 'state.txt').read_text() == 'corrected\n'
                assert (project / '.git/index').read_bytes() == index_before, 'User fixture index changed'
    finally:
        http.shutdown()
        http.server_close()
        if process is not None:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
