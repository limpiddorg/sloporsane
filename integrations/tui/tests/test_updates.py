"""Offline discovery, safe terminal handoff, and update dialog behavior."""
import json
import os
import pty
import signal
import time
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import MagicMock, patch

from async_fixture import wait_until
from sos_tui import updates
from sos_tui.app import SosApp
from sos_tui.update_screen import UpdateScreen


class UpdateDiscoveryTest(unittest.TestCase):
    def response(self, version):
        opener = MagicMock()
        opener.open.return_value.__enter__.return_value.read.return_value = json.dumps({"version": version}).encode()
        return patch.object(updates, "build_opener", return_value=opener)

    def test_dev_never_uses_network(self):
        with patch.object(updates, "build_opener") as opener:
            self.assertIsNone(updates.latest_update("dev"))
            opener.assert_not_called()

    def test_numeric_comparison_and_v_prefix(self):
        with self.response("v0.0.10"):
            self.assertEqual(updates.latest_update("0.0.9"), "v0.0.10")
        for value in ["v0.0.9", "v0.0.8", "v0.0.10-rc.1", "bad", None, "v00.0.10"]:
            with self.response(value):
                self.assertIsNone(updates.latest_update("v0.0.9"))

    def test_failures_and_oversized_response_are_silent(self):
        with patch.object(updates, "build_opener", side_effect=OSError("offline")):
            self.assertIsNone(updates.latest_update("0.0.1"))
        for data in [b"not json", b"{}", b"x" * 16_385]:
            with patch.object(updates, "build_opener") as opener:
                opener.return_value.open.return_value.__enter__.return_value.read.return_value = data
                self.assertIsNone(updates.latest_update("0.0.1"))

    def test_handoff_uses_exec_and_positional_arguments(self):
        with patch.dict(os.environ, {"LD_LIBRARY_PATH": "/old/bundle", "LD_LIBRARY_PATH_ORIG": "/original"}), \
                patch.object(updates.os, "execve") as execute:
            updates.install_and_restart("v0.0.2", Path.cwd(), ["--socket", "/tmp/socket with spaces"])
        command, args, environment = execute.call_args.args
        self.assertEqual(environment["LD_LIBRARY_PATH"], "/original")
        self.assertEqual(environment["PYINSTALLER_RESET_ENVIRONMENT"], "1")
        self.assertEqual(command, "/bin/bash")
        self.assertEqual(args[3:], ["sos-update", "v0.0.2", updates.INSTALL_URL,
                                  str(Path.home() / ".local/bin/sos"), str(Path.cwd()),
                                  "--socket", "/tmp/socket with spaces"])
        with self.assertRaises(ValueError):
            updates.install_and_restart("$(touch bad)", Path.cwd(), [])

    def test_handoff_installs_then_restarts_in_original_directory(self):
        self.run_handoff(0)

    def test_failed_install_does_not_restart(self):
        self.run_handoff(7)

    def test_handoff_preserves_the_controlling_terminal(self):
        self.run_handoff(0, terminal=True)

    def test_old_installer_cannot_ignore_confirmed_version(self):
        self.run_handoff(1, legacy=True)

    def run_handoff(self, install_status, terminal=False, legacy=False):
        root = Path(__file__).resolve().parents[3] / "build-ai" / "update-tests"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as directory:
            folder = Path(directory)
            workspace = folder / "workspace with spaces"
            workspace.mkdir()
            installer = folder / "source.sh"
            installer.write_text('#!/bin/bash\n# SOS_UPDATE_PROTOCOL=1\n[[ "$*" == "--yes --expected-version v0.0.2" ]] || exit 9\n'
                                 'if [[ -n "${TEST_TTY:-}" ]]; then tty > "$TEST_TTY"; fi\n'
                                 f'exit {install_status}\n')
            if legacy:
                installer.write_text("#!/bin/bash\nexit 0\n")
            curl = folder / "curl"
            curl.write_text('#!/bin/bash\nwhile [[ "$#" -gt 0 ]]; do\n'
                            'if [[ "$1" == --output ]]; then cp "$TEST_INSTALLER" "$2"; exit; fi\nshift\ndone\nexit 8\n')
            curl.chmod(0o755)
            launcher = folder / "new sos"
            launcher.write_text('#!/bin/bash\nprintf "%s\\n" "$PWD" "$@" "$PYINSTALLER_RESET_ENVIRONMENT" > "$TEST_RESULT"\n'
                                'if [[ -n "${TEST_TTY:-}" ]]; then tty >> "$TEST_TTY"; fi\n')
            launcher.chmod(0o755)
            result = folder / "result"
            env = {**os.environ, "PATH": f"{folder}:/usr/bin:/bin", "TEST_INSTALLER": str(installer), "TEST_RESULT": str(result)}
            command = ["/bin/bash", "-c", updates.HANDOFF, "sos-update", "v0.0.2",
                       updates.INSTALL_URL, str(launcher), str(workspace), "--socket", "a b"]
            if terminal:
                env["TEST_TTY"] = str(folder / "tty")
                pid, master = pty.fork()
                if pid == 0:
                    os.execve(command[0], command, env)
                status = None
                try:
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        done, value = os.waitpid(pid, os.WNOHANG)
                        if done:
                            status = value
                            break
                        time.sleep(0.01)
                    self.assertIsNotNone(status, "Updater did not exit")
                    self.assertEqual(os.waitstatus_to_exitcode(status), 0)
                    before, after = (folder / "tty").read_text().splitlines()
                    self.assertTrue(before.startswith("/dev/"))
                    self.assertEqual(before, after)
                finally:
                    if status is None:
                        os.kill(pid, signal.SIGKILL)
                        os.waitpid(pid, 0)
                    os.close(master)
            else:
                process = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(process.returncode, install_status, process.stderr)
            if install_status == 0:
                self.assertEqual(result.read_text().splitlines(), [str(workspace), "--socket", "a b", "1"])
            else:
                self.assertFalse(result.exists())


class UpdateUITest(unittest.IsolatedAsyncioTestCase):
    async def test_queued_spinner_frame_is_safe_after_chat_unmounts(self):
        from sos_tui.history_viewport import HistoryViewport
        from sos_tui.mode_button import ModeButton
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test() as pilot:
                app.busy = True
                await app.query_one(HistoryViewport).remove()
                app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(0))

    async def test_version_is_right_aligned_on_tabs_row_and_dev_skips_check(self):
        app = SosApp()
        with patch.object(app, "listen"), patch("sos_tui.app.VERSION", "dev"), patch("sos_tui.app.latest_update") as check:
            async with app.run_test(size=(100, 30)) as pilot:
                await pilot.pause()
                version = app.query_one("#app-version")
                tabs = app.query_one("#tabs")
                self.assertEqual(str(version.content), "dev")
                self.assertEqual(version.region.y, tabs.region.y)
                self.assertEqual(version.region.right, 100)
                self.assertEqual(tabs.region.right, version.region.x)
                check.assert_not_called()

    async def test_release_label_keeps_single_v_prefix(self):
        for value in ("v0.0.1", "0.0.1"):
            app = SosApp()
            with patch.object(app, "listen"), patch("sos_tui.app.VERSION", value), \
                    patch("sos_tui.app.latest_update", return_value=None):
                async with app.run_test() as pilot:
                    await pilot.pause()
                    self.assertEqual(str(app.query_one("#app-version").content), "v0.0.1")

    async def test_not_now_is_default_and_escape_preserves_application(self):
        app = SosApp()
        with patch.object(app, "listen"), patch("sos_tui.app.VERSION", "v0.0.1"), \
                patch("sos_tui.app.latest_update", return_value="v0.0.2"):
            async with app.run_test() as pilot:
                await wait_until(pilot, lambda: isinstance(app.screen, UpdateScreen), "update popup")
                await pilot.pause()
                self.assertTrue(app.screen.query_one("#update-later").has_focus)
                await pilot.press("escape")
                self.assertIsNone(app.update_version)
                self.assertNotIsInstance(app.screen, UpdateScreen)

    async def test_enter_defaults_to_not_now(self):
        app = SosApp()
        with patch.object(app, "listen"), patch("sos_tui.app.VERSION", "v0.0.1"), \
                patch("sos_tui.app.latest_update", return_value="v0.0.2"):
            async with app.run_test() as pilot:
                await wait_until(pilot, lambda: isinstance(app.screen, UpdateScreen), "update popup")
                await pilot.pause()
                await pilot.press("enter")
                await wait_until(pilot, lambda: not isinstance(app.screen, UpdateScreen), "dismissed update popup")
                self.assertIsNone(app.update_version)

    async def test_acceptance_returns_version_for_post_tui_handoff(self):
        app = SosApp()
        with patch.object(app, "listen"), patch("sos_tui.app.VERSION", "v0.0.1"), \
                patch("sos_tui.app.latest_update", return_value="v0.0.2"):
            async with app.run_test() as pilot:
                await wait_until(pilot, lambda: isinstance(app.screen, UpdateScreen), "update popup")
                await pilot.pause()
                await pilot.press("right", "enter")
            self.assertEqual(app.update_version, "v0.0.2")
