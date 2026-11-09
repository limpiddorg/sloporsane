"""Routing between Discussion, its active Implement session and frozen archives."""
from textual.widgets import Tabs, Tab

from .history_transfer import HistoryTransfer
from .message_input import MessageInput
from .mode_button import ModeButton
from .agent_info_bar import AgentInfoBar
from .objective_plan import ObjectivePlan
from .implementation_plan import ImplementationPlan
from .objective_workspace import ObjectiveWorkspace, ImplementationPlanWorkspace
from .implementation_workspace import ImplementationWorkspace
from .discussion_preview import DiscussionPreview


class ImplementationConversations:
    def __init__(self, app):
        self.app = app
        self.request_id = None
        self.transfer = None
        self.loading = None
        self.generate_after_restore = False
        self.auto_plan_pace = ""
        self.drafts = {}
        self.view_session = None
        self.discussion_preview = False

    async def request(self, kind, branch_id="", *, generate=False, objective_id="", auto_plan_pace=""):
        app = self.app
        if (not app.connected or app.busy or self.request_id or app.history_loads
                or app.pending_request or app.history_rendering or app.switch_request
                or app.queued_message or app.recap_request or app.new_request
                or app.turn_navigation.request_id):
            app.sync_implementation()
            return
        entry = app.query_one(MessageInput)
        self.drafts[app.session_id] = (entry.text, tuple(entry.images))
        app.request_number += 1
        self.request_id = str(app.request_number)
        self.generate_after_restore = generate or kind == "implementation.open"
        self.auto_plan_pace = auto_plan_pace
        if kind in ("implementation.open", "implementation.view"):
            app.query_one(ImplementationWorkspace).display = True
            app.query_one(ImplementationWorkspace).show_loading(True)
            app.query_one("#discussion-body").display = False
        self.loading = app.begin_history_loading(show_progress=kind not in ("implementation.open", "implementation.view"))
        app.switch_request = self.request_id
        try:
            await app.client.send({"type": kind, "requestId": self.request_id,
                                   "sessionId": app.runtime_info.get("discussion_id", app.session_id),
                                   "branchId": branch_id, "objectiveId": objective_id})
        except (OSError, ValueError) as error:
            self.receive({"type": "request.failed", "requestId": self.request_id, "error": str(error)})

    def receive(self, event):
        if self.request_id is None or event.get("requestId") != self.request_id:
            return False
        kind = event["type"]
        if kind == "session.attach_started":
            self.transfer = HistoryTransfer(event)
        elif kind in ("session.history_chunk", "session.attached"):
            if self.transfer is None:
                raise ValueError("Implement history transfer has not started")
            restored = self.transfer.accept(event)
            if restored is not None:
                self.app.restore_implementation(restored)
        elif kind == "request.failed":
            self.app.query_one(ImplementationWorkspace).show_loading(False)
            self.app.notify(event["error"], title="Implement", severity="error", markup=False)
            self.finish()
            self.app.sync_implementation()
        return True

    def finish(self):
        self.request_id = self.transfer = None
        self.generate_after_restore = False
        self.auto_plan_pace = ""
        self.app.switch_request = None
        if self.loading is not None:
            self.app.end_history_loading(self.loading)
            self.loading = None
        self.app.query_one(MessageInput).disabled = (not self.app.connected or
            self.app.runtime_info.get("implementation_archive", False))

    async def sync(self):
        app = self.app
        info = app.runtime_info
        tabs = app.query_one("#tabs", Tabs)
        active = bool(info.get("active_implementation_id"))
        archive = info.get("implementation_archive", False)
        for id, label, visible in (("implement", "Implement", active or archive),):
            present = bool(tabs.query(f"#{id}"))
            if visible and not present:
                await tabs.add_tab(Tab(label, id=id))
            elif not visible and present:
                await tabs.remove_tab(id)
        if self.view_session != app.session_id:
            self.discussion_preview = False
            self.view_session = app.session_id
        desired = self.desired_tab()
        tabs.active = desired
        workspace = app.query_one(ImplementationWorkspace)
        inspecting = desired == "implement" and bool(info.get("implementation_execution") or info.get("auto_plan_pending"))
        workspace.display = inspecting
        app.query_one("#discussion-body").display = not inspecting
        app.query_one(ObjectiveWorkspace).sync()
        app.query_one(ImplementationPlanWorkspace).sync()
        app.query_one("#discussion-column").display = not self.discussion_preview
        app.query_one("#agent-info").display = not inspecting
        app.query_one("#chat").display = not inspecting
        app.query_one("#composer").display = (not archive and not inspecting and not self.discussion_preview
                                             and not info.get("implementation_execution") and not info.get("auto_plan_pending"))
        preview = app.query_one(DiscussionPreview)
        preview.display = self.discussion_preview
        preview.sync()
        if self.discussion_preview:
            app.query_one("#agent-info", AgentInfoBar).query_one(ModeButton).set_busy(False)
        # Attach events publish branch metadata before history restoration installs
        # the new session ID. Never request state for a missing or previous session.
        if inspecting and app.session_id and not app.switch_request:
            await workspace.attach(app.session_id, archive)
            workspace.sync_auto_plan()
        if (inspecting and not workspace.chat_open and not workspace.has_focus
                and app.focused not in workspace.query("*") and not app.screen.is_modal):
            workspace.focus()
        button = app.active_info_bar().query_one(ModeButton)
        button.display = workspace.revision_page_visible or not inspecting or app.busy
        button.implementation_view = bool(info.get("implementation_view"))
        button.implementation_plan_ready = bool(info.get("implementation_plan_ready"))
        button.set_plan_ready(bool(info.get("objective_plan_ready")))
        workspace.update_review_button()
        if workspace.revision_page_visible:
            workspace.sync_revision_page()
        self.sync_history_plans()

    def sync_history_plans(self):
        info = self.app.runtime_info
        branches = {branch["objective_id"]: branch["id"] for branch in info.get("implementation_branches", [])}
        for plan in self.app.query(ObjectivePlan):
            if isinstance(plan, ImplementationPlan):
                plan.sync_acceptance(info)
            else:
                plan.set_branch(branches.get(plan.message_id, ""))

    def desired_tab(self):
        if self.discussion_preview or not self.app.runtime_info.get("implementation_view"):
            return "discussion"
        return "implement"

    async def show_plan(self, visible):
        await self.sync()
        self.app.query_one(ImplementationWorkspace).inspect_plan(visible)

    async def select_tab(self, tab):
        if self.app.runtime_info.get("implementation_view"):
            if tab == "implement":
                self.discussion_preview = False
                self.app.query_one("#message", MessageInput).disabled = (
                    not self.app.connected or bool(self.app.runtime_info.get("implementation_archive")))
                await self.sync()
                return
            workspace = self.app.query_one(ImplementationWorkspace)
            archive = self.app.runtime_info.get("implementation_archive", False)
            if (not self.app.busy and not workspace.review_running
                    and ((archive and not self.app.runtime_info.get("active_implementation_id"))
                         or (not archive and workspace.finished_review()))):
                await self.request("implementation.view" if archive else "implementation.close")
                return
            if (self.app.runtime_info.get("active_implementation_id") or self.app.busy or workspace.review_running or self.discussion_preview
                    or self.app.runtime_info.get("implementation_execution")):
                self.discussion_preview = True
                await self.sync()
                self.app.query_one(DiscussionPreview).open(self.app.runtime_info["discussion_id"])
                self.app.query_one(DiscussionPreview).query_one("HistoryViewport").focus()
                return
        branch = self.app.runtime_info.get("active_implementation_id", "") if tab != "discussion" else ""
        await self.request("implementation.view", branch)
