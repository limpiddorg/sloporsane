"""Controlled startup checks; all files are generated beneath build-ai/."""
import fcntl
import os
from pathlib import Path
import select
import subprocess
import sys
import time

root = Path(sys.argv[1])
helper = Path('integrations/neovim/scripts/start_daemon.py').resolve()
daemon = root / 'fake-sosd'
daemon.write_text('''#!/usr/bin/env python3
import os, socket, sys, threading
from pathlib import Path
path = sys.argv[1]
with open(path + '.starts', 'a') as f: f.write('start\\n')
s = socket.socket(socket.AF_UNIX)
s.bind(path)
s.listen(8)
s.settimeout(5)
def serve(c):
    try:
        c.sendall(b'{"type":"daemon.ready","protocolVersion":7}\\n')
        while c.recv(4096): pass
    finally: c.close()
try:
    while True:
        c, _ = s.accept()
        threading.Thread(target=serve, args=(c,), daemon=True).start()
except socket.timeout: pass
finally:
    s.close()
    Path(path).unlink(missing_ok=True)
''')
daemon.chmod(0o700)


def launch(path):
    return subprocess.Popen([sys.executable, str(helper), str(daemon), str(path), '3',
                             str(root / 'daemon.log')], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def ready(process):
    assert select.select([process.stdout], [], [], 5)[0], 'helper ready timeout'
    assert process.stdout.readline() == 'ready\n', process.stderr.read()


sys.dont_write_bytecode = True
from socket_paths import socket_directory

# Distinct concurrent contexts cannot collide; exception cleanup removes only owners.
import socket
with socket_directory() as outer:
    try:
        with socket_directory() as inner:
            assert outer != inner
            with socket.socket(socket.AF_UNIX) as endpoint:
                endpoint.bind(str(inner / 's'))
            raise RuntimeError('controlled cleanup failure')
    except RuntimeError as error:
        assert str(error) == 'controlled cleanup failure'
    assert not inner.exists() and outer.exists()
assert not outer.exists()

invalid = root / ('x' * 108)
p = launch(invalid)
_, error = p.communicate(timeout=5)
assert p.returncode != 0 and 'shorter absolute socket path' in error
assert not Path(str(invalid) + '.lock').exists()

with socket_directory() as sockets:
    path = sockets / 's'
    processes = []
    try:
        # A TUI-compatible lock must actually block startup, not merely exist.
        with open(str(path) + '.lock', 'a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            first = launch(path)
            processes.append(first)
            time.sleep(0.1)
            assert not Path(str(path) + '.starts').exists()
            fcntl.flock(lock, fcntl.LOCK_UN)
        ready(first)
        second = launch(path)
        processes.append(second)
        first.stdin.write('connected\n')
        first.stdin.flush()
        assert first.wait(timeout=4) == 0, first.stderr.read()
        ready(second)
        second.stdin.write('connected\n')
        second.stdin.flush()
        assert second.wait(timeout=4) == 0, second.stderr.read()
        assert Path(str(path) + '.starts').read_text().splitlines() == ['start']

        occupied = sockets / 'o'
        occupied.write_text('keep me')
        p = launch(occupied)
        processes.append(p)
        _, err = p.communicate(timeout=5)
        assert p.returncode != 0 and 'non-socket' in err
        assert occupied.read_text() == 'keep me'
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=4)
        # The fake daemon has a five-second accept timeout, including on failure.
        end = time.monotonic() + 7
        while path.exists() and time.monotonic() < end:
            time.sleep(0.05)
        assert not path.exists(), 'Controlled startup daemon did not drain'
assert not sockets.exists()
print('Startup lock, concurrent launch and regular-file safeguards passed')
