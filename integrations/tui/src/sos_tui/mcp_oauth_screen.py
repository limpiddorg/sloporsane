"""Temporary authorization dialog above MCP management."""

from collections.abc import Callable

from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, VerticalScroll
from textual.widgets import Button, Static

from .dialog_screen import DialogScreen


class McpOAuthScreen(DialogScreen):
    BINDINGS = [Binding("escape", "cancel", show=False)]
    DEFAULT_CSS = """
    McpOAuthScreen #mcp-oauth-url { height: auto; margin: 1 0; }
    McpOAuthScreen Horizontal { height: auto; }
    McpOAuthScreen Button { width: auto; min-width: 10; margin-right: 1; }
    """

    def __init__(self, url: str, instructions: str, open_browser: Callable[[], None],
                 cancel: Callable[[], None]) -> None:
        super().__init__()
        self.url = url
        self.instructions = instructions
        self.open_browser = open_browser
        self.cancel = cancel

    def compose(self) -> ComposeResult:
        with VerticalScroll(classes="popup-dialog"):
            yield Static("Authorize MCP connection", classes="popup-heading")
            yield Static(self.instructions, markup=False)
            yield Static(self.url, id="mcp-oauth-url", markup=False)
            with Horizontal():
                yield Button("Open in Browser", id="mcp-browser")
            yield Static("Enter Open in Browser · Esc Cancel", classes="popup-status")
            yield Static("", id="mcp-oauth-status", classes="popup-status", markup=False)

    def on_mount(self) -> None:
        self.query_one("#mcp-browser", Button).focus()
        self.call_after_refresh(self.open_browser)

    def on_button_pressed(self, event: Button.Pressed) -> None:
        event.stop()
        if event.button.id == "mcp-browser":
            self.open_browser()

    def action_cancel(self) -> None:
        self.cancel()

    def set_status(self, text: str) -> None:
        self.query_one("#mcp-oauth-status", Static).update(text)
