"""A plan and its optional stack, kept separate from ordinary assistant prose."""

from textual.app import ComposeResult
from textual.containers import Vertical
from textual.widgets import Static, Button
from textual.message import Message

from .response_markdown import ResponseMarkdown


class ObjectivePlan(Vertical):
    DEFAULT_CSS = """
    ObjectivePlan { height: auto; border-left: outer #A675DB; padding: 0 1; }
    ObjectivePlan .plan-heading { height: 1; color: #A675DB; text-style: bold; }
    ObjectivePlan .stack-heading { margin-top: 1; }
    ObjectivePlan ResponseMarkdown { margin: 0; padding: 0; }
    ObjectivePlan.accepted-plan { border-left: outer #05b07d; }
    ObjectivePlan.accepted-plan .plan-heading { color: #05b07d; }
    """

    class OpenImplementation(Message):
        def __init__(self, branch_id: str) -> None:
            super().__init__()
            self.branch_id = branch_id

    def __init__(self, plan: str, stack: str = "", message_id: str = "") -> None:
        super().__init__(classes="chat-message")
        self.set_class(type(self) is ObjectivePlan, "objective-history")
        self.plan = plan
        self.stack = stack
        self.message_id = message_id
        self.branch_id = ""

    def on_mount(self) -> None:
        branches = getattr(self.app, "runtime_info", {}).get("implementation_branches", [])
        self.set_branch(next((branch["id"] for branch in branches if branch["objective_id"] == self.message_id), ""))

    @property
    def show_in_history(self) -> bool:
        inline = type(self) is ObjectivePlan and "objective_plan_turns" in getattr(self.app, "runtime_info", {})
        return self.has_class("accepted-plan") and not inline

    def set_branch(self, branch_id: str) -> None:
        self.branch_id = branch_id
        self.set_class(bool(branch_id), "accepted-plan")
        self.display = self.show_in_history
        for button in self.query(Button):
            button.display = bool(branch_id)

    def on_button_pressed(self, event: Button.Pressed) -> None:
        event.stop()
        if self.branch_id:
            self.post_message(self.OpenImplementation(self.branch_id))

    def compose(self) -> ComposeResult:
        yield Static("Objective Plan", classes="plan-heading")
        yield ResponseMarkdown(self.plan)
        if self.stack.strip():
            yield Static("Stack", classes="plan-heading stack-heading")
            yield ResponseMarkdown(self.stack)
        button = Button("Implementation", classes="implementation-link")
        button.display = bool(self.branch_id)
        yield button
