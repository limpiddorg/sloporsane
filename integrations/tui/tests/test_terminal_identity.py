import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

from sos_tui.terminal_identity import resolve_terminal_identity
from sos_tui.terminal_identity.resolver import _resolve
from sos_tui.terminal_identity.system import _session_fingerprint, current_context
from sos_tui.terminal_identity.types import TerminalContext, TerminalDetector


def context(environment=None, *, tty="/dev/pts/4", session="boot:42:100", remote=False):
    return TerminalContext(environment or {}, tty, session, "host:user", remote)


class TerminalIdentityTest(unittest.TestCase):
    def test_native_emulators(self):
        cases = (
            ("iterm2", {"ITERM_SESSION_ID": "w0t0p0:uuid"}),
            ("term-session", {"TERM_SESSION_ID": "uuid"}),
            ("ghostty", {"GHOSTTY_SURFACE_ID": "surface"}),
            ("wezterm", {"WEZTERM_PANE": "3"}),
            ("kitty", {"KITTY_PID": "20", "KITTY_WINDOW_ID": "2"}),
            ("konsole", {"KONSOLE_DBUS_SERVICE": ":1.23", "KONSOLE_DBUS_SESSION": "/Sessions/2"}),
            ("windows-terminal", {"WT_SESSION": "uuid"}),
        )
        for source, env in cases:
            with self.subTest(source=source):
                identity = _resolve(context(env))
                self.assertEqual(identity.source, source)
                self.assertEqual(identity, _resolve(context(env)))
                self.assertTrue(identity.key.startswith("terminal-v1:"))

    def test_distinct_panes(self):
        for env in ({}, {"TMUX": "socket,5,0", "TMUX_PANE": "%1"},
                    {"ZELLIJ": "0", "ZELLIJ_SESSION_NAME": "work"}):
            with self.subTest(env=env):
                self.assertNotEqual(_resolve(context(env)), _resolve(context(env, tty="/dev/pts/5")))

    def test_multiplexers_override_outer_emulator(self):
        for variable, source in (("TMUX", "tmux"), ("ZELLIJ", "zellij")):
            env = {variable: "session", "ITERM_SESSION_ID": "outer"}
            self.assertEqual(_resolve(context(env)).source, source)
            # An inherited native ID must not merge panes when the OS probe fails.
            self.assertIsNone(_resolve(context(env, session=None)))

    def test_zellij_rename_does_not_change_identity(self):
        before = {"ZELLIJ": "0", "ZELLIJ_SESSION_NAME": "before"}
        after = {"ZELLIJ": "0", "ZELLIJ_SESSION_NAME": "after"}
        self.assertEqual(_resolve(context(before)), _resolve(context(after)))

    def test_nested_or_unknown_multiplexer_uses_actual_tty(self):
        for env in ({"TMUX": "socket", "ZELLIJ": "0"}, {"STY": "screen"}, {"TMUX_PANE": "%2"}):
            env["WT_SESSION"] = "outer"
            self.assertEqual(_resolve(context(env)).source, "system")

    def test_ssh_does_not_trust_forwarded_native_id(self):
        env = {"ITERM_SESSION_ID": "local", "SSH_CONNECTION": "connection"}
        self.assertEqual(_resolve(context(env, remote=True)).source, "system")
        self.assertIsNone(_resolve(context(env, remote=True, session=None)))

    def test_remote_multiplexer_survives_reattachment(self):
        env = {"TMUX": "socket,42,0"}
        self.assertEqual(_resolve(context(env)), _resolve(context(env, remote=True)))

    def test_recycled_tty_and_pid_do_not_collide(self):
        env = {"WEZTERM_PANE": "1"}
        self.assertNotEqual(_resolve(context(env)), _resolve(context(env, session="boot:42:101")))
        self.assertNotEqual(_resolve(context(env)), _resolve(context(env, session="new-boot:42:100")))

    def test_host_and_user_scope(self):
        original = context({"WT_SESSION": "uuid"})
        other = TerminalContext(original.environment, original.tty, original.system_session, "other:user")
        self.assertNotEqual(_resolve(original), _resolve(other))

    def test_incomplete_and_blank_native_values_fall_back(self):
        for env in ({"KITTY_WINDOW_ID": "1"}, {"WT_SESSION": "  "}, {"KONSOLE_DBUS_SERVICE": "service"}):
            self.assertEqual(_resolve(context(env)).source, "system")

    def test_missing_system_identity(self):
        self.assertIsNone(_resolve(context(tty=None)))
        self.assertIsNone(_resolve(context(session=None)))
        self.assertIsNone(_resolve(context({"WEZTERM_PANE": "1"}, session=None)))
        self.assertEqual(_resolve(context({"WT_SESSION": "uuid"}, tty=None, session=None)).source,
                         "windows-terminal")

    def test_public_api_uses_client_context(self):
        with patch("sos_tui.terminal_identity.resolver.current_context", return_value=context()) as probe:
            self.assertEqual(resolve_terminal_identity(), _resolve(context()))
            probe.assert_called_once_with()

    def test_context_handles_non_terminal_and_ssh(self):
        with patch.dict(os.environ, {"SSH_CONNECTION": "connection"}, clear=True), \
             patch("os.ttyname", side_effect=OSError):
            actual = current_context()
        self.assertTrue(actual.remote)
        self.assertIsNone(actual.tty)
        self.assertIsNone(actual.system_session)

    def test_linux_stat_handles_complex_process_name(self):
        fields = ["S"] + ["0"] * 18 + ["123456"]
        with patch("sys.platform", "linux"), patch.object(Path, "read_text", side_effect=[
            "42 (name with ) spaces) " + " ".join(fields), "boot\n"
        ]):
            self.assertEqual(_session_fingerprint(42), "boot:42:123456")

    def test_unavailable_process_information(self):
        with patch("sys.platform", "linux"), patch.object(Path, "read_text", side_effect=OSError):
            self.assertIsNone(_session_fingerprint(42))

    def test_mac_fingerprint(self):
        with patch("sys.platform", "darwin"), \
             patch("sos_tui.terminal_identity.system._command", side_effect=["start date", "boot time"]):
            self.assertEqual(_session_fingerprint(42), "boot time:42:start date")

    @unittest.skipUnless(sys.platform.startswith("linux") or sys.platform == "darwin", "Unix OS probe")
    def test_real_terminal_session_across_client_restarts(self):
        import pty

        master, slave = pty.openpty()
        child = "from sos_tui.terminal_identity import resolve_terminal_identity; print(resolve_terminal_identity().key)"
        launcher = (
            "import subprocess,sys,json; "
            f"print(json.dumps([subprocess.check_output([sys.executable,'-c',{child!r}], "
            "stdin=sys.stdin, text=True).strip() for _ in range(2)]))"
        )
        # Strip native/multiplexer variables to exercise the actual OS fallback.
        env = {name: value for name, value in os.environ.items()
               if name in ("PATH", "PYTHONPATH", "HOME", "SYSTEMROOT")}
        try:
            result = subprocess.run([sys.executable, "-c", launcher], stdin=slave,
                                    capture_output=True, text=True, env=env,
                                    start_new_session=True, timeout=10, check=True)
            keys = json.loads(result.stdout)
            self.assertEqual(keys[0], keys[1])
            self.assertTrue(keys[0].startswith("terminal-v1:"))
        finally:
            os.close(slave)
            os.close(master)


if __name__ == "__main__":
    unittest.main()
