"""Second-level lazy details inside an imported activity drawer."""

from typing import Any

from rich.text import Text
from textual.message import Message
from textual.widgets import Collapsible, Static

from .config import SLOP_COLOR
from .chat_collapsible import ChatCollapsible
from .response_markdown import ResponseMarkdown
from .tool_view import ToolView, format_arguments


class ActivityDetailRequested(Message):
    def __init__(self, view: "HistoryToolView | HistoryReasoning") -> None:
        super().__init__()
        self.view = view


class HistoryToolView(ToolView):
    def __init__(self, group_id: str, item: dict[str, Any], *, file_index: int | None = None) -> None:
        self.history_item = item
        self.group_id = group_id
        self.activity_index = item["activityIndex"]
        self.details_loaded = False
        self.detail_loading = False
        super().__init__(item.get("callId", ""), item["toolName"], item.get("arguments", {}), skill_script=item.get("skill_script"),
                         skill_script_path=item.get("skill_script_path"),
                         user_skill=item.get("user_skill"), file_index=file_index)
        if "result" in item:
            self.set_result(item["result"])

    def on_collapsible_expanded(self, event: Collapsible.Expanded) -> None:
        if event.collapsible is not self or self.multi_file:
            return
        event.prevent_default()
        if self.details_loaded:
            super().on_collapsible_expanded(event)
        elif not self.detail_loading:
            self.detail_loading = True
            self.show_detail_status("Loading details…")
            self.post_message(ActivityDetailRequested(self))

    def new_file_view(self, index: int) -> ToolView:
        view = HistoryToolView(self.group_id,
            {**self.history_item, "arguments": self.arguments, **({"result": self.result} if self.result is not None else {})},
            file_index=index)
        view.details_loaded = self.details_loaded
        return view

    def render_result(self):
        # A summary can contain status metadata but deliberately no output.
        if self.details_loaded:
            return super().render_result()

    async def load_detail(self, item: dict[str, Any]) -> None:
        if item.get("kind") != "tool" or item.get("callId", "") != self.call_id or item.get("toolName") != self.tool:
            raise ValueError("Mismatched tool detail")
        if not isinstance(item.get("arguments"), dict):
            raise ValueError("Invalid tool arguments")
        result = item.get("result")
        if "result" in item and (not isinstance(result, dict) or "output" not in result or type(result.get("isError")) is not bool):
            raise ValueError("Invalid tool result")
        self.arguments = item["arguments"]
        if self.tool not in ("edit", "apply_patch", "write"):
            self.arguments_view.update(format_arguments(self.arguments, self.tool))
        self.details_loaded = True
        if "result" in item:
            self.set_result(item["result"])
        elif self.result is not None and "output" in self.result and not self.collapsed:
            self.render_result()  # A live result may have arrived while details were loading.

    async def fail_detail(self, error: str) -> None:
        self.show_detail_status(Text(f"Could not load details: {error}. Close and reopen to retry.", style=SLOP_COLOR))

    def show_detail_status(self, text) -> None:
        target = self.results.query_one(Static) if self.tool in ("edit", "apply_patch", "write") else self.arguments_view
        target.update(text)


class HistoryReasoning(ChatCollapsible):
    detail_kind = "reasoning"

    def __init__(self, group_id: str, item: dict[str, Any]) -> None:
        self.group_id = group_id
        self.activity_index = item["activityIndex"]
        self.details_loaded = False
        self.detail_loading = False
        super().__init__(title="Reasoning", classes="reasoning", collapsed=True,
                         collapsed_symbol="▸", expanded_symbol="▾")

    async def on_collapsible_expanded(self, event: Collapsible.Expanded) -> None:
        if event.collapsible is self and not self.details_loaded and not self.detail_loading:
            self.detail_loading = True
            body = self.query_one(Collapsible.Contents)
            await body.remove_children()
            await body.mount(Static("Loading details…"))
            self.post_message(ActivityDetailRequested(self))

    async def load_detail(self, item: dict[str, Any]) -> None:
        if item.get("kind") != self.detail_kind or not isinstance(item.get("text"), str):
            raise ValueError("Invalid activity detail")
        body = self.query_one(Collapsible.Contents)
        await body.remove_children()
        await body.mount(self.detail_body(item["text"]))
        self.details_loaded = True

    def detail_body(self, text: str):
        return ResponseMarkdown(text)

    async def fail_detail(self, error: str) -> None:
        body = self.query_one(Collapsible.Contents)
        await body.remove_children()
        await body.mount(Static(Text(f"Could not load details: {error}. Close and reopen to retry.", style=SLOP_COLOR)))
