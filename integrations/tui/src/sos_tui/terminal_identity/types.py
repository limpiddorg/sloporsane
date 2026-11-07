"""Shared contract for terminal identity detectors."""

from dataclasses import dataclass
import hashlib
import json
from typing import Mapping, Protocol


@dataclass(frozen=True)
class TerminalIdentity:
    """Opaque persistence key; source is diagnostic, not application logic."""

    key: str
    source: str


@dataclass(frozen=True)
class TerminalContext:
    environment: Mapping[str, str]
    tty: str | None
    system_session: str | None
    host_scope: str
    remote: bool = False

    def identity(self, source: str, *parts: str) -> TerminalIdentity:
        payload = json.dumps([self.host_scope, source, *parts], ensure_ascii=True)
        digest = hashlib.sha256(payload.encode()).hexdigest()
        return TerminalIdentity(f"terminal-v1:{digest}", source)


class TerminalDetector(Protocol):
    def detect(self, context: TerminalContext) -> TerminalIdentity | None:
        """Return an identity only when enough information is available."""
        ...
