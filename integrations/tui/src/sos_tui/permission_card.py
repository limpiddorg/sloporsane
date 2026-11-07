"""An approval request at the current conversation position, outside Activity."""

from typing import Any
from rich.text import Text
from textual.app import ComposeResult
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.message import Message
from textual.events import DescendantFocus
from textual.widgets import Button, Static


class PermissionCard(Vertical):
    BINDINGS = [("left,up,shift+tab", "previous_choice", "Previous choice"),
                ("right,down", "next_choice", "Next choice")]
    DEFAULT_CSS = """
    PermissionCard { height: auto; border: none; padding: 0; margin: 1 0;
        background: $background; }
    PermissionCard > .permission-band { dock: left; width: 1; height: 100%; background: $title-md; }
    PermissionCard > .permission-content { height: auto; padding-left: 1; background: $background; }
    PermissionCard .permission-heading { height: auto; color: $title-md; text-style: bold; }
    PermissionCard .permission-preview { height: auto; max-height: 12; scrollbar-size: 1 1;
        background: $background; scrollbar-background: $background; }
    PermissionCard .permission-preview Static { height: auto; }
    PermissionCard .permission-buttons { height: 1; margin-top: 1; background: $background; }
    PermissionCard Button, PermissionCard Button:focus { width: auto; min-width: 0;
        height: 1; min-height: 1; padding: 0 1; margin-right: 1;
        color: $foreground; background: $input-background; border: none; }
    PermissionCard Button:hover, PermissionCard Button:focus { background: $depth-md; text-style: bold; }
    """

    class Decided(Message):
        def __init__(self, permission_id: str, decision: str) -> None:
            super().__init__()
            self.permission_id = permission_id
            self.decision = decision

    def __init__(self, request: dict[str, Any]) -> None:
        super().__init__()
        self.request = request
        self.permission_id = request["permissionId"]
        self.waiting = False
        self.selected_button = "permission-yes"

    def on_mount(self) -> None:
        self.call_after_refresh(self.reveal_choice)

    def on_descendant_focus(self, event: DescendantFocus) -> None:
        if not self.waiting and isinstance(event.widget, Button) and event.widget.has_focus:
            self.selected_button = event.widget.id

    def selected_choice(self) -> Button | None:
        # Deferred focus callbacks may run while the card's children are unmounting.
        return next((button for button in self.query(Button) if button.id == self.selected_button), None)

    def reveal_choice(self) -> None:
        if not self.is_attached or self.waiting or self.screen is not self.app.screen:
            return
        button = self.selected_choice()
        if button is None:
            return
        button.scroll_visible(animate=False, immediate=True)
        # Focus only after the repositioned layout has actually been displayed.
        self.call_after_refresh(self.focus_visible_choice)

    def focus_visible_choice(self) -> None:
        if not self.is_attached or self.waiting or self.screen is not self.app.screen:
            return
        button = self.selected_choice()
        if button is None:
            return
        if self.screen.can_view_entire(button):
            button.focus(scroll_visible=False)

    def owns_button_focus(self) -> bool:
        return any(button.has_focus for button in self.query(Button))

    def move_choice(self, direction: int) -> None:
        if self.waiting or not self.is_attached or self.screen is not self.app.screen:
            return
        buttons = list(self.query(Button))
        if not buttons:
            return
        remembered = next((i for i, button in enumerate(buttons) if button.id == self.selected_button), 0)
        button = buttons[(remembered + direction) % len(buttons)]
        self.selected_button = button.id
        self.reveal_choice()

    def action_previous_choice(self) -> None:
        self.move_choice(-1)

    def action_next_choice(self) -> None:
        self.move_choice(1)

    def compose(self) -> ComposeResult:
        request = self.request
        external = request["permission"] == "external_directory"
        heading = "Allow access outside the workspace?" if external else f"Allow {request['tool']}?"
        details = request.get("details") or {}
        text = details.get("command") or details.get("filepath") or "\n".join(request.get("patterns", []))
        if details.get("workdir"):
            text += "\nWorking directory: " + details["workdir"]
        if details.get("diff"):
            text += "\n\n" + details["diff"]
        yield Static("", classes="permission-band")
        with Vertical(classes="permission-content"):
            yield Static(Text(heading), classes="permission-heading")
            with VerticalScroll(classes="permission-preview"):
                yield Static(Text(text))
            with Horizontal(classes="permission-buttons"):
                yield Button("Yes", id="permission-yes", compact=True)
                yield Button("Always yes", id="permission-always", compact=True)
                yield Button("No", id="permission-no", compact=True)

    def on_button_pressed(self, event: Button.Pressed) -> None:
        event.stop()
        if self.waiting:
            return
        self.selected_button = event.button.id
        if self.screen is not self.app.screen or not self.screen.can_view_entire(event.button):
            # The user may have scrolled away since the button received focus.
            # Reveal it again, but never approve an off-screen choice with Enter.
            self.call_after_refresh(self.reveal_choice)
            return
        self.waiting = True
        for button in self.query(Button):
            button.disabled = True
        self.post_message(self.Decided(self.permission_id, event.button.id.removeprefix("permission-")))

    def retry(self) -> None:
        self.waiting = False
        for button in self.query(Button):
            button.disabled = False
        self.call_after_refresh(self.reveal_choice)
