"""Read-only review conversation using Discussion's message and Activity widgets."""
import asyncio
from time import monotonic

from rich.text import Text
from .stable_scroll import StableScroll
from textual.widgets import Static

from .chat_collapsible import ChatCollapsible, CompactionDrawer, compaction_record
from .response_markdown import ResponseMarkdown
from .tool_view import ReadView, ToolView
from .turn_activity import TurnActivity
from .user_message import UserMessage


class ImplementationReviewChat(StableScroll):
    DEFAULT_CSS = "ImplementationReviewChat .review-boundary { height: auto; margin: 1 0; color: $misc-md; }"
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.running = None
        self.compaction = None
        self.started_at = None
        self.info = {}
        self.frame = 0
        self.render_lock = asyncio.Lock()
        self.rendered_records = []
        self.render_activity = None
        self.pending_activity = None
        self.previous_activity = None
        self.render_after_text = False
        self.render_tools = {}
        self.follow_tail = True
        self.position_generation = 0
        self.programmatic_scroll = False
        self.review_anchor = None

    def on_mount(self):
        self.set_interval(.2, self.update_activity_spinner)

    def update_activity_spinner(self):
        if self.running is not None:
            self.frame += 1
            self.running.toggle.set_spinner(self.frame)
            self.running.toggle.update_elapsed()

    def start(self, text, info):
        self.follow_tail = True
        self.started_at = monotonic()
        self.info = info
        self.running = TurnActivity()
        self.running.toggle.start_generation(info, self.started_at)
        self.mount(UserMessage(text, "sane"), self.running)
        record = {"role": "user", "content": [{"type": "text", "text": text}]}
        self.rendered_records.append(record)
        self.render_activity = self.running
        self.render_after_text = False
        self.render_tools = {}
        self.call_after_refresh(self.scroll_end, animate=False)

    def finish(self):
        if self.running is not None:
            self.running.toggle.finish_generation()
        self.running = None
        if self.compaction is not None:
            self.compaction.running = False
            self.compaction.title = "Compaction"
        self.started_at = None

    async def reset_history(self):
        async with self.render_lock:
            self.finish()
            await self.remove_children()
            self.compaction = None
            self.rendered_records = []
            self.render_activity = None
            self.pending_activity = None
            self.previous_activity = None
            self.render_after_text = False
            self.render_tools = {}
            self.follow_tail = True
            self.position_generation += 1

    def retry(self, retry):
        if self.running is not None:
            notice = "Reconnecting…" if not retry["seconds"] else f'Retrying in {retry["seconds"]}s'
            self.running.add_activity(Static(Text(f'{retry.get("error") or retry["reason"]}\n{notice}')), "tools")

    async def show_records(self, records, running):
        async with self.render_lock:
            await self._show_records(records, running)

    async def _show_records(self, records, running):
        follow, scroll = self.follow_tail, self.scroll_y
        self.review_anchor = self.reading_anchor() if not follow else None
        if self.pending_activity is not None:
            await self.pending_activity.remove()
            self.render_activity = self.previous_activity
            self.render_after_text = True
            self.pending_activity = self.previous_activity = None
        common = min(len(self.rendered_records), len(records))
        matches = all(self._record_key(old) == self._record_key(new)
                      for old, new in zip(self.rendered_records[:common], records[:common]))
        if not matches or common != len(self.rendered_records):
            expanded = [activity.toggle.expanded for activity in self.query(TurnActivity)]
            await self.remove_children()
            self.compaction = None
            self.render_activity = None
            self.render_after_text = False
            self.render_tools = {}
            self.rendered_records = []
            activity = await self._render_messages(records, 0, running)
            self.rendered_records = list(records)
            self._restore_activity(expanded, activity if running else None, running)
        else:
            expanded = [activity.toggle.expanded for activity in self.query(TurnActivity)]
            activity = await self._render_messages(records, common, running)
            self.rendered_records = list(records)
            self._restore_activity(expanded, activity if running else None, running)
        self.position_generation += 1
        self.call_after_refresh(self.restore_position, self.position_generation, follow, scroll)

    def restore_position(self, generation, follow, scroll):
        if generation != self.position_generation:
            return
        self.programmatic_scroll = True
        if follow and self.follow_tail:
            self.scroll_end(animate=False, immediate=True, force=True)
        elif not follow:
            if self.review_anchor is not None:
                self.restore_reading_anchor(self.review_anchor)
            else:
                self.scroll_to(y=scroll, animate=False, immediate=True, force=True)
        self.programmatic_scroll = False

    def watch_scroll_y(self, old_value, new_value):
        super().watch_scroll_y(old_value, new_value)
        if old_value != new_value and not self.programmatic_scroll:
            self.follow_tail = new_value >= self.max_scroll_y - 1

    def on_markdown_table_of_contents_updated(self, event):
        # Markdown finishes parsing after its widget is mounted. Follow the
        # response's final height, not just its initially empty container.
        if event.markdown.parent is self and self.follow_tail:
            self.call_after_refresh(self.restore_position, self.position_generation, True, self.scroll_y)
        elif not self.follow_tail:
            self.call_after_refresh(self.restore_reading_anchor, self.review_anchor)

    @staticmethod
    def _record_key(message):
        return message["role"], message["content"], message.get("model_call"), message.get("metadata"), message.get("compaction")

    async def _render_messages(self, records, start, running=False):
        activity, after_text, tools = self.render_activity, self.render_after_text, self.render_tools
        for message_index, message in enumerate(records[start:], start):
            previous_children = set(self.children)
            checkpoint = compaction_record(message)
            if checkpoint is not None:
                status, text = checkpoint
                if activity is not None:
                    activity.toggle.finish_generation()
                    if not activity.toggle.step_count and not activity.toggle.model_call_count:
                        await activity.remove()
                if status == "started" or self.compaction is None:
                    self.compaction = CompactionDrawer()
                    await self.mount(self.compaction)
                if status != "started":
                    await self.compaction.finish(text)
                    self.compaction = None
                activity, after_text, tools = None, False, {}
                continue
            role = message["role"]
            if role == "system":
                if message.get("id", "").startswith("review-integrated-"):
                    await self.mount(Static("Review incorporated into construction · New review segment", classes="review-boundary"))
                    activity, after_text, tools = None, False, {}
                continue
            if role == "assistant" and not message["content"]:
                if message.get("model_call") and activity is not None:
                    activity.toggle.record_model_call(message["model_call"])
                if message.get("metadata") and activity is not None:
                    activity.toggle.finish_generation(message["metadata"])
                continue
            if role == "user":
                text = "\n".join(part["text"] for part in message["content"] if part["type"] == "text")
                await self.mount(UserMessage(text, "sane"))
                activity, after_text = None, False
            has_activity = any(part["type"] in ("reasoning", "tool_call") for part in message["content"])
            if role in ("user", "assistant") and (activity is None or (after_text and has_activity)):
                if activity is not None:
                    activity.toggle.is_terminal = False
                activity = TurnActivity()
                await self.mount(activity)
                after_text = False
            if role == "assistant" and message.get("model_call"):
                activity.toggle.record_model_call(message["model_call"])
            for part in message["content"]:
                after_text = await self._render_part(message, part, activity, tools) or after_text
            if role == "assistant" and message.get("metadata") and activity is not None:
                activity.toggle.finish_generation(message["metadata"])
            for part_index, widget in enumerate(child for child in self.children if child not in previous_children):
                widget.scroll_key = (message.get("id", message_index), part_index)
        if running and after_text:
            if activity is not None:
                activity.toggle.finish_generation()
                activity.toggle.is_terminal = False
            self.previous_activity = activity
            activity = TurnActivity()
            self.pending_activity = activity
            await self.mount(activity)
            after_text = False
        if running and activity is None and records and compaction_record(records[-1]) is not None:
            if compaction_record(records[-1])[0] == "completed":
                activity = TurnActivity()
                await self.mount(activity)
        self.render_activity, self.render_after_text, self.render_tools = activity, after_text, tools
        return activity

    async def _render_part(self, message, part, activity, tools):
        kind = part["type"]
        if kind == "text" and message["role"] == "assistant" and part["text"].strip():
            await self.mount(ResponseMarkdown(part["text"], classes="chat-message", generation=message.get("metadata")))
            return True
        if kind == "reasoning" and part.get("text"):
            activity.add_activity(ChatCollapsible(ResponseMarkdown(part["text"]), title="Reasoning", classes="reasoning",
                                                 collapsed=True, collapsed_symbol="▸", expanded_symbol="▾"), "reasoning")
        elif kind == "tool_call":
            view = (ReadView(part["callId"], part["arguments"]) if part["toolName"] == "read" else
                    ToolView(part["callId"], part["toolName"], part["arguments"]))
            tools[part["callId"]] = view
            activity.add_activity(view, "read" if isinstance(view, ReadView) else view.activity_kind)
        elif kind == "tool_result" and part["callId"] in tools:
            tools.pop(part["callId"]).set_result(part)
        return False

    def _restore_activity(self, expanded, running, generation_running=False):
        for index, drawer in enumerate(self.query(TurnActivity)):
            drawer.toggle.expanded = expanded[index] if index < len(expanded) else False
            drawer.body.display = drawer.toggle.expanded
            if running is None and drawer.toggle.started_at is not None:
                drawer.toggle.finish_generation()
        self.running = running
        if self.running is not None:
            self.started_at = self.started_at or monotonic()
            self.running.toggle.start_generation(self.info or self.app.generation_info(), self.started_at)
        else:
            self.started_at = None
        if self.compaction is not None and not generation_running:
            self.compaction.running = False
            self.compaction.title = "Compaction"
