"""Full-width, non-focusable input rejection feedback; never edits the draft."""

from rich.text import Text
from textual.timer import Timer
from textual.widgets import Static

from .config import INPUT_BACKGROUND_COLOR, SLOP_COLOR, TEXT_COLOR, TITLE_MD


class InputErrorNotice(Static):
    ALLOW_SELECT = False
    DURATION = 5.0
    DEFAULT_CSS = f"""
    InputErrorNotice {{
        position: absolute; overlay: screen; layer: _toastrack;
        height: auto; padding: 1; border-left: outer {SLOP_COLOR};
        background: {INPUT_BACKGROUND_COLOR}; color: {TEXT_COLOR};
    }}
    """

    def __init__(self) -> None:
        super().__init__(markup=False)
        self.hide_timer: Timer | None = None
        self.reposition_pending = False

    def on_mount(self) -> None:
        self.screen.screen_layout_refresh_signal.subscribe(self, lambda _: self.schedule_reposition())

    def schedule_reposition(self) -> None:
        if self.display and not self.reposition_pending:
            self.reposition_pending = self.call_after_refresh(self.finish_reposition)

    def finish_reposition(self) -> None:
        self.reposition_pending = False
        self.reposition()

    def show_error(self, message: str, warning: bool = False) -> None:
        self.styles.border_left = ("outer", TITLE_MD if warning else SLOP_COLOR)
        self.update(Text(message))
        self.display = True
        self.reposition()
        if self.hide_timer is not None:
            self.hide_timer.stop()
        self.hide_timer = self.set_timer(self.DURATION, self.hide)

    def hide(self) -> None:
        self.display = False

    def reposition(self) -> None:
        if not self.is_attached or not self.display:
            return
        width = max(1, self.screen.size.width - 2)
        if self.styles.width is None or self.styles.width.value != width:
            self.styles.width = width
        bar = next((bar for bar in self.screen.query("#agent-info, #implementation-agent-info") if bar.display), None)
        bottom = bar.region.y if bar is not None else self.screen.size.height - 1
        for panel in self.screen.query("#command-suggestions, SkillDetails"):
            if panel.display and panel.region.height:
                bottom = min(bottom, panel.region.y)
        self.styles.offset = (1, max(0, bottom - self.outer_size.height))
