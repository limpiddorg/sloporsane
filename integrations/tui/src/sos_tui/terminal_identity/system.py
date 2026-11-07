"""Local OS probes, independent of SOS and its daemon."""

import os
from pathlib import Path
import socket
import subprocess
import sys

from .types import TerminalContext, TerminalIdentity


def _command(*arguments: str) -> str | None:
    try:
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=1, check=False)
        return result.stdout.strip() or None if result.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def _session_fingerprint(session_id: int) -> str | None:
    try:
        if sys.platform.startswith("linux"):
            stat = Path(f"/proc/{session_id}/stat").read_text()
            # comm can contain spaces and parentheses. Remaining fields start at field 3.
            start_ticks = stat[stat.rfind(")") + 2:].split()[19]
            boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
            return f"{boot}:{session_id}:{start_ticks}" if boot else None
        if sys.platform == "darwin":
            started = _command("ps", "-p", str(session_id), "-o", "lstart=")
            boot = _command("sysctl", "-n", "kern.boottime")
            if started and boot:
                return f"{boot}:{session_id}:{started}"
    except (OSError, IndexError):
        pass
    return None


def current_context() -> TerminalContext:
    environment = dict(os.environ)
    tty = None
    for fd in (0, 1, 2):
        try:
            tty = os.ttyname(fd)
            break
        except (OSError, AttributeError):
            pass
    session = None
    if tty:
        try:
            session = _session_fingerprint(os.getsid(0))
        except (OSError, AttributeError):
            pass
    user = str(os.getuid()) if hasattr(os, "getuid") else environment.get("USERNAME", "")
    return TerminalContext(environment, tty, session, f"{socket.gethostname()}:{user}",
                           bool(environment.get("SSH_CONNECTION") or environment.get("SSH_TTY")))


class SystemTerminalDetector:
    def detect(self, context: TerminalContext) -> TerminalIdentity | None:
        if context.tty and context.system_session:
            return context.identity("system", context.tty, context.system_session)
        return None
