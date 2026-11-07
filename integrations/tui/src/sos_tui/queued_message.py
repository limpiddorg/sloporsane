"""One local next-turn message, not yet part of the session history."""

from rich.text import Text
from textual.widgets import Static

from .config import MISC_MD


class QueuedMessage(Static):
    DEFAULT_CSS = "QueuedMessage { height: auto; margin-top: 1; padding: 0 1; background: $content-background; }"

    def __init__(self, text: str, images=(), *, interpret_commands: bool = True) -> None:
        self.text = text
        self.images = images
        self.interpret_commands = interpret_commands
        super().__init__(self.formatted_text())

    def formatted_text(self) -> Text:
        content = Text()
        content.append("[Pending]\n", style=MISC_MD)
        content.append(self.text)
        return content

    def append(self, text: str) -> None:
        self.text += "\n" + text
        self.update(self.formatted_text())
