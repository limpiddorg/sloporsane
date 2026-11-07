"""Separate configuration approval and masked-token dialogs for MCP Connect."""

from collections.abc import Callable

from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import VerticalScroll
from textual.widgets import Input, Static

from .dialog_screen import DialogScreen


class McpConnectScreen(DialogScreen):
    BINDINGS = [Binding("escape", "cancel", show=False),
                Binding("enter", "confirm", show=False, priority=True)]
    DEFAULT_CSS = """
    McpConnectScreen Input { margin: 1 0; }
    McpConnectScreen #mcp-token-value, McpConnectScreen #mcp-token-value:focus {
        height: 1; min-height: 1; border: none; padding: 0 1; }
    """

    def __init__(self, text: str, submit: Callable[[str], None], cancel: Callable[[], None], *, token: bool = False) -> None:
        super().__init__()
        self.text, self.submit, self.cancel, self.token = text, submit, cancel, token

    def compose(self) -> ComposeResult:
        with VerticalScroll(classes="popup-dialog"):
            yield Static("MCP bearer token" if self.token else "Approve MCP connection", classes="popup-heading")
            yield Static(self.text, markup=False)
            if self.token:
                yield Input(password=True, placeholder="Bearer token", id="mcp-token-value")
            yield Static("Enter Confirm · Esc Cancel", classes="popup-status")
            yield Static("", id="mcp-form-status", classes="popup-status", markup=False)

    def on_mount(self) -> None:
        if self.token:
            self.query_one(Input).focus()

    def action_confirm(self) -> None:
        self.confirm()

    def confirm(self) -> None:
        value = self.query_one(Input).value if self.token else ""
        if self.token and not value:
            self.query_one("#mcp-form-status", Static).update("Enter a bearer token or cancel.")
            return
        self.clear_secret()
        self.submit(value)

    def clear_secret(self) -> None:
        if self.token:
            self.query_one(Input).value = ""

    def action_cancel(self) -> None:
        self.clear_secret()
        self.cancel()
