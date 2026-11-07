"""A separate revised-plan page: discussion left, candidate and acceptance right."""
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.message import Message
from textual.widgets import Button, Static
from .agent_info_bar import AgentInfoBar
from .implementation_review_chat import ImplementationReviewChat
from .message_input import MessageInput
from .mode_button import ModeButton
from .response_markdown import ResponseMarkdown
from .stable_scroll import StableScroll


class RevisionPlanInput(MessageInput):
    pass


class ImplementationRevisionPlan(Vertical):
    DEFAULT_CSS = """
    ImplementationRevisionPlan { height: 1fr; display: none; }
    #revision-plan-columns { height: 1fr; }
    #revision-plan-discussion { width: 1fr; }
    #revision-plan-document { width: 1fr; border-left: solid $sane; }
    #revision-plan-scroll { height: 1fr; padding: 1 2; }
    #revision-plan-actions { height: 3; align: center middle; }
    #revision-plan-accept, #revision-plan-back { height: 1; min-height: 1;
        min-width: 0; width: auto; border: none; padding: 0 1; }
    #revision-plan-heading { height: 1; text-align: center; color: $title-md; }
    #revision-plan-input { height: auto; max-height: 8; border: none; background: $input-background; }
    """

    class Discuss(Message):
        def __init__(self, text):
            super().__init__()
            self.text = text

    class Accept(Message):
        pass

    class Back(Message):
        pass

    class Cancel(Message):
        pass

    def compose(self):
        yield Static("", id="revision-plan-heading")
        with Horizontal(id="revision-plan-columns"):
            with Vertical(id="revision-plan-discussion"):
                yield ImplementationReviewChat(id="revision-plan-chat", classes="conversation-history")
                yield AgentInfoBar(id="revision-plan-agent-info")
                yield RevisionPlanInput(id="revision-plan-input")
            with Vertical(id="revision-plan-document"):
                with StableScroll(id="revision-plan-scroll"):
                    yield ResponseMarkdown("")
                with Horizontal(id="revision-plan-actions"):
                    yield Button("Implement", id="revision-plan-accept", compact=True)
        yield Button("Back to steps", id="revision-plan-back", compact=True)

    def sync(self, state, running, archive=False):
        self.query_one("#revision-plan-heading", Static).update("Proposed implementation plan")
        markdown = self.query_one("#revision-plan-scroll").query_one(ResponseMarkdown)
        text = state.get("proposed_plan_markdown", "")
        if markdown._markdown != text:
            markdown.update(text)
        self.query_one("#revision-plan-accept", Button).disabled = running or archive or not text
        self.query_one(RevisionPlanInput).disabled = archive
        button = self.query_one(ModeButton)
        button.display = True
        button.set_mode("sane")
        button.set_busy(running)
        button.label = "Cancel" if running else "Sane"

    def on_message_input_submitted(self, event):
        event.stop()
        self.post_message(self.Discuss(event.text))

    def on_button_pressed(self, event):
        event.stop()
        if event.button.id == "revision-plan-accept":
            self.post_message(self.Accept())
        elif event.button.id == "revision-plan-back":
            self.post_message(self.Back())
        elif isinstance(event.button, ModeButton):
            self.post_message(self.Cancel())
