"""Provider authentication dialog. Secrets never enter the conversation widgets."""

import asyncio
import webbrowser
from typing import Any

from rich.text import Text
from textual.app import ComposeResult
from textual import events
from textual.containers import Vertical, Horizontal
from textual.message import Message
from textual.widgets import Button, Input, Link, OptionList, Rule, Static
from textual.widgets.option_list import Option
from .dialog_screen import DialogScreen, dialog_row


class ConnectScreen(DialogScreen):
    CUSTOM_OPTION = "__new_custom_provider__"

    BINDINGS = [("escape", "close", "Close"), ("left", "navigate_left", "Previous"),
                ("right", "navigate_right", "Next")]
    DEFAULT_CSS = """
    #connect-dialog { overflow-y: auto; }
    ConnectScreen Input, ConnectScreen Input:focus { height: 1; min-height: 1;
        border: none; padding: 0 1; background: $input-background; }
    #connect-provider-picker { margin: 1 0; }
    #connect-method-picker { margin-bottom: 1; }
    #connect-providers, #connect-methods { height: auto; max-height: 9; }
    #connect-details { max-height: 8; overflow-y: auto;
        scrollbar-size: 1 1; scrollbar-color: $muted; scrollbar-background: $background; }
    #connect-help { margin-top: 1; }
    #connect-auth-url { width: 1fr; height: auto; color: $title-md; text-style: underline; }
    #connect-auth-url:hover, #connect-auth-url:focus { color: $bold-md; text-style: bold underline; }
    #connect-actions { height: auto; }
    #connect-actions Button { margin-right: 1; }
    #connect-actions Button.selected-action { background: $depth-md !important; color: $foreground !important; }
    """

    class Requested(Message):
        def __init__(self, screen: "ConnectScreen", payload: dict[str, Any]) -> None:
            super().__init__()
            self.screen = screen
            self.payload = payload

    class Closed(Message):
        def __init__(self, screen: "ConnectScreen") -> None:
            super().__init__()
            self.screen = screen

    def __init__(self) -> None:
        super().__init__()
        self.providers: dict[str, dict[str, Any]] = {}
        self.provider = ""
        self.method = ""
        self.auth_id = ""
        self.url = ""
        self.pending = False
        self.finished = False
        self.returning = False
        self.authenticated = False

    def compose(self) -> ComposeResult:
        with Vertical(id="connect-dialog", classes="popup-dialog"):
            yield Static("Connect a provider", id="connect-title", classes="popup-heading")
            with Vertical(id="connect-provider-picker", classes="popup-picker"):
                yield Static("Provider", id="connect-provider-columns", classes="popup-columns")
                yield Rule(line_style="solid", classes="popup-separator")
                yield OptionList(id="connect-providers", classes="popup-list")
            with Vertical(id="connect-method-picker", classes="popup-picker"):
                yield Static("Connection method", id="connect-method-label", classes="popup-columns")
                yield Rule(line_style="solid", classes="popup-separator")
                yield OptionList(id="connect-methods", classes="popup-list")
            yield Static("Name", id="connect-name-label", classes="popup-section")
            yield Input(id="connect-name", placeholder="My provider")
            yield Static("Base URL", id="connect-url-label", classes="popup-section")
            yield Input(id="connect-url", placeholder="http://localhost:1234/v1")
            yield Static("API key", id="connect-key-label", classes="popup-section")
            yield Input(id="connect-key", placeholder="API key", password=True)
            with Horizontal(id="connect-actions"):
                yield Button("Connect", id="connect-custom", compact=True)
                yield Button("Delete", id="connect-delete", compact=True)
                yield Button("Open browser", id="connect-browser", compact=True)
            yield Static("Loading providers…", id="connect-details", classes="popup-status")
            yield Link("", url="", id="connect-auth-url")
            yield Static("↑/↓ Select · Tab Next field · Enter Confirm · Esc Back / Close", id="connect-help", classes="popup-status")

    def on_mount(self) -> None:
        self.query_one("#connect-auth-url").display = False
        for selector in ("#connect-methods", "#connect-method-picker", "#connect-key", "#connect-key-label", "#connect-browser"):
            self.query_one(selector).display = False
        for selector in ("#connect-name", "#connect-name-label", "#connect-url", "#connect-url-label",
                         "#connect-custom", "#connect-delete"):
            self.query_one(selector).display = False
        self.request({"type": "provider.list"})

    def request(self, payload: dict[str, Any]) -> None:
        if self.finished:
            return
        self.pending = True
        self.query_one("#connect-providers", OptionList).disabled = True
        self.query_one("#connect-methods", OptionList).disabled = True
        for button in self.query("#connect-actions Button"):
            button.disabled = not (button.id == "connect-browser" and payload["type"] == "auth.poll"
                                   and self.auth_id and self.url)
        self.post_message(self.Requested(self, payload))

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        event.stop()
        if self.pending:
            return
        if event.option_list.id == "connect-providers":
            self.choose_provider(str(event.option.id))
        else:
            if self.authenticated and self.method == str(event.option.id):
                self.submit()
                return
            self.choose_method(str(event.option.id))
            if self.method != "api":
                self.submit()

    def method_options(self) -> list[Option]:
        methods = []
        if self.provider == "openai":
            methods.extend([("browser", "ChatGPT / Codex — Browser"), ("device", "ChatGPT / Codex — Device code")])
        methods.append(("api", "API key"))
        active = self.providers[self.provider].get("auth_method")
        return [self.connection_option(label, method, active == method) for method, label in methods]

    @staticmethod
    def connection_option(label: str, identifier: str, connected: bool) -> Option:
        return Option(dialog_row(label, "Connected" if connected else ""), id=identifier)

    def refresh_provider_options(self) -> None:
        options = self.query_one("#connect-providers", OptionList)
        highlighted = options.highlighted
        options.clear_options().add_options([
            self.connection_option(label, identifier, self.providers[identifier]["connected"])
            for identifier, item in self.providers.items()
            for label in [({"openai": "OpenAI", "openrouter": "OpenRouter"}.get(identifier)
                           or item.get("name", identifier))]])
        options.add_option(Option("Custom provider", id=self.CUSTOM_OPTION))
        options.highlighted = highlighted if highlighted is not None else 0

    def choose_provider(self, provider: str) -> None:
        if provider == self.CUSTOM_OPTION:
            self.show_custom_form()
            return
        if self.providers[provider].get("custom"):
            self.hide_custom_fields()
            for selector in ("#connect-methods", "#connect-method-picker", "#connect-key",
                             "#connect-key-label", "#connect-browser"):
                self.query_one(selector).display = False
            self.provider = provider
            self.show_custom_actions()
            self.query_one("#connect-custom", Button).focus()
            return
        self.hide_custom_fields()
        self.query_one("#connect-key-label", Static).update("API key")
        self.provider = provider
        self.authenticated = False
        self.url = ""
        self.query_one("#connect-browser").display = False
        self.query_one("#connect-key", Input).value = ""
        options = self.method_options()
        methods = self.query_one("#connect-methods", OptionList)
        methods.clear_options().add_options(options)
        active = self.providers[provider].get("auth_method")
        methods.highlighted = next((i for i, option in enumerate(options) if option.id == active), 0)
        methods.display = len(options) > 1
        self.query_one("#connect-method-picker").display = methods.display
        self.choose_method(str(options[methods.highlighted].id), move_focus=not methods.display)
        if methods.display:
            methods.focus()

    def set_details(self, text: str | Text) -> None:
        link = self.query_one("#connect-auth-url", Link)
        link.display = False
        link.text = link.url = ""
        self.query_one("#connect-browser").display = False
        details = self.query_one("#connect-details", Static)
        details.update(text)
        details.display = bool(text)

    def choose_method(self, method: str, move_focus: bool = True) -> None:
        self.method = method
        self.authenticated = False
        key = self.query_one("#connect-key", Input)
        key.display = method == "api"
        self.query_one("#connect-key-label").display = key.display
        self.set_details("")
        if move_focus:
            (key if key.display else self.query_one("#connect-methods", OptionList)).focus()

    def show_authorization(self, instructions: str) -> None:
        self.set_details(Text(instructions))
        link = self.query_one("#connect-auth-url", Link)
        link.text = link.url = self.url
        link.display = bool(self.url)
        browser = self.query_one("#connect-browser", Button)
        browser.disabled = False
        browser.display = bool(self.url)
        browser.focus()
        self.query_one("#connect-help", Static).update("Enter Open browser · Tab URL · Esc Back / Close")

    def action_focus_providers(self) -> None:
        if not self.pending:
            self.query_one("#connect-providers", OptionList).focus()
            self.show_custom_actions()

    def move_button_focus(self, direction: int) -> None:
        buttons = [button for button in self.query("#connect-actions Button") if button.display and not button.disabled]
        if not buttons:
            return
        index = (buttons.index(self.focused) if self.focused in buttons else
                 next((i for i, button in enumerate(buttons) if button.has_class("selected-action")),
                      -1 if direction > 0 else 0))
        selected = buttons[(index + direction) % len(buttons)]
        for button in buttons:
            button.set_class(button is selected and button.id in ("connect-custom", "connect-delete"), "selected-action")
        selected.focus()

    def action_navigate_left(self) -> None:
        if isinstance(self.focused, Input):
            return
        if isinstance(self.focused, Button) or self.query_one("#connect-custom").display:
            self.move_button_focus(-1)
        else:
            self.action_focus_providers()

    def action_navigate_right(self) -> None:
        if not isinstance(self.focused, Input):
            self.move_button_focus(1)

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        event.stop()
        if event.button.id == "connect-browser" and self.auth_id and self.url and not self.returning:
            await asyncio.to_thread(webbrowser.open, self.url)
            return
        if self.pending:
            return
        if event.button.id in ("connect-custom", "connect-delete"):
            provider = self.highlighted_provider()
            if provider not in self.providers or not self.providers[provider].get("custom"):
                return
            self.provider = provider
            if event.button.id == "connect-custom":
                self.select_provider()
            else:
                self.request({"type": "provider.delete", "provider": provider})

    def on_input_submitted(self, event: Input.Submitted) -> None:
        event.stop()
        self.submit()

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id == "connect-key" and event.value:
            self.authenticated = False

    def submit(self) -> None:
        if self.provider == self.CUSTOM_OPTION:
            self.submit_custom()
            return
        if self.pending or not self.method:
            return
        if self.authenticated:
            self.select_provider()
        elif self.method == "api":
            key = self.query_one("#connect-key", Input)
            value = key.value.strip()
            key.value = ""
            if not value:
                self.fail("Enter an API key.")
                return
            self.set_details("Checking API key…")
            self.request({"type": "auth.api", "provider": self.provider, "key": value})
        else:
            self.request({"type": "auth.start", "provider": self.provider, "method": self.method})

    def select_provider(self) -> None:
        self.set_details("Connecting…")
        self.request({"type": "provider.select", "provider": self.provider})

    def receive(self, event: dict[str, Any]) -> None:
        kind = event["type"]
        if self.returning:
            if kind in ("auth.cancelled", "request.failed"):
                self.returning = False
                self.show_providers()
            return
        if kind == "request.failed":
            self.auth_id = ""
            self.fail(event["error"])
        elif kind == "provider.listed":
            self.providers = {item["id"]: item for item in event["providers"]}
            self.provider = self.method = ""
            self.hide_custom_fields()
            for selector in ("#connect-methods", "#connect-method-picker", "#connect-key",
                             "#connect-key-label", "#connect-browser"):
                self.query_one(selector).display = False
            self.refresh_provider_options()
            created = event.get("created_provider")
            if created:
                self.query_one("#connect-providers", OptionList).highlighted = list(self.providers).index(created)
            self.show_providers()
        elif kind == "auth.started":
            self.auth_id = event["authId"]
            self.url = event["url"]
            self.show_authorization(event["instructions"])
            self.set_timer(0.5, self.poll)
        elif kind == "auth.pending":
            self.set_timer(0.5, self.poll)
        elif kind == "auth.saved":
            self.auth_id = ""
            self.authenticated = True
            self.providers[self.provider]["connected"] = True
            self.providers[self.provider]["auth_method"] = self.method
            self.refresh_provider_options()
            methods = self.query_one("#connect-methods", OptionList)
            index = methods.highlighted
            methods.clear_options().add_options(self.method_options())
            methods.highlighted = index
            self.select_provider()

    def poll(self) -> None:
        if self.auth_id and self.is_mounted and not self.finished and not self.returning:
            self.request({"type": "auth.poll", "authId": self.auth_id})

    def fail(self, error: str) -> None:
        self.returning = False
        self.pending = False
        self.set_details(Text(error))
        self.query_one("#connect-providers", OptionList).disabled = False
        self.query_one("#connect-methods", OptionList).disabled = False
        for button in self.query("#connect-actions Button"):
            button.disabled = False

    def action_close(self) -> None:
        if self.finished or self.returning:
            return
        if self.provider:
            self.back_to_providers()
            return
        self.finished = True
        self.query_one("#connect-key", Input).value = ""
        self.post_message(self.Closed(self))

    def back_to_providers(self) -> None:
        cancel_pending = self.pending or bool(self.auth_id)
        auth_id = self.auth_id
        self.provider = self.method = self.auth_id = self.url = ""
        self.authenticated = False
        self.hide_custom_fields()
        self.query_one("#connect-key", Input).value = ""
        for selector in ("#connect-methods", "#connect-method-picker", "#connect-key", "#connect-key-label", "#connect-browser"):
            self.query_one(selector).display = False
        self.query_one("#connect-methods", OptionList).clear_options()
        self.show_providers()
        if cancel_pending:
            self.returning = True
            self.request({"type": "auth.cancel", "authId": auth_id})

    def show_providers(self) -> None:
        self.pending = False
        self.set_details("Select a provider.")
        self.query_one("#connect-providers", OptionList).disabled = False
        self.query_one("#connect-methods", OptionList).disabled = False
        self.query_one("#connect-providers", OptionList).focus()
        for button in self.query("#connect-actions Button"):
            button.disabled = False
        self.show_custom_actions()

    def highlighted_provider(self) -> str:
        options = self.query_one("#connect-providers", OptionList)
        if options.highlighted is None:
            return ""
        return str(options.get_option_at_index(options.highlighted).id)

    def on_option_list_option_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        if event.option_list.id == "connect-providers" and (
                not self.provider or self.providers.get(self.provider, {}).get("custom")):
            self.provider = ""
            self.show_custom_actions()

    def show_custom_actions(self) -> None:
        provider = self.highlighted_provider()
        item = self.providers.get(provider, {})
        visible = bool(item.get("custom")) and self.provider != self.CUSTOM_OPTION
        for selector in ("#connect-custom", "#connect-delete"):
            self.query_one(selector).display = visible
        self.update_help()
        if not visible and not self.provider and not self.pending:
            self.set_details("Select a provider.")
        if visible:
            self.query_one("#connect-custom", Button).add_class("selected-action")
            self.query_one("#connect-delete", Button).remove_class("selected-action")
            self.set_details(item["base_url"])

    def hide_custom_fields(self) -> None:
        for selector in ("#connect-name", "#connect-name-label", "#connect-url", "#connect-url-label",
                         "#connect-custom", "#connect-delete"):
            self.query_one(selector).display = False
        self.query_one("#connect-name", Input).value = ""
        self.query_one("#connect-url", Input).value = ""
        self.update_help()

    def update_help(self) -> None:
        actions = " · ←/→ Action" if self.query_one("#connect-custom").display else ""
        self.query_one("#connect-help", Static).update(
            f"↑/↓ Select{actions} · Tab Next field · Enter Confirm · Esc Back / Close")

    def show_custom_form(self) -> None:
        self.provider = self.CUSTOM_OPTION
        self.method = ""
        for selector in ("#connect-methods", "#connect-method-picker", "#connect-browser",
                         "#connect-custom", "#connect-delete"):
            self.query_one(selector).display = False
        for selector in ("#connect-name", "#connect-name-label", "#connect-url", "#connect-url-label",
                         "#connect-key", "#connect-key-label"):
            self.query_one(selector).display = True
        self.query_one("#connect-key-label", Static).update("API key (optional)")
        self.query_one("#connect-key", Input).value = ""
        self.set_details(
            "OpenAI-compatible Chat Completions API\n"
            "the /models endpoint discovers models")
        self.update_help()
        self.query_one("#connect-name", Input).focus()

    def submit_custom(self) -> None:
        if self.pending:
            return
        name = self.query_one("#connect-name", Input).value.strip()
        url = self.query_one("#connect-url", Input).value.strip()
        if not name or not url:
            self.fail("Enter a provider name and base URL. Tab moves between fields; Enter saves.")
            return
        key = self.query_one("#connect-key", Input)
        value = key.value.strip()
        key.value = ""
        self.set_details("Discovering models…")
        self.request({"type": "provider.create", "name": name, "base_url": url, "key": value})

    def on_descendant_focus(self, event: events.DescendantFocus) -> None:
        if isinstance(event.widget, Button):
            for button in self.query("#connect-actions Button"):
                button.set_class(button is event.widget and button.id in ("connect-custom", "connect-delete"), "selected-action")
