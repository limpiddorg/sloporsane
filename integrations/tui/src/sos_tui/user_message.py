"""A user message retaining the mode selected when it was sent."""

from rich.text import Text
from textual.app import ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Static

from .config import MODE_COLORS, TITLE_MD, TEXT_COLOR


class UserMessage(Horizontal):
    DEFAULT_CSS = """
    UserMessage { height: auto; margin-bottom: 0; background: $content-background; }
    UserMessage > .mode-band { dock: left; width: 1; height: 100%; }
    UserMessage > .user-content { width: 1fr; height: auto; padding-left: 1; }
    UserMessage .user-text { height: auto; }
    UserMessage .user-quote { background: $quote-background; border-left: outer $muted; padding: 0 2; }
    UserMessage.sane-request > .mode-band { display: none; }
    UserMessage.sane-request .user-text { color: $sane; text-style: bold; }
    """

    def __init__(self, text: str, mode: str, images=()) -> None:
        super().__init__()
        self.text = text
        self.mode = mode
        self.images = images

    def display_text(self) -> Text:
        text = Text(self.text)
        cursor = 0
        for image in self.images:
            origin = image.get("origin")
            if not origin:
                continue
            label = "[Pasted Image]" if origin == "clipboard" else f"[Image • {origin}]"
            start = self.text.find(label, cursor)
            if start != -1:
                cursor = start + len(label)
                text.stylize(TITLE_MD, start, cursor)
        return text

    def set_images(self, images) -> None:
        self.images = images
        if self.is_mounted:
            for widget, (_, text) in zip(self.query(".user-text"), self.text_blocks()):
                widget.update(text)

    def text_blocks(self) -> list[tuple[bool, Text]]:
        blocks: list[tuple[bool, Text]] = []
        for line in self.display_text().split("\n", allow_blank=True):
            quoted = line.plain == ">" or line.plain.startswith("> ")
            if quoted:
                line = line[2:]
                line.stylize(TEXT_COLOR)
            if blocks and blocks[-1][0] == quoted:
                blocks[-1][1].append("\n").append(line)
            else:
                blocks.append((quoted, line))
        return blocks

    def compose(self) -> ComposeResult:
        band = Static("", classes="mode-band")
        # A colored cell background fills every row, including wrapped lines.
        band.styles.background = MODE_COLORS[self.mode]
        yield band
        with Vertical(classes="user-content"):
            for quoted, text in self.text_blocks():
                yield Static(text, classes="user-text user-quote" if quoted else "user-text")
