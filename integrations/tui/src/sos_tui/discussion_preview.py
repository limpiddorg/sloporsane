"""Read the attached implementation's parent without changing the active session."""
from textual import work
from textual.containers import Vertical
from textual.widgets import Button, Static

from .history_viewport import HistoryViewport
from .history_window import HistoryWindow
from .manual_approval import ManualApprovalView
from .objective_plan import ObjectivePlan
from .message_input import MessageInput


class DiscussionPreview(Vertical):
    DEFAULT_CSS = """
    DiscussionPreview { height: 1fr; display: none; }
    DiscussionPreview > Static { height: 1; color: $misc-md; margin: 0 1; }
    DiscussionPreview .approval-action, DiscussionPreview .implementation-link { display: none; }
    DiscussionPreview > Button { height: 1; min-height: 1; width: auto;
        border: none; padding: 0 1; margin: 0 1; }
    DiscussionPreview #discussion-locked-composer { height: auto; min-height: 1;
        padding: 0 1; background: $input-background; color: $muted; }
    """

    def __init__(self):
        super().__init__(id="discussion-preview")
        self.window = HistoryWindow()
        self.snapshot_source = None

    def compose(self):
        yield Static("Discussion • Read only. Generation continues in Implement.", markup=False)
        yield HistoryViewport(self.window, self.send, classes="conversation-history")
        yield Button("Open Discussion", id="discussion-preview-open", compact=True)
        yield Static("Finish the current implementation before writing in Discussion.",
                     id="discussion-locked-composer", markup=False)

    def sync(self):
        workspace = self.app.query_one("ImplementationWorkspace")
        constructing = bool(self.app.runtime_info.get("implementation_execution") or self.app.runtime_info.get("auto_plan_pending"))
        button = self.query_one("#discussion-preview-open", Button)
        button.display = not constructing and not self.app.runtime_info.get("active_implementation_id")
        button.disabled = self.app.busy or workspace.review_running
        if self.display:
            self.app.query_one("#message", MessageInput).disabled = True

    async def on_button_pressed(self, event):
        if event.button.id == "discussion-preview-open":
            event.stop()
            await self.app.implementation.request("implementation.view")

    async def send(self, payload):
        await self.app.client.send(dict(payload, requestId="preview:" + payload["requestId"]))

    def receive(self, event):
        request_id = event.get("requestId", "")
        if not request_id.startswith("preview:"):
            return False
        self.window.accept(dict(event, requestId=request_id.removeprefix("preview:")))
        return True

    def open(self, session_id):
        source = (session_id, self.app.session_id)
        if self.snapshot_source != source:
            self.snapshot_source = None
            self.load(session_id)

    @work(exclusive=True, group="discussion-preview")
    async def load(self, session_id, direction="latest", anchor=None):
        viewport = self.query_one(HistoryViewport)
        if self.window.session_id != session_id:
            self.window.reset()
            self.window.session_id = session_id
        viewport.set_history_loading(True)
        try:
            result = await self.window.navigate(self.send, direction, anchor=anchor)
            await viewport.replace(result, direction)
            self.snapshot_source = (session_id, self.app.session_id)
        except (OSError, ValueError) as error:
            self.app.notify(str(error), title="Discussion", severity="error", markup=False)
        finally:
            viewport.navigation_pending = False
            viewport.set_history_loading(False)

    def on_history_viewport_navigate(self, event):
        event.stop()
        self.load(self.window.session_id, event.direction, event.anchor)

    def on_manual_approval_view_requested(self, event: ManualApprovalView.Requested):
        event.stop()

    def on_objective_plan_open_implementation(self, event: ObjectivePlan.OpenImplementation):
        event.stop()
        if event.branch_id == self.app.runtime_info.get("active_implementation_id"):
            self.app.call_later(self.app.implementation.select_tab, "implement")
