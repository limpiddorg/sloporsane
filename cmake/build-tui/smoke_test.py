"""Exercise the extracted application with no Python or uv available in its PATH."""

import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tarfile
import tempfile
import termios
import time


def sessions(socket_path: Path) -> list:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(1)
        connection.connect(str(socket_path))
        with connection.makefile("rb") as stream:
            assert json.loads(stream.readline())["type"] == "daemon.ready"
            connection.sendall(b'{"type":"session.list"}\n')
            return json.loads(stream.readline())["sessions"]


def launch(executable: Path, root: Path, socket_path: Path) -> None:
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 35, 100, 0, 0))
    env = {"HOME": str(root), "XDG_STATE_HOME": str(root / "state"),
           "XDG_CONFIG_HOME": str(root / "config"),
           "PATH": str(root / "empty-path"), "TERM": "xterm-256color", "LANG": "en_US.UTF-8"}
    process = subprocess.Popen([str(executable), "--socket", str(socket_path)],
                               stdin=slave, stdout=slave, stderr=slave, env=env, cwd=root,
                               start_new_session=True)
    os.close(slave)
    output = bytearray()
    companion = None

    def read_output():
        if select.select([master], [], [], 0.05)[0]:
            try:
                output.extend(os.read(master, 65536))
            except OSError as error:
                if error.errno != errno.EIO:
                    raise

    def wait_for(text: bytes):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            read_output()
            if text in output:
                return
            if process.poll() is not None:
                break
        raise AssertionError(f"Standalone TUI did not display {text!r}: {bytes(output[-4000:])!r}")

    try:
        wait_for(b"Write a message and press Enter")
        # Composer rendering precedes the handshake; wait for the configured model too.
        wait_for(b"smoke-test-model")
        assert sessions(socket_path) == []
        companion = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        companion.settimeout(2)
        companion.connect(str(socket_path))
        with companion.makefile("rb") as stream:
            assert json.loads(stream.readline())["type"] == "daemon.ready"
        output.clear()
        os.write(master, b"/resume")
        wait_for(b"/resume")
        os.write(master, b"\r")
        wait_for(b"No saved sessions")
        os.write(master, b"\x1b")
        time.sleep(0.2)
        os.write(master, b"\x11")  # Ctrl+Q.
        deadline = time.monotonic() + 5
        while process.poll() is None and time.monotonic() < deadline:
            read_output()
        assert process.poll() == 0, f"Standalone TUI exit: {process.poll()}, {bytes(output[-4000:])!r}"
        assert sessions(socket_path) == []  # A second integration still owns a connection.
        companion.close()
        companion = None
        deadline = time.monotonic() + 3
        while socket_path.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        assert not socket_path.exists(), "Daemon did not stop after the last integration closed"
    finally:
        if companion is not None:
            companion.close()
        if process.poll() is None:
            process.kill()
        process.wait()
        os.close(master)


def main():
    archive = Path(sys.argv[1]).resolve()
    if not archive.is_file():
        print("Build build-tui before running the standalone smoke test")
        return 77
    with tempfile.TemporaryDirectory(prefix="sos-bundle-", dir="/tmp") as directory:
        root = Path(directory)
        log_path = root / "state/sos/sosd.log"
        socket_path = root / "daemon.sock"
        config_path = root / "config/sos/config.json"
        config_path.parent.mkdir(parents=True)
        config_path.write_text(json.dumps({"provider": "openrouter", "model": "smoke-test-model", "reasoning": "medium"}))
        try:
            with tarfile.open(archive) as package:
                package.extractall(root, filter="data")
            launch(root / "sos/sos", root, socket_path)
            launch(root / "sos/sos", root, socket_path)
            starts = re.findall(r"Started sosd \(pid (\d+)\)", log_path.read_text())
            assert len(starts) == 2 and starts[0] != starts[1], f"Expected a fresh daemon on the second launch, got {starts}"
            print("Standalone archive: launch, /resume, shared connection, automatic shutdown and restart passed with an empty PATH")
        finally:
            if log_path.exists():
                for pid in re.findall(r"Started sosd \(pid (\d+)\)", log_path.read_text()):
                    try:
                        os.kill(int(pid), signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                deadline = time.monotonic() + 2
                while socket_path.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
    return 0


if __name__ == "__main__":
    sys.exit(main())
