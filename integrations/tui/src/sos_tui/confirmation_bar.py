"""Full-width confirmation countdown, driven by the action's actual deadline."""

from math import ceil
from time import monotonic
from typing import Literal

from rich.style import Style
from rich.text import Text
from textual.strip import Strip
from textual.widgets import Button

from .config import BACKGROUND_COLOR, SLOP_COLOR, TEXT_COLOR


ConfirmationKind = Literal["exit", "cancel"]


class ConfirmationBar(Button, can_focus=False):
    FOCUS_ON_CLICK = False
    DEFAULT_CSS = """
    ConfirmationBar { display: none; width: 1fr; min-width: 0; height: 1; min-height: 1;
        border: none; padding: 0; margin: 0; background: $background; }
    """

    def __init__(self, **kwargs) -> None:
        super().__init__("", compact=True, **kwargs)
        self.active_effect_duration = 0
        self.kind: ConfirmationKind | None = None
        self.deadline: float | None = None
        self.duration = 1.0

    def start(self, kind: ConfirmationKind, deadline: float | None, duration: float) -> None:
        self.kind = kind
        self.deadline = deadline
        self.duration = duration
        self.label = ("Exit ? Confirm with CTRL-C" if kind == "exit" else
                      "Cancel Execution ? Confirm with ESC" if deadline is not None else "Cancelling…")
        self.refresh()

    def stop(self) -> None:
        self.kind = None
        self.deadline = None

    @property
    def remaining(self) -> float:
        if self.kind is None:
            return 0.0
        if self.deadline is None:
            return 1.0  # Cancellation confirmed; keep its feedback until the terminal event.
        return min(1.0, max(0.0, (self.deadline - monotonic()) / self.duration))

    def render(self) -> Text:
        width = self.content_size.width
        label = str(self.label)[:width].center(width)
        filled = ceil(width * self.remaining)
        text = Text(no_wrap=True)
        text.append(label[:filled], style=Style(color=BACKGROUND_COLOR, bgcolor=SLOP_COLOR, bold=True))
        text.append(label[filled:], style=Style(color=TEXT_COLOR, bgcolor=BACKGROUND_COLOR, bold=True))
        return text

    def render_line(self, y: int) -> Strip:
        return Strip(self.render().render(self.app.console), self.content_size.width)
