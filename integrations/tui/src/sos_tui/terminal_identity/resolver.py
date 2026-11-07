"""Resolve the terminal at the client, never from the shared daemon."""

from .multiplexers import MULTIPLEXER_DETECTORS, is_multiplexed
from .native import NATIVE_DETECTORS
from .system import SystemTerminalDetector, current_context
from .types import TerminalContext, TerminalDetector, TerminalIdentity


def _resolve(context: TerminalContext) -> TerminalIdentity | None:
    detectors: tuple[TerminalDetector, ...] = MULTIPLEXER_DETECTORS
    if not is_multiplexed(context):
        detectors += NATIVE_DETECTORS
    detectors += (SystemTerminalDetector(),)
    for detector in detectors:
        identity = detector.detect(context)
        if identity is not None:
            return identity
    return None


def resolve_terminal_identity() -> TerminalIdentity | None:
    return _resolve(current_context())
