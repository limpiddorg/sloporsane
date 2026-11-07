"""A persistent reading aid, separate from assistant responses and Activities."""

from rich.text import Text
from textual.app import ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Static
from textual.timer import Timer

from .config import MISC_MD
from .turn_activity import ActivityToggle


class ConversationRecap(Horizontal):
    DEFAULT_CSS = """
    ConversationRecap { height: auto; background: $content-background; }
    ConversationRecap > .recap-band { dock: left; width: 1; height: 100%; border-left: outer $misc-md; }
    ConversationRecap > .recap-content { height: auto; padding: 0 1; }
    ConversationRecap .recap-heading { height: 1; color: $misc-md; text-style: bold; }
    ConversationRecap .recap-text { height: auto; color: $misc-md; }
    """

    def __init__(self, text: str | None = None) -> None:
        super().__init__(classes="chat-message")
        self.text = text
        self.spinner_frame = 0
        self.spinner_timer: Timer | None = None

    @property
    def loading(self) -> bool:
        return self.text is None

    def body(self) -> Text:
        return (Text(ActivityToggle.SPINNER_FRAMES[self.spinner_frame], style=MISC_MD)
                if self.loading else Text(self.text))

    def on_mount(self) -> None:
        if self.loading:
            self.spinner_timer = self.set_interval(1 / 8, self.advance_spinner)

    def advance_spinner(self) -> None:
        self.spinner_frame = (self.spinner_frame + 1) % len(ActivityToggle.SPINNER_FRAMES)
        self.query_one(".recap-text", Static).update(self.body())

    def stop_spinner(self) -> None:
        if self.spinner_timer is not None:
            self.spinner_timer.stop()
            self.spinner_timer = None

    def finish(self, text: str) -> None:
        self.text = text
        self.stop_spinner()
        self.query_one(".recap-text", Static).update(self.body())

    def on_unmount(self) -> None:
        self.stop_spinner()

    def compose(self) -> ComposeResult:
        yield Static("", classes="recap-band")
        with Vertical(classes="recap-content"):
            yield Static("Conversation Recap", classes="recap-heading")
            yield Static(self.body(), classes="recap-text")
