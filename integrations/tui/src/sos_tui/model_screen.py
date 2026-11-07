"""Searchable model picker, keyed by provider and model together."""

from typing import Any

from rich.table import Table
from textual.app import ComposeResult
from textual.containers import Vertical
from textual.message import Message
from textual.widgets import Input, OptionList, Rule, Static
from textual.widgets.option_list import Option

from .dialog_screen import DialogScreen, dialog_row


class ModelScreen(DialogScreen):
    DEFAULT_CSS = """
    ModelScreen #model-dialog { height: 80%; }
    ModelScreen #model-search { height: 1; min-height: 1; padding: 0; margin: 0 0 1 0; }
    ModelScreen #model-search, ModelScreen #model-search:focus { border: none; }
    ModelScreen #model-picker { height: 1fr; margin-bottom: 1; }
    ModelScreen #model-list { height: 1fr; overflow-y: scroll; }
    ModelScreen #model-errors { height: auto; max-height: 4; overflow-y: auto; }
    ModelScreen #model-context-form { display: none; height: auto; }
    ModelScreen #model-context-form Static { height: auto; }
    ModelScreen #model-name { color: $foreground; margin-bottom: 1; }
    ModelScreen #model-minimum, ModelScreen #model-maximum { color: $muted; }
    ModelScreen #model-minimum { margin-top: 1; }
    ModelScreen.context-selection .popup-heading { color: $title-md; text-style: bold; }
    ModelScreen #model-context, ModelScreen #model-context:focus {
        height: 1; min-height: 1; border: none; padding: 0; }
    """

    class Requested(Message):
        def __init__(self, screen: "ModelScreen", payload: dict[str, Any]) -> None:
            super().__init__()
            self.screen = screen
            self.payload = payload

    class Closed(Message):
        def __init__(self, screen: "ModelScreen") -> None:
            super().__init__()
            self.screen = screen

    def __init__(self, info: dict[str, Any]) -> None:
        super().__init__()
        self.current = (info.get("provider"), info.get("model"))
        self.current_limit = info.get("context_limit")
        self.pending_model: dict[str, Any] | None = None
        self.models: list[dict[str, Any]] = []
        self.visible_models: list[dict[str, Any]] = []
        self.catalog_loading = True
        self.selecting = False
        self.finished = False

    def compose(self) -> ComposeResult:
        with Vertical(id="model-dialog", classes="popup-dialog"):
            yield Static("Select a model", classes="popup-heading")
            yield Input(placeholder="Search provider or model…", id="model-search")
            with Vertical(id="model-picker", classes="popup-picker"):
                yield Static(self.model_row("Provider", "Model"), id="model-columns", classes="popup-columns")
                yield Rule(line_style="solid", classes="popup-separator")
                yield OptionList(id="model-list", classes="popup-list")
            with Vertical(id="model-context-form"):
                yield Static("", id="model-name", markup=False)
                yield Static("Select the context limit for automatic compaction",
                             id="model-context-label", markup=False)
                yield Input(id="model-context", type="integer")
                yield Static("Minimum: 32768 tokens", id="model-minimum", markup=False)
                yield Static("", id="model-maximum", markup=False)
            yield Static("", id="model-errors", classes="popup-status", markup=False)
            yield Static("Loading models…", id="model-status", classes="popup-status", markup=False)

    def on_mount(self) -> None:
        self.query_one("#model-search", Input).focus()
        self.post_message(self.Requested(self, {"type": "model.list"}))

    @staticmethod
    def model_row(provider: str, model: str, current: bool = False) -> Table:
        return dialog_row(model, "Current" if current else "", detail=provider)

    @staticmethod
    def provider_label(model: dict[str, Any]) -> str:
        if model.get("provider_name"):
            return f"{model['provider_name']} (Custom)"
        provider = model["provider"].capitalize()
        method = model.get("auth_method")
        if method == "api":
            return f"{provider} (Api key)"
        if method in ("browser", "device"):
            return f"{provider} (Codex)"
        return provider

    def set_status(self, text: str) -> None:
        self.query_one("#model-status", Static).update(text)

    def receive(self, event: dict[str, Any]) -> None:
        if event["type"] == "request.failed":
            self.fail(event["error"])
        elif event["type"] == "model.listed":
            self.catalog_loading = False
            self.models = event["models"]
            errors = [f"{item['provider']}: {item['error']}" for item in event.get("errors", [])]
            self.query_one("#model-errors", Static).update("\n".join(errors))
            self.filter_models()

    def highlighted_model(self) -> dict[str, Any] | None:
        index = self.query_one(OptionList).highlighted
        return self.visible_models[index] if index is not None and index < len(self.visible_models) else None

    def filter_models(self) -> None:
        search = self.query_one("#model-search", Input).value.casefold()
        self.visible_models = [item for item in self.models
                               if search in f"{self.provider_label(item)} {item['model']}".casefold()]
        options = self.query_one(OptionList)
        options.clear_options().add_options([
            Option(self.model_row(self.provider_label(item), item["model"],
                                  (item["provider"], item["model"]) == self.current))
            for item in self.visible_models])
        options.highlighted = 0 if self.visible_models else None
        if self.catalog_loading:
            return
        if not self.models:
            self.set_status("No compatible models · Connect a provider with /connect · Esc Close")
        elif not self.visible_models:
            self.set_status("No matching models · Esc Close")
        else:
            self.set_status("↑/↓ Select · Enter Confirm · Tab Search / List · Esc Close")

    def on_input_changed(self, event: Input.Changed) -> None:
        event.stop()
        if event.input.id == "model-search":
            self.filter_models()

    def action_navigate_selection(self, direction: int) -> None:
        if not self.selecting and self.pending_model is None:
            self.move_selection(direction, focus=False)

    def on_input_submitted(self, event: Input.Submitted) -> None:
        event.stop()
        if event.input.id == "model-context":
            self.confirm_context()
        else:
            self.select_model()

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        event.stop()
        self.select_model()

    def select_model(self) -> None:
        model = self.highlighted_model()
        if self.catalog_loading or self.selecting or self.finished or model is None:
            return
        self.pending_model = model
        for selector in ("#model-search", "#model-picker", "#model-columns", "#model-list"):
            self.query_one(selector).display = False
        self.query_one("#model-context-form").display = True
        self.add_class("context-selection")
        maximum = model.get("max_context")
        field = self.query_one("#model-context", Input)
        field.value = ""
        field.placeholder = str(maximum if maximum is not None else 32768)
        self.query_one("#model-name", Static).update(model["model"])
        maximum_label = self.query_one("#model-maximum", Static)
        maximum_label.display = maximum is not None
        maximum_label.update(f"Maximum: {maximum} tokens" if maximum is not None else "")
        self.query_one(".popup-heading", Static).update("Model")
        self.set_status("Enter Save · Esc Back")
        field.focus()

    def confirm_context(self) -> None:
        if self.selecting or self.finished or self.pending_model is None:
            return
        field = self.query_one("#model-context", Input)
        text = field.value.strip()
        maximum = self.pending_model.get("max_context")
        if not text.isascii() or not text.isdigit() or len(text) > 20 or not 32768 <= int(text) <= 18446744073709551615:
            self.query_one("#model-errors", Static).update("Enter an integer of at least 32768 tokens.")
            return
        limit = int(text)
        if maximum is not None and limit > maximum:
            self.query_one("#model-errors", Static).update(f"Context limit must not exceed {maximum} tokens.")
            return
        model = self.pending_model
        self.selecting = True
        field.disabled = True
        self.query_one("#model-search", Input).disabled = True
        self.query_one(OptionList).disabled = True
        self.set_status("Selecting model…")
        self.post_message(self.Requested(self, {"type": "model.select", "provider": model["provider"],
                                               "model": model["model"], "context_limit": limit}))

    def fail(self, error: str) -> None:
        self.catalog_loading = self.selecting = False
        self.query_one("#model-search", Input).disabled = False
        self.query_one(OptionList).disabled = False
        self.query_one("#model-errors", Static).update(error)
        self.set_status("Enter Retry selection · Esc Close" if self.visible_models else "Esc Close and retry /model")
        self.query_one("#model-search", Input).focus()
        if self.pending_model is not None:
            field = self.query_one("#model-context", Input)
            field.disabled = False
            field.focus()

    def action_close(self) -> None:
        if self.finished or self.selecting:
            return
        if self.pending_model is not None:
            self.pending_model = None
            for selector in ("#model-search", "#model-picker", "#model-columns", "#model-list"):
                self.query_one(selector).display = True
            self.query_one("#model-context-form").display = False
            self.remove_class("context-selection")
            self.query_one(".popup-heading", Static).update("Select a model")
            self.query_one("#model-errors", Static).update("")
            self.filter_models()
            self.query_one("#model-search", Input).focus()
            return
        self.finished = True
        self.post_message(self.Closed(self))
