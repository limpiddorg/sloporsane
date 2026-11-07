"""Mutable human authorization, separate from the immutable tool transcript."""

from typing import Any

from rich.text import Text
from textual.color import Color
from textual.message import Message
from textual.widgets import Button, Collapsible, Static

from .chat_collapsible import ChatCollapsible
from .config import ALGAE_MD, BACKGROUND_COLOR, DEPTH_MD, MUTED_COLOR, SANE_COLOR, SLOP_COLOR, TEXT_COLOR
from .history_detail import ActivityDetailRequested
from .tool_view import format_arguments, tool_display_name, permission_suffix, styled_tool_title


APPROVAL_BACKGROUND = Color.parse(BACKGROUND_COLOR).blend(Color.parse(MUTED_COLOR), 0.20).hex
APPROVAL_HOVER_BACKGROUND = Color.parse(BACKGROUND_COLOR).blend(Color.parse(MUTED_COLOR), 0.30).hex


class ManualApprovalView(ChatCollapsible):
    DEFAULT_CSS = f"""
    ManualApprovalView > CollapsibleTitle {{ color: {SLOP_COLOR}; }}
    ManualApprovalView.approved > CollapsibleTitle {{ color: {SANE_COLOR}; }}
    ManualApprovalView Button.approval-action {{ height: 1; min-width: 0; width: auto; margin: 0;
        border: none; padding: 0 1; }}
    ManualApprovalView Button.approval-action,
    ManualApprovalView Button.approval-action:hover,
    ManualApprovalView Button.approval-action:focus,
    ManualApprovalView Button.approval-action:disabled {{
        background: {APPROVAL_BACKGROUND}; color: {ALGAE_MD};
        background-tint: transparent; tint: transparent; text-style: bold; text-opacity: 1; opacity: 1;
    }}
    ManualApprovalView.approved Button.approval-action,
    ManualApprovalView.approved Button.approval-action:hover,
    ManualApprovalView.approved Button.approval-action:focus,
    ManualApprovalView.approved Button.approval-action:disabled {{ color: {SLOP_COLOR}; }}
    ManualApprovalView Button.approval-action:hover {{ background: {APPROVAL_HOVER_BACKGROUND}; }}
    ManualApprovalView Button.approval-action:focus,
    ManualApprovalView.approved Button.approval-action:focus {{
        background: {DEPTH_MD}; color: {TEXT_COLOR}; text-style: bold;
    }}
    ManualApprovalView .approval-error {{ height: auto; color: {SLOP_COLOR}; }}
    """

    def _watch_title(self, title: str) -> None:
        self._title.label = styled_tool_title(title, self.permission_records())

    class Requested(Message):
        def __init__(self, view: "ManualApprovalView", approved: bool) -> None:
            super().__init__()
            self.view = view
            self.approved = approved

    def __init__(self, item: dict[str, Any], group_id: str = "", *, lazy: bool = False) -> None:
        metadata = item.get("result", {}).get("metadata", {})
        self.refused_id = metadata.get("refused_message_id", "")
        self.approved = metadata.get("manually_approved", False)
        self.group_id = group_id
        self.activity_index = item.get("activityIndex", 0)
        self.details_loaded = not lazy
        self.detail_loading = False
        self.tool = item["toolName"]
        self.display_name = tool_display_name(self.tool, metadata.get("mcp_server"), metadata.get("mcp_tool"))
        self.confidence = next((record.get("confidence") for record in reversed(metadata.get("permissions", []))
                                if record.get("decision") == "ai_refused"), None)
        self.call_id = item.get("callId", "")
        self.arguments_view = Static("Loading details…" if lazy else format_arguments(item.get("arguments", {})))
        self.context_view = Static(self.context_text(item))
        self.context_view.display = bool(self.context_text(item))
        self.button: Button | None = None
        self.pending = False
        self.error = Static("" if self.refused_id else "This older refusal has no saved authorization target.", classes="approval-error")
        self.error.display = not self.refused_id
        super().__init__(self.arguments_view, self.context_view, self.error, title=self.label(),
                         collapsed=True, collapsed_symbol="▸", expanded_symbol="▾", classes="tool-call")
        self.set_class(self.approved, "approved")

    def label(self) -> str:
        return self.display_name + permission_suffix(self.permission_records())

    def permission_records(self) -> list[dict[str, Any]]:
        decision = "manually_approved" if self.approved else "ai_refused"
        return [{"decision": decision, "confidence": self.confidence}]

    @staticmethod
    def context_text(item: dict[str, Any]) -> str:
        action = item.get("result", {}).get("metadata", {}).get("refused_action", {})
        if not action:
            return ""
        return f"Working directory: {action['working_directory']}\nPermission workspace: {action['workspace']}"

    def on_button_pressed(self, event: Button.Pressed) -> None:
        event.stop()
        if event.button is not self.button or self.pending or not self.refused_id or not self.details_loaded:
            return
        self.pending = True
        self.button.disabled = True
        self.error.update("")
        self.error.display = False
        self.post_message(self.Requested(self, not self.approved))

    def update_button(self) -> None:
        if self.button is not None:
            self.button.label = "Revoke approval" if self.approved else "Approve"
            self.button.refresh(layout=True)
            self.button.disabled = self.pending or not self.refused_id or not self.details_loaded

    def set_approved(self, approved: bool) -> None:
        self.approved = approved
        self.pending = False
        self.update_button()
        self.title = self.label()
        self.set_class(approved, "approved")

    def fail(self, error: str) -> None:
        self.set_approved(self.approved)
        self.error.update(error)
        self.error.display = True

    async def on_collapsible_expanded(self, event: Collapsible.Expanded) -> None:
        if event.collapsible is not self or self.collapsed:
            return
        if self.button is None:
            self.button = Button(classes="approval-action", compact=True)
            self.button.active_effect_duration = 0
            self.update_button()
            await self.query_one(Collapsible.Contents).mount(self.button, before=self.error)
        if not self.details_loaded and not self.detail_loading:
            self.detail_loading = True
            self.post_message(ActivityDetailRequested(self))

    async def on_collapsible_collapsed(self, event: Collapsible.Collapsed) -> None:
        if event.collapsible is self and self.button is not None:
            button, self.button = self.button, None
            await button.remove()

    async def load_detail(self, item: dict[str, Any]) -> None:
        if item.get("callId") != self.call_id or item.get("toolName") != self.tool:
            raise ValueError("Mismatched refused action detail")
        metadata = item.get("result", {}).get("metadata", {})
        if isinstance(metadata.get("mcp_server"), str) and isinstance(metadata.get("mcp_tool"), str):
            self.display_name = tool_display_name(self.tool, metadata["mcp_server"], metadata["mcp_tool"])
            self.title = self.label()
        self.arguments_view.update(format_arguments(item["arguments"]))
        self.context_view.update(self.context_text(item))
        self.context_view.display = bool(self.context_text(item))
        self.details_loaded = True
        self.update_button()

    async def fail_detail(self, error: str) -> None:
        self.arguments_view.update(Text(f"Could not load details: {error}. Close and reopen to retry.", style=SLOP_COLOR))


def was_ai_refused(item: dict[str, Any]) -> bool:
    return any(record.get("decision") == "ai_refused"
               for record in item.get("result", {}).get("metadata", {}).get("permissions", []))
