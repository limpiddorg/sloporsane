"""Small, non-focusable clipboard feedback above the active screen."""

from textual.app import ComposeResult
from textual.containers import Horizontal
from textual.timer import Timer
from textual.widgets import Static

from .config import INPUT_BACKGROUND_COLOR, TITLE_MD


class CopiedNotice(Horizontal):
    ALLOW_SELECT = False
    DURATION = 1.4
    DEFAULT_CSS = f"""
    CopiedNotice {{
        position: absolute; overlay: screen; layer: _toastrack;
        width: 14; height: 3; padding: 0; border: none;
        background: {INPUT_BACKGROUND_COLOR};
    }}
    CopiedNotice > .copied-band {{
        width: 1; height: 3; border-left: outer {TITLE_MD};
    }}
    CopiedNotice > .copied-label {{
        width: 13; height: 3; padding: 1 4 1 3;
        background: {INPUT_BACKGROUND_COLOR}; color: {TITLE_MD}; text-style: bold;
    }}
    """

    def __init__(self) -> None:
        super().__init__()
        self.hide_timer: Timer | None = None

    def compose(self) -> ComposeResult:
        yield Static("", classes="copied-band")
        yield Static("Copied", markup=False, classes="copied-label")

    def on_mount(self) -> None:
        self.screen.screen_layout_refresh_signal.subscribe(self, lambda _: self.reposition())

    def show_copy(self) -> None:
        self.display = True
        self.reposition()
        if self.hide_timer is not None:
            self.hide_timer.stop()
        self.hide_timer = self.set_timer(self.DURATION, self.hide)

    def hide(self) -> None:
        self.display = False

    def reposition(self) -> None:
        if self.is_attached:
            bar = next((bar for bar in self.screen.query("#agent-info, #implementation-agent-info") if bar.display), None)
            bottom = bar.region.y if bar is not None else self.screen.size.height - 1
            self.styles.offset = (1, max(0, bottom - 3))
