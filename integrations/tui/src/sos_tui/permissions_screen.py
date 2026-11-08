"""Permission policy picker, independent of Sane/Slop execution mode."""

from typing import Any

from textual import events
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.message import Message
from textual.widgets import Button, Input, OptionList, Rule, Static
from textual.widgets.option_list import Option

from .dialog_screen import DialogScreen, dialog_row
from .decision_model_screen import DecisionModelScreen, DecisionModelDraft


MODES = (("ask", "Ask before"), ("auto", "AI Check"), ("danger", "Danger"))
DESCRIPTIONS = {
    "ask": "You will be asked before any command with side effects",
    "auto": "A decision model checks the estimated probability that an action should be allowed.",
    "danger": "Danger: no checks. Any available action can run, including deleting all your files. "
              "Be careful. If unsure, choose AI Check.",
}


class ConfidenceInput(Input):
    BINDINGS = [Binding("left", "adjust(-1)", show=False, priority=True),
                Binding("right", "adjust(1)", show=False, priority=True)]

    def action_adjust(self, amount: int) -> None:
        try:
            self.value = str(max(0, min(100, int(self.value) + amount)))
        except ValueError:
            self.value = "80"


class PermissionsScreen(DialogScreen):
    BINDINGS = [Binding("left", "move_action(-1)", show=False),
                Binding("right", "move_action(1)", show=False)]
    DEFAULT_CSS = """
    PermissionsScreen .popup-dialog { overflow-y: auto; }
    PermissionsScreen.compact .popup-dialog { padding: 0 2; }
    PermissionsScreen #permissions-picker { margin: 1 0; }
    PermissionsScreen #permissions-list { height: 3; }
    PermissionsScreen #permissions-description { height: auto; }
    PermissionsScreen #permissions-status { margin-top: 1; }
    PermissionsScreen #permissions-description.danger,
    PermissionsScreen #confidence-warning.danger { color: $error; text-style: bold; }
    PermissionsScreen #permissions-confidence { display: none; height: auto; }
    PermissionsScreen #permissions-confidence Static { height: auto; }
    PermissionsScreen #permissions-model { margin-bottom: 1; }
    PermissionsScreen #confidence-row { height: auto; }
    PermissionsScreen #confidence-value, PermissionsScreen #confidence-value:focus {
        width: 4; height: 1; min-height: 1; border: none; padding: 0; margin: 0; }
    PermissionsScreen #confidence-unit { width: 2; height: 1; }
    PermissionsScreen #confidence-hints { width: 1fr; height: auto; margin-left: 2; color: $muted; }
    PermissionsScreen #confidence-actions { height: 1; margin: 0; }
    PermissionsScreen #permissions-confidence Button { margin-right: 1; }
    PermissionsScreen #confidence-actions Button.selected-action {
        background: $depth-md !important; color: $foreground !important; text-style: bold !important; }
    """

    class Requested(Message):
        def __init__(self, screen: "PermissionsScreen", payload: dict[str, Any]) -> None:
            super().__init__()
            self.screen = screen
            self.payload = payload

    class Closed(Message):
        def __init__(self, screen: "PermissionsScreen") -> None:
            super().__init__()
            self.screen = screen

    def __init__(self, info: dict[str, Any]) -> None:
        super().__init__()
        self.current = info.get("permission_mode", "ask")
        self.confidence = info.get("permission_confidence", 80)
        self.check_supported = (info.get("permission_check_version") == 1
                                and type(self.confidence) is int and 0 <= self.confidence <= 100)
        model = info.get("permission_model") or {}
        self.model = {name: model.get(name, default) for name, default in (
            ("protocol", "typesafe"), ("endpoint", "https://openrouter.ai/api/alpha/decisions"),
            ("model", "typesafe/jev-1.13"))}
        self.saved_identity = (self.model["protocol"], self.model["endpoint"])
        self.has_saved_key = bool(model.get("has_key", info.get("has_permission_key", False)))
        self.pending_key = ""
        self.model_screen: DialogScreen | None = None
        self.selecting = False
        self.finished = False
        self.selected_action = "confidence-save"

    def mode_options(self) -> list[Option]:
        return [Option(dialog_row(label, "Current" if mode == self.current else ""), id=mode)
                for mode, label in MODES]

    def compose(self) -> ComposeResult:
        with Vertical(classes="popup-dialog"):
            yield Static("Permissions", classes="popup-heading")
            with Vertical(id="permissions-picker", classes="popup-picker"):
                yield Static("Mode", id="permissions-columns", classes="popup-columns")
                yield Rule(line_style="solid", classes="popup-separator")
                yield OptionList(*self.mode_options(), id="permissions-list", classes="popup-list")
            with Vertical(id="permissions-confidence"):
                yield Static("", id="permissions-model", markup=False)
                yield Static("Minimum allow probability", id="confidence-heading", classes="popup-section")
                with Horizontal(id="confidence-row"):
                    yield ConfidenceInput(str(self.confidence), restrict=r"[0-9]*", max_length=3, id="confidence-value")
                    yield Static("%", id="confidence-unit")
                    with Vertical(id="confidence-hints"):
                        yield Static("0%: No AI checks (like Danger).", id="confidence-warning")
                        yield Static("100%: Allow probability must be 100%.")
                with Horizontal(id="confidence-actions"):
                    yield Button("Save", id="confidence-save", classes="selected-action")
                    yield Button("Change model", id="permissions-change-model")
            yield Static("", id="permissions-description", markup=False)
            yield Static("↑/↓ Select · Enter Confirm · Esc Close", id="permissions-status",
                         classes="popup-status", markup=False)

    def on_mount(self) -> None:
        options = self.query_one(OptionList)
        options.highlighted = next((i for i, item in enumerate(MODES) if item[0] == self.current), 0)
        self.update_model_label()
        self.show_description()
        options.focus()
        if self.current == "auto" and self.check_supported:
            self.select_action("confidence-save")

    def on_resize(self, event: events.Resize) -> None:
        self.set_class(event.size.height < 30, "compact")

    def update_model_label(self) -> None:
        label = self.query_one("#permissions-model", Static)
        configured = bool(self.pending_key or self.can_reuse_key())
        protocol = {"openai_decisions": "OpenAI Decisions", "typesafe": "TypeSafe"}.get(self.model["protocol"], "")
        lines = []
        if configured and protocol:
            lines.append(f"API: {protocol}")
        if configured and self.model["model"]:
            lines.append(f"Model: {self.model['model']}")
        label.update("\n".join(lines))
        label.display = bool(lines)

    def receive(self, event: dict[str, Any]) -> None:
        if event["type"] == "request.failed":
            self.fail(event["error"])
        elif event["type"] == "permissions.selected":
            self.current = event["mode"]
            self.confidence = event.get("confidence", self.confidence)
            self.selecting = False
            self.pending_key = ""
            self.action_close()

    def status(self, text: str) -> None:
        self.query_one("#permissions-status", Static).update(text)

    def show_description(self) -> None:
        index = self.query_one(OptionList).highlighted
        if index is None:
            return
        mode = MODES[index][0]
        description = self.query_one("#permissions-description", Static)
        description.update(DESCRIPTIONS[mode])
        description.set_class(mode == "danger", "danger")
        panel = self.query_one("#permissions-confidence")
        visible = mode == "auto" and self.check_supported
        if visible and not panel.display:
            self.select_action("confidence-save", focus=False)
        panel.display = visible
        description.display = not visible
        self.status("↑/↓ Select · ←/→ Actions · Tab Settings · Enter Confirm · Esc Close")
        if mode == "auto" and not self.check_supported:
            self.status("Update and restart the daemon to configure AI Check probabilities and models.")
        value = self.query_one("#confidence-value", Input).value
        self.query_one("#confidence-warning").set_class(value == "0", "danger")

    def on_option_list_option_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        event.stop()
        self.show_description()

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        event.stop()
        if self.selecting or self.finished:
            return
        mode = MODES[event.option_index][0]
        if mode == "auto":
            if self.check_supported:
                self.select_action("confidence-save")
        else:
            self.select(mode)

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id == "confidence-value":
            self.query_one("#confidence-warning").set_class(event.value == "0", "danger")

    def on_input_submitted(self, event: Input.Submitted) -> None:
        event.stop()
        if event.input.id == "confidence-value":
            self.save_confidence()

    def on_button_pressed(self, event: Button.Pressed) -> None:
        event.stop()
        if self.selecting or self.finished:
            return
        if event.button.id == "permissions-change-model":
            self.show_change_model()
        elif event.button.id == "confidence-save":
            self.save_confidence()

    def show_change_model(self) -> None:
        if self.model_screen is not None or not self.check_supported or self.selecting:
            return
        self.model_screen = DecisionModelScreen(self.model, self.pending_key,
            saved_identity=self.saved_identity, has_saved_key=self.has_saved_key)
        self.app.push_screen(self.model_screen, self.model_chosen)

    def model_chosen(self, draft: DecisionModelDraft | None) -> None:
        self.model_screen = None
        if self.finished:
            return
        if draft is not None:
            self.model = dict(draft.config)
            self.pending_key = draft.key
            draft.key = ""
            self.update_model_label()
            self.status("Model selected in draft · Save to apply · Esc Discard")
        self.select_action("confidence-save")

    def can_reuse_key(self) -> bool:
        return self.has_saved_key and (self.model["protocol"], self.model["endpoint"]) == self.saved_identity

    def save_confidence(self) -> None:
        if self.selecting or self.finished or not self.check_supported:
            return
        field = self.query_one("#confidence-value", Input)
        if not field.value.isdigit() or not 0 <= int(field.value) <= 100:
            self.status("Enter a whole percentage between 0 and 100 · Esc Close")
            field.focus()
            return
        threshold = int(field.value)
        if threshold and not (self.pending_key or self.can_reuse_key()):
            self.show_change_model()
            return
        self.select("auto", confidence=threshold)

    def set_selecting(self, selecting: bool) -> None:
        self.selecting = selecting
        for selector in ("#permissions-list", "#permissions-confidence"):
            self.query_one(selector).disabled = selecting

    def action_move_action(self, direction: int) -> None:
        if self.selecting or not self.query_one("#permissions-confidence").display:
            return
        buttons = list(self.query("#confidence-actions Button"))
        index = next(i for i, button in enumerate(buttons) if button.id == self.selected_action)
        self.select_action(buttons[(index + direction) % len(buttons)].id)

    def select_action(self, action: str, *, focus: bool = True) -> None:
        self.selected_action = action
        for button in self.query("#confidence-actions Button"):
            button.set_class(button.id == action, "selected-action")
        if focus:
            self.query_one(f"#{action}").focus()

    def on_descendant_focus(self, event: events.DescendantFocus) -> None:
        if event.widget.id in ("confidence-save", "permissions-change-model"):
            self.select_action(event.widget.id, focus=False)

    def select(self, mode: str, *, confidence: int | None = None) -> None:
        self.set_selecting(True)
        self.status("Saving permissions…")
        payload: dict[str, Any] = {"type": "permissions.select", "mode": mode, "key": ""}
        if confidence is not None:
            payload.update(confidence=confidence, check_version=1, model_config=dict(self.model), key=self.pending_key)
        self.post_message(self.Requested(self, payload))

    def fail(self, error: str) -> None:
        self.set_selecting(False)
        if self.query_one("#permissions-confidence").display:
            self.query_one("#confidence-value").focus()
        self.status(error + " · Correct settings or Esc Close")

    def discard_secrets(self) -> None:
        self.pending_key = ""
        if self.model_screen is not None:
            self.model_screen.clear_secrets()

    def on_unmount(self) -> None:
        self.discard_secrets()

    def action_close(self) -> None:
        if self.finished or self.selecting:
            return
        self.discard_secrets()
        self.finished = True
        self.post_message(self.Closed(self))
