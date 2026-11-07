"""Read-only full-text preview of a compact paste in the composer."""

from collections.abc import Callable

from rich.text import Text
from textual.app import ComposeResult
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.widgets import Button, Static

from .dialog_screen import DialogScreen


class PastePreview(DialogScreen):
    BINDINGS = [("escape", "close", "Close")]
    DEFAULT_CSS = """
    PastePreview .popup-dialog { height: 80%; }
    PastePreview .paste-preview-content { height: 1fr; margin: 1 0;
        scrollbar-size: 1 1; scrollbar-color: $muted; scrollbar-background: $background; }
    PastePreview .paste-preview-actions { height: auto; }
    PastePreview Button { width: auto; margin-right: 1; }
    """

    def __init__(self, text: str, remove: Callable[[], None], title: str = "Pasted text") -> None:
        super().__init__()
        self.text = text
        self.remove_paste = remove
        self.heading = title

    def compose(self) -> ComposeResult:
        with Vertical(classes="popup-dialog"):
            yield Static(self.heading, classes="popup-heading")
            with VerticalScroll(classes="paste-preview-content"):
                yield Static(Text(self.text), id="paste-preview-text")
            with Horizontal(classes="paste-preview-actions"):
                yield Button("Remove", id="paste-remove", compact=True)
            yield Static("Tab Actions · Enter Remove · Esc Close", classes="popup-status")

    def on_button_pressed(self, event: Button.Pressed) -> None:
        event.stop()
        if event.button.id == "paste-remove":
            self.remove_paste()
        self.action_close()

    def action_close(self) -> None:
        self.dismiss()
