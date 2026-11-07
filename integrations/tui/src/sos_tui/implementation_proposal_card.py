"""Review proposals use the same inline approval presentation as tool requests."""
from rich.text import Text
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.widgets import Button, Static
from .permission_card import PermissionCard
from .response_markdown import ResponseMarkdown


class ImplementationProposalCard(PermissionCard):
    DEFAULT_CSS = """
    ImplementationProposalCard .permission-preview { max-height: 8; }
    ImplementationProposalCard .proposal-description { height: auto; margin: 0; padding: 0; }
    """

    def __init__(self):
        super().__init__({"permissionId": "", "permission": "tool", "tool": "rework"})
        self.display = False
        self.revision = None
        self.plan_proposal = False
        self.focus_pending = False

    def on_resize(self):
        if self.focus_pending:
            self.call_after_refresh(self.reveal_choice)

    def on_markdown_table_of_contents_updated(self, event):
        if self.focus_pending:
            self.call_after_refresh(self.reveal_choice)

    def focus_visible_choice(self):
        super().focus_visible_choice()
        if self.owns_button_focus():
            self.focus_pending = False

    def compose(self):
        yield Static("", classes="permission-band")
        with Vertical(classes="permission-content"):
            yield Static("", classes="permission-heading")
            with VerticalScroll(classes="permission-preview"):
                yield ResponseMarkdown("", classes="proposal-description")
            with Horizontal(classes="permission-buttons"):
                yield Button("Accept", id="permission-yes", compact=True)
                yield Button("Decline", id="permission-no", compact=True)

    def sync(self, state, *, disabled=False, archive=False):
        state = state or {}
        request = state.get("proposed_rework", "")
        token = state.get("revision")
        was_actionable = self.display and not self.query_one("#permission-yes", Button).disabled
        changed = token != self.revision
        self.display = bool(request) and not archive and not state.get("revision_planning")
        if token != self.revision:
            self.revision = token
            self.permission_id = str(token)
            self.waiting = False
        self.plan_proposal = bool(state.get("proposed_plan_markdown"))
        self.query_one(".permission-heading", Static).update(Text(
            "Propose a revised implementation?" if self.plan_proposal else "Accept step corrections?"))
        text = "Open a new Implementation plan." if self.plan_proposal else request + "\n\nAccept starts corrections; human validation is still required."
        preview = self.query_one(".proposal-description", ResponseMarkdown)
        if preview._markdown != text:
            preview.update(text)
        for button in self.query(Button):
            button.disabled = disabled or self.waiting or state.get("status") == "revising"
        if self.display and not disabled and not self.waiting and (changed or not was_actionable):
            self.selected_button = "permission-yes"
            self.focus_pending = True
            self.call_after_refresh(self.reveal_choice)
