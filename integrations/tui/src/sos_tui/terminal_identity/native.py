"""Native emulator IDs. Numeric IDs are scoped to the live OS session."""

from dataclasses import dataclass

from .types import TerminalContext, TerminalIdentity


@dataclass(frozen=True)
class NativeTerminalDetector:
    source: str
    variables: tuple[str, ...]
    needs_system_scope: bool = False

    def detect(self, context: TerminalContext) -> TerminalIdentity | None:
        if context.remote:
            return None
        values = tuple(context.environment.get(name, "").strip() for name in self.variables)
        if not all(values):
            return None
        if self.needs_system_scope:
            if not context.tty or not context.system_session:
                return None
            values += (context.tty, context.system_session)
        return context.identity(self.source, *values)


NATIVE_DETECTORS = (
    NativeTerminalDetector("iterm2", ("ITERM_SESSION_ID",)),
    NativeTerminalDetector("term-session", ("TERM_SESSION_ID",)),
    NativeTerminalDetector("ghostty", ("GHOSTTY_SURFACE_ID",), needs_system_scope=True),
    NativeTerminalDetector("wezterm", ("WEZTERM_PANE",), needs_system_scope=True),
    NativeTerminalDetector("kitty", ("KITTY_PID", "KITTY_WINDOW_ID"), needs_system_scope=True),
    NativeTerminalDetector("konsole", ("KONSOLE_DBUS_SERVICE", "KONSOLE_DBUS_SESSION"), needs_system_scope=True),
    NativeTerminalDetector("windows-terminal", ("WT_SESSION",)),
)
