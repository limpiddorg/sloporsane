"""MCP management with one explicit connect/authorization journey."""

import asyncio
import json
import webbrowser
from typing import Any

from rich.table import Table
from rich.text import Text
from textual import events
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.message import Message
from textual.widgets import Button, OptionList, Rule, Static
from textual.widgets.option_list import Option

from .dialog_screen import DialogScreen
from .mcp_oauth_screen import McpOAuthScreen
from .mcp_connect_screen import McpConnectScreen


def server_row(name: str, scope: str, connected: str, enabled: str) -> Table:
    row = Table.grid(expand=True, padding=(0, 1))
    row.add_column(ratio=1, no_wrap=True, overflow="ellipsis")
    row.add_column(width=11, no_wrap=True)
    row.add_column(width=9, justify="center", no_wrap=True)
    row.add_column(width=7, justify="center", no_wrap=True)
    row.add_row(*(Text(value) for value in (name, scope, connected, enabled)))
    return row


class McpsScreen(DialogScreen):
    AUTO_FOCUS = "#mcp-list"
    BINDINGS = [Binding("escape", "close", show=False),
                Binding("left", "select_action('connect')", show=False, priority=True),
                Binding("right", "select_action('enable')", show=False, priority=True),
                Binding("enter", "confirm", show=False, priority=True)]
    DEFAULT_CSS = """
    McpsScreen .popup-dialog { height: auto; }
    McpsScreen #mcp-body { height: auto; max-height: 100%; }
    McpsScreen #mcp-picker { margin-top: 1; }
    McpsScreen #mcp-list { height: auto; min-height: 1; max-height: 8; }
    McpsScreen #mcp-footer { height: 1; margin-top: 1; }
    McpsScreen #mcp-actions { height: 1; width: 1fr; }
    McpsScreen Button { margin-right: 1; }
    McpsScreen #mcp-connect { min-width: 14; }
    McpsScreen #mcp-enable { min-width: 11; }
    McpsScreen Button.selected-action { background: $depth-md !important; color: $foreground !important;
        text-style: bold !important; }
    McpsScreen #mcp-help { height: auto; margin-top: 1; }
    McpsScreen #mcp-detail { height: auto; margin-top: 1; }
    """

    class Requested(Message):
        def __init__(self, screen: "McpsScreen", payload: dict[str, Any]) -> None:
            super().__init__()
            self.screen, self.payload = screen, payload

    class Closed(Message):
        def __init__(self, screen: "McpsScreen") -> None:
            super().__init__()
            self.screen = screen

    def __init__(self) -> None:
        super().__init__()
        self.servers: list[dict[str, Any]] = []
        self.finished = False
        self.pending = False
        self.last_action = ""
        self.oauth_active = False
        self.oauth_url = ""
        self.oauth_screen: McpOAuthScreen | None = None
        self.connect_screen: McpConnectScreen | None = None
        self.oauth_cancelling = False
        self.reviewing: dict[str, Any] | None = None
        self.review_action = ""
        self.connect_target: tuple[str, str] | None = None
        self.authorization_attempted = False
        self.background_refresh = False
        self.deferred_request: dict[str, Any] | None = None
        self.rendered_rows: list[tuple[str, str, str, str]] = []
        self.selected_action = "connect"

    def compose(self) -> ComposeResult:
        with Vertical(classes="popup-dialog"):
            yield Static("MCP servers", classes="popup-heading")
            with VerticalScroll(id="mcp-body"):
                with Vertical(id="mcp-picker", classes="popup-picker"):
                    yield Static(server_row("Server", "Scope", "Connected", "Enabled"), id="mcp-columns",
                                 classes="popup-columns")
                    yield Rule(line_style="solid", classes="popup-separator")
                    yield OptionList(id="mcp-list", classes="popup-list")
                yield Static("", id="mcp-detail", markup=False)
                yield Static("", id="mcp-status", classes="popup-status", markup=False)
            with Horizontal(id="mcp-footer"):
                with Horizontal(id="mcp-actions"):
                    yield Button("Connect", id="mcp-connect", classes="selected-action")
                    yield Button("Enable", id="mcp-enable")
            yield Static("↑/↓ Select server · ←/→ Select action · Enter Confirm · Esc Close",
                         id="mcp-help", classes="popup-status")

    def on_mount(self) -> None:
        self.request({"type": "mcp.list"})
        self.set_interval(0.5, self.poll_oauth)
        self.shared_state_timer = self.set_interval(2, self.refresh_shared_state)

    def refresh_shared_state(self) -> None:
        if self.finished or self.pending or self.connect_target is not None or self.reviewing is not None:
            return
        self.background_refresh = True
        self.pending = True
        self.last_action = "mcp.list"
        self.update_actions()
        self.post_message(self.Requested(self, {"type": "mcp.list"}))

    def selected(self) -> dict[str, Any] | None:
        index = self.query_one("#mcp-list", OptionList).highlighted
        return self.servers[index] if index is not None and index < len(self.servers) else None

    def request(self, payload: dict[str, Any]) -> None:
        if self.background_refresh:
            # Finish the read before sending the user's action, without flashing
            # disabled controls or dropping the click while the reply is in flight.
            self.deferred_request = payload
            self.update_actions()
            return
        if self.pending:
            return
        self.last_action = payload["type"]
        self.pending = True
        self.set_status({"mcp.connect": "Connecting to the server…",
                         "mcp.oauth_start": "Preparing the authorization link…",
                         "mcp.oauth_poll": "Waiting for browser authorization…"}.get(self.last_action, "Working…"))
        self.update_actions()
        self.post_message(self.Requested(self, payload))

    def set_text(self, selector: str, text: str) -> None:
        widget = self.query_one(selector, Static)
        widget.display = bool(text)
        if widget.content != text:
            widget.update(text)

    def set_status(self, text: str) -> None:
        self.set_text("#mcp-status", text)

    def fail(self, text: str) -> None:
        self.pending = False
        self.background_refresh = False
        self.oauth_cancelling = False
        self.stop_connect()
        self.set_status(text)
        self.update_actions()

    def receive(self, event: dict[str, Any]) -> None:
        if self.oauth_cancelling and event["type"] not in ("mcp.cancelled", "request.failed"):
            return
        self.pending = False
        if self.background_refresh:
            self.background_refresh = False
            if event["type"] == "mcp.listed":
                self.update_servers(event)
            payload, self.deferred_request = self.deferred_request, None
            if payload is not None:
                self.request(payload)
            self.update_actions()
            return
        if event["type"] == "request.failed":
            action = self.last_action
            self.fail(event.get("error", "MCP request failed"))
            if action == "mcp.connect":
                self.request({"type": "mcp.list"})
            return
        if event["type"] == "mcp.oauth_started":
            if self.connect_target is None:
                self.request({"type": "mcp.cancel"})
                return
            self.oauth_active = True
            self.oauth_url = event["url"]
            self.oauth_screen = McpOAuthScreen(
                self.oauth_url, event["instructions"],
                lambda: self.run_worker(self.open_browser()), self.cancel_oauth)
            self.app.push_screen(self.oauth_screen)
            self.set_status(event["instructions"])
            self.update_actions()
            return
        if event["type"] == "mcp.oauth_pending":
            self.set_status("Waiting for browser authorization…")
            self.update_actions()
            return
        if event["type"] == "mcp.cancelled":
            self.oauth_cancelling = False
            self.stop_connect()
            self.request({"type": "mcp.list"})
            return
        completed = self.last_action
        self.oauth_active = False
        self.close_oauth_dialog()
        self.update_servers(event)
        self.hide_forms()
        self.continue_connect(completed)
        self.update_actions()

    def update_servers(self, event: dict[str, Any]) -> None:
        previous = self.selected()
        self.servers = event.get("servers", [])
        listing = self.query_one("#mcp-list", OptionList)
        rows = [(item["name"], item["scope"],
                 "✓" if item["state"] == "connected" else "..." if item["state"] == "connecting" else "-",
                 "✓" if item["config"]["enabled"] else "-") for item in self.servers]
        if [row[0] for row in rows] != [row[0] for row in self.rendered_rows]:
            listing.set_options(Option(server_row(*row)) for row in rows)
            if self.servers:
                listing.highlighted = next((i for i, item in enumerate(self.servers)
                                            if previous and item["name"] == previous["name"]), 0)
        else:
            for index, row in enumerate(rows):
                if row != self.rendered_rows[index]:
                    listing.replace_option_prompt_at_index(index, server_row(*row))
        self.rendered_rows = rows
        self.set_status("\n".join(item["message"] for item in event.get("issues", [])) or
                        ("Connecting MCP servers…" if event.get("restoring") else
                         "No servers. Configure .sos/mcps.json in this workspace or your global sos/mcps.json, then reopen /mcps." if not self.servers else event.get("notice", "")))

    def hide_forms(self) -> None:
        dialog, self.connect_screen = self.connect_screen, None
        if dialog is not None:
            dialog.clear_secret()
            dialog.dismiss()
        self.reviewing = None

    def stop_connect(self) -> None:
        self.deferred_request = None
        self.connect_target = None
        self.authorization_attempted = False
        self.oauth_active = False
        self.oauth_url = ""
        self.close_oauth_dialog()
        self.hide_forms()

    def update_actions(self) -> None:
        item = self.selected()
        waiting = self.pending and not self.background_refresh
        busy = waiting or self.oauth_active or self.connect_target is not None or self.deferred_request is not None
        for name in ("connect", "enable"):
            self.query_one(f"#mcp-{name}", Button).disabled = busy or item is None
        # Reading the list must not remove its focus, including the initial load.
        self.query_one("#mcp-list", OptionList).disabled = (
            (waiting and self.last_action != "mcp.list") or self.oauth_active
            or self.connect_target is not None or self.deferred_request is not None)
        if item:
            self.query_one("#mcp-enable", Button).label = "Disable" if item["config"]["enabled"] else "Enable"
            self.query_one("#mcp-connect", Button).label = "Disconnect" if item["state"] == "connected" else "Connect"
            self.set_text("#mcp-detail",
                f'{item["tool_count"]} tools' if item["state"] == "connected" else
                "Connecting…" if item["state"] == "connecting" else item.get("error", ""))
        else:
            self.query_one("#mcp-enable", Button).label = "Enable"
            self.query_one("#mcp-connect", Button).label = "Connect"
            self.set_text("#mcp-detail", "")

    def on_option_list_option_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        self.update_actions()

    def action_select_action(self, action: str) -> None:
        self.selected_action = action
        for name in ("connect", "enable"):
            self.query_one(f"#mcp-{name}", Button).set_class(name == action, "selected-action")
        self.query_one(f"#mcp-{action}", Button).focus()

    def on_descendant_focus(self, event: events.DescendantFocus) -> None:
        if event.widget.id in ("mcp-connect", "mcp-enable"):
            self.action_select_action(event.widget.id.removeprefix("mcp-"))

    def action_confirm(self) -> None:
        self.query_one(f"#mcp-{self.selected_action}", Button).press()

    def targeted(self, item: dict[str, Any], action: str, **values: Any) -> dict[str, Any]:
        return {"type": f"mcp.{action}", "name": item["name"], "identity": item["identity"], **values}

    def begin_connect(self, item: dict[str, Any]) -> None:
        self.hide_forms()
        self.connect_target = (item["name"], item["identity"])
        self.authorization_attempted = False
        if not item["trusted"]:
            self.review(item, "trust", "Approve this exact configuration and connect? Local programs run with your user privileges.")
        else:
            self.request(self.targeted(item, "connect"))
        self.update_actions()

    def continue_connect(self, completed: str) -> None:
        if self.connect_target is None:
            return
        item = next((item for item in self.servers if (item["name"], item["identity"]) == self.connect_target), None)
        if item is None:
            self.fail("Configuration changed. Review it and connect again.")
        elif completed in ("mcp.trust", "mcp.token", "mcp.oauth_poll"):
            self.request(self.targeted(item, "connect"))
        elif completed == "mcp.connect" and item["state"] == "needs_auth" and not self.authorization_attempted:
            self.authorize(item)
        else:
            self.stop_connect()

    def authorize(self, item: dict[str, Any]) -> None:
        self.authorization_attempted = True
        auth = item["config"].get("authentication", "auto")
        if auth in ("auto", "oauth"):
            self.request(self.targeted(item, "oauth_start"))
        elif auth == "token":
            self.reviewing = item
            self.show_connect_dialog("This server requires a bearer token to connect.", token=True)
        else:
            self.fail("Check authentication settings in your MCP configuration file.")

    def review(self, item: dict[str, Any], action: str, warning: str) -> None:
        self.hide_forms()
        self.reviewing, self.review_action = item, action
        self.show_connect_dialog(warning + "\n" + item["scope"] + " / " + item["name"] + "\n" + json.dumps(item["config"], indent=2))

    def show_connect_dialog(self, text: str, *, token: bool = False) -> None:
        self.connect_screen = McpConnectScreen(text, self.submit_connect_dialog, self.cancel_connect_dialog, token=token)
        self.app.push_screen(self.connect_screen)

    def submit_connect_dialog(self, token: str) -> None:
        if (self.pending and not self.background_refresh) or self.reviewing is None or self.connect_screen is None:
            return
        item = self.reviewing
        action = "token" if self.connect_screen.token else self.review_action
        self.hide_forms()
        self.request(self.targeted(item, action, **({"token": token} if action == "token" else {"accepted": True})))

    def cancel_connect_dialog(self) -> None:
        self.stop_connect()
        self.update_actions()

    def on_button_pressed(self, event: Button.Pressed) -> None:
        action = (event.button.id or "").removeprefix("mcp-")
        event.stop()
        self.action_select_action(action)
        if (not self.pending or self.background_refresh) and not self.oauth_active and self.deferred_request is None:
            self.handle_action(action)

    def handle_action(self, action: str) -> None:
        item = self.selected()
        if item:
            if action == "enable":
                self.request(self.targeted(item, "enable", enabled=not item["config"]["enabled"]))
            elif action == "connect":
                if item["state"] == "connected":
                    self.request(self.targeted(item, "disconnect"))
                else:
                    self.begin_connect(item)

    def close_oauth_dialog(self) -> None:
        dialog, self.oauth_screen = self.oauth_screen, None
        if dialog is not None:
            dialog.dismiss()

    def cancel_oauth(self) -> None:
        self.oauth_cancelling = True
        self.stop_connect()
        # Cancellation bypasses a pending poll; its acknowledgement resets the list.
        self.pending = True
        self.update_actions()
        self.post_message(self.Requested(self, {"type": "mcp.cancel"}))

    def poll_oauth(self) -> None:
        if self.oauth_active and not self.pending and not self.finished:
            self.request({"type": "mcp.oauth_poll"})

    async def open_browser(self) -> None:
        dialog = self.oauth_screen
        def report(text: str) -> None:
            if dialog is not None and self.oauth_screen is dialog and dialog.is_mounted:
                dialog.set_status(text)
        if not self.oauth_url.startswith(("https://", "http://127.0.0.1:", "http://localhost:")):
            report("Invalid OAuth browser URL")
            return
        try:
            opened = await asyncio.to_thread(webbrowser.open, self.oauth_url)
            if not opened:
                report("Browser could not open. Open the displayed URL manually.")
        except OSError:
            report("Browser could not open. Open the displayed URL manually.")

    def action_close(self) -> None:
        if self.connect_screen is not None:
            self.cancel_connect_dialog()
            return
        if self.oauth_screen is not None:
            self.cancel_oauth()
            return
        if not self.finished:
            self.deferred_request = None
            self.connect_target = None
            self.hide_forms()
            self.post_message(self.Closed(self))
