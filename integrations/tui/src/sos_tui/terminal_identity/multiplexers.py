"""Multiplexed panes must not inherit the outer emulator's identity."""

from dataclasses import dataclass

from .types import TerminalContext, TerminalIdentity


def is_multiplexed(context: TerminalContext) -> bool:
    env = context.environment
    return any(name in env for name in ("TMUX", "TMUX_PANE", "ZELLIJ", "STY"))


@dataclass(frozen=True)
class MultiplexerDetector:
    source: str
    variable: str

    def detect(self, context: TerminalContext) -> TerminalIdentity | None:
        env = context.environment
        if self.variable not in env or not context.tty or not context.system_session:
            return None
        # With nested multiplexers, inherited environment cannot tell which is innermost.
        if "TMUX" in env and "ZELLIJ" in env:
            return None
        # TTY + session birth identifies the actual pane, including over SSH.
        # Zellij's session name is not a pane ID and may be stale after renaming.
        return context.identity(self.source, context.tty, context.system_session)


MULTIPLEXER_DETECTORS = (
    MultiplexerDetector("tmux", "TMUX"),
    MultiplexerDetector("zellij", "ZELLIJ"),
)
