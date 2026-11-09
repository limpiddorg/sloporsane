"""Construction inspection and ordered review, separate from execution progress."""
from pathlib import PurePosixPath
from rich.text import Text
from textual import work
from textual.app import ComposeResult
from textual.content import Content
from textual.geometry import Region, Spacing
from textual.visual import Padding
from textual.containers import Horizontal, Vertical, VerticalScroll
from .stable_scroll import StableScroll
from .implementation_step_list import ImplementationStepList
from textual.widgets import Button, OptionList, Static, Tree
from textual.widgets.option_list import Option
from .question_screen import QuestionScreen
from .message_input import MessageInput
from .command_suggestions import CommandSuggestions
from .skill_details import SkillDetails
from .skills import parse_skill_commands
from .slash_commands import COMMANDS
from .history_transfer import HistoryTransfer
from .mode_button import ModeButton
from .implementation_code import HistoricalCode, change_zones
from .workspace_path import WorkspacePath
from .implementation_changes_tree import ImplementationChangesTree
from .agent_info_bar import AgentInfoBar
from .implementation_review_chat import ImplementationReviewChat
from .implementation_proposal_card import ImplementationProposalCard
from .implementation_revision_plan import ImplementationRevisionPlan, RevisionPlanInput
from .response_markdown import ResponseMarkdown
from .implementation_step_view import ImplementationStepView, substep_progress, UNNECESSARY_DESCRIPTION, performance_summary
from .config import MISC_MD, TITLE_MD, BOLD_MD, ALGAE_MD, MUTED_COLOR


class ImplementationModeScreen(QuestionScreen):
    def __init__(self):
        super().__init__({"questionId": "", "sessionId": "", "question": "How should the implementation run?",
                          "options": ["Step by step", "Everything"]})


class ReviewInput(MessageInput):
    pass


REVIEW_COMMANDS = ("/connect", "/model", "/permissions", "/mcps")


class SubstepTitle(Text):
    """Keep the option's indentation outside its wrapping text."""

    INDENT = 2

    def visualize(self):
        return Padding(Content.from_rich_text(self[self.INDENT:]), Spacing(0, 0, 0, self.INDENT))


class ImplementationVersionLabel(Static, can_focus=True):
    """Select the version overview without opening a plan or step."""

    def on_focus(self):
        self.call_after_refresh(self.app.query_one(ImplementationWorkspace).show_selection)

    def on_click(self):
        if not self.disabled:
            self.focus()


class ImplementationWorkspace(Vertical):
    can_focus = True
    DEFAULT_CSS = """
    ImplementationWorkspace { height: 1fr; min-height: 5; display: none; }
    #implementation-body { height: 1fr; }
    #implementation-sidebar { width: 28%; min-width: 18; max-width: 42; border-right: solid $depth-md; }
    #implementation-step-list { height: 1fr; }
    #implementation-steps { height: auto; overflow: hidden hidden; background: $background; border: none; }
    #implementation-loading { display: none; height: 1fr; content-align: center middle; color: $misc-md; }
    #implementation-version-navigation { height: 1; align: left middle; background: $input-background; }
    #implementation-version-navigation Button { height: 1; min-height: 1; width: 3; min-width: 3;
        border: none; padding: 0; background: #3A4655; color: $foreground;
        text-style: bold; background-tint: transparent; tint: transparent; }
    #implementation-version-navigation Button:hover,
    #implementation-version-navigation Button:focus { background: $depth-md; }
    #implementation-version-navigation Button.-active { background: #3A4655; tint: transparent; }
    #implementation-version-navigation Button:disabled { background: #3A4655;
        color: $muted; text-opacity: 100%; }
    #implementation-version-label { height: 1; width: 1fr; color: $misc-md;
        content-align: center middle; text-align: center; text-style: bold; }
    #implementation-version-label:focus { background: $depth-md; color: white; text-style: bold; }
    #implementation-version-label:disabled { opacity: 1; }
    #implementation-version-overview { display: none; height: 1fr; content-align: center middle;
        text-align: center; text-style: bold; color: $title-md; }
    #implementation-plan-action, #implementation-conclusion-action {
        height: 3; padding: 0; align: left middle; background: $content-background; }
    #implementation-show-plan, #implementation-conclusion {
        height: 3; min-height: 3; width: 1fr; min-width: 0;
        border: none; padding: 0 2; background: $content-background; color: white; text-style: bold;
        content-align: left middle; text-align: left; background-tint: transparent; tint: transparent; }
    #implementation-show-plan:hover, #implementation-conclusion:hover { background: $block-hover-background; }
    #implementation-show-plan:focus, #implementation-show-plan.selected,
    #implementation-conclusion:focus, #implementation-conclusion.selected { background: $depth-md; color: white; }
    #implementation-auto-spinner { width: 2; height: 1; color: $misc-md; display: none; }
    #implementation-auto-retry { display: none; height: 1; min-height: 1; width: auto; min-width: 0; border: none; }
    #implementation-resume-plan { display: none; height: 1; min-height: 1; width: auto; min-width: 0; border: none; }
    #implementation-accepted-plan { height: 1fr; padding: 1 2; display: none; }
    #implementation-accepted-plan ResponseMarkdown { height: auto; }
    #implementation-close { display: none; height: 1; min-height: 1; width: auto;
        border: none; padding: 0 1; }
    #implementation-close:focus { background: $depth-md; color: white; text-style: bold; }
    #implementation-tree { height: 1fr; border-top: solid $depth-md; background: $background; }
    #implementation-main { width: 1fr; }
    #implementation-content { height: 1fr; }
    #implementation-diff-column { width: 1fr; }
    #implementation-diff { width: 100%; height: 1fr; padding: 0; scrollbar-gutter: stable; }
    #implementation-patch { height: auto; width: 100%; }
    #implementation-diff.description #implementation-patch { width: 100%; min-width: 0; }
    #implementation-diff.centered { align: center middle; }
    #implementation-diff.centered #implementation-patch { width: auto; max-width: 100%; text-align: left; }
    #implementation-diff-header { height: 1; display: none; }
    #implementation-file { height: 1; width: 1fr; min-width: 0; padding: 0 1; color: $title-md; }
    #implementation-explanation-hint { height: 1; width: auto; padding: 0 1; color: $text-muted; }
    #implementation-explanation-panel { display: none; height: auto; max-height: 6;
        padding: 0 1; border-bottom: solid $depth-md; }
    #implementation-explanation { height: auto; }
    #implementation-diff-count { height: 1; width: 1fr; content-align: left middle; padding: 0 1; background: $input-background; text-style: bold; }
    #implementation-chat { width: 1fr; border-left: solid $depth-md; display: none; }
    #implementation-review-composer { height: auto; background: $input-background; }
    #implementation-proposal-slot { height: auto; }
    #implementation-review-mode { width: 1; height: 100%; background: $sane; }
    #implementation-review-agent-info #mode-button { color: black; }
    #implementation-review-input { height: 1; border: none; padding: 0 0 0 1; margin: 0; background: $input-background; }
    #implementation-hint { height: 1; color: $text-muted; padding: 0 1; }
    #implementation-status { height: auto; color: $text-muted; }
    #implementation-validate { height: 1; min-height: 1; width: auto; min-width: 0;
        border: none; padding: 0 1; background: $depth-md; color: white; text-style: bold; }
    #implementation-step-action { height: 1; min-height: 1; width: auto; min-width: 0;
        border: none; padding: 0 1; background: $misc-md; color: white; text-style: bold; }
    #implementation-step-action.implementation-cancel { background: $depth-md; }
    """

    def __init__(self):
        super().__init__(id="implementation-workspace")
        self.state = None
        self.live_state = None
        self.version_states = []
        self.version_index = 0
        self.version_count = 1
        self.requested_version = None
        self.session_id = ""
        self.archive = False
        self.selection = None
        self.expanded = set()
        self.rows = []
        self.diff_index = 0
        self.diff_positions = {}
        self.chat_open = False
        self.explanations_visible = True
        self.plan_visible = False
        self.conclusion_visible = False
        self.revision_page_visible = False
        self.revision_running = False
        self.revision_records = []
        self.proposal_card = None
        self.rework_preparing = False
        self.review_running = False
        self.transfers = {}
        self.review_records = []
        self.step_reviews = {}
        self.pending = {}
        self.pending_validation = {}
        self.validating_step = None
        self.pending_directories = {}
        self.permission_popup = None
        self.spinner = 0
        self.shown_view = None
        self.pending_step_render = None
        self.step_rendering = False
        self.zones = []
        self.change_targets = {}
        self.navigation_target = "implementation-steps"
        self.visible_selection = None
        self.loading = False

    def compose(self) -> ComposeResult:
        with Horizontal(id="implementation-body"):
            with Vertical(id="implementation-sidebar"):
                with VerticalScroll(id="implementation-step-list"):
                    with Horizontal(id="implementation-version-navigation"):
                        yield Button("←", id="implementation-version-previous", compact=True, disabled=True)
                        yield ImplementationVersionLabel("Version 1", id="implementation-version-label", markup=False, disabled=True)
                        yield Button("→", id="implementation-version-next", compact=True, disabled=True)
                    with Horizontal(id="implementation-plan-action"):
                        yield Button("Implementation plan", id="implementation-show-plan", compact=True)
                        yield Static("", id="implementation-auto-spinner", markup=False)
                    yield Button("Retry auto plan", id="implementation-auto-retry", compact=True)
                    yield Button("Resume revised plan", id="implementation-resume-plan", compact=True)
                    yield ImplementationStepList(id="implementation-steps")
                    with Horizontal(id="implementation-conclusion-action"):
                        yield Button("Conclusion", id="implementation-conclusion", compact=True, disabled=True)
                yield ImplementationChangesTree(id="implementation-tree")
            with Vertical(id="implementation-main"):
                yield Static("", id="implementation-loading", markup=False)
                with Horizontal(id="implementation-content"):
                    with Vertical(id="implementation-diff-column"):
                        yield Static("", id="implementation-version-overview", markup=False)
                        yield Static("", id="implementation-diff-count", markup=False)
                        with Horizontal(id="implementation-diff-header"):
                            yield WorkspacePath("", id="implementation-file", markup=False)
                            yield Static("E Toggle explanation", id="implementation-explanation-hint", markup=False)
                        with VerticalScroll(id="implementation-explanation-panel"):
                            yield Static("", id="implementation-explanation", markup=False)
                        with HistoricalCode(id="implementation-diff"):
                            yield Static("", id="implementation-patch", markup=False)
                        yield ImplementationStepView(id="implementation-step-view", classes="conversation-history")
                        with StableScroll(id="implementation-accepted-plan"):
                            yield ResponseMarkdown("")
                            yield Button("Close", id="implementation-close", compact=True)
                        yield Button("Validate", id="implementation-validate", compact=True)
                        yield Static("", id="implementation-hint")
                        yield AgentInfoBar(id="implementation-agent-info")
                        yield Static("", id="implementation-status", markup=False)
                    with Vertical(id="implementation-chat"):
                        yield ImplementationReviewChat(id="implementation-review-history", classes="conversation-history")
                        yield Vertical(id="implementation-proposal-slot")
                        details = SkillDetails(id="implementation-skill-details")
                        suggestions = CommandSuggestions(skills=lambda: self.app.runtime_info.get("skills", []),
                            details=details, commands=REVIEW_COMMANDS, id="implementation-command-suggestions")
                        yield details
                        yield suggestions
                        yield AgentInfoBar(id="implementation-review-agent-info")
                        with Horizontal(id="implementation-review-composer"):
                            yield Static("", id="implementation-review-mode")
                            yield ReviewInput(id="implementation-review-input", placeholder="Write a message and press Enter", compact=True,
                                               highlight_cursor_line=False, suggestions=suggestions)
        yield ImplementationRevisionPlan()

    def on_mount(self):
        for button in self.query("#implementation-version-navigation Button"):
            button.can_focus = False
        self.set_interval(.2, self.animate_spinner)

    def animate_spinner(self):
        if self.display and self.loading:
            self.spinner = (self.spinner + 1) % 4
            spinner = "|/-\\"[self.spinner]
            self.query_one("#implementation-loading", Static).update(f"Loading implementation... {spinner}")
            return
        if self.display and self.app.runtime_info.get("auto_plan_pending"):
            if self.app.runtime_info.get("auto_plan_running"):
                self.spinner = (self.spinner + 1) % 4
                self.query_one("#implementation-auto-spinner", Static).update("|/-\\"[self.spinner])
            return
        if self.display and self.state and self.state["status"] == "running":
            self.spinner = (self.spinner + 1) % 4
            step = self.state["work_step"]
            if (step, -1) in self.rows:
                listing = self.query_one("#implementation-steps", OptionList)
                listing.replace_option_prompt_at_index(self.rows.index((step, -1)), self.step_prompt(step))

    async def attach(self, session_id, archive=False):
        if not session_id:
            return
        self.archive = archive
        if self.session_id == session_id:
            if self.app.runtime_info.get("auto_plan_pending"):
                self.show_loading(False)
            elif self.loading and "implementation.state" not in self.pending.values():
                self.command("implementation.state")
            return
        self.session_id = session_id
        self.diff_positions.clear()
        self.show_loading(True)
        timeline = self.query_one(ImplementationStepView)
        timeline.step_index = None
        timeline.signature = None
        timeline.clock.clear()
        self.state = None
        self.live_state = None
        self.version_states = []
        self.version_index, self.version_count = 0, 1
        self.requested_version = None
        self.selection = None
        self.shown_view = None
        self.visible_selection = None
        self.plan_visible = False
        self.conclusion_visible = False
        self.diff_index = 0
        self.revision_page_visible = self.revision_running = False
        self.rework_preparing = False
        self.revision_records = []
        await self.query_one(ImplementationRevisionPlan).query_one(ImplementationReviewChat).reset_history()
        self.navigation_target = "implementation-steps"
        self.expanded.clear()
        self.review_records = []
        self.review_running = False
        self.step_reviews.clear()
        self.transfers.clear()
        self.pending_directories.clear()
        self.validating_step = None
        self.query_one(ImplementationChangesTree).reset_baseline(None)
        await self.query_one("#implementation-review-history", ImplementationReviewChat).reset_history()
        if not self.app.runtime_info.get("auto_plan_pending"):
            self.command("implementation.state")
        else:
            self.show_loading(False)

    @work(group="implementation-command")
    async def command(self, kind, *, directory_key=None, **fields):
        if kind in ("implementation.validate", "implementation.unvalidate") and self.state:
            fields.setdefault("revision", self.state.get("revision", 0))
        self.app.request_number += 1
        request_id = f"implementation-{self.app.request_number}"
        self.pending[request_id] = kind
        if directory_key is not None:
            self.pending_directories[request_id] = (directory_key, fields["path"])
        if kind == "implementation.validate" and self.state:
            self.pending_validation[request_id] = (fields["step"], fields["substep"])
        try:
            await self.app.client.send({"type": kind, "requestId": request_id, "sessionId": self.session_id or self.app.session_id, **fields})
        except (OSError, ValueError) as error:
            self.pending.pop(request_id, None)
            if kind == "implementation.history":
                self.requested_version = None
                self.sync_version_navigation()
            if kind == "implementation.rework":
                self.restore_revision_review()
            if request_id in self.pending_validation:
                self.pending_validation.pop(request_id)
                self.validating_step = None
                self.show_selection()
            if request_id in self.pending_directories:
                key, path = self.pending_directories.pop(request_id)
                self.query_one(ImplementationChangesTree).fail_directory(key, path)
            self.app.notify(str(error), title="Implement", severity="error", markup=False)

    def choose_mode(self):
        self.app.push_screen(ImplementationModeScreen(), self.mode_chosen)

    def mode_chosen(self, choice):
        if choice is not None:
            self.session_id = self.app.session_id
            self.command("implementation.start", pace=("step_by_step", "everything")[choice])

    @property
    def archive(self):
        return self._archive or self.version_index < self.version_count - 1

    @archive.setter
    def archive(self, value):
        self._archive = value

    def sync_version_navigation(self):
        preparing = self.rework_preparing or (self.live_state or {}).get("status") == "revising"
        number = self.version_index + 1 + int(preparing and self.version_index == self.version_count - 1)
        label = self.query_one("#implementation-version-label", Static)
        label.update(f"Version {number}")
        label.disabled = self.version_count <= 1
        self.query_one("#implementation-version-overview", Static).update(f"Version {number}")
        loading = self.requested_version is not None or preparing or self.validating_step is not None
        self.query_one("#implementation-version-previous", Button).disabled = loading or self.version_index == 0
        self.query_one("#implementation-version-next", Button).disabled = loading or self.version_index == self.version_count - 1

    def apply_state(self, state):
        count = (state or {}).get("version_count", 1)
        changed = count != self.version_count
        self.live_state = state
        self.query_one(ImplementationStepView).observe_execution(state)
        if changed:
            self.version_states = []
            self.requested_version = None
            self.version_index = count - 1
        self.version_count = count
        if self.version_index == count - 1:
            self.render_state(state)
        # Browsing an archive must not hide the real execution's busy state.
        self.app.busy = bool(state and state["status"] in ("running", "revising") and not self._archive)
        self.sync_version_navigation()

    def select_version(self, index):
        if (not 0 <= index < self.version_count or self.rework_preparing or self.validating_step is not None
                or (self.live_state or {}).get("status") == "revising"):
            return
        if index < self.version_count - 1 and len(self.version_states) != self.version_count - 1:
            self.requested_version = index
            self.sync_version_navigation()
            self.command("implementation.history", all=True)
            return
        self.requested_version = None
        self.version_index = index
        target = self.live_state if index == self.version_count - 1 else self.version_states[index]
        selected = self.selection[0] if self.selection else 0
        identity = self.state["steps"][selected].get("identity") if self.state and self.selection else None
        selected = next((i for i, step in enumerate(target["steps"]) if identity and step.get("identity") == identity),
                        min(selected, len(target["steps"]) - 1))
        self.chat_open = self.conclusion_visible = self.revision_page_visible = False
        self.expanded.clear()
        self.selection = (selected, -1) if target["steps"] else None
        self.visible_selection = None
        self.state = None
        self.shown_view = None
        self.diff_index = 0
        timeline = self.query_one(ImplementationStepView)
        timeline.signature = timeline.step_index = None
        self.query_one("#implementation-accepted-plan").scroll_home(animate=False)
        self.query_one(ImplementationChangesTree).reset_baseline(None)
        self.render_state(target)
        self.app.busy = bool(self.live_state and self.live_state["status"] in ("running", "revising") and not self._archive)
        self.sync_version_navigation()

    def render_state(self, state):
        if state and (state.get("status") == "revising" or not state.get("proposed_rework")):
            self.rework_preparing = False
        self.show_loading(False)
        was_finished = self.finished_review()
        previous = self.state
        identity = (previous["steps"][self.selection[0]].get("identity")
                    if previous and self.selection and self.selection[0] < len(previous["steps"]) else None)
        self.state = state
        if state and (previous or {}).get("review_step") != state.get("review_step"):
            self.review_records, self.review_running = self.step_reviews.get(state.get("review_step"), ([], False))
            self.render_review()
        self.query_one("#implementation-conclusion", Button).disabled = not bool(
            state and state["steps"] and all(step["whole"]["complete"] for step in state["steps"]))
        if self.query_one("#implementation-conclusion", Button).disabled:
            self.conclusion_visible = False
        if identity and state:
            index = next((i for i, step in enumerate(state["steps"]) if step.get("identity") == identity), None)
            if index is not None and index != self.selection[0]:
                self.selection = (index, -1)
        self.sync_rework_proposal()
        if not state:
            self.sync_auto_plan()
            return
        self.select_interrupted_step()
        self.collapse_revised_steps(previous)
        work_step = state["work_step"]
        if state["status"] == "running" and work_step < len(state["steps"]):
            current = state["steps"][work_step]
            old = next((step for step in (previous or {}).get("steps", [])
                        if step.get("identity") == current.get("identity")), {})
            if current.get("display_revision") and current.get("display_revision") != old.get("display_revision"):
                self.selection = (work_step, -1)
                self.plan_visible = False
                self.expanded.discard(work_step)
        self.app.busy = state["status"] in ("running", "revising") and not self.archive
        self.query_one("#implementation-agent-info").query_one(ModeButton).display = self.app.busy
        self.refresh_rows()
        self.show_selection()
        close = self.query_one("#implementation-close", Button)
        if not self.archive and state.get("revision_planning") and not (previous or {}).get("revision_planning"):
            self.revision_page_visible = True
            if not state.get("revision_branch"):
                self.revision_records = []
                self.revision_running = False
                self.query_one(RevisionPlanInput).text = ""
                self.reset_revision_discussion()
            self.call_after_refresh(self.query_one(RevisionPlanInput).focus)
        if not state.get("revision_planning"):
            if self.revision_page_visible and state["work_step"] < len(state["steps"]):
                self.plan_visible = False
                self.selection = (state["work_step"], -1)
                self.show_selection()
            self.revision_page_visible = False
        self.sync_revision_page()
        self.query_one("#implementation-resume-plan").display = bool(state.get("revision_planning"))
        close.disabled = self.review_running
        if self.finished_review() and not self.archive and not was_finished:
            self.open_conclusion()
            close.focus()
        status = self.query_one("#implementation-status", Static)
        status.update(state.get("error", ""))
        status.display = bool(state.get("error"))

    def sync_auto_plan(self):
        info = self.app.runtime_info
        pending = bool(info.get("auto_plan_pending"))
        running = pending and bool(info.get("auto_plan_running"))
        self.query_one("#implementation-auto-spinner").display = running
        self.query_one("#implementation-auto-retry").display = pending and not running and not self.archive
        self.query_one("#implementation-show-plan", Button).disabled = pending and not info.get("implementation_plan_ready")
        if not pending:
            return
        self.app.busy = running
        self.query_one("#implementation-agent-info").query_one(ModeButton).display = running
        self.query_one("#implementation-conclusion").display = False
        self.query_one("#implementation-conclusion-action").display = False
        self.query_one("#implementation-close").display = False
        for selector in ("#implementation-diff", "#implementation-step-view", "#implementation-validate",
                         "#implementation-file", "#implementation-diff-header", "#implementation-diff-count", "#implementation-chat", "#implementation-explanation-panel"):
            self.query_one(selector).display = False
        self.query_one(ImplementationChangesTree).show_files([], [], [], str, 0)
        self.query_one("#implementation-accepted-plan").display = True
        message = "Generating the implementation plan…" if running else (
            info.get("auto_plan_error") or "Auto plan was interrupted. Retry when ready.")
        self.query_one("#implementation-accepted-plan", VerticalScroll).query_one(ResponseMarkdown).update(message)
        self.query_one("#implementation-hint", Static).update("The steps will appear when the plan is ready." if running else "Retry auto plan to continue.")

    def collapse_revised_steps(self, previous):
        old_steps = (previous or {}).get("steps", [])
        for index, step in enumerate(self.state["steps"]):
            old = next((item for item in old_steps if item.get("identity") == step.get("identity")), {})
            if step.get("display_revision") != old.get("display_revision"):
                self.expanded.discard(index)

    def select_interrupted_step(self):
        """Restore the construction context once, without moving live selection."""
        if self.selection is not None or self.archive or self.state["status"] not in ("interrupted", "failed"):
            return
        step = self.state["work_step"]
        if 0 <= step < len(self.state["steps"]) and not self.state["steps"][step]["whole"]["complete"]:
            self.selection = (step, -1)

    def sync_rework_proposal(self):
        state = self.state or {}
        visible = bool(state.get("proposed_rework")) and not self.archive and not state.get("revision_planning")
        self.query_one("#implementation-proposal-slot").display = visible
        if not visible:
            if self.proposal_card is not None:
                self.proposal_card.remove()
                self.proposal_card = None
            return
        if self.proposal_card is None:
            self.proposal_card = ImplementationProposalCard()
            self.query_one("#implementation-proposal-slot").mount(self.proposal_card)
            self.call_after_refresh(self.sync_rework_proposal)
            return
        if not self.proposal_card.is_mounted:
            self.call_after_refresh(self.sync_rework_proposal)
            return
        self.proposal_card.sync(self.state, archive=self.archive,
            disabled=self.review_running or any(kind in ("implementation.rework", "implementation.revision.open", "implementation.rework.reject") for kind in self.pending.values()))

    def confirm_rework(self):
        if self.archive or not self.state or self.review_running or self.rework_preparing or not self.state.get("proposed_rework"):
            return
        kind = "implementation.revision.open" if self.state.get("proposed_plan_markdown") else "implementation.rework"
        if kind == "implementation.rework":
            self.prepare_revision_view()
        self.command(kind, revision=self.state["revision"])

    def prepare_revision_view(self):
        self.rework_preparing = True
        self.sync_version_navigation()
        self.chat_open = False
        self.query_one("#implementation-chat").display = False
        self.revision_page_visible = False
        self.plan_visible = self.conclusion_visible = False
        self.selection = (self.state["review_step"], -1)
        self.expanded.discard(self.selection[0])
        self.show_selection()
        for preparation in self.query("#implementation-step-view .step-preparation"):
            preparation.display = True
        self.sync_revision_page()
        self.focus()

    def restore_revision_review(self):
        self.rework_preparing = False
        if (self.state or {}).get("status") == "revising":
            # Stop UI preparation after failure; the server retains the revision for retry.
            self.state = dict(self.state, status="revision_pending", preparing=False)
            self.state["steps"] = [dict(step, preparing=False) for step in self.state["steps"]]
            self.live_state = self.state
            self.app.busy = False
        self.sync_version_navigation()
        if (self.state or {}).get("revision_planning"):
            self.revision_page_visible = True
            self.sync_revision_page()
            return
        self.chat_open = True
        self.query_one("#implementation-chat").display = True
        if self.proposal_card is not None:
            self.proposal_card.waiting = False
        self.show_selection()
        self.sync_rework_proposal()

    def refresh_rows(self):
        if not self.state:
            return
        options, rows = self.row_options()
        widget = self.query_one("#implementation-steps", OptionList)
        scroll = widget.scroll_y
        with widget.prevent(OptionList.OptionHighlighted):
            if rows == self.rows:
                for index, option in enumerate(options):
                    previous = widget.get_option_at_index(index)
                    if previous.prompt != option.prompt or previous.prompt.style != option.prompt.style:
                        widget.replace_option_prompt_at_index(index, option.prompt)
                    if previous.disabled != option.disabled:
                        (widget.disable_option_at_index if option.disabled else widget.enable_option_at_index)(index)
            else:
                widget.clear_options().add_options(options)
        self.rows = rows
        selectable = [row for row, option in zip(rows, options) if not option.disabled]
        if self.selection not in selectable:
            # A finalized Validate entry returns to the step's permanent summary.
            header = (self.selection[0], -1) if self.selection else None
            self.selection = header if header in selectable else next(iter(selectable), None)
        highlight = rows.index(self.selection) if self.selection and not self.plan_visible and not self.conclusion_visible and not self.query_one("#implementation-close").has_focus else None
        with widget.prevent(OptionList.OptionHighlighted):
            if widget.highlighted != highlight:
                widget.highlighted = highlight
        widget.scroll_to(y=scroll, animate=False, force=True)

    def on_permission_card_decided(self, event):
        card = self.proposal_card
        if card is None:
            return
        if event.permission_id != card.permission_id:
            return
        event.stop()
        if self.archive or self.review_running or not self.state or event.permission_id != str(self.state.get("revision")):
            card.retry()
            return
        if event.decision == "yes":
            self.confirm_rework()
        else:
            self.command("implementation.rework.reject", revision=self.state["revision"])
            self.query_one(ReviewInput).focus()

    def sync_revision_page(self):
        page = self.query_one(ImplementationRevisionPlan)
        page.display = self.revision_page_visible
        self.query_one("#implementation-body").display = not self.revision_page_visible
        if self.state:
            page.sync(self.state, self.revision_running, self.archive)

    def on_implementation_revision_plan_accept(self, event):
        event.stop()
        if self.state and self.state.get("revision_planning") and not self.revision_running and not self.archive and not self.rework_preparing:
            self.prepare_revision_view()
            self.command("implementation.rework", revision=self.state["revision"])

    def on_implementation_revision_plan_back(self, event):
        event.stop()
        self.revision_page_visible = False
        self.sync_revision_page()

    def on_implementation_revision_plan_cancel(self, event):
        event.stop()
        if self.revision_running:
            self.command("implementation.review.cancel")

    def on_implementation_revision_plan_discuss(self, event):
        event.stop()
        if self.revision_running or self.archive or not event.text.strip():
            return
        page = self.query_one(ImplementationRevisionPlan)
        self.revision_draft = page.query_one(RevisionPlanInput).text
        page.query_one(RevisionPlanInput).text = ""
        self.revision_running = True
        page.query_one(ImplementationReviewChat).start(event.text, self.app.generation_info())
        self.command("implementation.revision.review", text=event.text)
        self.sync_revision_page()

    def step_prompt(self, step):
        plan, built = self.state["plan"]["steps"][step], self.state["steps"][step]
        active = self.state["status"] == "running" and step == self.state["work_step"]
        expandable = built["whole"]["complete"]
        arrow = ("▾" if step in self.expanded else "▸") if expandable else " "
        done, total = substep_progress(built["substeps"])
        spinner = "|/-\\"[self.spinner]
        style = ALGAE_MD if built["whole"]["reviewed"] else MISC_MD if active else "white" if expandable else MUTED_COLOR
        prompt = Text(f'{arrow} {step+1}. {plan["title"]}', style=style)
        if active:
            substep = self.state["work_substep"]
            title = ("Preparing implementation" if built.get("preparing") else
                     plan["substeps"][substep]["title"] if substep < len(plan["substeps"])
                     else "Validating step")
            prompt.append(f'\n  [{done}/{total}] {" ".join(title.split()[:2])}... {spinner}')
        elif summary := performance_summary(built["whole"]):
            prompt.append("\n  " + summary, style=self.statistics_color((step, -1)))
        return prompt

    def statistics_color(self, row):
        selected = (row == self.selection and not self.plan_visible
                    and not self.query_one("#implementation-close").has_focus
                    and not self.query_one("#implementation-show-plan").has_focus)
        return TITLE_MD if selected else MUTED_COLOR

    def row_options(self):
        options, rows = [], []
        for s, (plan, built) in enumerate(zip(self.state["plan"]["steps"], self.state["steps"])):
            rows.append((s, -1))
            options.append(Option(self.step_prompt(s)))
            if s in self.expanded and built["whole"]["complete"]:
                parts = built["substeps"] + ([] if built["whole"]["reviewed"] else [built["whole"]])
                for u, part in enumerate(parts):
                    if u == len(built["substeps"]) and not self.can_validate_step(s):
                        continue
                    if (u < len(built["substeps"]) and part["complete"] and not part["diffs"]
                            and not part.get("unnecessary")):
                        continue
                    title = plan["substeps"][u]["title"] if u < len(built["substeps"]) else "Human validation"
                    rows.append((s, u))
                    skipped = u < len(built["substeps"]) and self.skipped(part)
                    style = "dim strike" if skipped else ALGAE_MD if part["reviewed"] else MISC_MD if u == len(built["substeps"]) else "white"
                    prompt = SubstepTitle(f'  {title}', style=style)
                    if u < len(built["substeps"]) and (summary := performance_summary(part)):
                        prompt.append("\n" + summary, style=self.statistics_color((s, u)))
                    options.append(Option(prompt,
                                          disabled=not built["whole"]["complete"] or not part["complete"] or skipped))
        return options, rows

    @staticmethod
    def skipped(part):
        return part.get("unnecessary", False) or (part["complete"] and not part["diffs"])

    def finished_review(self):
        return bool(self.state and self.state["status"] == "completed" and self.state["steps"]
                    and all(step["whole"]["reviewed"] for step in self.state["steps"]))

    def can_validate_step(self, step):
        first = next((s for s, built in enumerate(self.state["steps"]) if not built["whole"]["reviewed"]), None)
        return not self.archive and not self.state.get("proposed_rework") and self.state["status"] != "revising" and step == first and self.state["steps"][step]["whole"]["complete"]

    def select_after_validation(self, step, state):
        self.expanded.discard(step)
        self.selection = (min(step + 1, len(state["steps"]) - 1), -1)
        self.diff_index = 0

    def advance_validated_step(self, step):
        self.select_after_validation(step, self.state)
        self.refresh_rows()
        self.show_selection()
        if self.finished_review() and not self.archive:
            self.open_conclusion()
            self.query_one("#implementation-close", Button).focus()

    def selected_item(self):
        if not self.state or self.selection is None:
            return None
        s, u = self.selection
        if u < -1:
            return None
        built = self.state["steps"][s]
        return built["substeps"][u] if 0 <= u < len(built["substeps"]) else built["whole"]

    def summary(self, step, *, include_overview=True):
        plan, built = self.state["plan"]["steps"][step], self.state["steps"][step]
        text = Text()
        reviewed = built["whole"]["reviewed"]
        text.append(plan["title"] + "\n\n", style=f"bold {BOLD_MD}")
        if include_overview:
            text.append(self.bullet_description(built["whole"]["explanation"] or plan["objective"]) + "\n\n", style=ALGAE_MD if reviewed else "")
        for index, (definition, part) in enumerate(zip(plan["substeps"], built["substeps"]), 1):
            skipped = part.get("unnecessary", False)
            if index > 1:
                text.append("─" * 40 + "\n", style="dim")
            text.append(f'{index}. {definition["title"]}\n',
                        style=f"bold {BOLD_MD}" + (" dim strike" if skipped else ""))
            explanation = UNNECESSARY_DESCRIPTION if skipped else self.bullet_description(part["explanation"])
            text.append(explanation + "\n\n", style=ALGAE_MD if reviewed or part["reviewed"] else "")
        return text

    @staticmethod
    def bullet_description(description):
        return "\n".join(line if line.startswith(("- ", "* ", "• ")) else "- " + line
                         for line in (part.strip() for part in description.splitlines()) if line)

    def show_selection(self, *, diff_index=None):
        overview = self.query_one("#implementation-version-overview")
        overview.display = False
        if self.query_one("#implementation-version-label").has_focus:
            self.chat_open = self.plan_visible = self.conclusion_visible = False
            self.query_one("#implementation-steps", OptionList).highlighted = None
            self.show_accepted_plan()
            self.query_one("#implementation-accepted-plan").display = False
            self.query_one("#implementation-close").display = False
            self.query_one("#implementation-conclusion").set_class(False, "selected")
            overview.display = True
            self.query_one("#implementation-hint", Static).update("←/→ Version · ↓ Implementation plan")
            return
        if self.app.runtime_info.get("auto_plan_pending"):
            self.sync_auto_plan()
            return
        self.query_one("#implementation-conclusion").display = True
        self.query_one("#implementation-conclusion-action").display = True
        self.query_one("#implementation-close", Button).display = (
            self.conclusion_visible and self.finished_review() and not self.archive)
        self.refresh_rows()
        self.query_one("#implementation-conclusion", Button).set_class(self.conclusion_visible, "selected")
        if self.conclusion_visible:
            self.show_conclusion()
            return
        if self.plan_visible:
            self.show_accepted_plan()
            return
        if self.selection != self.visible_selection:
            self.visible_selection = self.selection
            self.call_after_refresh(self.scroll_to_selection)
        self.query_one("#implementation-show-plan", Button).set_class(False, "selected")
        self.query_one("#implementation-accepted-plan").display = False
        self.query_one("#implementation-chat").display = self.chat_open
        part = self.selected_item()
        s, u = self.selection if self.selection else (0, -1)
        built = self.state["steps"][s] if self.state else None
        plan = self.state["plan"]["steps"][s] if self.state else None
        substep = bool(built and 0 <= u < len(built["substeps"]))
        self.zones = change_zones(part["diffs"]) if part and substep else []
        key = (self.version_index, built.get("identity", s), built.get("display_revision", 0), u) if built else None
        if diff_index is not None:
            self.diff_index = diff_index
        else:
            position = self.diff_positions.get(key)
            self.diff_index = next((i for i, zone in enumerate(self.zones, 1)
                                    if position and position[:2] == (zone.path, zone.hunk)),
                                   position[2] if position else 1)
        self.diff_index = max(1, min(self.diff_index, len(self.zones))) if self.zones else 0
        if self.diff_index:
            zone = self.zones[self.diff_index - 1]
            self.diff_positions[key] = (zone.path, zone.hunk, self.diff_index)
        self.show_change_tree()
        self.show_item_view(s, u, plan, built, part, substep)
        self.show_controls(u, built, part)

    def scroll_to_selection(self):
        if not self.display or self.plan_visible or self.selection not in self.rows:
            return
        listing = self.query_one("#implementation-steps", OptionList)
        index = self.rows.index(self.selection)
        # Use wrapped row positions to scroll the outer menu, not OptionList's viewport.
        listing._update_lines()
        y = listing._index_to_line.get(index)
        height = listing._heights.get(index)
        if y is None or height is None:
            return
        menu = self.query_one("#implementation-step-list", VerticalScroll)
        region = Region(0, listing.virtual_region.y + listing.styles.gutter.top
                        + y - int(listing.scroll_y), menu.content_size.width, height)
        menu.scroll_to_region(region, animate=False, immediate=True, force=True)

    def inspect_plan(self, visible):
        self.conclusion_visible = False
        self.plan_visible = visible
        self.show_selection()

    def show_conclusion(self):
        self.update_review_button()
        self.zones = []
        self.diff_index = 0
        self.query_one(ImplementationChangesTree).display = False
        self.query_one(ImplementationChangesTree).show_files([], [], [], str, 0)
        self.query_one("#implementation-show-plan", Button).set_class(False, "selected")
        for selector in ("#implementation-diff", "#implementation-step-view", "#implementation-validate",
                         "#implementation-file", "#implementation-diff-header", "#implementation-diff-count", "#implementation-chat", "#implementation-explanation-panel"):
            self.query_one(selector).display = False
        document = self.query_one("#implementation-accepted-plan")
        document.display = True
        text = self.state.get("conclusion", "").strip()
        if not text:
            # Historical runs have no model-written global conclusion.
            text = "\n\n".join("## " + plan["title"] + "\n\n" + built["whole"]["explanation"]
                               for plan, built in zip(self.state["plan"]["steps"], self.state["steps"]))
        markdown = document.query_one(ResponseMarkdown)
        text = "# Conclusion\n\n" + text
        if markdown._markdown != text:
            markdown.update(text)
        self.query_one("#implementation-hint", Static).update("↑/↓ Steps · Select a step to return")

    def show_accepted_plan(self):
        self.zones = []
        self.query_one(ImplementationChangesTree).display = False
        self.query_one(ImplementationChangesTree).show_files([], [], [], str, 0)
        self.query_one("#implementation-show-plan", Button).set_class(self.plan_visible, "selected")
        for selector in ("#implementation-diff", "#implementation-step-view", "#implementation-validate",
                         "#implementation-file", "#implementation-diff-header", "#implementation-diff-count", "#implementation-chat", "#implementation-explanation-panel"):
            self.query_one(selector).display = False
        document = self.query_one("#implementation-accepted-plan")
        document.display = True
        plan = self.app.runtime_info.get("implementation_plan")
        text = "# Implementation Plan\n\n" + "\n".join(
            part["text"] for part in plan["content"] if part["type"] == "text") if plan else ""
        if self.state and self.state.get("accepted_plan_markdown"):
            text = self.state["accepted_plan_markdown"]
        markdown = document.query_one(ResponseMarkdown)
        if markdown._markdown != text:
            markdown.update(text)
        self.query_one("#implementation-hint", Static).update("↑/↓ Steps · Select a step to return")

    def show_change_tree(self):
        self.query_one(ImplementationChangesTree).display = True
        built = self.state["steps"][self.selection[0]] if self.state and self.selection else {}
        baseline = built
        if self.state and not built.get("repository_tree"):
            baseline = next((step for step in self.state["steps"] if step.get("repository_tree")), built)
        key = (self.session_id, baseline.get("repository_root", ""), baseline["repository_tree"]) if baseline.get("repository_tree") else None
        changes, zones, selected = self.change_tree_scope(built)
        self.query_one(ImplementationChangesTree).show_files(
            key, changes, zones,
            self.display_path, selected, unavailable=baseline.get("repository_tree_unavailable", False),
            step=self.selection[0] if self.selection else None)

    def change_tree_scope(self, built):
        self.change_targets = {}
        substeps = built.get("substeps", [])
        substep = self.selection[1] if self.selection else -1
        if 0 <= substep < len(substeps):
            self.change_targets = {index: (substep, index) for index in range(1, len(self.zones) + 1)}
            return substeps[substep]["diffs"], self.zones, self.diff_index
        zones = []
        changes = built.get("whole", {}).get("diffs", [])
        changed_paths = {self.display_path(diff["path"]) for diff in changes}
        if built.get("whole", {}).get("complete"):
            for index, part in enumerate(substeps):
                if not part["complete"] or self.skipped(part):
                    continue
                for diff_index, zone in enumerate(change_zones(part["diffs"]), 1):
                    if self.display_path(zone.path) not in changed_paths:
                        continue
                    zones.append(zone)
                    self.change_targets[len(zones)] = (index, diff_index)
        return changes, zones, 0

    def on_implementation_changes_tree_directory_requested(self, event):
        event.stop()
        tree = self.query_one(ImplementationChangesTree)
        if self.selection and event.baseline == tree.baseline:
            self.command("implementation.directory", directory_key=event.baseline,
                         step=self.selection[0], tree=event.baseline[-1], path=event.path)

    def display_path(self, path):
        root = self.state["steps"][self.selection[0]].get("repository_root", "") if self.selection else ""
        if root and PurePosixPath(path).is_absolute():
            try:
                return str(PurePosixPath(path).relative_to(root))
            except ValueError:
                pass
        return path

    def show_item_view(self, s, u, plan, built, part, substep):
        code = self.query_one("#implementation-diff", HistoricalCode)
        self.sync_diff_explanation()
        timeline = self.query_one(ImplementationStepView)
        timeline.display = bool(plan and u == -1)
        code.display = not timeline.display
        if timeline.display:
            if (self.rework_preparing or self.state["status"] == "revising") and s == self.state["review_step"]:
                built = dict(built, preparing=True, pending_correction=self.state.get("proposed_rework", ""))
            self.pending_step_render = (s, plan, built,
                (self.state["status"] == "running" and self.state["work_step"] == s)
                or ((self.state["status"] == "revising" or self.rework_preparing) and self.state["review_step"] == s))
            if not self.step_rendering:
                self.step_rendering = True
                self.render_step()
            return
        zone = self.zones[self.diff_index - 1] if self.diff_index else None
        zone_signature = (zone.path, zone.center, zone.end, zone.explanation, tuple(zone.rows)) if zone else None
        view = (self.selection, self.diff_index, part["explanation"] if part else "", part["complete"] if part else False,
                tuple(p["reviewed"] for p in built["substeps"]) if built else (), built["whole"]["reviewed"] if built else False,
                zone_signature)
        if view != self.shown_view:
            self.shown_view = view
            if self.diff_index:
                code.show_zone(self.zones[self.diff_index - 1])
            elif substep:
                code.show_description(Text(self.bullet_description(part["explanation"])))
            elif built and (u >= 0 or built["whole"]["reviewed"]):
                code.show_description(self.summary(s, include_overview=u == -1), centered=u == -1)
            else:
                text = Text()
                if plan:
                    text.append(plan["title"] + "\n\n", style=f"bold {TITLE_MD}")
                    text.append(self.bullet_description(built["whole"]["explanation"] or plan["objective"]))
                else:
                    text.append("Waiting for a step")
                code.show_description(text, centered=True)

    @work(group="implementation-step-render")
    async def render_step(self):
        try:
            while self.pending_step_render is not None:
                step, plan, built, running = self.pending_step_render
                self.pending_step_render = None
                await self.query_one(ImplementationStepView).show_step(step, plan, built,
                    running, self.app.generation_info(), self.bullet_description,
                    not self.archive and self.state["status"] in ("interrupted", "failed") and self.state["work_step"] == step,
                    not self.archive and running and self.state["status"] == "running")
        finally:
            self.step_rendering = False

    def show_controls(self, u, built, part):
        self.update_review_button()
        validate = self.query_one("#implementation-validate", Button)
        validate.display = bool(built and u == len(built["substeps"]) and self.can_validate_step(self.selection[0]))
        validate.disabled = self.validating_step is not None or self.review_running
        validate.label = "Applying changes..." if self.validating_step is not None else "Validate"
        zone = self.zones[self.diff_index - 1] if self.diff_index else None
        filename = self.query_one("#implementation-file", Static)
        filename.update(self.display_path(zone.path) if zone else "")
        filename.display = zone is not None
        self.query_one("#implementation-diff-header").display = zone is not None
        self.query_one("#implementation-explanation-hint").display = zone is not None and not self.chat_open
        counter = self.query_one("#implementation-diff-count", Static)
        counter.update(f"{self.diff_index}/{len(self.zones)}" if zone else "")
        counter.display = zone is not None
        self.query_one("#implementation-hint", Static).update(self.keyboard_help())
        self.query_one("#implementation-review-input", ReviewInput).disabled = not self.review_available()

    def update_review_button(self):
        for preview in self.app.query("DiscussionPreview"):
            preview.sync()
        self.query_one("#implementation-close", Button).disabled = self.review_running
        self.query_one("#implementation-agent-info").query_one(ModeButton).display = bool(
            self.app.runtime_info.get("auto_plan_pending") and self.app.runtime_info.get("auto_plan_running"))
        button = self.query_one("#implementation-review-agent-info").query_one(ModeButton)
        button.mode_background = None
        button.set_mode("sane")
        button.set_busy(self.review_running)
        button.label = "Cancel" if self.review_running else "Sane"
        button.display = self.chat_open

    def keyboard_help(self):
        if self.app.runtime_info.get("auto_plan_pending"):
            return "The steps will appear when the plan is ready." if self.app.busy else "Retry auto plan to continue."
        if self.plan_visible or self.conclusion_visible:
            return "↑/↓ Steps · Select a step to return"
        navigation = "Ctrl+←/→ Diff · " if self.chat_open else "←/→ Diff · "
        listing = "↑/↓ Steps"
        enter = "Enter Open/Expand" if self.query_one(ImplementationChangesTree).has_focus else "Enter Toggle/Validate"
        if (self.selection and self.selection[1] == -1 and not self.archive
                and not self.query_one(ImplementationChangesTree).has_focus):
            step = self.selection[0]
            if not self.state["steps"][step]["whole"]["complete"] and step == self.state["work_step"]:
                if self.state["status"] == "running":
                    enter = "Enter Cancel"
                elif self.state["status"] in ("interrupted", "failed"):
                    enter = "Enter Continue"
        return (navigation if len(self.zones) > 1 else "") + listing + (
            " · Tab Close chat" if self.chat_open else f" · {enter} · Tab Chat")

    def sync_diff_explanation(self):
        zone = self.zones[self.diff_index - 1] if self.diff_index and self.zones else None
        panel = self.query_one("#implementation-explanation-panel")
        panel.display = bool(zone and self.explanations_visible)
        text = (zone.explanation or "No explanation available. This change may come from a terminal command.") if zone else ""
        body = self.query_one("#implementation-explanation", Static)
        if body.render().plain != text:
            body.update(text)
            panel.scroll_home(animate=False)

    def toggle_diff_explanation(self):
        if self.chat_open or not self.zones:
            return
        self.explanations_visible = not self.explanations_visible
        self.sync_diff_explanation()

    def navigate(self, direction, *, diffs=False):
        version = self.query_one("#implementation-version-label")
        if version.has_focus:
            if diffs:
                self.select_version(self.version_index + direction)
            elif direction > 0:
                self.query_one("#implementation-show-plan").focus()
                self.call_after_refresh(self.inspect_plan, True)
            return
        if diffs:
            if self.zones:
                index = max(1, min(len(self.zones), self.diff_index + direction))
                if index == self.diff_index:
                    return
                self.diff_index = index
        else:
            plan = self.query_one("#implementation-show-plan", Button)
            if plan.has_focus:
                if direction < 0:
                    if not version.disabled:
                        version.focus()
                    return
                self.focus()
                self.plan_visible = False
                self.conclusion_visible = False
                self.selection = self.rows[0] if self.rows else None
                self.refresh_rows()
                self.show_selection()
                return
            close = self.query_one("#implementation-close", Button)
            if close.has_focus:
                if direction < 0:
                    self.open_conclusion()
                    return
                else:
                    return
            conclusion = self.query_one("#implementation-conclusion", Button)
            if self.conclusion_visible or conclusion.has_focus:
                if direction > 0:
                    if close.display:
                        close.focus()
                    return
                self.conclusion_visible = False
                widget = self.query_one("#implementation-steps", OptionList)
                self.selection = next((row for i, row in reversed(list(enumerate(self.rows)))
                                       if not widget.get_option_at_index(i).disabled), None)
                self.focus()
                self.show_selection()
                return
            widget = self.query_one("#implementation-steps", OptionList)
            available = [i for i in range(len(self.rows)) if not widget.get_option_at_index(i).disabled]
            if not available:
                return
            current = self.rows.index(self.selection) if self.selection in self.rows else available[0]
            position = available.index(current) if current in available else 0
            if direction < 0 and position == 0:
                if not plan.disabled:
                    plan.focus()
                    widget.highlighted = None
                    self.inspect_plan(True)
                return
            if direction > 0 and position == len(available) - 1 and not conclusion.disabled:
                self.open_conclusion()
                widget.highlighted = None
                return
            self.selection = self.rows[available[max(0, min(len(available)-1, position + direction))]]
            self.plan_visible = False
            widget.highlighted = self.rows.index(self.selection)
        self.show_selection(diff_index=self.diff_index if diffs else None)

    def enter(self):
        if self.conclusion_visible or self.query_one("#implementation-conclusion").has_focus:
            if self.query_one("#implementation-close").has_focus:
                self.close_implementation()
            else:
                self.open_conclusion()
            return
        if self.query_one("#implementation-show-plan").has_focus:
            self.query_one("#implementation-show-plan", Button).press()
            return
        if self.query_one("#implementation-close").has_focus:
            self.close_implementation()
            return
        if self.chat_open or self.selection is None:
            return
        tree = self.query_one(ImplementationChangesTree)
        if tree.has_focus:
            tree.action_select_cursor()
            return
        s, u = self.selection
        if u < -1:
            self.show_selection()
            return
        if u == -1:
            built = self.state["steps"][s]
            if not built["whole"]["complete"]:
                if not self.archive and s == self.state["work_step"]:
                    if self.state["status"] == "running":
                        self.confirm_construction_cancel()
                    elif self.state["status"] in ("interrupted", "failed"):
                        self.command("implementation.resume")
                return
            if s in self.expanded:
                self.expanded.remove(s)
            else:
                self.expanded.add(s)
                built = self.state["steps"][s]
                first = next((u for u, part in enumerate(built["substeps"]) if part["complete"] and not self.skipped(part)), None)
                if first is not None:
                    self.selection = (s, first)
            self.refresh_rows()
            self.show_selection()
        elif not self.archive:
            built = self.state["steps"][s]
            if built["whole"]["reviewed"]:
                return
            if u == len(built["substeps"]):
                self.validate_step(s)
            else:
                kind = "implementation.unvalidate" if built["substeps"][u]["reviewed"] else "implementation.validate"
                self.command(kind, step=s, substep=u)

    def validate_step(self, step):
        if self.validating_step is not None or not self.can_validate_step(step):
            return
        built = self.state["steps"][step]
        def validate(choice=0):
            if choice == 0 and self.validating_step is None:
                self.validating_step = step
                self.show_selection()
                self.command("implementation.validate", step=step, substep=len(built["substeps"]))
        if all(part["reviewed"] or self.skipped(part) for part in built["substeps"]):
            validate()
        else:
            self.app.push_screen(QuestionScreen({"questionId": "", "sessionId": "", "question": "Some substeps are not validated. Validate all substeps and finalize this step?",
                                                "options": ["Validate", "Cancel"]}), validate)

    async def escape(self):
        if self.app.runtime_info.get("auto_plan_pending") and self.app.busy:
            await self.app.request_execution_cancel()
            return
        if self.revision_page_visible:
            if self.revision_running:
                self.command("implementation.review.cancel")
            else:
                self.revision_page_visible = False
                self.sync_revision_page()
            return
        if self.permission_popup:
            self.permission_popup.dismiss(None)
            return
        if self.chat_open:
            if self.review_running:
                self.command("implementation.review.cancel")

    def toggle_chat(self):
        if self.app.runtime_info.get("auto_plan_pending"):
            return
        if self.plan_visible or self.conclusion_visible:
            return
        if not self.chat_open and not self.require_review_available():
            return
        self.chat_open = not self.chat_open
        self.query_one("#implementation-chat").display = self.chat_open
        self.query_one("#implementation-agent-info").display = not self.chat_open
        self.show_selection()
        if self.chat_open and self.proposal_card is not None:
            # The chat's new layout must settle before its actionable proposal takes focus.
            self.proposal_card.focus_pending = True
            self.call_after_refresh(self.proposal_card.reveal_choice)
        else:
            (self.query_one("#implementation-review-input") if self.chat_open else self).focus()

    def open_review_command(self):
        if self.loading or self.archive or self.revision_page_visible:
            return
        if not self.require_review_available():
            return
        entry = self.query_one(ReviewInput)
        if entry.disabled or self.plan_visible or self.conclusion_visible:
            return
        if not self.chat_open:
            self.toggle_chat()
        entry.focus()
        entry.insert("/")

    def review_available(self):
        if self.archive or not self.state or self.selection is None or self.plan_visible or self.conclusion_visible:
            return False
        step, _ = self.selection
        return step == self.state["review_step"] and self.state["steps"][step]["whole"]["complete"]

    def require_review_available(self):
        if self.review_available():
            return True
        self.app.notify("Review is only available for the first unvalidated step.", severity="error", markup=False)
        return False

    def show_loading(self, loading):
        self.loading = loading
        self.query_one("#implementation-loading").display = loading
        self.query_one("#implementation-content").display = not loading
        if loading:
            self.query_one("#implementation-loading", Static).update("Loading implementation... |")

    def on_descendant_focus(self, event):
        if self.is_mounted:
            self.refresh_rows()
            self.query_one("#implementation-hint", Static).update(self.keyboard_help())

    def on_option_list_option_highlighted(self, event):
        if (event.option_list.id == "implementation-steps" and event.option_index < len(self.rows)
                and event.option_index == event.option_list.highlighted
                and not event.option_list.get_option_at_index(event.option_index).disabled):
            selected = self.rows[event.option_index]
            if selected == self.selection and not self.plan_visible and not self.conclusion_visible:
                return
            self.selection = selected
            self.plan_visible = False
            self.conclusion_visible = False
            self.show_selection()

    def on_option_list_option_selected(self, event):
        if event.option_list.id == "implementation-steps":
            event.stop()
            if event.option_list.get_option_at_index(event.option_index).disabled:
                return
            self.selection = self.rows[event.option_index]
            self.plan_visible = False
            self.conclusion_visible = False
            step, substep = self.selection
            if substep == -1 and self.state["steps"][step]["whole"]["complete"]:
                self.enter()
            else:
                self.show_selection()

    def on_tree_node_selected(self, event):
        if event.control.id != "implementation-tree" or self.selection is None:
            return
        target = self.change_targets.get(event.node.data)
        if target is not None:
            step = self.selection[0]
            substep, diff_index = target
            self.expanded.add(step)
            self.selection = (step, substep)
            self.show_selection(diff_index=diff_index)

    async def on_button_pressed(self, event):
        if event.button.id in ("implementation-version-previous", "implementation-version-next"):
            event.stop()
            self.select_version(self.version_index + (-1 if event.button.id.endswith("previous") else 1))
            return
        if event.button.id == "implementation-conclusion":
            event.stop()
            self.open_conclusion()
            return
        if event.button.id == "implementation-show-plan":
            event.stop()
            await self.app.implementation.show_plan(True)
            return
        if event.button.id == "implementation-auto-retry":
            event.stop()
            self.command("implementation.auto_plan", pace=self.app.runtime_info["auto_plan_pace"])
            return
        if event.button.id == "implementation-resume-plan":
            event.stop()
            self.revision_page_visible = True
            self.sync_revision_page()
            return
        if event.button.id == "implementation-close":
            event.stop()
            self.close_implementation()
            return
        if isinstance(event.button, ModeButton) and self.chat_open:
            event.stop()
            await self.escape()
            return
        if event.button.id == "implementation-validate":
            event.stop()
            if self.selection and not self.archive:
                self.validate_step(self.selection[0])
        elif event.button.has_class("implementation-continue") and not self.archive:
            event.stop()
            event.button.disabled = True
            self.command("implementation.resume")
        elif event.button.has_class("implementation-cancel") and not self.archive:
            event.stop()
            self.confirm_construction_cancel()

    def confirm_construction_cancel(self):
        session, step = self.app.session_id, self.state["work_step"]
        def decided(choice):
            if choice == 1:
                self.cancel_construction(session, step)
        self.app.push_screen(QuestionScreen({"questionId": "", "sessionId": session,
            "question": "Cancel the current construction? You can continue it later.",
            "options": ["Keep running", "Cancel construction"]}), decided)

    def open_conclusion(self):
        button = self.query_one("#implementation-conclusion", Button)
        if button.disabled:
            return
        self.plan_visible = False
        self.conclusion_visible = True
        button.focus()
        self.show_selection()

    @work(group="implementation-close")
    async def close_implementation(self):
        if self.finished_review() and not self.archive and not self.review_running:
            await self.app.implementation.request("implementation.close")

    @work(group="implementation-cancel")
    async def cancel_construction(self, session, step):
        if (not self.archive and self.app.session_id == session and self.state
                and self.state["status"] == "running" and self.state["work_step"] == step):
            await self.app.request_execution_cancel(confirmed=True)

    async def on_message_input_submitted(self, event):
        event.stop()
        entry = self.query_one(ReviewInput)
        text = event.text.strip()
        command = text.split(maxsplit=1)[0] if text and event.interpret_commands else ""
        if command in COMMANDS:
            if command not in REVIEW_COMMANDS:
                self.app.notify(f"{command} is not available in implementation review.", severity="warning", markup=False)
                return
            if not self.app.validate_command(text):
                return
            await self.app.open_settings_command(command, entry)
            return
        if event.interpret_commands and not self.app.validate_command(text):
            return
        if self.selection is None or self.conclusion_visible or self.archive or self.review_running or not entry.text.strip():
            return
        if not self.require_review_available():
            return
        skills = parse_skill_commands(event.text, self.app.runtime_info.get("skills", [])) if event.interpret_commands else []
        s, u = self.selection
        zone = self.zones[self.diff_index - 1] if self.diff_index else None
        self.command("implementation.review", step=s, substep=u if u >= 0 else len(self.state["steps"][s]["substeps"]), text=event.text,
                     **({"path": zone.path, "hunk": zone.hunk} if zone else {}),
                     **({"skills": skills} if skills else {}))
        self.review_submitted_text = entry.text
        entry.text = ""
        self.review_running = True
        self.query_one("#implementation-review-history", ImplementationReviewChat).start(event.text, self.app.generation_info())
        self.show_selection()

    @work(group="implementation-review-render")
    async def render_review(self):
        await self.query_one("#implementation-review-history", ImplementationReviewChat).show_records(self.review_records, self.review_running)

    def receive(self, event):
        kind = event["type"]
        request = event.get("requestId", "")
        if kind == "implementation.directory" and event.get("sessionId") == self.session_id:
            context = self.pending_directories.pop(request, None)
            self.pending.pop(request, None)
            if context is not None:
                key, path = context
                if event.get("tree") == key[-1] and event.get("path") == path:
                    self.query_one(ImplementationChangesTree).accept_directory(key, path, event["entries"])
            return True
        if self.display and self.app.runtime_info.get("auto_plan_pending"):
            if kind == "request.failed" and self.pending.get(request) == "implementation.auto_plan":
                self.pending.pop(request, None)
                self.app.runtime_info.update(auto_plan_running=False, auto_plan_error=event["error"])
                self.sync_auto_plan()
                return True
            if kind == "message.accepted" and self.pending.get(request) == "implementation.auto_plan":
                self.pending.pop(request, None)
                return True
            if kind.startswith("execution.") or kind.startswith("message.") or kind in ("harness.message_arrived", "provider.message_arrived"):
                if kind in ("execution.completed", "execution.failed", "execution.cancelled"):
                    self.app.runtime_info["auto_plan_running"] = False
                    self.app.cancel_request_id = None
                    self.app.reset_cancel_confirmation()
                self.sync_auto_plan()
                return True
        if kind == "implementation.history" and event.get("sessionId") == self.session_id:
            command = self.pending.pop(request, None)
            if command == "implementation.history" and self.requested_version is not None:
                self.version_states = event["implementations"]
                index = self.requested_version
                self.requested_version = None
                if len(self.version_states) == self.version_count - 1:
                    self.select_version(index)
                else:
                    self.sync_version_navigation()
            return True
        if kind == "request.failed" and request in self.pending:
            if request in self.pending_directories:
                key, path = self.pending_directories.pop(request)
                self.query_one(ImplementationChangesTree).fail_directory(key, path)
            failed_validation = self.pending_validation.pop(request, None)
            if failed_validation is not None:
                self.validating_step = None
            command = self.pending.pop(request)
            if command == "implementation.history":
                self.requested_version = None
                self.sync_version_navigation()
            if command == "implementation.rework":
                self.restore_revision_review()
            if self.proposal_card is not None:
                self.proposal_card.waiting = False
            if command == "implementation.revision.review":
                self.revision_running = False
                page = self.query_one(ImplementationRevisionPlan)
                entry = page.query_one(RevisionPlanInput)
                entry.text = "\n\n".join(t for t in (getattr(self, "revision_draft", ""), entry.text) if t)
                page.query_one(ImplementationReviewChat).finish()
                self.sync_revision_page()
            if command == "implementation.resume":
                for button in self.query(".implementation-continue"):
                    button.disabled = False
            if command == "implementation.state":
                self.show_loading(False)
                self.query_one("#implementation-status", Static).update(event["error"])
            if command == "implementation.review":
                self.review_running = False
                entry = self.query_one(ReviewInput)
                draft = entry.text
                entry.text = "\n\n".join(text for text in (getattr(self, "review_submitted_text", ""), draft) if text)
                self.review_submitted_text = ""
                self.query_one("#implementation-review-history", ImplementationReviewChat).finish()
            if command == "implementation.validate":
                self.app.call_later(self.app.show_input_error, event["error"])
            else:
                self.app.notify(event["error"], title="Implement", severity="error", markup=False)
            self.show_selection()
            self.sync_rework_proposal()
            return True
        if kind == "implementation.updated" and event.get("sessionId") == self.app.session_id:
            self.pending.pop(request, None)
            validated = self.pending_validation.pop(request, None)
            if validated is not None:
                self.validating_step = None
                step, substep = validated
                state = event["implementation"]
                if substep == len(state["steps"][step]["substeps"]):
                    # Select first: never rebuild the old step while preparing its successor.
                    self.select_after_validation(step, state)
            self.apply_state(event["implementation"])
            if validated is not None:
                step, substep = validated
                if substep != len(self.state["steps"][step]["substeps"]) and self.selection == validated:
                    self.navigate(1)
            return True
        if kind == "implementation.review.accepted":
            self.pending.pop(request, None)
            self.review_submitted_text = ""
            return True
        if kind == "implementation.revision.review.accepted":
            self.pending.pop(request, None)
            self.revision_draft = ""
            return True
        if self.display and kind in ("execution.completed", "execution.failed", "execution.cancelled"):
            for message in event.get("messages", []):
                if message.get("implementation_plan") and self.app.runtime_info.get("implementation_execution"):
                    self.app.display_message(message)
            # Construction state is already finalized by implementation.updated.
            # Do not route terminal focus or old chat content into the hidden composer.
            self.app.busy = bool(self.state and self.state["status"] == "running")
            return True
        if kind == "btw.started" and event["branch"].get("review"):
            self.transfers[request] = (HistoryTransfer(event), event["branch"])
            return True
        if kind == "btw.retry" and (self.review_running or self.revision_running):
            chat = self.query_one(ImplementationRevisionPlan).query_one(ImplementationReviewChat) if event.get("branchId") == (self.state or {}).get("revision_branch") else self.query_one("#implementation-review-history", ImplementationReviewChat)
            chat.retry(event["retry"])
            return True
        if kind in ("btw.chunk", "btw.finished") and request in self.transfers:
            transfer, branch = self.transfers[request]
            result = transfer.accept(dict(event, type="session.history_chunk" if kind == "btw.chunk" else "session.attached"))
            if result is not None:
                self.transfers.pop(request)
                if branch.get("revision_planning"):
                    self.revision_records = result["messages"]
                    self.revision_running = branch["status"] == "running"
                    self.render_revision_discussion()
                    self.sync_revision_page()
                    if branch.get("error"):
                        self.app.notify(branch["error"], title="Implementation plan", severity="error", markup=False)
                    return True
                step = branch.get("review_step", (self.state or {}).get("review_step", 0))
                self.step_reviews[step] = (result["messages"], branch["status"] == "running")
                if self.state and step != self.state.get("review_step"):
                    return True
                self.review_records, self.review_running = self.step_reviews[step]
                self.sync_rework_proposal()
                self.render_review()
                self.show_selection()
                if branch.get("error"):
                    self.app.notify(branch["error"], title="Review", severity="error", markup=False)
            return True
        if kind == "permission.requested" and self.display:
            permission_id = event["permissionId"]
            self.permission_popup = QuestionScreen({"questionId": permission_id, "sessionId": self.session_id,
                "question": f'Allow {event["tool"]}?\n' + str(event.get("details", {}).get("command") or "\n".join(event.get("patterns", []))),
                "options": ["Yes", "Always yes", "No"]})
            def decided(choice):
                self.permission_popup = None
                self.command("permission.reply", permissionId=permission_id, decision=("yes", "always", "no")[choice] if choice is not None else "no")
            self.app.push_screen(self.permission_popup, decided)
            return True
        return False

    @work(group="implementation-revision-render")
    async def render_revision_discussion(self):
        await self.query_one(ImplementationRevisionPlan).query_one(ImplementationReviewChat).show_records(self.revision_records, self.revision_running)

    @work(group="implementation-revision-render")
    async def reset_revision_discussion(self):
        await self.query_one(ImplementationRevisionPlan).query_one(ImplementationReviewChat).reset_history()
