"""Bracketed indeterminate fill displayed in place of context usage."""

from rich.text import Text
from textual.timer import Timer
from textual.widget import Widget

from .config import MISC_MD


class HistoryLoadingBar(Widget):
    can_focus = False
    DEFAULT_CSS = "HistoryLoadingBar { display: none; height: 1; width: 100%; }"

    def __init__(self, **kwargs) -> None:
        super().__init__(**kwargs)
        self.phase = 0
        self.timer: Timer | None = None

    def on_mount(self) -> None:
        self.timer = self.set_interval(0.05, self.advance, pause=True)

    def start(self) -> None:
        self.phase = 0
        self.display = True
        if self.timer is not None:
            self.timer.resume()
        self.refresh()

    def stop(self) -> None:
        self.display = False
        if self.timer is not None:
            self.timer.pause()

    def advance(self) -> None:
        # Repeating sweep, not a percentage of completed work.
        self.phase = (self.phase + 1) % 33
        self.refresh()

    def render(self) -> Text:
        width = self.size.width
        if width < 3:
            return Text("")
        cells = width - 2
        filled = min(cells, max(1, cells * self.phase // 32))
        return Text("[" + "#" * filled + " " * (cells - filled) + "]", style=f"bold {MISC_MD}")
