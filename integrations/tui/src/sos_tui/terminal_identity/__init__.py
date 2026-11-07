"""Terminal identity discovery without persistence or resume UI side effects."""

from .resolver import resolve_terminal_identity
from .types import TerminalIdentity

__all__ = ["TerminalIdentity", "resolve_terminal_identity"]
