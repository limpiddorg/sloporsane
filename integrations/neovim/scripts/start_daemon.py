"""Serialize daemon startup with the same flock used by the Textual client.

Keep a probe connected until Neovim completes its handshake so the shared daemon
cannot exit between discovery and attachment. No provider credentials are read.
"""
import fcntl
import os
from pathlib import Path
import select
import socket
import stat
import subprocess
import sys
import time


def main():
    binary, address, timeout, logpath = sys.argv[1:]
    limit = 103 if sys.platform == 'darwin' else 107
    if len(os.fsencode(address)) > limit:
        raise ValueError(f'Unix socket path is too long (maximum {limit} bytes); '
                         'configure a shorter absolute socket path')
    deadline = time.monotonic() + float(timeout)
    process = None
    probe = None
    with open(address + '.lock', 'a', opener=lambda p, f: os.open(p, f, 0o600)) as lock:
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Timed out acquiring daemon startup lock')
                time.sleep(0.05)
        while time.monotonic() < deadline:
            probe = socket.socket(socket.AF_UNIX)
            probe.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                probe.connect(address)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                probe.close()
                probe = None
            if process is None:
                try:
                    mode = os.lstat(address).st_mode
                except FileNotFoundError:
                    pass
                else:
                    if not stat.S_ISSOCK(mode):
                        raise RuntimeError('Socket path is occupied by a non-socket file')
                    os.unlink(address)
                Path(logpath).parent.mkdir(parents=True, exist_ok=True)
                with open(logpath, 'ab') as log:
                    process = subprocess.Popen([binary, address], stdin=subprocess.DEVNULL,
                                               stdout=log, stderr=log, start_new_session=True,
                                               cwd='/')
            elif process.poll() is not None:
                raise RuntimeError(f'sosd exited with code {process.returncode}; see {logpath}')
            time.sleep(0.05)
        if probe is None:
            raise TimeoutError(f'Timed out waiting for sosd; see {logpath}')
        print('ready', flush=True)
        remaining = max(0, deadline - time.monotonic())
        if not select.select([sys.stdin], [], [], remaining)[0] or sys.stdin.readline() != 'connected\n':
            raise RuntimeError('Neovim did not complete daemon connection')
        probe.close()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
