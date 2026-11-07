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


MODES = (("ask", "Ask before"), ("auto", "AI Validation (auto)"), ("danger", "Danger"))
DESCRIPTIONS = {
    "ask": "You will be asked before any command with side effects",
    "auto": "An AI model through OpenRouter decides whether a command should be accepted.",
    "danger": "Danger: no checks. Any available action can run, including deleting all your files. "
              "Be careful. If unsure, choose AI Validation.",
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
    PermissionsScreen #permissions-key, PermissionsScreen #permissions-confidence { display: none; }
    PermissionsScreen #permissions-key, PermissionsScreen #permissions-key:focus {
        height: 1; min-height: 1; border: none; padding: 0 1; }
    PermissionsScreen #permissions-confidence { height: auto; }
    PermissionsScreen #permissions-confidence Static { height: auto; }
    PermissionsScreen #confidence-description { color: $foreground; margin-bottom: 1; }
    PermissionsScreen #confidence-row { height: auto; }
    PermissionsScreen #confidence-value, PermissionsScreen #confidence-value:focus {
        width: 4; height: 1; min-height: 1; border: none; padding: 0; margin: 0; }
    PermissionsScreen #confidence-unit { width: 2; height: 1; }
    PermissionsScreen #confidence-hints { width: 1fr; height: auto; margin-left: 2; color: $muted; }
    PermissionsScreen #confidence-actions { height: 1; margin: 0; }
    PermissionsScreen #permissions-confidence Button { width: auto; min-width: 0; height: 1;
        border: none; padding: 0 1; margin-right: 1; }
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
        self.has_saved_key = info.get("has_permission_key", False)
        self.confidence = info.get("permission_confidence", 80)
        self.confidence_supported = type(info.get("permission_confidence")) is int and 0 <= self.confidence <= 100
        self.selecting = False
        self.reusing_saved_key = False
        self.close_after_save = False
        self.entering_key = False
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
                yield Static("AI checks commands before they run. Commands below your confidence threshold are refused.",
                             id="confidence-description", markup=False)
                yield Static("How confident should the AI be before running an action?", id="confidence-heading",
                             classes="popup-section")
                with Horizontal(id="confidence-row"):
                    yield ConfidenceInput(str(self.confidence), restrict=r"[0-9]*", max_length=3, id="confidence-value")
                    yield Static("%", id="confidence-unit")
                    with Vertical(id="confidence-hints"):
                        yield Static("0%: No AI checks (like Danger).", id="confidence-warning")
                        yield Static("100%: Full confidence required.")
                with Horizontal(id="confidence-actions"):
                    yield Button("Save", id="confidence-save", classes="selected-action")
                    yield Button("Change API key", id="permissions-change-key")
            yield Static("", id="permissions-description", markup=False)
            yield Input(placeholder="OpenRouter API key", password=True, id="permissions-key")
            yield Static("↑/↓ Select · Enter Confirm · Esc Close", id="permissions-status",
                         classes="popup-status", markup=False)

    def on_mount(self) -> None:
        self.show_modes()

    def on_resize(self, event: events.Resize) -> None:
        self.set_class(event.size.height < 30, "compact")

    def show_modes(self) -> None:
        self.entering_key = False
        self.query_one("#permissions-key", Input).value = ""
        self.query_one("#permissions-key").display = False
        options = self.query_one(OptionList)
        options.clear_options().add_options(self.mode_options())
        self.query_one("#permissions-picker").display = True
        options.display = True
        options.highlighted = next((i for i, item in enumerate(MODES) if item[0] == self.current), 0)
        self.query_one("#confidence-value", Input).value = str(self.confidence)
        self.query_one("#confidence-warning").set_class(self.confidence == 0, "danger")
        self.show_description()
        options.focus()
        if self.query_one("#permissions-confidence").display:
            self.select_action("confidence-save")
        self.status("↑/↓ Select · ←/→ Actions · Tab Settings · Enter Confirm · Esc Close" if self.current == "auto"
                    else "↑/↓ Select · Enter Confirm · Esc Close")

    def receive(self, event: dict[str, Any]) -> None:
        if event["type"] == "request.failed":
            self.fail(event["error"])
        elif event["type"] == "permissions.selected":
            self.reusing_saved_key = False
            self.current = event["mode"]
            self.confidence = event.get("confidence", self.confidence)
            self.has_saved_key = event.get("info", {}).get("has_permission_key", self.has_saved_key or self.entering_key)
            self.set_selecting(False)
            if self.close_after_save:
                self.action_close()
                return
            self.show_modes()
            self.status("Permissions saved · ←/→ Actions · Tab Settings · Enter Confirm · Esc Close")

    def status(self, text: str) -> None:
        self.query_one("#permissions-status", Static).update(text)

    def show_description(self) -> None:
        index = self.query_one(OptionList).highlighted
        if index is None:
            return
        mode = "auto" if self.entering_key else MODES[index][0]
        description = self.query_one("#permissions-description", Static)
        description.update(DESCRIPTIONS[mode])
        description.set_class(mode == "danger", "danger")
        show_confidence = (not self.entering_key and self.current == "auto"
                            and mode == "auto" and self.confidence_supported)
        panel = self.query_one("#permissions-confidence")
        returning_to_jev = show_confidence and not panel.display
        panel.display = show_confidence
        description.display = not show_confidence
        if returning_to_jev:
            self.select_action("confidence-save", focus=False)

    def on_option_list_option_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        event.stop()
        self.show_description()

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        event.stop()
        if self.selecting or self.finished:
            return
        mode = MODES[event.option_index][0]
        if mode == "auto":
            if not self.confidence_supported:
                self.status("This daemon does not support configurable AI confidence. Update and restart the daemon.")
                return
            if not self.has_saved_key:
                self.show_key()
            elif self.current == "auto":
                self.select_action("confidence-save")
            else:
                self.select("auto")
        else:
            self.select(mode)

    def show_key(self) -> None:
        self.entering_key = True
        self.query_one("#permissions-picker").display = False
        self.query_one(OptionList).display = False
        self.query_one("#permissions-confidence").display = False
        self.show_description()
        field = self.query_one("#permissions-key", Input)
        field.display = True
        field.focus()
        self.status("Enter a new OpenRouter key · Enter Connect · Esc Back")

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id == "confidence-value":
            self.query_one("#confidence-warning").set_class(event.value.isdigit() and int(event.value) == 0, "danger")

    def on_input_submitted(self, event: Input.Submitted) -> None:
        event.stop()
        if self.selecting or self.finished:
            return
        if event.input.id == "confidence-value" and not self.entering_key:
            self.save_confidence()
        elif self.entering_key:
            key = event.value.strip()
            if not key:
                self.status("Enter a new key · Esc Back")
                return
            self.select("auto", key)

    def on_button_pressed(self, event: Button.Pressed) -> None:
        event.stop()
        if self.selecting or self.finished:
            return
        button = event.button.id
        if button == "permissions-change-key":
            self.select_action(button, focus=False)
            self.show_key()
        elif button == "confidence-save":
            self.select_action(button, focus=False)
            self.save_confidence()

    def save_confidence(self) -> None:
        if self.current != "auto" or self.entering_key or not self.confidence_supported:
            return
        field = self.query_one("#confidence-value", Input)
        if not field.value.isdigit() or not 0 <= int(field.value) <= 100:
            self.status("Enter a whole percentage between 0 and 100 · Esc Close")
            field.focus()
            return
        confidence = int(field.value)
        if confidence > 0 and not self.has_saved_key:
            self.show_key()
            self.status("An OpenRouter key is required when AI checks are enabled · Esc Back")
            return
        self.select("auto", confidence=confidence)

    def set_selecting(self, selecting: bool) -> None:
        self.selecting = selecting
        for selector in ("#permissions-list", "#permissions-key", "#permissions-confidence"):
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
        if event.widget.id in ("confidence-save", "permissions-change-key"):
            self.select_action(event.widget.id, focus=False)

    def select(self, mode: str, key: str = "", *, confidence: int | None = None) -> None:
        self.close_after_save = confidence is not None
        self.reusing_saved_key = mode == "auto" and self.current != "auto" and not self.entering_key
        self.set_selecting(True)
        self.query_one("#permissions-key", Input).value = ""
        self.status("Saving permissions…")
        payload = {"type": "permissions.select", "mode": mode, "key": key}
        if confidence is not None:
            payload["confidence"] = confidence
        self.post_message(self.Requested(self, payload))

    def fail(self, error: str) -> None:
        self.close_after_save = False
        self.set_selecting(False)
        if self.reusing_saved_key:
            self.reusing_saved_key = False
            self.show_key()
        if self.entering_key:
            self.query_one("#permissions-key").focus()
        elif self.current == "auto":
            self.query_one("#confidence-value").focus()
        self.status(error + " · Esc Close")

    def action_close(self) -> None:
        if self.finished or self.selecting:
            return
        if self.entering_key:
            self.show_modes()
            return
        self.finished = True
        self.post_message(self.Closed(self))
