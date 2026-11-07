"""The finalized, structured implementation plan, rendered like the objective."""
from textual.app import ComposeResult
from textual.widgets import Button, Static
from textual.message import Message

from .objective_plan import ObjectivePlan
from .response_markdown import ResponseMarkdown


class ImplementationPlan(ObjectivePlan):
    class OpenConstruction(Message):
        pass

    def on_mount(self):
        self.sync_acceptance(getattr(self.app, "runtime_info", {}))

    def set_branch(self, branch_id):
        # ObjectivePlan's mount handler also runs for this subclass. Its branch
        # lookup must not overwrite acceptance of the implementation version.
        self.sync_acceptance(getattr(self.app, "runtime_info", {}))

    def sync_acceptance(self, info):
        accepted = info.get("implementation_plan") or {}
        ready = bool(info.get("implementation_execution") and self.message_id == accepted.get("id"))
        self.set_class(ready, "accepted-plan")
        self.display = ready

    def on_button_pressed(self, event):
        event.stop()
        if self.has_class("accepted-plan"):
            self.post_message(self.OpenConstruction())

    def compose(self) -> ComposeResult:
        yield Static("Implementation Plan", classes="plan-heading")
        yield ResponseMarkdown(self.plan)
        yield Button("Implementation", classes="implementation-link", compact=True)
