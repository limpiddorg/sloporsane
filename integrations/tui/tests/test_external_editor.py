"""Exercise the real terminal driver across external-editor suspension/resume."""

import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest


PROBE = r'''
import asyncio
import faulthandler
import json
import os
import shlex
import sys
import traceback
from unittest.mock import AsyncMock

from textual import work
from textual.drivers.linux_driver import LinuxDriver
from textual.widgets import Static
from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput

diagnostics = os.fdopen(int(sys.argv[1]), "w", buffering=1)
outcome = sys.argv[2]
faulthandler.dump_traceback_later(6, file=diagnostics, exit=True)

class EditorProbe(SosApp):
    def listen(self):
        pass  # No daemon or provider is needed for this terminal regression.

    def on_mount(self):
        self.results = []
        self.failed = True
        self.connected = True
        self.client.send = AsyncMock()
        self.query_one(MessageInput).disabled = False
        self.ticks = 0
        self.stopped_ticks = 0
        self.stopped_queue = 0
        self.set_interval(0.01, self.repaint)
        self.exercise()

    def repaint(self):
        self.ticks += 1
        writer = self._driver._writer_thread
        if not writer.is_alive():
            self.stopped_ticks += 1
            self.stopped_queue = max(self.stopped_queue, writer._queue.qsize())
        self.query_one("#session-title", Static).update(str(self.ticks))

    @work
    async def exercise(self):
        try:
            await asyncio.sleep(0.1)
            assert isinstance(self._driver, LinuxDriver)
            for round in range(2):
                self.busy = bool(round)  # Repeat with the agent working on the second edit.
                entry = self.query_one(MessageInput)
                original = f"Original draft {round}"
                entry.text = original
                await asyncio.sleep(0.05)
                self.stopped_ticks = self.stopped_queue = 0
                if outcome == "missing":
                    os.environ["EDITOR"] = shlex.quote(sys.argv[3])
                else:
                    script = (
                        "import sys, time; from pathlib import Path; time.sleep(1); "
                        "Path(sys.argv[1]).write_text('Edited draft\\nSecond line\\n', encoding='utf-8'); "
                        f"sys.exit({0 if outcome == 'success' else 7})"
                    )
                    os.environ["EDITOR"] = shlex.join([sys.executable, "-c", script])
                previous_writer = self._driver._writer_thread
                await self.action_edit_external()
                assert self._driver._writer_thread is not previous_writer
                assert self._driver._writer_thread.is_alive()
                assert self._batch_count == 0
                assert self.busy == bool(round)
                assert self.stopped_queue == 0
                if outcome != "missing":
                    assert self.stopped_ticks > 10, "The event loop must stay active during editing"
                expected = "Edited draft\nSecond line" if outcome == "success" else original
                assert entry.text == expected, repr(entry.text)
                if outcome == "success":
                    assert entry.cursor_location == entry.document.end
                if outcome != "success":
                    assert any("Editor failed" in str(widget.render()) for widget in self.query_one("#chat").query(Static))
                self.results.append({"outcome": outcome, "stopped_ticks": self.stopped_ticks,
                                     "stopped_queue": self.stopped_queue})
                await asyncio.sleep(0.05)  # Exercise painting on the restarted writer as well.
            self.client.send.assert_not_awaited()
            self.failed = False
        except BaseException:
            traceback.print_exc(file=diagnostics)
        finally:
            self.exit()

try:
    app = EditorProbe()
    app.run(size=(80, 24))
    faulthandler.cancel_dump_traceback_later()
    if app.failed:
        sys.exit(1)
    print(json.dumps(app.results), file=diagnostics)
except BaseException:
    traceback.print_exc(file=diagnostics)
    raise
'''


@unittest.skipUnless(os.name == "posix" and hasattr(os, "openpty"), "Requires a Unix pseudo-terminal")
class ExternalEditorTerminalTest(unittest.TestCase):
    def run_editor(self, outcome):
        import fcntl
        import struct
        import termios

        master, slave = os.openpty()
        self.addCleanup(os.close, master)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        with tempfile.TemporaryDirectory(prefix="sos-editor-test-") as directory, tempfile.TemporaryFile() as report:
            try:
                process = subprocess.Popen(
                    [sys.executable, "-c", PROBE, str(report.fileno()), outcome, str(Path(directory) / "missing-editor")],
                    stdin=slave, stdout=slave, stderr=slave, pass_fds=(report.fileno(),), start_new_session=True,
                )
            finally:
                os.close(slave)
            try:
                deadline = time.monotonic() + 10
                terminal_output = bytearray()
                while process.poll() is None and time.monotonic() < deadline:
                    # Always drain the PTY so terminal backpressure isn't the cause of a failure.
                    if select.select([master], [], [], 0.05)[0]:
                        try:
                            terminal_output.extend(os.read(master, 65536))
                        except OSError:
                            break  # Linux reports EIO when the slave is closed.
                process.wait(timeout=1)
                while select.select([master], [], [], 0)[0]:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    terminal_output.extend(chunk)
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=2)
            report.seek(0)
            diagnostics = report.read().decode("utf-8", errors="replace")
        self.assertEqual(process.returncode, 0, diagnostics)
        # Startup, two editor round trips, and final shutdown must balance.
        self.assertEqual(terminal_output.count(b"\x1b[?1049h\x1b[>4;2m"), 3)
        self.assertEqual(terminal_output.count(b"\x1b[>4m\x1b[?1049l"), 3)
        rounds = json.loads(diagnostics)
        self.assertEqual(len(rounds), 2)
        self.assertTrue(all(round["stopped_queue"] == 0 for round in rounds))

    def test_editor_roundtrips_without_writes_to_a_stopped_driver(self):
        self.run_editor("success")

    def test_nonzero_editor_exit_preserves_draft_and_restarts_driver(self):
        self.run_editor("failure")

    def test_launch_failure_preserves_draft_and_restarts_driver(self):
        self.run_editor("missing")
