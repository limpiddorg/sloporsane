"""Textual application for daemon-backed discussions and implementation workflows."""

import argparse
import asyncio
import os
import shlex
import subprocess
import tempfile
from dataclasses import dataclass
from time import monotonic
from pathlib import Path
from typing import Any

from rich.text import Text
from textual import events, work
from textual.app import App, ComposeResult, SuspendNotSupported
from textual.binding import Binding
from textual.reactive import reactive
from textual.screen import ModalScreen, Screen
from textual.timer import Timer
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.widgets import Button, Collapsible, Static, Tab, Tabs
from textual.widgets._collapsible import CollapsibleTitle
from textual.widget import Widget

from .client import DaemonClient
from .copied_notice import CopiedNotice
from .input_error_notice import InputErrorNotice
from .images import ImagePaste, check_budget
from .config import BACKGROUND_COLOR, TEXT_COLOR, INPUT_BACKGROUND_COLOR, CONTENT_BACKGROUND_COLOR, CODE_BACKGROUND_COLOR, QUOTE_BACKGROUND_COLOR, MODE_COLORS, TITLE_MD, BOLD_MD, MISC_MD, DEPTH_MD, SLOP_COLOR, MUTED_COLOR
from .config import SCROLLBAR_HOVER_COLOR, SCROLLBAR_HOVER_BACKGROUND_COLOR
from .message_input import MessageInput
from .command_suggestions import CommandSuggestions
from .skill_details import SkillDetails
from .slash_commands import COMMANDS
from .skills import parse_skill_commands, skill_commands
from .config import ALGAE_MD
from .btw import BtwConversations
from .user_message import UserMessage
from .queued_message import QueuedMessage
from .agent_info_bar import AgentInfoBar
from .confirmation_bar import ConfirmationBar
from .chat_collapsible import ChatCollapsible
from .mode_button import ModeButton
from .response_markdown import ResponseMarkdown
from .objective_plan import ObjectivePlan
from .objective_drawer import ObjectivePlanDrawer
from .objective_workspace import ObjectiveWorkspace, ImplementationPlanWorkspace
from .implementation import ImplementationConversations
from .discussion_preview import DiscussionPreview
from .implementation_plan import ImplementationPlan
from .implementation_workspace import ImplementationWorkspace, ImplementationModeScreen, ReviewInput
from .connect_screen import ConnectScreen
from .conversation_recap import ConversationRecap
from .recap_idle import RecapIdle
from .model_screen import ModelScreen
from .mcps_screen import McpsScreen
from .permissions_screen import PermissionsScreen
from .decision_model_screen import DecisionModelScreen
from .manual_approval import ManualApprovalView, was_ai_refused
from .tool_view import ReadView, ToolView
from .turn_activity import ActivityToggle, TurnActivity
from .resume_screen import ResumeScreen
from .worktree_screen import WorktreeScreen
from .worktree_delete_screen import WorktreeDeleteScreen
from .workspace_path import WorkspacePath
from .terminal_identity import resolve_terminal_identity
from .history_transfer import HistoryTransfer
from .turn_navigation import TurnNavigation
from .history_window import HistoryWindow
from .history_viewport import HistoryActivity, HistoryViewport
from .permission_card import PermissionCard
from .question_screen import QuestionScreen
from .updates import VERSION, latest_update, install_and_restart
from .update_screen import UpdateScreen


@dataclass
class UserTurnView:
    user: UserMessage
    activity: TurnActivity


class SosApp(App[None]):
    busy = reactive(False, init=False, repaint=False)
    TITLE = "sloporsane"
    BINDINGS = [Binding("ctrl+q", "quit", "Quit"),
                Binding("slash", "implementation_slash", "Review commands", show=False, priority=True),
                Binding("e,E", "implementation_explanation", "Diff explanation", show=False, priority=True),
                Binding("ctrl+c", "clear_or_quit", "Clear or quit", show=False, priority=True),
                Binding("ctrl+g", "edit_external", "Open editor", show=False, priority=True),
                Binding("ctrl+t", "next_reasoning", "Next reasoning level", show=False, priority=True),
                Binding("pageup", "scroll_chat_page(-1)", "Scroll chat up", show=False, priority=True),
                Binding("pagedown", "scroll_chat_page(1)", "Scroll chat down", show=False, priority=True),
                Binding("tab", "tab_key", "Next suggestion or toggle mode", show=False, priority=True),
                Binding("shift+tab", "tab_key(-1)", "Previous suggestion or control", show=False, priority=True),
                Binding("left", "permission_choice(-1)", "Previous permission choice", show=False, priority=True),
                 Binding("right", "permission_choice(1)", "Next permission choice", show=False, priority=True),
                 Binding("up", "implementation_move(-1, False)", "Previous item", show=False, priority=True),
                 Binding("down", "implementation_move(1, False)", "Next item", show=False, priority=True),
                 Binding("ctrl+left", "implementation_control_move(-1, True)", "Previous diff", show=False, priority=True),
                 Binding("ctrl+right", "implementation_control_move(1, True)", "Next diff", show=False, priority=True),
                 Binding("ctrl+up", "implementation_control_move(-1, False)", "Previous item", show=False, priority=True),
                 Binding("ctrl+down", "implementation_control_move(1, False)", "Next item", show=False, priority=True),
                 Binding("enter", "implementation_enter", "Expand or validate", show=False, priority=True),
                Binding("escape", "request_cancel", "Cancel execution", show=False, priority=True)]
    CANCEL_CONFIRM_SECONDS = 3.0
    CSS = """
    * { scrollbar-size: 1 1; scrollbar-color: $muted; scrollbar-background: $background;
        scrollbar-color-hover: $scrollbar-hover; scrollbar-color-active: $scrollbar-hover;
        scrollbar-background-hover: $scrollbar-hover-background;
        scrollbar-background-active: $scrollbar-hover-background; }
    Screen { background: $background; color: $foreground; }
    DialogScreen { background: $background 70%; }
    #top-bar { height: 1; }
    #tabs { height: 1; width: 1fr; }
    #app-version { height: 1; width: auto; padding: 0 1; color: $muted; }
    #discussion-body { height: 1fr; }
    #discussion-column { width: 1fr; height: 1fr; }
    HistoryViewport .objective-history { display: none; }
    HistoryViewport ImplementationPlan { display: none; }
    HistoryViewport .accepted-plan { display: block; }
    #tabs Underline { display: none; }
    #tabs Tab { color: $foreground; text-style: bold; }
    #tabs Tab.-active { background: $depth-md; color: $foreground; }
    .conversation-history { height: 1fr; min-height: 1; padding: 0; background: $background; }
    .conversation-history > * { margin-left: 1; margin-right: 1; }
    .conversation-history > TurnActivity { margin-left: 0; margin-right: 0; }
    .conversation-history > BtwPanel { margin-left: 3; }
    .conversation-history > BtwPanel.standalone { margin-left: 1; }
    .conversation-history > .chat-message { height: auto; margin-bottom: 1; }
    .conversation-history > .generation-retry, .conversation-history > .generation-error { margin: 0 1; }
    .conversation-history > ResponseMarkdown.chat-message { margin: 0 1; }
    .conversation-history > ObjectivePlan { margin-left: 0; margin-right: 0; }
    .conversation-history > ObjectivePlanDrawer.chat-message { margin-left: 0; margin-right: 0; }
    .conversation-history > UserMessage.next-turn { margin-top: 1; }
    .conversation-history .read-call { padding-left: 2; margin-bottom: 0; }
    .conversation-history ToolView.skill-call > CollapsibleTitle,
    .conversation-history ToolView.skill-call > CollapsibleTitle:hover,
    .conversation-history ToolView.skill-call > CollapsibleTitle:focus { color: $algae-md; }
    .conversation-history ResponseMarkdown, .conversation-history ResponseMarkdown * {
        color: $foreground; background: transparent; tint: transparent; background-tint: transparent;
        link-color: $depth-md; link-background: transparent; link-style: underline;
        link-color-hover: $depth-md; link-background-hover: transparent; link-style-hover: bold underline;
    }
    .conversation-history > Markdown { padding: 0; }
    .conversation-history MarkdownHeader { color: $title-md; background: transparent;
                           text-style: bold; content-align: left middle; margin: 0; }
    .conversation-history MarkdownBlock > .strong { color: $bold-md; text-style: bold; }
    .conversation-history MarkdownBlock > .em { color: $misc-md; text-style: italic; }
    .conversation-history MarkdownBlock > .code_inline { color: $misc-md; background: $code-background; }
    .conversation-history MarkdownBullet { color: $misc-md; }
    .conversation-history MarkdownBlockQuote { color: $misc-md; border-left: outer $misc-md; background: transparent;
                              margin: 0 0 1 0; }
    .conversation-history MarkdownBlockQuote MarkdownBlock { color: $misc-md; }
    .conversation-history MarkdownHorizontalRule { color: $misc-md; border-bottom: solid $misc-md; padding-top: 0; }
    .conversation-history MarkdownFence { color: $foreground; background: $code-background; margin: 0 0 1 0; }
    .conversation-history MarkdownFence > Label { padding: 0 1; }
    .conversation-history MarkdownTableContent > .markdown-table--header { color: $title-md; background: transparent; }
    .conversation-history MarkdownTableContent > .markdown-table--lines { color: $depth-md; background: transparent; }
    .conversation-history MarkdownTableContent { keyline: thin $depth-md; grid-gutter: 0 1; }
    .conversation-history MarkdownTableContent > .header { color: $title-md; text-style: bold; }
    .conversation-history Collapsible { background: $background; border: none; padding: 0; }
    .conversation-history Collapsible, .conversation-history Collapsible:focus-within { background-tint: transparent; }
    .conversation-history CollapsibleTitle, .conversation-history CollapsibleTitle:hover, .conversation-history CollapsibleTitle:focus {
        background: $background; background-tint: transparent; tint: transparent; padding: 0 1 0 0;
    }
    .conversation-history Collapsible > Contents { padding: 0 0 0 4; }
    .conversation-history Collapsible.compaction { border-top: heavy $slop; margin-left: 0; margin-right: 0; }
    .conversation-history Collapsible.compaction.sane-compaction { border: none; }
    .conversation-history Collapsible.compaction > CollapsibleTitle { margin: 0 1; }
    .conversation-history Collapsible.compaction.sane-compaction > CollapsibleTitle { margin: 0 1 0 3; padding: 0; }
    .conversation-history Collapsible.compaction > Contents { padding-right: 1; padding-left: 5; }
    .conversation-history Collapsible.compaction.sane-compaction > Contents { padding-left: 7; }
    .conversation-history .compaction > CollapsibleTitle,
    .conversation-history .compaction > CollapsibleTitle:hover,
    .conversation-history .compaction > CollapsibleTitle:focus { color: $slop; text-style: bold; }
    .conversation-history .reasoning > CollapsibleTitle { color: $misc-md; }
    .conversation-history .reasoning > CollapsibleTitle:hover, .conversation-history .reasoning > CollapsibleTitle:focus { color: $misc-md; }
    .conversation-history .reasoning ResponseMarkdown { margin: 0; padding: 0; }
    .conversation-history ToolView { background: $background; background-tint: transparent; }
    .conversation-history ToolView > CollapsibleTitle { color: $bold-md; background: $background;
                                      text-style: none; background-tint: transparent; }
    .conversation-history ToolView > CollapsibleTitle:hover, .conversation-history ToolView > CollapsibleTitle:focus {
        color: $bold-md; background: $background; background-tint: transparent;
    }
    .conversation-history .tool-results { height: auto; }
    .conversation-history .tool-arguments { margin-bottom: 0; }
    .conversation-history .tool-code { background: $code-background; padding: 0 1; }
    .conversation-history ToolView ResponseMarkdown { padding: 0; }
    #composer { height: auto; min-height: 1; background: $input-background; }
    #mode-indicator { dock: left; width: 1; height: 100%; }
    #message { height: 1; border: none; padding: 0 0 0 1; margin: 0;
               background: $input-background; color: $foreground;
               scrollbar-color: $foreground; scrollbar-background: $input-background; }
    #session-bar { height: 1; padding: 0 1; }
    #workspace, #session-title { width: 1fr; height: 1; text-overflow: ellipsis; color: $muted; }
    #session-title { text-align: right; }
    """

    def get_css_variables(self) -> dict[str, str]:
        return {**super().get_css_variables(), "background": BACKGROUND_COLOR,
                "foreground": TEXT_COLOR, "input-background": INPUT_BACKGROUND_COLOR,
                "muted": MUTED_COLOR, "algae-md": ALGAE_MD,
                "scrollbar-hover": SCROLLBAR_HOVER_COLOR,
                "scrollbar-hover-background": SCROLLBAR_HOVER_BACKGROUND_COLOR,
                "button-focus-text-style": "bold",
                "content-background": CONTENT_BACKGROUND_COLOR,
                "code-background": CODE_BACKGROUND_COLOR,
                "quote-background": QUOTE_BACKGROUND_COLOR,
                "sane": MODE_COLORS["sane"], "slop": MODE_COLORS["slop"],
                "title-md": TITLE_MD, "bold-md": BOLD_MD, "misc-md": MISC_MD, "depth-md": DEPTH_MD}

    def get_driver_class(self):
        driver = super().get_driver_class()
        if os.name == "posix":
            from textual.drivers.linux_driver import LinuxDriver
            from .terminal_driver import TerminalDriver

            if driver is LinuxDriver:
                return TerminalDriver
        return driver

    def __init__(self, socket_path: str = "/tmp/sosd.sock") -> None:
        super().__init__()
        self.workspace = str(Path.cwd().resolve())
        self.launch_workspace = self.workspace
        self.terminal_identity = resolve_terminal_identity()
        self.client = DaemonClient(socket_path)
        self.session_id: str | None = None
        self.connected = False
        self.starting = True
        self.startup_message: dict[str, Any] | None = None
        self.request_number = 0
        self.pending_request: str | None = None
        self.turn_navigation = TurnNavigation(self)
        self.pending_text: str | None = None
        self.pending_interpret_commands = True
        self.pending_sane_request = False
        self.pending_images: tuple[ImagePaste, ...] = ()
        self.editor_restored_images: tuple[ImagePaste, ...] = ()
        self.editing_external = False
        self.editor_restored_text = ""
        self.editor_restored_pasted = False
        self.editor_restore_separator = "\n\n"
        self.pending_turn: UserTurnView | None = None
        self.queued_message: QueuedMessage | None = None
        self.queued_ready = False
        self.new_request: str | None = None
        self.switch_request: str | None = None
        self.compaction_request: str | None = None
        self.recap_request: str | None = None
        self.running_recap: ConversationRecap | None = None
        self.recap_idle = RecapIdle()
        self.recap_idle_session: str | None = None
        self.recap_automatic = False
        self.running_compaction: Collapsible | None = None
        self.compaction_attempt = 0
        self.compaction_attempt_limit = 3
        self.mode = "sane"
        self.submitted_mode = self.mode
        self.pending_tools: dict[str, ToolView | ReadView] = {}
        self.manual_requests: dict[str, tuple[str, ManualApprovalView]] = {}
        self.current_activity: TurnActivity | None = None
        self.activity_after_text = False
        self.activity_source: tuple[str, int] = ("", 0)
        self.running_activity: TurnActivity | None = None
        self.retry_notice: Static | None = None
        self.btw = BtwConversations(self)
        self.implementation = ImplementationConversations(self)
        self.retry_status: dict[str, Any] | None = None
        self.activity_clock: tuple[str, float, dict[str, Any]] | None = None
        self.runtime_info: dict[str, Any] = {}
        self.request_started_at: float | None = None
        self.cancel_deadline: float | None = None
        self.cancel_timer: Timer | None = None
        self.cancel_request_id: str | None = None
        self.quit_deadline: float | None = None
        self.quit_timer: Timer | None = None
        self.resume_screen: ResumeScreen | None = None
        self.resume_requests: dict[str, tuple[str, str, ResumeScreen | None]] = {}
        self.resume_transfer: HistoryTransfer | None = None
        self.history_window = HistoryWindow()
        self.history_rendering = False
        self.history_live: list[dict[str, Any]] = []
        self.cleaning_history = False
        self.history_navigation_number = 0
        self.history_render_lock = asyncio.Lock()
        self.history_loads: set[object] = set()
        self.history_input_disabled = True
        self.permission_card: PermissionCard | None = None
        self.question_screen: QuestionScreen | None = None
        self.permission_reply_id: str | None = None
        self.connect_screen: ConnectScreen | None = None
        self.connect_requests: dict[str, ConnectScreen] = {}
        self.model_screen: ModelScreen | None = None
        self.mcps_screen: McpsScreen | None = None
        self.mcps_requests: dict[str, McpsScreen] = {}
        self.worktree_screen: WorktreeScreen | None = None
        self.worktree_requests: dict[str, WorktreeScreen] = {}
        self.model_requests: dict[str, ModelScreen] = {}
        self.permissions_screen: PermissionsScreen | None = None
        self.permissions_requests: dict[str, PermissionsScreen] = {}

    def compose(self) -> ComposeResult:
        with Horizontal(id="top-bar"):
            yield Tabs(Tab("Discussion", id="discussion"), id="tabs")
            yield Static(VERSION if VERSION == "dev" else "v" + VERSION.removeprefix("v"), id="app-version", markup=False)
        chat = HistoryViewport(self.history_window, self.client.send, id="chat", classes="conversation-history")
        details = SkillDetails()
        suggestions = CommandSuggestions(quote_source=lambda: chat.quote_source,
                                         skills=lambda: self.runtime_info.get("skills", []), details=details, id="command-suggestions")
        with Horizontal(id="discussion-body"):
            with Vertical(id="discussion-column"):
                yield chat
                yield details
                yield suggestions
                yield AgentInfoBar(id="agent-info")
                with Horizontal(id="composer"):
                    indicator = Static("", id="mode-indicator")
                    indicator.styles.background = MODE_COLORS[self.mode]
                    yield indicator
                    yield MessageInput(placeholder="Write a message and press Enter", id="message", disabled=False,
                                        compact=True, highlight_cursor_line=False, suggestions=suggestions)
            yield DiscussionPreview()
            yield ObjectiveWorkspace()
            yield ImplementationPlanWorkspace()
        yield ImplementationWorkspace()
        with Horizontal(id="session-bar"):
            yield WorkspacePath(self.workspace, id="workspace", markup=False)
            yield Static("-", id="session-title", markup=False)

    def on_mount(self) -> None:
        self._disable_tooltips = True
        self.query_one(MessageInput).focus()
        self.set_interval(1, self.check_auto_recap)
        self.listen()
        self.update_version = None
        if VERSION != "dev":
            self.check_updates()

    @work(group="update-check", exclusive=True)
    async def check_updates(self):
        version = await asyncio.to_thread(latest_update, VERSION)
        if version is None:
            return
        # Do not replace another dialog or discard an approval being answered.
        while isinstance(self.screen, ModalScreen):
            await asyncio.sleep(0.2)
        self.push_screen(UpdateScreen(version), lambda accepted: self.update_decided(accepted, version))

    def update_decided(self, accepted, version):
        if accepted:
            self.update_version = version
            self.exit()

    async def check_auto_recap(self) -> None:
        if self.recap_idle_session != self.session_id:
            self.recap_idle_session = self.session_id
            self.recap_idle.activity()
        if (not self.connected or self.session_id is None or self.busy
                or self.runtime_info.get("last_history_is_recap", False)
                or self.runtime_info.get("objective_plan_ready", False)
                or self.runtime_info.get("implementation_plan_ready", False)
                or self.runtime_info.get("implementation_execution", False)
                or self.editing_external or self.cleaning_history or self.history_rendering
                or self.history_loads or self.new_request is not None or self.queued_message is not None
                or self.pending_request is not None or self.recap_request is not None
                or self.turn_navigation.request_id is not None
                or self.connect_requests or self.model_requests or self.permissions_requests
                or self.manual_requests or self.resume_requests or self.permission_card is not None
                or self.quit_deadline is not None or isinstance(self.screen, ModalScreen)
                or not (entry := next(iter(self.query("#message")), None)) or entry.disabled):
            self.recap_idle.pause()
            return
        if self.recap_idle.due(monotonic()):
            await self.request_recap(automatic=True)

    def on_text_selected(self, event: events.TextSelected) -> None:
        text = self.screen.get_selected_text()
        if text:
            self.copy_to_clipboard(text)
            self.screen.clear_selection()

    def copy_to_clipboard(self, text: str) -> None:
        super().copy_to_clipboard(text)  # OSC 52 also reaches a remote terminal.
        if text:
            self.call_later(self.show_copied_notice, self.screen)
        if self._driver is not None and not self._driver.is_headless and not self._driver.is_web:
            self.copy_native_clipboard(text)

    async def show_copied_notice(self, screen: Screen) -> None:
        if screen is not self.screen or not screen.is_mounted:
            return
        notice = next(iter(screen.query(CopiedNotice)), None)
        if notice is None:
            notice = CopiedNotice()
            await screen.mount(notice)
        notice.show_copy()

    async def show_input_error(self, message: str, warning: bool = False) -> None:
        screen = self.screen
        notice = next(iter(screen.query(InputErrorNotice)), None)
        if notice is None:
            notice = InputErrorNotice()
            await screen.mount(notice)
        notice.show_error(message, warning=warning)

    @work(group="clipboard", exclusive=True)
    async def copy_native_clipboard(self, text: str) -> None:
        from .clipboard import copy_native

        await copy_native(text)

    def on_descendant_focus(self) -> None:
        self.call_after_refresh(self.focus_composer)

    def on_descendant_blur(self) -> None:
        self.call_after_refresh(self.focus_composer)

    def on_app_focus(self) -> None:
        # Restore popup focus first, then the composer before the next queued key.
        self.call_next(self.focus_composer)

    def focus_composer(self) -> None:
        if (not self.app_focus or self.editing_external
                or isinstance(self.screen, ModalScreen) or self.history_loads):
            return
        if self.implementation.discussion_preview:
            self.query_one(DiscussionPreview).query_one(HistoryViewport).focus()
            return
        # Screen focus is restored synchronously; the button's has_focus flag
        # may still be waiting for its Focus event.
        if (self.permission_card is not None and not self.permission_card.waiting
                and self.focused in self.permission_card.query(Button)):
            return
        workspace = self.query_one(ImplementationWorkspace)
        if workspace.display:
            proposal = workspace.proposal_card
            if proposal is not None and proposal.display and not proposal.waiting and self.focused in proposal.query(Button):
                return
            if workspace.revision_page_visible:
                return
            if workspace.chat_open:
                entry = workspace.query_one("#implementation-review-input")
                if not entry.disabled:
                    self.set_focus(entry, scroll_visible=False)
            elif self.focused is None or self.focused not in workspace.walk_children():
                self.set_focus(workspace, scroll_visible=False)
            return
        if self.runtime_info.get("implementation_archive"):
            self.set_focus(self.query_one(HistoryViewport), scroll_visible=False)
            return
        entry = self.query_one(MessageInput)
        if not entry.disabled and not entry.has_focus:
            self.set_focus(entry, scroll_visible=False)

    def watch_busy(self, busy: bool) -> None:
        if self.recap_request is None:
            self.recap_idle.activity()
        if self.is_mounted:
            self.query_one(DiscussionPreview).sync()
            self.query_one(ObjectiveWorkspace).show_version()
            self.query_one(ImplementationPlanWorkspace).sync()
            self.active_info_bar().query_one(ModeButton).set_busy(busy and self.recap_request is None)
            workspace = self.query_one(ImplementationWorkspace)
            if workspace.display:
                workspace.update_review_button()
            if not busy:
                self.clear_retry_notice()
                self.activity_clock = None
                footer = self.query_one(HistoryViewport).live_footer
                if footer is not None:
                    footer.finish_generation()
                self.reset_cancel_confirmation()
                self.cancel_request_id = None
            if not busy and self.running_activity is not None:
                self.running_activity.toggle.finish_generation()
                self.running_activity.toggle.set_spinner(None)
                self.running_activity = None

    def on_mode_button_spinner_frame(self, event: ModeButton.SpinnerFrame) -> None:
        # A queued spinner message may arrive after shutdown unmounted the chat.
        viewport = next(iter(self.query(HistoryViewport)), None)
        if viewport is None or self.implementation.discussion_preview:
            return
        if self.busy and self.running_compaction is not None:
            self.running_compaction.title = self.compaction_title(ActivityToggle.SPINNER_FRAMES[event.index % 4])
        if self.busy:
            footer = viewport.live_footer
            if footer is not None:
                footer.update_elapsed()
            activities = {activity for activity in (self.running_activity, self.current_activity)
                          if activity is not None and activity.toggle.started_at is not None}
            for activity in activities:
                activity.toggle.set_spinner(event.index)
                activity.toggle.update_elapsed()

    def check_action(self, action: str, parameters: tuple[object, ...]) -> bool | None:
        workspace = self.query_one(ImplementationWorkspace) if self.is_mounted else None
        inspecting = workspace is not None and workspace.display and not workspace.revision_page_visible and not isinstance(self.screen, ModalScreen)
        if action == "request_cancel" and isinstance(self.screen, (UpdateScreen, DecisionModelScreen)):
            return False
        if action == "implementation_slash":
            return inspecting and not isinstance(self.focused, MessageInput)
        if action == "implementation_explanation":
            return (inspecting and not self.implementation.discussion_preview and not workspace.chat_open
                    and bool(workspace.zones) and not isinstance(self.focused, MessageInput))
        if action == "implementation_enter":
            return inspecting and not workspace.chat_open and not isinstance(self.focused, (Button, ActivityToggle, CollapsibleTitle))
        if action == "implementation_move":
            if inspecting and isinstance(self.focused, MessageInput) and self.focused.suggestions is not None and self.focused.suggestions.display:
                return False
            return inspecting and (not workspace.chat_open or not parameters[1])
        if action == "implementation_control_move":
            return inspecting
        if action == "scroll_chat_page":
            return not self.editing_external and not isinstance(self.screen, ModalScreen)
        if action == "permission_choice":
            if self.implementation.discussion_preview:
                return False
            card = self.permission_card
            proposal = workspace.proposal_card if workspace is not None else None
            if inspecting and workspace.chat_open and proposal is not None and proposal.display and not proposal.waiting:
                return True
            empty_review = (workspace is not None and workspace.chat_open
                            and isinstance(self.focused, ReviewInput) and self.focused.text == "")
            return (inspecting and (not workspace.chat_open or empty_review)) or (card is not None and card.is_attached and not self.editing_external
                    and not isinstance(self.screen, ModalScreen) and card.screen is self.screen)
        return super().check_action(action, parameters)

    def action_scroll_chat_page(self, direction: int) -> None:
        workspace = self.query_one(ImplementationWorkspace)
        if workspace.revision_page_visible:
            viewport = workspace.query_one("#revision-plan-chat")
            viewport.action_page_up() if direction < 0 else viewport.action_page_down()
            return
        if workspace.display:
            if workspace.chat_open and workspace.query_one("#implementation-chat").display:
                viewport = workspace.query_one("#implementation-review-history")
            elif workspace.plan_visible or workspace.conclusion_visible:
                viewport = workspace.query_one("#implementation-accepted-plan")
            elif workspace.query_one("#implementation-step-view").display:
                viewport = workspace.query_one("#implementation-step-view")
            else:
                viewport = workspace.query_one("#implementation-diff")
        else:
            viewport = self.query_one(HistoryViewport)
        if self.implementation.discussion_preview:
            viewport = self.query_one(DiscussionPreview).query_one(HistoryViewport)
        if direction < 0:
            viewport.action_page_up()
        else:
            viewport.action_page_down()

    def action_permission_choice(self, direction: int) -> None:
        if self.permission_card is not None:
            self.permission_card.move_choice(direction)
        elif self.query_one(ImplementationWorkspace).display:
            workspace = self.query_one(ImplementationWorkspace)
            if workspace.chat_open and workspace.proposal_card is not None and workspace.proposal_card.display:
                workspace.proposal_card.move_choice(direction)
            else:
                workspace.navigate(direction, diffs=True)

    def action_implementation_move(self, direction: int, diffs: bool) -> None:
        self.query_one(ImplementationWorkspace).navigate(direction, diffs=diffs)

    def action_implementation_control_move(self, direction: int, diffs: bool) -> None:
        self.action_implementation_move(direction, diffs)

    def action_implementation_enter(self) -> None:
        self.query_one(ImplementationWorkspace).enter()

    def action_implementation_slash(self) -> None:
        self.query_one(ImplementationWorkspace).open_review_command()

    def action_implementation_explanation(self) -> None:
        self.query_one(ImplementationWorkspace).toggle_diff_explanation()

    def action_tab_key(self, direction: int = 1) -> None:
        if self.implementation.discussion_preview:
            self.screen.focus_next() if direction > 0 else self.screen.focus_previous()
            return
        workspace = self.query_one(ImplementationWorkspace)
        if workspace.display and not isinstance(self.screen, ModalScreen):
            if isinstance(self.focused, MessageInput):
                entry = self.focused
                entry.update_suggestions()
                if entry.suggestions is not None and entry.suggestions.display:
                    entry.suggestions.select_next(direction)
                    return
            if workspace.revision_page_visible:
                self.screen.focus_next() if direction > 0 else self.screen.focus_previous()
                return
            workspace.toggle_chat()
            return
        entry = self.query_one(MessageInput)
        if entry.has_focus:
            # Like Enter, Tab must follow text already queued in the editor.
            entry.call_later(self.action_tab, direction)
        else:
            self.action_tab(direction)

    def action_tab(self, direction: int = 1) -> None:
        entry = self.query_one(MessageInput)
        entry.update_suggestions()
        if entry.has_focus and not entry.disabled and entry.suggestions is not None and entry.suggestions.display:
            entry.suggestions.select_next(direction)
            return
        if direction < 0:
            if self.permission_card is not None and self.permission_card.owns_button_focus():
                self.permission_card.action_previous_choice()
            else:
                self.screen.focus_previous()
            return
        self.action_toggle_mode()

    def action_toggle_mode(self) -> None:
        if isinstance(self.screen, DecisionModelScreen):
            self.screen.focus_next()
            return
        if self.runtime_info.get("implementation_view"):
            return
        if self.question_screen is not None:
            return
        if self.mcps_screen is not None:
            self.mcps_screen.focus_next()
            return
        if self.permissions_screen is not None:
            self.permissions_screen.focus_next()
            return
        if self.model_screen is not None:
            self.model_screen.focus_next()
            return
        if self.connect_screen is not None:
            self.connect_screen.focus_next()
            return
        if self.permission_card is not None and self.permission_card.owns_button_focus():
            self.permission_card.action_next_choice()
            return
        self.set_mode("slop" if self.mode == "sane" else "sane")
        if self.connected:
            self.send_mode(self.mode)

    def set_mode(self, mode: str) -> None:
        self.mode = mode
        for drawer in self.query(ObjectivePlanDrawer):
            drawer.sync()
        self.query_one(ObjectiveWorkspace).sync()
        self.query_one(ImplementationPlanWorkspace).sync()
        indicator = self.query_one("#mode-indicator", Static)
        indicator.styles.background = MODE_COLORS[self.mode]
        workspace = self.query_one(ImplementationWorkspace)
        self.active_info_bar().set_mode("sane" if workspace.display and workspace.chat_open else self.mode)

    @work(group="mode")
    async def send_mode(self, mode: str) -> None:
        self.request_number += 1
        try:
            await self.client.send({"type": "mode.set", "requestId": str(self.request_number), "mode": mode})
        except (OSError, ValueError) as error:
            self.append_chat("Mode update failed", str(error), f"bold {SLOP_COLOR}")

    async def on_permission_card_decided(self, event: PermissionCard.Decided) -> None:
        if self.permission_card is None or self.permission_card.permission_id != event.permission_id:
            return
        self.request_number += 1
        self.permission_reply_id = str(self.request_number)
        try:
            await self.client.send({"type": "permission.reply", "requestId": self.permission_reply_id,
                                    "permissionId": event.permission_id, "decision": event.decision})
        except (OSError, ValueError) as error:
            if self.permission_card is not None:
                self.permission_card.retry()
            self.append_chat("Permission reply failed", str(error), f"bold {SLOP_COLOR}")

    def clear_permission_card(self) -> None:
        if self.permission_card is not None:
            self.permission_card.remove()
            self.permission_card = None
        self.permission_reply_id = None

    def action_clear_or_quit(self) -> None:
        entry = self.query_one("#message", MessageInput)
        if entry.text:
            self.reset_quit_confirmation()
            entry.text = ""
            return
        self.reset_cancel_confirmation()
        now = monotonic()
        if self.quit_deadline is None or now > self.quit_deadline:
            self.reset_quit_confirmation()
            self.quit_deadline = now + self.CANCEL_CONFIRM_SECONDS
            self.active_info_bar().show_confirmation("exit", self.quit_deadline, self.CANCEL_CONFIRM_SECONDS)
            self.quit_timer = self.set_timer(self.CANCEL_CONFIRM_SECONDS, self.reset_quit_confirmation)
            return
        self.reset_quit_confirmation()
        self.exit()

    def reset_quit_confirmation(self) -> None:
        self.quit_deadline = None
        if self.quit_timer is not None:
            self.quit_timer.stop()
            self.quit_timer = None
        self.active_info_bar().hide_confirmation("exit")

    def on_text_area_changed(self, event: MessageInput.Changed) -> None:
        if self.quit_deadline is not None and isinstance(event.text_area, MessageInput) and event.text_area.text:
            self.reset_quit_confirmation()

    async def action_next_reasoning(self) -> None:
        if (not self.connected or self.connect_screen is not None or self.model_screen is not None
                or self.permissions_screen is not None or self.mcps_screen is not None):
            return
        self.request_number += 1
        try:
            await self.client.send({"type": "reasoning.next", "requestId": str(self.request_number)})
        except (OSError, ValueError) as error:
            self.append_chat("Reasoning update failed", str(error), f"bold {SLOP_COLOR}")

    async def action_edit_external(self) -> None:
        entry = self.query_one("#message", MessageInput)
        if self.editing_external or entry.disabled or isinstance(self.screen, ModalScreen):
            return
        self.editing_external = True
        self.recap_idle.pause()
        try:
            editor = os.environ.get("EDITOR", "")
            if not editor.strip():
                raise ValueError("EDITOR is not set")
            command = shlex.split(editor)
            if not command:
                raise ValueError("EDITOR is empty")
            with tempfile.TemporaryDirectory(prefix="sos-editor-") as directory:
                path = Path(directory) / "message.md"
                # Round-trip one file terminator without losing intentional trailing lines.
                original = entry.expanded_text
                original_images = entry.images
                path.write_text(original + "\n", encoding="utf-8")
                launch_error = None
                # Keep I/O alive while suspending repaints until the driver resumes.
                with self.batch_update(), self.suspend():
                    # Leave suspend normally even on launch failure so Textual restores the terminal.
                    try:
                        result = await asyncio.to_thread(subprocess.run, [*command, str(path)], check=False)
                    except OSError as error:
                        launch_error = error
                if launch_error is not None:
                    raise launch_error
                if result.returncode != 0:
                    raise ValueError(f"Editor exited with code {result.returncode}")
                edited = path.read_text(encoding="utf-8").removesuffix("\n")
                if edited != original:
                    entry.text = ""
                    entry.restore_images(edited, original_images)
                entry.move_cursor(entry.document.end)
        except (OSError, ValueError, SuspendNotSupported) as error:
            self.append_chat("Editor failed", str(error), f"bold {SLOP_COLOR}")
        finally:
            self.finish_external_edit(entry)

    def finish_external_edit(self, entry: MessageInput) -> None:
        self.editing_external = False
        self.recap_idle.pause()
        if self.editor_restored_text:
            merged = self.editor_restored_text + (self.editor_restore_separator + entry.expanded_text if entry.text else "")
            images = self.editor_restored_images + entry.images
            entry.text = ""
            entry.restore_images(merged, images, pasted=self.editor_restored_pasted)
            self.editor_restored_images = ()
            self.editor_restored_text = ""
            self.editor_restored_pasted = False
            self.editor_restore_separator = "\n\n"
            entry.move_cursor(entry.document.end)
        if self.permission_card is not None and not self.permission_card.waiting:
            focused = self.screen.focused
            if isinstance(focused, Button) and focused in self.permission_card.query(Button):
                self.permission_card.selected_button = focused.id
            self.call_after_refresh(self.permission_card.reveal_choice)
        else:
            self.focus_composer()

    def reset_cancel_confirmation(self) -> None:
        self.cancel_deadline = None
        if self.cancel_timer is not None:
            self.cancel_timer.stop()
            self.cancel_timer = None
        self.active_info_bar().hide_confirmation("cancel")

    async def action_request_cancel(self) -> None:
        if isinstance(self.screen, WorktreeDeleteScreen):
            self.screen.action_back()
            return
        if self.worktree_screen is not None:
            self.worktree_screen.action_back()
            return
        if not isinstance(self.screen, ModalScreen) and isinstance(self.focused, MessageInput):
            entry = self.focused
            if entry.suggestions is not None and entry.suggestions.display:
                entry.suggestions.dismiss(entry.text, cursor=entry.document.get_index_from_location(entry.cursor_location))
                return
        workspace = self.query_one(ImplementationWorkspace)
        if workspace.permission_popup:
            workspace.permission_popup.dismiss(None)
            return
        if isinstance(self.screen, QuestionScreen):
            self.screen.dismiss(None)
            return
        if workspace.display and not isinstance(self.screen, ModalScreen) and not workspace.archive:
            await workspace.escape()
            return
        if self.runtime_info.get("implementation_archive") and not isinstance(self.screen, ModalScreen):
            await self.implementation.request("implementation.view")
            return
        if self.quit_deadline is not None and monotonic() <= self.quit_deadline:
            return  # Escape must not dismiss or replace an active Ctrl+C confirmation.
        self.reset_quit_confirmation()
        if self.question_screen is not None:
            self.question_screen.dismiss(None)
            return
        if self.mcps_screen is not None:
            self.mcps_screen.action_close()
            return
        if self.permissions_screen is not None:
            self.permissions_screen.action_close()
            return
        if self.model_screen is not None:
            self.model_screen.action_close()
            return
        if self.connect_screen is not None:
            self.connect_screen.action_close()
            return
        if self.resume_screen is not None:
            if not self.resume_screen.attaching:
                self.resume_screen.dismiss()
                self.resume_screen = None
                self.query_one(MessageInput).focus()
            return
        if self.queued_message is not None:
            self.restore_queued_message()
            self.reset_cancel_confirmation()
            self.query_one(MessageInput).focus(scroll_visible=False)
            return
        await self.request_execution_cancel()

    async def request_execution_cancel(self, *, confirmed: bool = False) -> None:
        if not self.busy or (not self.connected and self.startup_message is None) or self.cancel_request_id is not None:
            return
        now = monotonic()
        if not confirmed and (self.cancel_deadline is None or now > self.cancel_deadline):
            self.reset_cancel_confirmation()
            self.cancel_deadline = now + self.CANCEL_CONFIRM_SECONDS
            self.active_info_bar().show_confirmation("cancel", self.cancel_deadline, self.CANCEL_CONFIRM_SECONDS)
            self.cancel_timer = self.set_timer(self.CANCEL_CONFIRM_SECONDS, self.reset_cancel_confirmation)
            return
        if self.cancel_timer is not None:
            self.cancel_timer.stop()
            self.cancel_timer = None
        self.cancel_deadline = None
        if self.startup_message is not None:
            self.startup_message = None
            self.busy = False
            self.restore_queued_message()
            self.restore_pending_text()
            self.pending_request = None
            self.reset_cancel_confirmation()
            self.set_status("Ready")
            return
        self.request_number += 1
        self.cancel_request_id = str(self.request_number)
        if not confirmed:
            self.active_info_bar().show_confirmation("cancel", None, self.CANCEL_CONFIRM_SECONDS)
        self.set_status("Cancelling…")
        try:
            await self.client.send({"type": "execution.cancel", "requestId": self.cancel_request_id})
        except (OSError, ValueError) as error:
            self.cancel_request_id = None
            self.reset_cancel_confirmation()
            self.append_chat("Cancel failed", str(error), f"bold {SLOP_COLOR}")
            self.set_status("Working…")

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        if isinstance(event.button, (ModeButton, ConfirmationBar)):
            event.stop()
            if self.implementation.discussion_preview:
                return
            self.reset_quit_confirmation()
            if self.busy or isinstance(event.button, ConfirmationBar):
                await self.request_execution_cancel()
            elif (self.connected and not self.history_loads and not self.history_rendering
                  and not self.cleaning_history and self.new_request is None and self.pending_request is None
                  and self.queued_message is None and self.recap_request is None
                  and self.turn_navigation.request_id is None and not isinstance(self.screen, ModalScreen)):
                if self.runtime_info.get("implementation_view"):
                    return  # Planning and acceptance actions live below the plan.

    def question_answered(self, choice: int | None) -> None:
        screen = self.question_screen
        self.question_screen = None
        if screen is not None and screen.session_id == self.session_id:
            self.send_question_answer(screen.question_id, choice)

    @work(group="question")
    async def send_question_answer(self, question_id: str, choice: int | None) -> None:
        self.request_number += 1
        request: dict[str, Any] = {"type": "question.reject" if choice is None else "question.reply",
                                   "questionId": question_id, "requestId": str(self.request_number)}
        if choice is not None:
            request["choice"] = choice
        try:
            await self.client.send(request)
        except (OSError, ValueError) as error:
            self.append_chat("Question reply failed", str(error), f"bold {SLOP_COLOR}")

    def active_info_bar(self) -> AgentInfoBar:
        workspace = self.query_one(ImplementationWorkspace)
        if workspace.display and workspace.revision_page_visible:
            return workspace.query_one("#revision-plan-agent-info", AgentInfoBar)
        if workspace.display or self.implementation.discussion_preview:
            return workspace.query_one("#implementation-review-agent-info" if workspace.chat_open else "#implementation-agent-info", AgentInfoBar)
        return self.query_one("#agent-info", AgentInfoBar)

    def update_agent_info(self, info: dict[str, Any]) -> None:
        if all(key in self.runtime_info and self.runtime_info[key] == value for key, value in info.items()):
            return
        errors = info.get("skill_errors", [])
        if errors and errors != self.runtime_info.get("skill_errors"):
            self.call_later(self.show_input_error, "Skills:\n" + "\n".join(errors), warning=True)
        implementation_changed = any(key in info and self.runtime_info.get(key) != info[key] for key in (
            "discussion_id", "implementation_view", "implementation_archive", "active_implementation_id",
            "implementation_branches", "objective_plan_ready", "implementation_plan_ready", "implementation_execution",
            "auto_plan_pending", "auto_plan_running", "auto_plan_error"))
        skills_changed = "skills" in info and self.runtime_info.get("skills") != info["skills"]
        if skills_changed:
            collisions = sorted(f"/{skill['name']}" for skill in info["skills"] if f"/{skill['name']}" in COMMANDS)
            if collisions:
                self.call_later(self.show_input_error,
                                "Skills unavailable: native command names are reserved: " + ", ".join(collisions))
        self.runtime_info.update(info)
        if skills_changed:
            for entry in self.query(MessageInput):
                entry.refresh_skill_references()
                entry.update_suggestions()
        if info.get("workspace") and self.workspace != info["workspace"]:
            self.workspace = info["workspace"]
            self.query_one("#workspace", Static).update(Text(self.workspace))
        self.implementation.sync_history_plans()
        self.query_one(ObjectiveWorkspace).sync()
        self.query_one(ImplementationPlanWorkspace).sync()
        self.active_info_bar().query_one(ModeButton).set_plan_ready(bool(self.runtime_info.get("objective_plan_ready")))
        if implementation_changed:
            self.sync_implementation()
        footer = self.query_one(HistoryViewport).live_footer
        if self.busy and footer is not None and info.get("execution_model") is not None:
            footer.set_generation(self.generation_info())
        if self.running_activity is not None:
            self.running_activity.toggle.update_details(self.generation_info())
        for bar in self.query(AgentInfoBar):
            bar.update_info(info)
            bar.set_mode("sane" if bar.id in ("implementation-review-agent-info", "revision-plan-agent-info") else self.mode)
        self.query_one(ImplementationWorkspace).update_review_button()

    @work(exclusive=True, group="implementation-tabs")
    async def sync_implementation(self) -> None:
        await self.implementation.sync()

    async def on_tabs_tab_activated(self, event: Tabs.TabActivated) -> None:
        info = self.runtime_info
        desired = self.implementation.desired_tab()
        if event.tab.id == desired or event.tabs.id != "tabs":
            return
        await self.implementation.select_tab(event.tab.id)

    async def on_objective_plan_open_implementation(self, event: ObjectivePlan.OpenImplementation) -> None:
        await self.implementation.request("implementation.view", event.branch_id, generate=True)

    async def on_implementation_plan_open_construction(self, event: ImplementationPlan.OpenConstruction) -> None:
        await self.implementation.select_tab("implement")
        await self.implementation.show_plan(False)

    @work(exclusive=True, group="implementation-restore")
    async def restore_implementation(self, event: dict[str, Any]) -> None:
        generate = self.implementation.generate_after_restore
        auto_plan_pace = self.implementation.auto_plan_pace
        try:
            await self.render_restored_session(event)
            text, images = self.implementation.drafts.get(self.session_id, ("", ()))
            self.query_one(MessageInput).restore_images(text, images)
            await self.implementation.sync()
        finally:
            self.implementation.finish()
        if (auto_plan_pace and self.connected and not self.runtime_info.get("implementation_archive")
                and not self.runtime_info.get("implementation_execution")):
            self.runtime_info.update(auto_plan_pending=True, auto_plan_running=True, auto_plan_pace=auto_plan_pace, auto_plan_error="")
            await self.implementation.sync()
            self.query_one(ImplementationWorkspace).command("implementation.auto_plan", pace=auto_plan_pace)
        elif (generate and self.connected and not self.runtime_info.get("implementation_archive")
                and not self.runtime_info.get("implementation_plan_ready")):
            await self.send_user_message("Implementation plan request", implementation_request=True)

    def generation_info(self) -> dict[str, Any]:
        info = dict(self.runtime_info)
        if info.get("execution_model") is not None:
            info["model"] = info["execution_model"]
        if info.get("execution_reasoning") is not None:
            info["reasoning"] = info["execution_reasoning"]
        return info

    def set_status(self, status: str) -> None:
        entry = self.query_one("#message", MessageInput)
        if self.implementation.discussion_preview:
            entry.disabled = True
            return
        entry.placeholder = ("Write your next message…" if self.busy else
                             "Write a message and press Enter" if status in ("Ready", "Connected") else status)

    def restore_pending_text(self) -> None:
        if self.pending_text is not None:
            self.discard_pending_turn()
            self.prepend_draft(self.pending_text, pasted=not self.pending_interpret_commands)
            if self.pending_images:
                if self.editing_external:
                    self.editor_restored_images = self.pending_images + self.editor_restored_images
                else:
                    entry = self.query_one(MessageInput)
                    # Replace only the prefix just restored, preserving new draft atoms.
                    end = entry.document.get_location_from_index(len(self.pending_text))
                    entry.delete((0, 0), end)
                    entry.restore_images(self.pending_text, self.pending_images, pasted=not self.pending_interpret_commands)
            self.pending_images = ()
            self.pending_text = None
        elif self.pending_sane_request and self.pending_turn is not None:
            self.discard_pending_turn()
        self.pending_sane_request = False
        self.pending_turn = None

    def prepend_draft(self, text: str, separator: str = "\n\n", *, pasted: bool = False) -> None:
        if self.editing_external:
            self.editor_restored_pasted = pasted
            # The editor owns the current draft; merge recovered text on return.
            if self.editor_restored_text:
                self.editor_restored_text = text + separator + self.editor_restored_text
            else:
                self.editor_restored_text = text
                self.editor_restore_separator = separator
        else:
            entry = self.query_one(MessageInput)
            entry.restore_images(text + (separator if entry.text else ""), (), pasted=pasted)

    def queue_message(self, text: str, *, interpret_commands: bool = True) -> None:
        images = self.query_one(MessageInput).images
        try:
            check_budget((self.queued_message.images if self.queued_message else ()) + images)
        except ValueError as error:
            self.call_later(self.show_input_error, str(error))
            return
        self.recap_idle.activity()
        viewport = self.query_one(HistoryViewport)
        viewport.prepare_live_update()
        if self.queued_message is None:
            self.queued_message = QueuedMessage(text, images, interpret_commands=interpret_commands)
            self.queued_message.display = self.history_window.latest
            mounted = viewport.mount(self.queued_message)
            viewport.follow_response(self.queued_message, mounted)
        else:
            self.queued_message.append(text)
            self.queued_message.images += images
            viewport.call_after_refresh(viewport.finish_mount_scroll, self.queued_message, viewport.scroll_input_number)
        self.query_one(MessageInput).text = ""
        self.reset_cancel_confirmation()

    def take_queued_message(self) -> str | None:
        queued, self.queued_message = self.queued_message, None
        self.queued_ready = False
        if queued is None:
            return None
        queued.display = False
        queued.remove()
        return queued.text

    def restore_queued_message(self) -> None:
        images = self.queued_message.images if self.queued_message else ()
        pasted = self.queued_message is not None and not self.queued_message.interpret_commands
        text = self.take_queued_message()
        if text is not None:
            if self.editing_external:
                self.prepend_draft(text, "\n", pasted=pasted)
                self.editor_restored_images = images + self.editor_restored_images
            else:
                entry = self.query_one(MessageInput)
                entry.restore_images(text + ("\n" if entry.text else ""), images, pasted=pasted)
            if not self.editing_external:
                entry = self.query_one(MessageInput)
                entry.move_cursor(entry.document.end)

    def dispatch_queued_message(self) -> None:
        if (not self.queued_ready or self.queued_message is None or not self.connected or self.busy
                or self.cleaning_history or self.history_rendering or self.history_loads
                or self.new_request is not None or self.switch_request is not None):
            return
        images = self.queued_message.images
        interpret_commands = self.queued_message.interpret_commands
        text = self.take_queued_message()
        self.busy = True  # Reserve the next turn before the worker can yield.
        self.send_queued_message(text, images, interpret_commands=interpret_commands)

    @work(group="queued-send")
    async def send_queued_message(self, text: str, images=(), *, interpret_commands: bool = True) -> None:
        await self.send_user_message(text, images, interpret_commands=interpret_commands)

    def mount_user_turn(self, text: str, mode: str | None = None) -> UserTurnView:
        self.activity_after_text = False
        user = UserMessage(text, mode if mode is not None else self.submitted_mode)
        children = self.query_one("#chat").children
        previous = children[-1] if children else None
        has_footer = isinstance(previous, ResponseMarkdown) and previous.generation is not None
        user.set_class(bool(children) and not has_footer, "next-turn")
        self.current_activity = HistoryActivity.live()
        self.activate_activity()
        chat = self.query_one(HistoryViewport)
        mounted = chat.mount_live(user, self.current_activity)
        if self.busy:
            chat.start_generation_footer(self.generation_info(),
                                         self.request_started_at if self.request_started_at is not None else monotonic())
        chat.show_user_turn(user, mounted)
        return UserTurnView(user, self.current_activity)

    def confirm_pending_turn(self, text: str) -> bool:
        if self.pending_turn is None or self.pending_turn.user.text != text:
            return False
        self.pending_turn.user.remove_class("pending-turn")
        self.pending_turn = None
        return True

    def discard_pending_turn(self) -> None:
        turn = self.pending_turn
        if turn is None:
            return
        if self.running_activity is turn.activity:
            self.running_activity = None
        if self.current_activity is turn.activity:
            self.current_activity = None
        turn.user.remove()
        turn.activity.remove()
        self.query_one(HistoryViewport).clear_generation_footer()
        self.pending_turn = None

    def append_chat(self, label: str, text: str, style: str = "bold cyan", *, classes: str = "") -> None:
        content = Text()
        if label:
            content.append(label + ("\n" if text else ""), style=style)
        content.append(text)
        self.query_one(HistoryViewport).mount_live(Static(content, classes=f"chat-message {classes}"))

    def clear_retry_notice(self, *, keep_status: bool = False) -> None:
        if not keep_status:
            self.retry_status = None
        if self.retry_notice is not None:
            self.retry_notice.remove()
            self.retry_notice = None

    def show_generation_retry(self, retry: dict[str, Any]) -> None:
        if not retry["attempt"]:
            self.clear_retry_notice()
            return
        if not self.busy:
            return
        self.retry_status = retry
        if self.history_rendering or not self.history_window.latest:
            self.clear_retry_notice(keep_status=True)
            return
        seconds = retry["seconds"]
        state = f"Retrying in {seconds}s" if seconds else "Reconnecting…"
        cause = retry.get("error") or retry["reason"]
        text = Text(f'{cause}\nRetry {retry["attempt"]}/{retry["limit"]} · {state}',
                    style=f"bold {SLOP_COLOR}")
        if self.retry_notice is None:
            self.retry_notice = Static(text, classes="chat-message generation-retry")
            activity = self.running_activity or self.ensure_activity()
            self.query_one(HistoryViewport).mount(self.retry_notice, before=activity)
        else:
            self.retry_notice.update(text)

    def display_message(self, message: dict[str, Any]) -> None:
        if message.get("recap"):
            text = "\n".join(part["text"] for part in message["content"] if part["type"] == "text")
            chat = self.query_one(HistoryViewport)
            chat.prepare_live_update()
            card, self.running_recap = self.running_recap, None
            if card is not None and card.is_mounted:
                card.finish(text)
                if chat.follow_input_number == chat.scroll_input_number:
                    chat.call_after_refresh(chat.finish_mount_scroll, card, chat.scroll_input_number)
            else:
                card = ConversationRecap(text)
                mounted = chat.mount(card, before=self.queued_message)
                chat.follow_response(card, mounted)
            chat.register_message(message, card)
            return
        if message.get("compaction") is not None:
            text = "\n".join(part["text"] for part in message["content"] if part["type"] == "text")
            chat = self.query_one(HistoryViewport)
            if self.running_compaction is not None:
                drawer = self.running_compaction
                drawer.title = "Compaction"
                self.call_after_refresh(lambda: drawer.query_one(Collapsible.Contents).mount(ResponseMarkdown(text)))
                self.running_compaction = None
            else:
                chat.mount_live(ChatCollapsible(ResponseMarkdown(text), title="Compaction",
                                                classes="compaction chat-message" + (" sane-compaction" if message.get("compaction", {}).get("mode") == "sane" else ""), collapsed=True,
                                                collapsed_symbol="▸", expanded_symbol="▾"))
            self.current_activity = None
            self.activity_after_text = False
            return
        role = message["role"]
        if role == "system":
            return
        self.query_one(HistoryViewport).prepare_live_update()
        self.activity_source = (message.get("id", ""), 0)
        if role == "assistant" and not message.get("user_skill") and (self.current_activity is not None or message.get("model_call")):
            (self.current_activity or self.ensure_activity()).toggle.record_model_call(message.get("model_call"))
        if role == "assistant" and message.get("metadata") is not None:
            (self.current_activity or self.ensure_activity()).toggle.finish_generation(message["metadata"])
        if role == "user":
            text = "\n".join(content["text"] for content in message["content"] if content["type"] == "text")
            turn = self.pending_turn
            if not self.confirm_pending_turn(text):
                turn = self.mount_user_turn(text, (message.get("metadata") or {}).get("mode"))
            turn.user.set_images(message.get("images", ()))
            metadata = message.get("metadata") or {}
            turn.user.set_class(bool(metadata.get("sane_request") or metadata.get("implementation_request")), "sane-request")
            if isinstance(turn.activity, HistoryActivity) and message.get("id"):
                turn.activity.group_id = f"{message['id']}/activity/0"
            self.query_one(HistoryViewport).register_message(message, turn.user)
            self.query_one(HistoryViewport).register_activity(turn.activity)
            return
        for part_index, content in enumerate(message["content"]):
            self.activity_source = (message.get("id", ""), part_index)
            kind = content["type"]
            if kind == "reasoning":
                if content.get("text"):
                    self.mount_activity(ChatCollapsible(ResponseMarkdown(content["text"]), title="Reasoning",
                                                        classes="reasoning", collapsed=True, collapsed_symbol="▸",
                                                        expanded_symbol="▾"), kind="reasoning")
            elif kind == "text":
                if role == "assistant":
                    if not content["text"].strip():
                        continue
                    chat = self.query_one(HistoryViewport)
                    if self.current_activity is None:
                        self.ensure_activity()
                    last_text = not any(part["type"] == "text" and part["text"].strip()
                                        for part in message["content"][part_index + 1:])
                    if last_text and message.get("metadata") is not None:
                        chat.clear_generation_footer()
                    response = (ImplementationPlan(content["text"], message_id=message.get("id", "")) if message.get("implementation_plan") else
                                ObjectivePlan(content["text"], message.get("objective_stack", ""), message.get("id", ""))
                                if message.get("objective_plan") else
                                ResponseMarkdown(content["text"], classes="chat-message",
                                                 generation=message.get("metadata") if last_text else None))
                    mounted = chat.mount_live(response)
                    chat.follow_response(response, mounted)
                    chat.register_message(message, response, [content])
                    self.activity_after_text = True
                else:
                    self.append_chat("", content["text"])
            elif kind == "tool_call":
                view = (ReadView(content["callId"], content["arguments"]) if content["toolName"] == "read"
                        else ToolView(content["callId"], content["toolName"], content["arguments"],
                                      skill_script=content.get("skill_script"), skill_script_path=content.get("skill_script_path"),
                                      user_skill=message.get("user_skill")))
                self.pending_tools[content["callId"]] = view
                self.mount_activity(view, kind="read" if isinstance(view, ReadView) else view.activity_kind)
            elif kind == "tool_result":
                view = self.pending_tools.pop(content["callId"], None)
                if view is None:
                    view = ToolView(content["callId"], "tool", {})
                    self.mount_activity(view)
                view.set_result(content)
                item = {"toolName": view.tool if isinstance(view, ToolView) else "read", "callId": content["callId"],
                        "arguments": view.arguments, "result": content}
                if was_ai_refused(item):
                    drawer = next((parent for parent in view.ancestors if isinstance(parent, TurnActivity)), self.current_activity)
                    if drawer is not None:
                        drawer.add_refusal(ManualApprovalView(item))

    def mount_activity(self, widget: Widget, kind: str = "tools") -> None:
        self.ensure_activity().add_activity(widget, kind)

    def ensure_activity(self) -> TurnActivity:
        if self.current_activity is None or self.activity_after_text:
            if self.current_activity is not None:
                self.current_activity.toggle.is_terminal = False
                self.current_activity.toggle.refresh()
            if self.running_activity is not None:
                self.running_activity.toggle.finish_generation()
                self.running_activity.toggle.set_spinner(None)
            message_id, part_index = self.activity_source
            follows_activity = self.current_activity is not None
            self.current_activity = HistoryActivity.live(f"{message_id}/activity/{part_index}" if message_id else "")
            self.current_activity.activity_separator.display = follows_activity
            self.activate_activity(monotonic() if self.activity_after_text else None)
            self.activity_after_text = False
            self.query_one(HistoryViewport).mount_live(self.current_activity)
            self.query_one(HistoryViewport).register_activity(self.current_activity)
        return self.current_activity

    def activate_activity(self, started_at: float | None = None) -> None:
        if self.busy and self.current_activity is not None:
            self.running_activity = self.current_activity
            self.running_activity.toggle.start_generation(self.generation_info(),
                                                         started_at if started_at is not None else self.request_started_at or monotonic())
            self.running_activity.toggle.set_spinner(self.active_info_bar().query_one(ModeButton).spinner_frame)

    def handle_event(self, event: dict[str, Any]) -> None:
        self.update_agent_info(event.get("info", {}))
        if self.query_one(ImplementationWorkspace).receive(event):
            return
        if self.btw.receive(event):
            return
        self.update_agent_info(event.get("info", {}))
        if self.implementation.receive(event):
            return
        if self.query_one(DiscussionPreview).receive(event):
            return
        if self.turn_navigation.receive(event):
            return
        if self.history_window.accept(event):
            if (event["type"] == "session.attach_started" and event.get("purpose") == "page"
                    and event.get("requestId") in self.history_window.pending):
                self.history_live.clear()  # These events precede the daemon's page snapshot.
            return
        kind = event["type"]
        if kind == "question.requested":
            if self.question_screen is None and event["sessionId"] == self.session_id:
                self.question_screen = QuestionScreen(event)
                self.push_screen(self.question_screen, self.question_answered)
            return
        if kind == "question.answered":
            return
        if kind == "mcp.restored":
            if self.mcps_screen is not None and not self.mcps_screen.finished:
                self.mcps_screen.request({"type": "mcp.list"})
            if event.get("errors"):
                self.call_later(self.show_input_error, "MCP connection failed:\n" + "\n".join(event["errors"]))
            return
        if kind == "request.failed" and event.get("requestId") == self.switch_request:
            self.switch_request = None
        if self.recap_request is not None and event.get("requestId") == self.recap_request:
            if kind == "recap.started":
                return
            if kind in ("recap.completed", "recap.cancelled", "request.failed"):
                self.busy = False
                self.recap_request = None
                if kind == "recap.completed":
                    self.display_live_messages(event["messages"])
                else:
                    self.restore_queued_message()
                    if kind == "recap.cancelled":
                        self.append_chat("Recap cancelled", event.get("error", ""), f"bold {SLOP_COLOR}")
                self.clear_running_recap()
                self.set_status("Ready")
                if not self.recap_automatic:
                    self.query_one(MessageInput).focus()
                if kind == "recap.completed":
                    self.queued_ready = self.queued_message is not None
                    self.dispatch_queued_message()
                return
        if kind == "compaction.progress":
            if self.running_compaction is not None:
                self.compaction_attempt = event["attempt"]
                self.compaction_attempt_limit = event["limit"]
                self.running_compaction.title = self.compaction_title("-")
            return
        if event.get("automatic") and kind.startswith("compaction."):
            if kind == "compaction.started":
                if self.running_activity is not None:
                    self.running_activity.toggle.finish_generation()
                    self.running_activity.toggle.set_spinner(None)
                    self.running_activity = None
                self.show_running_compaction()
                self.set_status("Compacting…")
            elif kind == "compaction.completed":
                self.display_live_messages(event["messages"])
                if (not self.runtime_info.get("implementation_execution") and not self.history_rendering
                        and self.history_window.latest):
                    self.activity_source = (event["messages"][0].get("id", ""), 0)
                    self.ensure_activity()
                    self.activate_activity(monotonic())
                self.set_status("Working…")
            else:
                self.clear_running_compaction()
                # The following terminal execution event reports the failure/cancellation.
            return
        if self.compaction_request is not None and event.get("requestId") == self.compaction_request:
            if kind == "compaction.started":
                self.set_status("Compacting…")
                return
            if kind in ("compaction.completed", "compaction.cancelled", "request.failed"):
                self.compaction_request = None
                self.busy = False
                if kind == "compaction.completed":
                    self.display_live_messages(event["messages"])
                else:
                    self.restore_queued_message()
                    self.clear_running_compaction()
                    self.append_chat("Compaction cancelled" if kind == "compaction.cancelled" else "Compaction failed",
                                     event.get("error", ""), f"bold {SLOP_COLOR}")
                self.set_status("Ready")
                self.query_one(MessageInput).focus()
                if kind == "compaction.completed":
                    self.queued_ready = self.queued_message is not None
                    self.dispatch_queued_message()
                return
        worktree_screen = self.worktree_requests.pop(event.get("requestId", ""), None)
        if worktree_screen is not None:
            if kind == "worktree.switched":
                self.session_id = event.get("sessionId", self.session_id)
                self.handle_event({"type": "session.updated", "info": event["info"]})
            if self.worktree_screen is worktree_screen and not worktree_screen.finished:
                worktree_screen.receive(event)
            return
        mcps_screen = self.mcps_requests.pop(event.get("requestId", ""), None)
        if mcps_screen is not None:
            if self.mcps_screen is mcps_screen and not mcps_screen.finished:
                mcps_screen.receive(event)
            return
        connect_screen = self.connect_requests.pop(event.get("requestId", ""), None)
        if connect_screen is not None:
            if self.connect_screen is connect_screen and not connect_screen.finished:
                if kind == "provider.selected":
                    self.finish_connection(event)
                else:
                    connect_screen.receive(event)
            return
        manual = self.manual_requests.pop(event.get("requestId", ""), None)
        if manual is not None:
            session_id, view = manual
            if session_id == self.session_id:
                if kind == "permission.manual_updated":
                    states = event["states"]
                    for row in self.query_one(HistoryViewport).query(ManualApprovalView):
                        if row.refused_id in states:
                            row.set_approved(states[row.refused_id])
                    for record in self.history_window.records:
                        for item in record.get("activities", []):
                            metadata = item.get("result", {}).get("metadata", {})
                            if metadata.get("refused_message_id") in states:
                                metadata["manually_approved"] = states[metadata["refused_message_id"]]
                elif view.is_attached:
                    view.fail(event.get("error", "Could not save manual approval"))
            return
        permissions_screen = self.permissions_requests.pop(event.get("requestId", ""), None)
        if permissions_screen is not None:
            if self.permissions_screen is permissions_screen and not permissions_screen.finished:
                if kind == "permissions.selected" and event["mode"] != "auto":
                    self.close_permissions_screen(permissions_screen)
                else:
                    permissions_screen.receive(event)
            return
        model_screen = self.model_requests.pop(event.get("requestId", ""), None)
        if model_screen is not None:
            if self.model_screen is model_screen and not model_screen.finished:
                if kind == "model.selected":
                    self.close_model_screen(model_screen)
                else:
                    model_screen.receive(event)
            return
        if kind == "permission.requested":
            self.clear_permission_card()
            self.permission_card = PermissionCard(event)
            self.query_one(HistoryViewport).mount_live(self.permission_card)
            return
        if kind in ("permission.resolved", "permission.dismissed"):
            if kind == "permission.resolved" and event["callId"] in self.pending_tools:
                self.pending_tools[event["callId"]].record_permission(event)
            if self.permission_card is not None and self.permission_card.permission_id == event["permissionId"]:
                self.clear_permission_card()
            return
        if kind == "session.reset" and event.get("requestId") == self.new_request:
            self.reset_session(event)
            return
        resume_request = self.resume_requests.get(event.get("requestId", ""))
        if resume_request is not None:
            request_type, session_id, screen = resume_request
            if kind not in ("session.attach_started", "session.history_chunk"):
                self.resume_requests.pop(event.get("requestId", ""), None)
            if self.resume_screen is not screen:
                return
            if kind == "session.listed":
                self.resume_screen.set_sessions(event["sessions"])
            elif kind == "session.previewed":
                if self.resume_screen in self.screen_stack:
                    self.resume_screen.set_preview(event["sessionId"], event["text"])
            elif kind == "session.attach_started":
                if self.resume_transfer is not None or (event["sessionId"] != session_id and
                        event.get("session", {}).get("discussion_id") != session_id):
                    raise ValueError("Unexpected session history transfer")
                self.resume_transfer = HistoryTransfer(event)
                if screen is not None:
                    screen.set_status(f"Receiving history… 0/{self.resume_transfer.count}")
            elif kind in ("session.history_chunk", "session.attached"):
                if self.resume_transfer is None:
                    raise ValueError("Session history transfer has not started")
                restored = self.resume_transfer.accept(event)
                if screen is not None:
                    screen.set_status(f"Receiving history… {len(self.resume_transfer.messages)}/{self.resume_transfer.count}")
                if restored is not None:
                    self.resume_transfer = None
                    if screen is None:
                        restored["automatic_reconnect"] = True
                    self.restore_session(restored)
            elif kind == "request.failed":
                if request_type == "session.preview":
                    self.resume_screen.set_preview(session_id, event["error"])
                else:
                    self.resume_transfer = None
                    if screen is not None:
                        screen.attach_failed(event["error"])
                    else:
                        self.query_one("#message", MessageInput).disabled = True
                        self.notify(event["error"], title="Could not restore session", severity="error", markup=False)
            return
        entry = self.query_one("#message", MessageInput)
        if kind == "execution.preparing":
            self.set_status("Preparing conversation…")
            entry.placeholder = "Preparing conversation…"
        elif kind in ("session.created", "message.accepted"):
            self.session_id = event["session"]["id"]
            self.history_window.session_id = self.session_id
            title = " ".join(event["session"]["title"].split())
            self.query_one("#session-title", Static).update(Text(title))
            if event.get("requestId") == self.pending_request:
                self.pending_request = None
                self.pending_text = None
                self.pending_sane_request = False
                self.pending_images = ()
            self.set_status("Working…")
        elif kind in ("harness.message_arrived", "provider.message_arrived"):
            self.display_live_messages(event["messages"])
        elif kind == "execution.retry":
            self.show_generation_retry(event["retry"])
        elif kind in ("execution.completed", "execution.failed", "execution.cancelled"):
            if self.question_screen is not None:
                screen = self.question_screen
                self.question_screen = None
                screen.dismiss(None)
            if event.get("messages"):
                self.display_live_messages(event["messages"])
            self.clear_running_compaction()
            self.clear_permission_card()
            if self.running_activity is not None and event.get("metadata") is not None:
                self.running_activity.toggle.finish_generation(event["metadata"])
            footer = self.query_one(HistoryViewport).live_footer
            if footer is not None and event.get("metadata") is not None:
                footer.finish_generation(event["metadata"])
            self.busy = False
            if kind != "execution.completed":
                self.restore_queued_message()
            self.restore_pending_text()
            self.pending_request = None
            self.finish_pending_tools()
            if kind == "execution.cancelled":
                self.append_chat("Execution cancelled", "", f"bold {SLOP_COLOR}")
            elif kind == "execution.failed":
                if event.get("code") == "retries_exhausted":
                    self.append_chat(event["error"], "", f"bold {SLOP_COLOR}", classes="generation-error")
                elif event.get("code") == "image_input":
                    self.call_later(self.show_input_error, event.get("error") or kind)
                else:
                    self.append_chat("Execution error", event.get("error") or kind, f"bold {SLOP_COLOR}",
                                     classes="generation-error")
            entry.disabled = not self.connected
            if self.connected:
                entry.focus()
            self.set_status("Ready")
            if kind == "execution.completed":
                self.queued_ready = self.queued_message is not None
                self.dispatch_queued_message()
        elif kind == "request.failed":
            if event.get("requestId") == self.permission_reply_id and self.permission_card is not None:
                self.permission_card.retry()
                self.permission_reply_id = None
            if event.get("code") == "image_input":
                self.call_later(self.show_input_error, event["error"])
            else:
                self.append_chat("Request failed", event["error"], f"bold {SLOP_COLOR}")
            if event.get("requestId") == self.new_request:
                self.new_request = None
                entry.disabled = not self.connected
                entry.focus()
                self.set_status("Ready")
            if event.get("requestId") == self.cancel_request_id:
                self.cancel_request_id = None
                self.reset_cancel_confirmation()
                self.set_status("Working…" if self.busy else "Ready")
            if event.get("requestId") == self.pending_request:
                self.restore_queued_message()
                self.restore_pending_text()
                self.pending_request = None
                self.busy = False
                entry.disabled = not self.connected
                entry.focus()
                self.set_status("Ready")
        elif kind == "execution.cancel_ignored" and event.get("requestId") == self.cancel_request_id:
            self.cancel_request_id = None
            self.reset_cancel_confirmation()
            self.set_status("Working…" if self.busy else "Ready")

    @work(exclusive=True)
    async def listen(self) -> None:
        while True:
            retry = await self.listen_once()
            if not retry:
                return
            self.set_status("Reconnecting to daemon…")
            await asyncio.sleep(1)

    async def listen_once(self) -> bool:
        try:
            await self.client.connect()
            configuration = {"type": "integration.configure", "requestId": "configure",
                             "workspace": self.launch_workspace, "mode": self.mode}
            if self.terminal_identity is not None:
                configuration["terminalSessionId"] = self.terminal_identity.key
            await self.client.send(configuration)
            configured = await self.client.receive()
            if configured["type"] != "integration.configured":
                raise ValueError(configured.get("error", "Could not configure integration permissions"))
            self.update_agent_info(self.client.info)
            self.update_agent_info(configured.get("info", {}))
            self.connected = True
            self.starting = False
            self.set_status("Connected")
            entry = self.query_one("#message", MessageInput)
            entry.disabled = False
            entry.focus()
            if self.session_id:
                await self.send_resume_request("session.attach", self.session_id)
                entry.disabled = True
            request, self.startup_message = self.startup_message, None
            if request is not None:
                await self.dispatch_user_message(request, self.pending_images)
            while True:
                self.handle_event(await self.client.receive())
        except asyncio.CancelledError:
            raise
        except (OSError, ValueError, KeyError, TypeError) as error:
            self.query_one(ImplementationWorkspace).show_loading(False)
            self.history_window.invalidate()
            self.connected = False
            self.starting = False
            self.startup_message = None
            self.turn_navigation.reset()
            self.busy = False
            self.clear_permission_card()
            self.restore_queued_message()
            self.restore_pending_text()
            self.pending_request = None
            self.finish_pending_tools()
            self.btw.disconnected()
            self.implementation.finish()
            self.query_one("#message", MessageInput).disabled = True
            self.set_status("Disconnected")
            if self.resume_screen is not None and self.resume_screen in self.screen_stack:
                self.resume_screen.attach_failed("Daemon disconnected · Esc Close")
            if self.connect_screen is not None:
                self.connect_screen.auth_id = ""
                self.connect_screen.fail("Daemon disconnected · Esc Close")
            if self.model_screen is not None:
                self.model_screen.fail("Daemon disconnected · Esc Close")
            if self.mcps_screen is not None:
                self.mcps_screen.fail("Daemon disconnected · Esc Close")
            if self.worktree_screen is not None:
                self.worktree_screen.receive({"type": "request.failed", "error": "Daemon disconnected"})
            if self.permissions_screen is not None:
                self.permissions_screen.fail("Daemon disconnected")
            self.append_chat("Connection error", str(error) or type(error).__name__, f"bold {SLOP_COLOR}")
            return isinstance(error, OSError)
        finally:
            self.connected = False
            self.resume_transfer = None
            self.resume_requests.clear()
            self.worktree_requests.clear()
            self.compaction_request = None
            self.clear_running_compaction()
            self.recap_request = None
            self.clear_running_recap()
            await self.client.close()

    def clear_running_recap(self) -> None:
        if self.running_recap is not None:
            self.running_recap.stop_spinner()
            self.running_recap.remove()
            self.running_recap = None

    def show_running_recap(self):
        chat = self.query_one(HistoryViewport)
        chat.prepare_live_update()
        self.running_recap = ConversationRecap()
        self.running_recap.display = self.history_window.latest
        mounted = chat.mount(self.running_recap, before=self.queued_message)
        chat.follow_response(self.running_recap, mounted)
        return mounted

    async def request_recap(self, *, automatic: bool = False) -> None:
        self.recap_idle_session = self.session_id
        self.recap_idle.mark_requested()
        self.recap_automatic = automatic
        self.request_number += 1
        self.recap_request = str(self.request_number)
        self.busy = True
        self.set_status("Ready")
        await self.show_running_recap()
        try:
            await self.client.send({"type": "session.recap", "requestId": self.recap_request})
        except (OSError, ValueError) as error:
            self.handle_event({"type": "request.failed", "requestId": self.recap_request, "error": str(error)})

    def clear_running_compaction(self) -> None:
        if self.running_compaction is not None:
            self.running_compaction.remove()
            self.running_compaction = None

    def compaction_title(self, spinner: str) -> str:
        return f"Compaction {spinner}" + (f" {self.compaction_attempt} / {self.compaction_attempt_limit}" if self.compaction_attempt > 1 else "")

    def show_running_compaction(self):
        self.compaction_attempt = 0
        self.compaction_attempt_limit = 3
        self.running_compaction = ChatCollapsible(title="Compaction -", classes="compaction chat-message" + (" sane-compaction" if self.mode == "sane" else ""),
                                                 collapsed=True, collapsed_symbol="▸", expanded_symbol="▾")
        mounted = self.query_one(HistoryViewport).mount_live(self.running_compaction)
        return mounted

    async def on_message_input_submitted(self, event: MessageInput.Submitted) -> None:
        if self.implementation.discussion_preview:
            return
        if ((not self.connected and not self.starting) or self.editing_external or self.cleaning_history or self.history_loads
                or self.history_rendering or not event.text.strip()):
            return
        command = event.text.strip() if event.interpret_commands else ""
        command_name = command.split(maxsplit=1)[0] if command else ""
        # Permission replies restore composer focus; ignore repeated Enter until acknowledged.
        if self.permission_card is not None and self.permission_card.waiting and not command.startswith("/"):
            return
        entry = self.query_one("#message", MessageInput)
        if self.new_request is not None or self.turn_navigation.request_id is not None:
            return
        if self.quit_deadline is not None:
            self.reset_quit_confirmation()
        if not self.connected and command.startswith("/"):
            await self.show_input_error("Commands will be available when the daemon is ready. Your draft is preserved.", warning=True)
            return
        if event.interpret_commands and not self.validate_command(event.text):
            return
        if command in ("/undo", "/redo"):
            await self.turn_navigation.request(command == "/redo")
            return
        if command_name == "/btw":
            await self.btw.submit(event.text)
            return
        if self.busy and command in ("/compact", "/recap"):
            await self.show_input_error(f"Wait for the current execution to finish before using {command}.", warning=True)
            return
        if self.busy and (not command.startswith("/") or
                          command_name in skill_commands(self.runtime_info.get("skills", []))):
            self.queue_message(event.text, interpret_commands=event.interpret_commands)
            return
        if command == "/recap":
            if self.session_id is None:
                self.append_chat("Conversation Recap", "Start a conversation before requesting a recap.")
                return
            entry.text = ""
            await self.request_recap()
            return
        if command == "/compact":
            if self.session_id is None:
                self.append_chat("Compaction", "Start a conversation before compacting.")
                return
            entry.text = ""
            self.request_number += 1
            self.compaction_request = str(self.request_number)
            self.busy = True
            await self.show_running_compaction()
            self.set_status("Compacting…")
            try:
                await self.client.send({"type": "session.compact", "requestId": self.compaction_request})
            except (OSError, ValueError) as error:
                self.handle_event({"type": "request.failed", "requestId": self.compaction_request, "error": str(error)})
            return
        if await self.open_settings_command(command, entry):
            return
        if command == "/worktree":
            if self.busy:
                await self.show_input_error("Wait for the current execution to finish before using /worktree.", warning=True)
                return
            if self.worktree_screen is None:
                entry.text = ""
                self.worktree_screen = WorktreeScreen()
                await self.push_screen(self.worktree_screen)
            return
        if command == "/new":
            self.request_number += 1
            self.new_request = str(self.request_number)
            entry.disabled = True
            self.set_status("Starting fresh…")
            try:
                await self.client.send({"type": "session.new", "requestId": self.new_request})
            except (OSError, ValueError) as error:
                self.handle_event({"type": "request.failed", "requestId": self.new_request, "error": str(error)})
            return
        if command == "/resume":
            if self.resume_screen is None:
                entry.text = ""
                self.resume_screen = ResumeScreen()
                await self.push_screen(self.resume_screen)
                await self.send_resume_request("session.list")
            return
        images = entry.images
        entry.text = ""
        entry.focus()
        await self.send_user_message(event.text, images, interpret_commands=event.interpret_commands)

    async def send_user_message(self, text: str, images=(), *, sane_request: bool = False, implementation_request: bool = False,
                                interpret_commands: bool = True) -> None:
        self.btw.collapse()
        self.recap_idle.activity()
        self.request_started_at = monotonic()
        self.busy = True
        self.submitted_mode = self.mode
        planning_request = sane_request or implementation_request
        self.pending_text = None if planning_request else text
        self.pending_interpret_commands = interpret_commands
        self.pending_sane_request = planning_request
        self.pending_images = images
        self.request_number += 1
        self.pending_request = str(self.request_number)
        request: dict[str, Any] = {
            "type": "message.send", "requestId": self.pending_request, "text": text, "mode": self.mode,
        }
        if sane_request:
            request["sane_request"] = True
        if implementation_request:
            request["implementation_request"] = True
        if self.session_id is None:
            request["workspace"] = self.workspace
        self.pending_tools.clear()
        self.pending_turn = self.mount_user_turn(text)
        self.pending_turn.user.set_class(planning_request, "sane-request")
        self.pending_turn.user.set_images([{"origin": image.origin} for image in images])
        self.pending_turn.user.add_class("pending-turn")
        self.set_status("Sending…")
        if not self.connected and self.starting:
            self.startup_message = request
            return
        await self.dispatch_user_message(request, images)

    async def dispatch_user_message(self, request: dict[str, Any], images=()) -> None:
        text = request["text"]
        try:
            if not await self.prepare_history_for_send():
                self.busy = False
                self.restore_queued_message()
                self.restore_pending_text()
                self.pending_request = None
                return
            # Optimistic display is local only. The daemon checks image support
            # before acceptance; request.failed restores the draft and attachments.
            check_budget(images)
            invocations = parse_skill_commands(text, self.runtime_info.get("skills", [])) if self.pending_interpret_commands else []
            if self.pending_interpret_commands and text.lstrip().startswith("/") and not invocations:
                raise ValueError("Skill is no longer available. Check the current command list.")
            if len(invocations) == 1:
                request["skill"] = invocations[0]
            elif invocations:
                request["skills"] = invocations
            if images:
                request["images"] = [image.payload() for image in images]
            await self.client.send(request)
        except (OSError, ValueError) as error:
            self.busy = False
            self.restore_queued_message()
            self.restore_pending_text()
            self.pending_request = None
            self.query_one(MessageInput).disabled = not self.connected
            self.append_chat("Send failed", str(error), f"bold {SLOP_COLOR}")
            self.set_status("Ready" if self.connected else "Disconnected")

    async def open_settings_command(self, command, entry):
        settings = {
            "/permissions": ("permissions_screen", lambda: PermissionsScreen(self.runtime_info)),
            "/model": ("model_screen", lambda: ModelScreen(self.runtime_info)),
            "/mcps": ("mcps_screen", McpsScreen),
            "/connect": ("connect_screen", ConnectScreen),
        }
        if command not in settings:
            return False
        attribute, create = settings[command]
        if getattr(self, attribute) is None:
            entry.text = ""
            screen = create()
            setattr(self, attribute, screen)
            await self.push_screen(screen)
        return True

    def validate_command(self, text: str) -> bool:
        text = text.strip()
        if not text.startswith("/"):
            return True
        command, *arguments = text.split(maxsplit=1)
        if command in skill_commands(self.runtime_info.get("skills", [])):
            return True
        if command not in COMMANDS:
            self.notify(f"Unknown command: {command}. Type / to see available commands.",
                        title="Command", severity="error", markup=False)
            return False
        if arguments and command != "/btw":
            self.notify(f"{command} does not accept arguments.", title="Command", severity="error", markup=False)
            return False
        return True

    async def prepare_history_for_send(self) -> bool:
        self.cleaning_history = True
        try:
            await self.query_one(HistoryViewport).purge_activities(exclude=self.pending_turn.activity)
            if self.session_id is not None and not self.history_window.latest:
                return await self.load_history_page("latest", clean=True)
            return True
        finally:
            self.cleaning_history = False

    async def on_unmount(self) -> None:
        await self.client.close()

    async def on_permissions_screen_requested(self, event: PermissionsScreen.Requested) -> None:
        if self.permissions_screen is not event.screen or event.screen.finished:
            return
        self.request_number += 1
        request_id = str(self.request_number)
        self.permissions_requests[request_id] = event.screen
        try:
            await self.client.send({**event.payload, "requestId": request_id})
        except (OSError, ValueError) as error:
            self.handle_event({"type": "request.failed", "requestId": request_id, "error": str(error)})

    def on_permissions_screen_closed(self, event: PermissionsScreen.Closed) -> None:
        if self.permissions_screen is event.screen:
            self.close_permissions_screen(event.screen)

    def close_permissions_screen(self, screen: PermissionsScreen) -> None:
        screen.discard_secrets()
        screen.finished = True
        screen.dismiss()
        self.permissions_screen = None
        self.call_after_refresh(self.focus_composer)

    async def on_manual_approval_view_requested(self, event: ManualApprovalView.Requested) -> None:
        event.stop()
        if not self.connected or self.session_id is None:
            event.view.fail("Connect to a session before changing approval")
            return
        self.request_number += 1
        request_id = str(self.request_number)
        self.manual_requests[request_id] = (self.session_id, event.view)
        try:
            await self.client.send({"type": "permission.manual", "requestId": request_id, "sessionId": self.session_id,
                                    "refusedMessageId": event.view.refused_id, "approved": event.approved})
        except (OSError, ValueError) as error:
            self.handle_event({"type": "request.failed", "requestId": request_id, "error": str(error)})

    async def on_model_screen_requested(self, event: ModelScreen.Requested) -> None:
        if self.model_screen is not event.screen or event.screen.finished:
            return
        await self.send_model_request(event.screen, event.payload)

    async def on_mcps_screen_requested(self, event: McpsScreen.Requested) -> None:
        if self.mcps_screen is not event.screen or event.screen.finished:
            return
        self.request_number += 1
        request_id = str(self.request_number)
        self.mcps_requests[request_id] = event.screen
        try:
            await self.client.send({**event.payload, "requestId": request_id})
        except (OSError, ValueError) as error:
            self.handle_event({"type": "request.failed", "requestId": request_id, "error": str(error)})

    async def on_worktree_screen_requested(self, event: WorktreeScreen.Requested) -> None:
        if self.worktree_screen is not event.screen or event.screen.finished:
            return
        self.request_number += 1
        request_id = str(self.request_number)
        self.worktree_requests[request_id] = event.screen
        try:
            await self.client.send({**event.payload, "requestId": request_id})
        except (OSError, ValueError) as error:
            self.handle_event({"type": "request.failed", "requestId": request_id, "error": str(error)})

    def on_worktree_screen_closed(self, event: WorktreeScreen.Closed) -> None:
        if self.worktree_screen is event.screen:
            event.screen.dismiss()
            self.worktree_screen = None
            self.call_after_refresh(self.focus_composer)

    async def on_mcps_screen_closed(self, event: McpsScreen.Closed) -> None:
        if self.mcps_screen is not event.screen:
            return
        if event.screen.pending or event.screen.oauth_active:
            try:
                await self.client.send({"type": "mcp.cancel"})
            except (OSError, ValueError):
                pass
        event.screen.finished = True
        event.screen.dismiss()
        self.mcps_screen = None
        self.call_after_refresh(self.focus_composer)

    async def send_model_request(self, screen: ModelScreen, payload: dict[str, Any]) -> None:
        self.request_number += 1
        request_id = str(self.request_number)
        self.model_requests[request_id] = screen
        try:
            await self.client.send({**payload, "requestId": request_id})
        except (OSError, ValueError) as error:
            self.handle_event({"type": "request.failed", "requestId": request_id, "error": str(error)})

    async def on_model_screen_closed(self, event: ModelScreen.Closed) -> None:
        if self.model_screen is not event.screen:
            return
        if event.screen.catalog_loading:
            await self.send_model_request(event.screen, {"type": "model.cancel"})
        self.close_model_screen(event.screen)

    def close_model_screen(self, screen: ModelScreen) -> None:
        screen.finished = True
        screen.dismiss()
        self.model_screen = None
        self.call_after_refresh(self.focus_composer)

    async def on_connect_screen_requested(self, event: ConnectScreen.Requested) -> None:
        if self.connect_screen is not event.screen or event.screen.finished:
            return
        self.request_number += 1
        request_id = str(self.request_number)
        self.connect_requests[request_id] = event.screen
        payload = event.payload
        if payload.get("type") == "provider.select":
            self.switch_request = request_id
        try:
            await self.client.send({**payload, "requestId": request_id})
        except (OSError, ValueError):
            self.handle_event({"type": "request.failed", "requestId": request_id, "error": "Could not send connection request"})
        finally:
            payload.pop("key", None)

    async def on_connect_screen_closed(self, event: ConnectScreen.Closed) -> None:
        if self.connect_screen is not event.screen:
            return
        self.request_number += 1
        request_id = str(self.request_number)
        self.connect_requests[request_id] = event.screen
        try:
            await self.client.send({"type": "auth.cancel", "requestId": request_id, "authId": event.screen.auth_id})
        except (OSError, ValueError):
            self.connect_requests.pop(request_id, None)
        await event.screen.dismiss()
        self.connect_screen = None
        self.query_one(MessageInput).focus()

    @work(group="connect")
    async def finish_connection(self, event: dict[str, Any]) -> None:
        if self.connect_screen is not None:
            self.connect_screen.finished = True
            await self.connect_screen.dismiss()
            self.connect_screen = None
        await self.reset_session(event).wait()

    async def send_resume_request(self, kind: str, session_id: str = "") -> None:
        self.request_number += 1
        request_id = str(self.request_number)
        if kind == "session.attach":
            self.switch_request = request_id
        self.resume_requests[request_id] = (kind, session_id, self.resume_screen)
        request = {"type": kind, "requestId": request_id}
        if session_id:
            request["sessionId"] = session_id
        try:
            await self.client.send(request)
        except (OSError, ValueError) as error:
            self.handle_event({"type": "request.failed", "requestId": request_id, "error": str(error)})

    async def on_resume_screen_preview_requested(self, event: ResumeScreen.PreviewRequested) -> None:
        if self.resume_screen is not None:
            await self.send_resume_request("session.preview", event.session_id)

    async def on_resume_screen_resume_requested(self, event: ResumeScreen.ResumeRequested) -> None:
        if self.resume_screen is not None:
            await self.send_resume_request("session.attach", event.session_id)

    def display_live_messages(self, messages: list[dict[str, Any]]) -> None:
        if self.runtime_info.get("implementation_view") and self.runtime_info.get("implementation_execution"):
            return  # Construction belongs to the workspace, never to the plan chat.
        self.query_one(HistoryViewport).remember_reply(messages)
        if self.history_rendering:
            self.history_live.extend(messages)
            return
        if not self.history_window.latest:
            return
        for message in messages:
            self.display_message(message)
            if (message.get("id") and message["role"] in ("user", "assistant") and not message.get("compaction")
                    and any(part["type"] == "text" and part["text"].strip() for part in message["content"])):
                self.history_window.page["last"] = message["id"]
                self.history_window.page.setdefault("first", message["id"])
        self.call_after_refresh(self.query_one(ObjectiveWorkspace).sync)

    @work(exclusive=True, group="history-navigation")
    async def on_history_viewport_navigate(self, event: HistoryViewport.Navigate) -> None:
        event.stop()
        if not self.cleaning_history and self.connected and self.session_id:
            await self.load_history_page(event.direction, anchor=event.anchor)
        else:
            self.query_one(HistoryViewport).navigation_pending = False

    async def load_history_page(self, direction: str, clean: bool = False, anchor: str | None = None) -> bool:
        viewport = self.query_one(HistoryViewport)
        preview = self.pending_turn if clean else None
        self.history_window.session_id = self.session_id
        self.history_navigation_number += 1
        number = self.history_navigation_number
        session_id = self.session_id
        self.history_rendering = True
        self.btw.pause()
        self.clear_retry_notice(keep_status=True)
        # Scrolling pages is silent; explicit returns to the latest page keep
        # the same loading feedback as session restoration.
        loading = self.begin_history_loading() if direction == "latest" else None
        if loading is not None:
            self.set_status("Loading history…")
        else:
            viewport.set_history_loading(True)
        try:
            result = await self.history_window.navigate(self.client.send, direction, clean, anchor=anchor)
            if session_id != self.session_id or number != self.history_navigation_number:
                return False
            self.pending_tools.clear()
            if self.running_activity is not None and self.running_activity.toggle.started_at is not None:
                toggle = self.running_activity.toggle
                self.activity_clock = (self.running_activity.group_id, toggle.started_at,
                                       {"model": toggle.model, "reasoning": toggle.reasoning})
            self.current_activity = preview.activity if preview is not None else None
            self.running_activity = self.current_activity if self.busy else None
            self.pending_turn = preview
            self.activity_after_text = False
            async with self.history_render_lock:
                if number != self.history_navigation_number:
                    return False
                await viewport.replace(result, direction, collapse=clean,
                                       preview=(preview.user, preview.activity) if preview is not None else None)
            # If navigating back into an active turn, reconnect its live tool views.
            if preview is None and self.busy and self.history_window.latest and result["messages"]:
                last = result["messages"][-1]
                if last["role"] == "activity":
                    drawer = next(item for item in viewport.query(HistoryActivity) if item.group_id == last["id"])
                    record = await self.history_window.activities(self.client.send, last["id"])
                    await drawer.populate(record)
                    if not self.busy:
                        return True
                    self.current_activity = drawer
                    self.running_activity = drawer
                    if self.activity_clock is not None and self.activity_clock[0] == drawer.group_id:
                        _, started_at, settings = self.activity_clock
                        drawer.toggle.start_generation(settings, started_at)
                        drawer.toggle.set_spinner(self.active_info_bar().query_one(ModeButton).spinner_frame)
                    elif self.request_started_at is not None and record.get("started_ms") is not None:
                        drawer.toggle.start_generation(record.get("generation") or self.generation_info(),
                                                       self.request_started_at + record["started_ms"] / 1000)
                        drawer.toggle.set_spinner(self.active_info_bar().query_one(ModeButton).spinner_frame)
                    for tool in drawer.query(ToolView):
                        if tool.result is None:
                            self.pending_tools[tool.call_id] = tool
            if loading is not None:
                self.set_status("Working…" if self.busy else "Ready")
            return True
        except asyncio.CancelledError:
            return False
        except (OSError, ValueError) as error:
            self.set_status(f"Could not load history: {error}")
            return False
        finally:
            if number == self.history_navigation_number:
                self.history_rendering = False
                viewport.navigation_pending = False
                queued, self.history_live = self.history_live, []
                self.display_live_messages(queued)
                self.btw.render()
                if self.retry_status is not None:
                    self.show_generation_retry(self.retry_status)
                if loading is None:
                    viewport.set_history_loading(False)
            if loading is not None:
                self.end_history_loading(loading)
            self.dispatch_queued_message()

    def begin_history_loading(self, *, show_progress: bool = True) -> object:
        token = object()
        entry = self.query_one(MessageInput)
        if not self.history_loads:
            self.history_input_disabled = entry.disabled
            if show_progress:
                self.active_info_bar().set_history_loading(True)
            self.query_one(HistoryViewport).set_history_loading(True)
        self.history_loads.add(token)
        entry.disabled = True
        return token

    def end_history_loading(self, token: object) -> None:
        self.history_loads.discard(token)
        if self.history_loads:
            return
        entry = self.query_one(MessageInput)
        entry.disabled = self.history_input_disabled or not self.connected or self.runtime_info.get("implementation_archive", False)
        self.active_info_bar().set_history_loading(False)
        self.query_one(HistoryViewport).set_history_loading(False)
        self.focus_composer()

    @work(exclusive=True, group="resume")
    async def restore_session(self, event: dict[str, Any]) -> None:
        loading = self.begin_history_loading()
        if event.get("automatic_reconnect"):
            self.history_input_disabled = False
        try:
            if self.resume_screen is not None:
                await self.resume_screen.dismiss()
            self.set_status("Restoring history…")
            await self.render_restored_session(event)
        finally:
            if self.resume_screen is not None and self.resume_screen not in self.screen_stack:
                self.resume_screen = None
            self.end_history_loading(loading)

    async def render_restored_session(self, event: dict[str, Any]) -> None:
        if event.get("info", {}).get("implementation_execution"):
            workspace = self.query_one(ImplementationWorkspace)
            workspace.display = True
            workspace.show_loading(True)
            self.query_one("#discussion-body").display = False
        self.take_queued_message()
        self.switch_request = None
        self.busy = False
        self.btw.reset()
        self.query_one(HistoryViewport).clear_generation_footer()
        entry = self.query_one(MessageInput)
        entry.disabled = True
        chat = self.query_one("#chat", VerticalScroll)
        self.workers.cancel_group(self, "history-navigation")
        self.history_window.reset()
        self.history_navigation_number += 1
        self.history_live.clear()
        self.history_rendering = False
        self.pending_tools.clear()
        self.current_activity = None
        self.running_activity = None
        self.pending_turn = None
        self.activity_after_text = False
        session = event["session"]
        self.session_id = session["id"]
        self.set_mode(session.get("mode", "sane"))
        self.submitted_mode = self.mode
        self.workspace = session["workspace"]
        self.query_one("#workspace", Static).update(Text(self.workspace))
        self.query_one("#session-title", Static).update(Text(" ".join(session["title"].split())))
        async with self.history_render_lock:
            await self.query_one(HistoryViewport).replace(event)
        self.finish_pending_tools()
        if self.resume_screen is not None:
            if self.resume_screen in self.screen_stack:
                await self.resume_screen.dismiss()
            self.resume_screen = None
        if not event.get("automatic_reconnect"):
            entry.text = ""
        entry.disabled = not self.connected
        entry.focus()
        self.set_status("Ready" if self.connected else "Disconnected")
        self.call_after_refresh(chat.scroll_end, animate=False)
        try:
            if self.connected:
                await self.btw.refresh()
            await self.implementation.sync()
        except OSError as error:
            self.set_status(f"Could not restore session details: {error}")

    @work(exclusive=True, group="new-session")
    async def reset_session(self, event: dict[str, Any]) -> None:
        self.take_queued_message()
        self.switch_request = None
        self.busy = False
        self.btw.reset()
        self.query_one(HistoryViewport).clear_generation_footer()
        self.query_one(HistoryViewport).quote_source = ""
        self.history_window.reset()
        self.history_navigation_number += 1
        self.history_rendering = False
        self.history_live.clear()
        await self.query_one("#chat", VerticalScroll).remove_children()
        self.query_one(HistoryViewport).rendered.clear()
        self.query_one(HistoryViewport).message_ids.clear()
        self.session_id = None
        self.pending_request = None
        self.pending_sane_request = False
        self.pending_tools.clear()
        self.current_activity = None
        self.running_activity = None
        self.pending_turn = None
        self.activity_after_text = False
        self.request_started_at = None
        self.reset_cancel_confirmation()
        self.cancel_request_id = None
        self.workspace = event.get("workspace", self.workspace)
        self.query_one("#workspace", Static).update(Text(self.workspace))
        self.query_one("#session-title", Static).update("-")
        self.set_mode("sane")
        self.submitted_mode = self.mode
        self.runtime_info.clear()
        self.update_agent_info(event["info"])
        entry = self.query_one(MessageInput)
        entry.text = ""
        entry.disabled = not self.connected
        self.new_request = None
        entry.focus()
        self.set_status("Ready" if self.connected else "Disconnected")

    def finish_pending_tools(self) -> None:
        for view in self.pending_tools.values():
            view.set_result({"output": "No result received; execution ended or the connection was lost.", "isError": True})
        self.pending_tools.clear()


def main() -> None:
    parser = argparse.ArgumentParser(description="Chat with the local sosd daemon")
    parser.add_argument("--socket", default="/tmp/sosd.sock", help="Daemon Unix socket path")
    parser.add_argument("--version", action="version", version=f"SOS {VERSION.removeprefix('v')}")
    args = parser.parse_args()
    app = SosApp(args.socket)
    app.run()
    if app.update_version is not None:
        install_and_restart(app.update_version, app.workspace, ["--socket", args.socket])
