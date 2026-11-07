"""Pick or create a Git worktree without replacing the conversation."""
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.message import Message
from textual.widgets import Button, Input, OptionList, Rule, Static
from textual.widgets.option_list import Option
from .config import TITLE_MD
from .dialog_screen import DialogScreen, dialog_row
from .worktree_delete_screen import WorktreeDeleteScreen


class WorktreeScreen(DialogScreen):
    BINDINGS = [Binding("escape", "back", show=False, priority=True),
                Binding("left", "select_action('select')", show=False, priority=True),
                Binding("right", "select_action('delete')", show=False, priority=True),
                Binding("enter", "confirm", show=False, priority=True)]
    DEFAULT_CSS = """
    WorktreeScreen #worktree-picker { margin: 1 0; }
    WorktreeScreen #worktree-list { height: auto; max-height: 18; }
    WorktreeScreen #worktree-name { margin: 1 0; }
    WorktreeScreen #worktree-actions { height: 1; margin-bottom: 1; }
    WorktreeScreen Button { margin-right: 1; }
    WorktreeScreen Button.selected-action { background: $depth-md !important; color: $foreground !important; }
    """

    class Requested(Message):
        def __init__(self, screen, payload):
            super().__init__()
            self.screen, self.payload = screen, payload

    class Closed(Message):
        def __init__(self, screen):
            super().__init__()
            self.screen = screen

    def __init__(self):
        super().__init__()
        self.pending = False
        self.pending_change = False
        self.finished = False
        self.paths = []
        self.creating = False
        self.entries = []
        self.selected_action = "select"

    def compose(self) -> ComposeResult:
        with Vertical(classes="popup-dialog"):
            yield Static("Worktrees", classes="popup-heading")
            with Vertical(id="worktree-picker", classes="popup-picker"):
                yield Static(dialog_row("Path", detail="Branch", status_width=25), id="worktree-columns", classes="popup-columns")
                yield Rule(line_style="solid", classes="popup-separator")
                yield OptionList(id="worktree-list", classes="popup-list")
            with Horizontal(id="worktree-actions"):
                yield Button("Select", id="worktree-select", classes="selected-action")
                yield Button("Delete", id="worktree-delete")
            yield Input(placeholder="Worktree name", id="worktree-name")
            yield Static("Loading worktrees…", id="worktree-status", classes="popup-status", markup=False)

    def on_mount(self):
        self.query_one(Input).display = False
        self.request({"type": "worktree.list"})

    def request(self, payload):
        if self.pending or self.finished:
            return
        self.pending = True
        self.pending_change = payload["type"] != "worktree.list"
        self.query_one(OptionList).disabled = True
        self.query_one(Input).disabled = True
        self.update_actions()
        self.query_one("#worktree-status", Static).update(
            "Deleting worktree…" if payload["type"] == "worktree.delete" else
            "Switching worktree…" if self.pending_change else "Loading worktrees… · Esc Close")
        self.post_message(self.Requested(self, payload))

    def receive(self, event):
        self.pending = False
        self.query_one(OptionList).disabled = False
        self.query_one(Input).disabled = False
        status = self.query_one("#worktree-status", Static)
        if event["type"] == "worktree.switched":
            self.action_back(close=True)
        elif event["type"] in ("worktree.listed", "worktree.deleted"):
            listing = self.query_one(OptionList)
            listing.clear_options()
            entries = [item for item in event["worktrees"] if not item.get("bare") and not item.get("prunable")]
            self.entries = entries
            self.paths = [item["path"] for item in entries]
            current = event["workspace"]
            listing.add_option(Option("New…"))
            for item in entries:
                branch = item.get("branch", "detached").removeprefix("refs/heads/")
                status_label = "Current" if item.get("current", item["path"] == current) else "Opened in another session" if item.get("opened") else ""
                label = dialog_row(item["path"], status_label,
                                   detail=branch, detail_style=TITLE_MD, status_width=25)
                listing.add_option(Option(label))
            listing.highlighted = next((index + 1 for index, item in enumerate(entries)
                                        if item.get("current", item["path"] == current)), 0)
            self.selected_action = "select"
            status.update("↑/↓ Worktree · ←/→ Action · Enter Confirm · Esc Close")
            listing.focus()
        else:
            status.update(event.get("error", "Worktree request failed") + " · Esc Back")
            (self.query_one(Input) if self.creating else self.query_one(OptionList)).focus()
        self.update_actions()

    def selected(self):
        index = self.query_one(OptionList).highlighted
        return self.entries[index - 1] if index is not None and 0 < index <= len(self.entries) else None

    def check_action(self, action, parameters):
        if action in ("select_action", "confirm"):
            return not self.creating and not self.pending
        return super().check_action(action, parameters)

    def action_select_action(self, action):
        self.selected_action = action
        self.update_actions()

    def update_actions(self):
        item = self.selected()
        self.query_one("#worktree-actions").display = not self.creating
        for action in ("select", "delete"):
            button = self.query_one(f"#worktree-{action}", Button)
            button.set_class(self.selected_action == action, "selected-action")
            button.disabled = self.pending or (action == "delete" and (item is None or not item.get("deletable", False)))

    def on_option_list_option_highlighted(self, event):
        self.update_actions()

    def on_button_pressed(self, event):
        event.stop()
        self.action_select_action("delete" if event.button.id == "worktree-delete" else "select")
        self.action_confirm()

    def action_confirm(self):
        if self.pending or self.creating:
            return
        index = self.query_one(OptionList).highlighted
        if index is None:
            return
        if index == 0:
            self.begin_creation()
        elif self.selected_action == "delete":
            item = self.selected()
            if not item.get("deletable", False):
                return
            self.app.push_screen(WorktreeDeleteScreen(item), lambda confirmed: self.request(
                {"type": "worktree.delete", "path": item["path"]}) if confirmed else None)
        else:
            self.request({"type": "worktree.switch", "path": self.paths[index - 1]})

    def on_option_list_option_selected(self, event):
        event.stop()
        self.action_confirm()

    def begin_creation(self):
        self.creating = True
        self.query_one("#worktree-picker").display = False
        self.query_one(Input).display = True
        self.query_one(Input).focus()
        self.update_actions()
        self.query_one("#worktree-status", Static).update("Create from committed HEAD; local changes are not copied. Enter Create · Esc Back")

    def on_input_submitted(self, event):
        event.stop()
        if event.value.strip():
            self.request({"type": "worktree.create", "name": event.value.strip()})

    def action_back(self, close=False):
        if self.pending and self.pending_change:
            return
        if self.creating and not close:
            self.creating = False
            self.query_one(Input).display = False
            self.query_one("#worktree-picker").display = True
            self.query_one(OptionList).focus()
            self.selected_action = "select"
            self.update_actions()
            self.query_one("#worktree-status", Static).update("↑/↓ Worktree · ←/→ Action · Enter Confirm · Esc Close")
            return
        self.finished = True
        self.post_message(self.Closed(self))
