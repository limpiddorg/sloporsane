"""Plan surfaces: inline objectives in Discussion, split implementation planning."""
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.widgets import Button

from .response_markdown import ResponseMarkdown
from .stable_scroll import StableScroll


class ImplementationPlanWorkspace(Vertical):
    """Keep the finalized implementation plan visible alongside its discussion."""
    DEFAULT_CSS = """
    ImplementationPlanWorkspace { width: 1fr; height: 1fr; border-left: solid $sane; display: none; }
    ImplementationPlanWorkspace VerticalScroll { height: 1fr; padding: 0 1; }
    ImplementationPlanWorkspace ResponseMarkdown { height: auto; margin: 0; }
    ImplementationPlanWorkspace .implementation-plan-action { height: 1; align: center middle; margin: 1 0; }
    ImplementationPlanWorkspace #implementation-plan-start { height: 1; min-height: 1;
        width: auto; min-width: 0; border: none; padding: 0 1; background: $depth-md; color: white; }
    """

    def __init__(self):
        super().__init__(id="implementation-plan-panel")
        self.shown = None

    def compose(self):
        with StableScroll():
            yield ResponseMarkdown("No Implementation Plan yet. Discuss how to build the accepted objective.")
        with Horizontal(classes="implementation-plan-action"):
            yield Button("Implement", id="implementation-plan-start", compact=True, disabled=True)

    def on_mount(self):
        for widget in self.query("Button, VerticalScroll"):
            widget.can_focus = False

    async def on_button_pressed(self, event: Button.Pressed):
        event.stop()
        self.app.query_one("ImplementationWorkspace").choose_mode()

    def sync(self):
        info = self.app.runtime_info
        self.display = (bool(info.get("implementation_view")) and not info.get("implementation_execution") and not info.get("auto_plan_pending")
                        and not self.app.implementation.discussion_preview)
        self.query_one("#implementation-plan-start", Button).disabled = (
            not info.get("implementation_plan_ready") or self.app.busy or not self.app.connected
            or bool(info.get("implementation_execution")) or bool(info.get("implementation_archive")))
        plan = info.get("implementation_plan")
        if plan == self.shown:
            return
        self.shown = plan
        text = "No Implementation Plan yet. Discuss how to build the accepted objective."
        if plan:
            text = "# Implementation Plan\n\n" + "\n".join(
                part["text"] for part in plan["content"] if part["type"] == "text")
        self.query_one(ResponseMarkdown).update(text)


class ObjectiveWorkspace(Vertical):
    """Synchronize turn drawers without reserving any Discussion layout space."""
    DEFAULT_CSS = "ObjectiveWorkspace { display: none; }"

    def sync(self):
        from .history_viewport import HistoryViewport
        for viewport in self.app.query(HistoryViewport):
            viewport.sync_objective_plans()

    def show_version(self):
        self.sync()
