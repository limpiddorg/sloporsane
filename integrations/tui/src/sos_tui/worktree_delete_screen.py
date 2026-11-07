"""Explicit confirmation before deleting a worktree directory, never its branch."""
from textual.binding import Binding
from textual import events, on
from textual.containers import Horizontal, Vertical
from textual.widgets import Button, Static
from .dialog_screen import DialogScreen


class WorktreeDeleteScreen(DialogScreen):
    AUTO_FOCUS = "#worktree-delete-cancel"
    BINDINGS = [Binding("enter", "confirm", show=False, priority=True),
                Binding("left", "select_action('cancel')", show=False, priority=True),
                Binding("right", "select_action('validate')", show=False, priority=True),
                Binding("escape", "back", show=False, priority=True)]
    DEFAULT_CSS = """
    WorktreeDeleteScreen #worktree-delete-actions { height: 1; margin-top: 1; }
    WorktreeDeleteScreen Button { margin-right: 1; }
    WorktreeDeleteScreen Button.selected-action { background: $depth-md !important; color: $foreground !important; }
    """

    def __init__(self, entry):
        super().__init__()
        self.entry = entry
        self.selected_action = "cancel"

    def compose(self):
        with Vertical(classes="popup-dialog"):
            yield Static("Delete worktree", classes="popup-heading")
            yield Static("Branch: " + self.entry.get("branch", "detached").removeprefix("refs/heads/"), markup=False)
            yield Static(self.entry["path"], markup=False)
            with Horizontal(id="worktree-delete-actions"):
                yield Button("Cancel", id="worktree-delete-cancel", classes="selected-action")
                yield Button("Validate", id="worktree-delete-validate")
            yield Static("←/→ Action · Enter Confirm · Esc Cancel", classes="popup-status")

    def action_select_action(self, action):
        self.query_one(f"#worktree-delete-{action}", Button).focus()

    @on(events.DescendantFocus)
    def select_focused_action(self, event):
        if isinstance(event.widget, Button):
            self.selected_action = "validate" if event.widget.id == "worktree-delete-validate" else "cancel"
            for button in self.query(Button):
                button.set_class(button is event.widget, "selected-action")

    def on_button_pressed(self, event):
        event.stop()
        self.dismiss(event.button.id == "worktree-delete-validate")

    def action_confirm(self):
        self.dismiss(self.selected_action == "validate")

    def action_back(self):
        self.dismiss(False)
