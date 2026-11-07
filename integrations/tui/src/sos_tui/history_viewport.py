"""Paged chat widgets. Historical drawers build their contents only when opened."""

from collections.abc import Awaitable, Callable
import asyncio
from typing import Any

from rich.text import Text
from textual import events, work
from .stable_scroll import StableScroll, ScrollExtent
from textual.message import Message
from textual.widget import AwaitMount, Widget
from textual.widgets import Collapsible, Static

from .history_window import HistoryWindow
from .chat_collapsible import ChatCollapsible
from .history_layout import HistoryLayout
from .history_detail import ActivityDetailRequested, HistoryToolView, HistoryReasoning
from .response_markdown import GenerationFooter, ResponseMarkdown
from .objective_plan import ObjectivePlan
from .objective_drawer import ObjectivePlanDrawer
from .implementation_plan import ImplementationPlan
from .tool_view import ReadView, ToolView
from .turn_activity import ActivityToggle, TurnActivity
from .user_message import UserMessage
from .conversation_recap import ConversationRecap
from .permission_card import PermissionCard
from .manual_approval import ManualApprovalView, was_ai_refused
from .queued_message import QueuedMessage


class HistoryActivity(TurnActivity):
    class Requested(Message):
        def __init__(self, drawer: "HistoryActivity") -> None:
            super().__init__()
            self.drawer = drawer

    def __init__(self, record: dict[str, Any]) -> None:
        super().__init__()
        self.group_id = record["id"]
        self.built = False
        self.loading = False
        self.toggle.is_terminal = record.get("terminal", True)
        self.toggle.step_count = record.get("count", len(record.get("activities", [])))
        for kind in record["kinds"]:
            self.toggle.record_activity(kind)
        if record.get("metadata"):
            self.toggle.finish_generation(record["metadata"])
        if record.get("generation"):
            self.toggle.model = record["generation"]["model"]
            self.toggle.reasoning = record["generation"]["reasoning"]
        if record.get("model_call"):
            self.toggle.record_model_call(record["model_call"])
        if not self.toggle.is_terminal and record.get("duration_ms") is not None:
            self.toggle.duration_ms = record["duration_ms"]
            self.toggle.update_elapsed()

    @classmethod
    def live(cls, group_id: str = "") -> "HistoryActivity":
        drawer = cls({"id": group_id, "kinds": []})
        drawer.built = True  # The current run fills this body directly.
        return drawer

    def on_activity_toggle_toggled(self, event: ActivityToggle.Toggled) -> None:
        event.prevent_default()
        super().on_activity_toggle_toggled(event)
        if self.toggle.expanded and not self.built and not self.loading:
            self.loading = True
            self.post_message(self.Requested(self))

    async def populate(self, record: dict[str, Any]) -> None:
        widgets = []
        await self.refusals.remove_children()
        self.refused_views.clear()
        for activity in record["activities"]:
            if was_ai_refused(activity):
                self.add_refusal(ManualApprovalView(activity, self.group_id, lazy="activityIndex" in activity))
            if "activityIndex" in activity and activity.get("toolName") != "read":
                widgets.append(HistoryReasoning(self.group_id, activity) if activity["kind"] == "reasoning"
                               else HistoryToolView(self.group_id, activity))
                continue
            if activity["kind"] == "reasoning":
                widgets.append(ChatCollapsible(ResponseMarkdown(activity["text"]), title="Reasoning",
                                               classes="reasoning", collapsed=True,
                                               collapsed_symbol="▸", expanded_symbol="▾"))
                continue
            name = activity["toolName"]
            widget = (ReadView(activity.get("callId", ""), activity["arguments"]) if name == "read"
                      else ToolView(activity.get("callId", ""), name, activity["arguments"],
                                    skill_script=activity.get("skill_script"), skill_script_path=activity.get("skill_script_path"),
                                    user_skill=activity.get("user_skill")))
            if "result" in activity:
                widget.set_result(activity["result"])
            widgets.append(widget)
        await self.body.remove_children()
        if widgets:
            await self.body.mount_all(widgets)
        self.built = True
        self.update_refusal_visibility()


class HistoryCompaction(ChatCollapsible):
    """Do not parse a potentially large summary until its drawer is opened."""

    def __init__(self, text: str) -> None:
        super().__init__(title="Compaction", collapsed=True, classes="compaction chat-message",
                         collapsed_symbol="▸", expanded_symbol="▾")
        self.text = text
        self.built = False

    async def on_collapsible_expanded(self, event: Collapsible.Expanded) -> None:
        if event.collapsible is self and not self.built:
            self.built = True
            await self.query_one(Collapsible.Contents).mount(ResponseMarkdown(self.text))


class HistoryViewport(StableScroll):
    # Avoid restyling the history on focus changes; child interactions still work.
    FOCUS_ON_CLICK = False
    WINDOW_SIZE = 20
    class Navigate(Message):
        def __init__(self, direction: str, anchor: str | None = None) -> None:
            super().__init__()
            self.direction = direction
            self.anchor = anchor

    def __init__(self, window: HistoryWindow, send: Callable[[dict], Awaitable[None]], **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self.window = window
        self.history_layout = HistoryLayout(window)
        self.send = send
        self.replacing = False
        self.navigation_pending = False
        self.history_loading = False
        self.ignore_scroll_before = 0.0
        self.rendered: list[tuple[dict[str, Any], Widget]] = []
        self.message_ids: dict[Widget, str] = {}
        self.scroll_input_number = 0
        self.scroll_position_number = 0
        self.follow_input_number: int | None = None
        self.live_footer: GenerationFooter | None = None
        self.quote_source = ""
        self.objective_drawers: dict[str, ObjectivePlanDrawer] = {}
        self.seek_position: float | None = None

    def _logical_rows(self):
        start = self.window.page.get("start", 0)
        return [(start + index, widget) for index, (_, widget) in enumerate(self.rendered)
                if widget.is_attached and widget.display and widget.region.height]

    def scroll_extent(self):
        total = self.window.page.get("total", 0)
        rows = self._logical_rows()
        if not total or not rows:
            return None
        if self.seek_position is not None:
            position = self.seek_position
        elif self.window.latest and self.is_vertical_scroll_end:
            position = total
        else:
            top = self.content_region.y
            index, widget = next(((index, widget) for index, widget in reversed(rows)
                                  if widget.region.y <= top), rows[0])
            position = index + max(0, min(1, (top - widget.region.y) / widget.region.height))
        return ScrollExtent(total, position, min(3, total))

    def seek(self, position):
        if self.scroll_locked:
            return
        total = self.window.page.get("total", 0)
        if position >= total and self.window.latest:
            self.scroll_end(animate=False, immediate=True, force=True)
            return
        position = max(0, min(max(0, total - 1), position))
        self.follow_input_number = None
        if self._seek_loaded(position):
            return
        self.seek_position = position
        self.navigation_pending = True
        self.freeze_scroll()
        self._sync_global_scrollbar()
        self.post_message(self.Navigate("at", str(int(position))))

    def _seek_loaded(self, position):
        rows = self._logical_rows()
        for index, widget in rows:
            if index <= position < index + 1:
                y = widget.virtual_region.y + (position - index) * widget.region.height
                self.scroll_to(y=y, animate=False, immediate=True, force=True)
                return True
        return False

    def objective_widgets(self, rendered, session_id):
        info = getattr(self.app, "runtime_info", {})
        widgets = [widget for _, widget in rendered]
        if session_id != info.get("discussion_id", getattr(self.app, "session_id", None)):
            return widgets
        versions = {plan["id"]: plan for plan in info.get("objective_plans", [])}
        turns = {turn["message_id"]: turn for turn in info.get("objective_plan_turns", [])}
        last = {record["id"]: widget for record, widget in rendered if record["role"] == "assistant"}
        result = []
        for record, widget in rendered:
            result.append(widget)
            turn = turns.get(record["id"])
            if not turn or last.get(record["id"]) is not widget or turn["objective_id"] not in versions:
                continue
            key = f"{session_id}/{record['id']}"
            drawer = self.objective_drawers.get(key)
            if drawer is None:
                drawer = ObjectivePlanDrawer(versions[turn["objective_id"]], turn["updated"])
                self.objective_drawers[key] = drawer
            result.append(drawer)
            if isinstance(widget, ResponseMarkdown):
                footer = widget.separate_generation_footer()
                if footer is not None:
                    result.append(footer)
        return result

    def sync_objective_plans(self):
        if self.replacing:
            return
        session_id = self.window.session_id or (getattr(self.app, "session_id", None) if self.id == "chat" else None)
        widgets = self.objective_widgets(self.rendered, session_id)
        for index, widget in enumerate(widgets):
            if isinstance(widget, GenerationFooter) and widget.parent is not self:
                if widgets[index - 1] in self.children:
                    self.mount(widget, after=widgets[index - 1])
            if isinstance(widget, ObjectivePlanDrawer):
                if widget.parent is not self and widgets[index - 1] in self.children:
                    mounted = self.mount(widget, after=widgets[index - 1])
                    self.follow_response(widget, mounted)
                widget.sync()
        for key, drawer in list(self.objective_drawers.items()):
            if drawer not in widgets:
                drawer.remove()
                del self.objective_drawers[key]

    @property
    def layout(self) -> HistoryLayout:
        return self.history_layout

    def mount_live(self, *widgets: Widget) -> AwaitMount:
        queued = next((child for child in self.children if isinstance(child, QueuedMessage)), None)
        return self.mount(*widgets, before=self.live_footer or queued)

    def start_generation_footer(self, metadata: dict, started_at: float | None = None) -> None:
        self.clear_generation_footer()
        if not metadata.get("model"):
            return
        footer = GenerationFooter(metadata, started_at)
        self.mount_live(footer)
        self.live_footer = footer

    def clear_generation_footer(self) -> None:
        if self.live_footer is not None:
            self.live_footer.display = False
            self.live_footer.remove()
            self.live_footer = None

    def show_user_turn(self, user: UserMessage, mounted: AwaitMount) -> None:
        self.history_layout.scroll_anchor = None
        self.scroll_input_number += 1
        self.follow_input_number = self.scroll_input_number
        self.scroll_after_mount(user, mounted, self.scroll_input_number)

    def prepare_live_update(self) -> None:
        # Remember waiting at the bottom before an activity or a text changes
        # its position. Activities alone must not move the view.
        if (self.history_layout.scroll_anchor is None and self.is_vertical_scroll_end
                and self.scroll_target_y == self.max_scroll_y):
            self.follow_input_number = self.scroll_input_number

    def follow_response(self, response: Widget, mounted: AwaitMount) -> None:
        if self.follow_input_number == self.scroll_input_number:
            self.scroll_after_mount(response, mounted, self.scroll_input_number)

    def watch_scroll_y(self, old_value: float, new_value: float) -> None:
        super().watch_scroll_y(old_value, new_value)
        if old_value != new_value and not self.is_vertical_scroll_end:
            self.follow_input_number = None

    @work(group="chat-position")
    async def scroll_after_mount(self, widget: Widget, mounted: AwaitMount, input_number: int) -> None:
        await mounted
        self.call_after_refresh(self.finish_mount_scroll, widget, input_number)

    def finish_mount_scroll(self, widget: Widget, input_number: int) -> None:
        user = isinstance(widget, UserMessage)
        preview = user and widget.has_class("pending-turn")
        if (input_number == self.scroll_input_number and widget.is_attached and widget.parent is self
                and (user or self.follow_input_number == input_number)
                and (preview or (self.window.latest and not self.scroll_locked))):
            self.scroll_position_number += 1
            self.scroll_end(animate=False, immediate=True, force=True)
            self.follow_input_number = input_number

    @property
    def scroll_locked(self) -> bool:
        return self.history_loading or self.replacing or self.navigation_pending or self.window.loading

    @property
    def allow_vertical_scroll(self) -> bool:
        return not self.scroll_locked and super().allow_vertical_scroll

    @property
    def allow_horizontal_scroll(self) -> bool:
        return not self.scroll_locked and super().allow_horizontal_scroll

    def freeze_scroll(self) -> None:
        # Stop an animation already started by a key before the request began.
        self.scroll_to(x=self.scroll_x, y=self.scroll_y, animate=False, immediate=True, force=True)

    def set_history_loading(self, loading: bool) -> None:
        self.history_loading = loading
        if loading:
            self.freeze_scroll()
        else:
            self.ignore_scroll_before = events.Event().time
            self.seek_position = None
            self._sync_global_scrollbar()

    def ignore_scroll_event(self, event: events.Event) -> bool:
        if self.scroll_locked or event.time <= self.ignore_scroll_before:
            event.prevent_default()
            event.stop()
            return True
        self.scroll_input_number += 1
        return False

    def on_key(self, event: events.Key) -> None:
        if event.key in ("up", "down", "left", "right", "home", "end", "pageup", "pagedown",
                         "ctrl+pageup", "ctrl+pagedown"):
            self.ignore_scroll_event(event)

    def remember_reply(self, messages: list[dict[str, Any]], *, replace: bool = False) -> None:
        # History may split one assistant message into several records. Ignore
        # tools, reasoning, recaps and compaction, keeping only its actual text.
        replies = [(message.get("id"), "\n".join(part["text"] for part in message["content"] if part["type"] == "text"))
                   for message in messages if message["role"] == "assistant"
                   and not message.get("recap") and message.get("compaction") is None]
        replies = [(id, text) for id, text in replies if text.strip()]
        if replies:
            self.quote_source = "\n".join(text for id, text in replies if id == replies[-1][0])
        elif replace:
            self.quote_source = ""

    def register_message(self, message: dict[str, Any], widget: Widget, content: list[dict] | None = None) -> None:
        """Keep accepted live messages in the same bounded window as history."""
        self.remember_reply([message])
        if not message.get("id"):
            return
        record = {"id": message["id"], "role": message["role"],
                  "content": content if content is not None else [part for part in message["content"] if part["type"] == "text"]}
        if message.get("metadata") is not None:
            record["metadata"] = message["metadata"]
        if message.get("recap"):
            record["recap"] = True
        if message.get("objective_plan"):
            record["objective_plan"] = True
        if "objective_stack" in message:
            record["objective_stack"] = message["objective_stack"]
        if message.get("images"):
            record["images"] = message["images"]
        self.message_ids[widget] = record["id"]
        self.rendered.append((record, widget))
        self._grow_extent()
        self.call_later(self.trim_live_window)

    def _grow_extent(self):
        if "total" in self.window.page:
            self.window.page["total"] += 1
            self.window.page["end"] = self.window.page["total"]

    def register_activity(self, drawer):
        if not drawer.group_id or any(widget is drawer for _, widget in self.rendered):
            return
        self.rendered.append(({"id": drawer.group_id, "role": "activity", "content": []}, drawer))
        self._grow_extent()

    async def trim_live_window(self) -> None:
        if self.replacing or not self.window.latest:
            return
        visible: dict[str, Widget] = {}
        for widget in self.children:
            if widget in self.message_ids:
                visible.setdefault(self.message_ids[widget], widget)
        if not visible:
            return
        ids = list(visible)
        if len(ids) > self.WINDOW_SIZE:
            ids = ids[-self.WINDOW_SIZE:]
            first = visible[ids[0]]
            children = list(self.children)
            removed = [widget for widget in children[:children.index(first)]
                       if not isinstance(widget, (PermissionCard, QueuedMessage))]
            anchor = self.find_scroll_anchor([widget for widget in children if widget not in removed])
            input_number = self.scroll_input_number
            position_number = self.scroll_position_number
            follow_input_number = self.follow_input_number
            with self.app.batch_update():
                await self.remove_children(removed)
                self.rendered = [(record, widget) for record, widget in self.rendered if widget not in removed]
                if "end" in self.window.page:
                    self.window.page["start"] = self.window.page["end"] - len(self.rendered)
                self.message_ids = {widget: id for widget, id in self.message_ids.items() if widget not in removed}
                if isinstance(first, UserMessage):
                    first.remove_class("next-turn")
                self.screen._refresh_layout()
                if input_number == self.scroll_input_number and position_number == self.scroll_position_number:
                    self.restore_scroll_anchor(anchor)
                    self.follow_input_number = follow_input_number
                self.screen._refresh_layout(scroll=True)
            # Retain lazy activity records only for the surviving part of the snapshot.
            records = self.window.records
            start = next((i for i, record in enumerate(records) if record["id"] == ids[0]), len(records))
            self.window.records = records[start:]
            retained_groups = {record["id"] for record in self.window.records if record["role"] == "activity"}
            for group_id, task in list(self.window.groups.items()):
                if group_id not in retained_groups:
                    task.cancel()
            self.window.page["hasOlder"] = True
        self.window.page.update(first=ids[0], last=ids[-1])

    def boundary(self, direction: str, input_time: float | None = None) -> None:
        if self.scroll_locked or (input_time is not None and input_time <= self.ignore_scroll_before):
            return
        available = self.window.page.get("hasOlder" if direction == "before" else "hasNewer", False)
        reached = self.scroll_y <= 0 if direction == "before" else self.scroll_y >= self.max_scroll_y
        if available and reached:
            self.navigation_pending = True
            self.freeze_scroll()
            self.post_message(self.Navigate(direction))

    def _on_mouse_scroll_up(self, event: events.MouseScrollUp) -> None:
        event.prevent_default()
        if self.ignore_scroll_event(event):
            return
        super()._on_mouse_scroll_up(event)
        if not event.ctrl and not event.shift:
            self.call_after_refresh(self.boundary, "before", event.time)

    def _on_mouse_scroll_down(self, event: events.MouseScrollDown) -> None:
        event.prevent_default()
        if self.ignore_scroll_event(event):
            return
        super()._on_mouse_scroll_down(event)
        if not event.ctrl and not event.shift:
            self.call_after_refresh(self.boundary, "after", event.time)

    def action_scroll_up(self) -> None:
        if self.scroll_locked:
            return
        self.scroll_input_number += 1
        self.scroll_up()
        self.call_after_refresh(self.boundary, "before", events.Event().time)

    def action_scroll_down(self) -> None:
        if self.scroll_locked:
            return
        self.scroll_input_number += 1
        self.scroll_down()
        self.call_after_refresh(self.boundary, "after", events.Event().time)

    def action_page_up(self) -> None:
        if self.scroll_locked:
            return
        self.scroll_input_number += 1
        self.scroll_page_up()
        self.call_after_refresh(self.boundary, "before", events.Event().time)

    def action_page_down(self) -> None:
        if self.scroll_locked:
            return
        self.scroll_input_number += 1
        self.scroll_page_down()
        self.call_after_refresh(self.boundary, "after", events.Event().time)

    def action_scroll_home(self) -> None:
        if not self.scroll_locked:
            self.scroll_input_number += 1
            if self.scroll_extent() is not None:
                self.seek(0)
            else:
                super().action_scroll_home()

    def action_scroll_end(self) -> None:
        if not self.scroll_locked:
            self.scroll_input_number += 1
            if self.scroll_extent() is not None:
                self.seek(self.window.page["total"])
            else:
                super().action_scroll_end()

    def _on_mouse_scroll_left(self, event: events.MouseScrollLeft) -> None:
        event.prevent_default()
        if not self.ignore_scroll_event(event):
            super()._on_mouse_scroll_left(event)

    def _on_mouse_scroll_right(self, event: events.MouseScrollRight) -> None:
        event.prevent_default()
        if not self.ignore_scroll_event(event):
            super()._on_mouse_scroll_right(event)

    def close_activities(self) -> None:
        for drawer in self.query(TurnActivity):
            drawer.body.display = False
            drawer.toggle.expanded = False
            drawer.update_refusal_visibility()
            drawer.toggle.refresh()
        for drawer in self.query(Collapsible):
            drawer.collapsed = True

    async def purge_activities(self, exclude: HistoryActivity | None = None) -> None:
        self.close_activities()
        self.window.clear_activity_cache()
        cancelled = self.workers.cancel_group(self, "history-activity")
        # Drain in-progress population before deleting children, so a completed
        # response cannot remount the old contents after the purge.
        if cancelled:
            await asyncio.gather(*(worker.wait() for worker in cancelled), return_exceptions=True)
        for record, _ in self.rendered:
            if record["role"] == "activity":
                record.pop("activities", None)
        for drawer in self.query(HistoryActivity):
            if drawer is exclude:
                continue
            drawer.pending.clear()
            await drawer.body.remove_children()
            await drawer.refusals.remove_children()
            drawer.refused_views.clear()
            drawer.update_refusal_visibility()
            drawer.built = False
            drawer.loading = False

    @staticmethod
    def same_record(old: dict[str, Any], new: dict[str, Any]) -> bool:
        if old == new:
            return True
        # Summary-only snapshots must retain fetched drawer contents when metadata is unchanged.
        return (old.get("role") == new.get("role") == "activity" and "activities" not in new
                and all(item["kind"] == "reasoning" or "result" in item for item in old.get("activities", []))
                and {key: value for key, value in old.items() if key != "activities"} == new)

    async def replace(self, result: dict[str, Any], direction: str = "latest", *, collapse: bool = False,
                      preview: tuple[UserMessage, HistoryActivity] | None = None) -> None:
        if direction != "at":
            self.seek_position = None
        self.follow_input_number = None
        self.history_layout.scroll_anchor = None
        self.replacing = True
        self.freeze_scroll()
        staged: list[Widget] = []
        committed = False
        positioned = asyncio.get_running_loop().create_future()
        try:
            same_session = self.window.session_id == result["session"]["id"]
            previous = list(self.rendered) if same_session else []
            rendered: list[tuple[dict[str, Any], Widget]] = []
            for record_index, record in enumerate(result["messages"]):
                match = next(((old, widget) for old, widget in previous
                              if widget.is_mounted and self.same_record(old, record)), None)
                if match is not None:
                    previous.remove(match)
                    if "activities" in match[0] and "activities" not in record:
                        record["activities"] = match[0]["activities"]
                    rendered.append((record, match[1]))
                    continue
                if record["role"] == "activity":
                    rendered.append((record, HistoryActivity(record)))
                    continue
                text = "\n".join(part["text"] for part in record["content"] if part["type"] == "text")
                if record.get("recap"):
                    rendered.append((record, ConversationRecap(text)))
                elif record.get("compaction") is not None:
                    rendered.append((record, HistoryCompaction(text)))
                elif record["role"] == "user":
                    widget = UserMessage(text, (record.get("metadata") or {}).get("mode", result["session"].get("mode", "sane")), record.get("images", ()))
                    metadata = record.get("metadata") or {}
                    widget.set_class(bool(metadata.get("sane_request") or metadata.get("implementation_request")), "sane-request")
                    rendered.append((record, widget))
                elif record["role"] == "assistant" and text.strip():
                    last_text = not any(later["id"] == record["id"] and later["role"] == "assistant"
                                        for later in result["messages"][record_index + 1:])
                    rendered.append((record, ImplementationPlan(text, message_id=record.get("id", "")) if record.get("implementation_plan") else
                        ObjectivePlan(text, record.get("objective_stack", ""), record.get("id", "")) if record.get("objective_plan")
                        else ResponseMarkdown(text, classes="chat-message", generation=record.get("metadata") if last_text else None)))
            widgets = self.objective_widgets(rendered, result["session"]["id"])
            seen_activity = False
            for record, widget in rendered:
                if record["role"] == "user" and not record.get("recap"):
                    seen_activity = False
                elif isinstance(widget, HistoryActivity):
                    widget.activity_separator.display = seen_activity
                    seen_activity = True
            if preview is not None:
                widgets.extend(preview)
            elif (not result.get("page", {}).get("hasNewer", False) and widgets
                   and (isinstance(widgets[-1], GenerationFooter)
                        or isinstance(widgets[-1], ResponseMarkdown) and widgets[-1].generation is not None)):
                # Completion may have arrived while the user was on an older page.
                self.clear_generation_footer()
            if self.live_footer is not None:
                widgets.append(self.live_footer)
            staged = [widget for widget in widgets if not widget.is_mounted]
            # Mount and parse off-layout while the previous page remains visible.
            for widget in staged:
                widget.display = False
            if staged:
                await self.mount_all(staged)
            anchor = self.find_scroll_anchor(widgets) if direction != "latest" and preview is None else None
            self.workers.cancel_group(self, "history-activity")
            with self.app.batch_update():
                await self.remove_children([child for child in self.children
                                             if child not in widgets and not isinstance(child, (PermissionCard, QueuedMessage))])
                for index, widget in enumerate(widgets):
                    self.move_child(widget, before=index)
                    widget.display = widget.show_in_history if isinstance(widget, ObjectivePlan) else True
                    if widget is self.live_footer:
                        widget.display = preview is not None or not result.get("page", {}).get("hasNewer", False)
                    if isinstance(widget, UserMessage):
                        previous = widgets[index - 1] if index else None
                        has_footer = (isinstance(previous, GenerationFooter)
                                      or isinstance(previous, ResponseMarkdown) and previous.generation is not None)
                        widget.set_class(index > 0 and not has_footer, "next-turn")
                if collapse:
                    self.close_activities()
                self.rendered = rendered
                self.message_ids = {widget: record["id"] for record, widget in rendered
                                    if record["role"] in ("user", "assistant") and not record.get("compaction")
                                    and not isinstance(widget, HistoryCompaction)}
                self.window.install(result)
                if not same_session or self.window.latest:
                    self.remember_reply(result["messages"], replace=True)
                for queued in self.query(QueuedMessage):
                    queued.display = self.window.latest
                self._clear_arrangement_cache()  # The last-turn extent depends on hasNewer.
                # Reflow and anchor before painting, avoiding a frame at the old offset.
                # Confine Textual's internal layout hook to this atomic swap.
                self.screen._refresh_layout()
                self.position_page(direction, anchor, positioned)
                self.screen._refresh_layout(scroll=True)
                committed = True
            await positioned
        finally:
            if not positioned.done():
                positioned.cancel()
            if not committed:
                await self.remove_children([widget for widget in staged if widget.is_mounted])
            self.replacing = False
            self.navigation_pending = False
            self.ignore_scroll_before = events.Event().time
        self.sync_objective_plans()

    def find_scroll_anchor(self, widgets: list[Widget]) -> tuple[Widget, int] | None:
        retained = [widget for _, widget in self.rendered
                    if widget in widgets and widget.is_mounted and widget.region.height]
        if not retained:
            return None
        top = self.content_region.y
        visible = [widget for widget in retained if widget.region.bottom > top and widget.region.y < self.content_region.bottom]
        widget = min(visible or retained, key=lambda item: abs(item.region.y - top))
        return widget, widget.region.y - top

    def position_page(self, direction: str, anchor: tuple[Widget, int] | None,
                      positioned: asyncio.Future[None]) -> None:
        if positioned.done():
            return
        try:
            if direction == "at" and self.seek_position is not None:
                if self.window.latest and self.seek_position >= self.window.page["total"] - 1:
                    self.scroll_end(animate=False, immediate=True, force=True)
                else:
                    self._seek_loaded(self.seek_position)
                self.seek_position = None
            elif anchor is not None and anchor[0].is_attached:
                self.restore_scroll_anchor(anchor)
            elif direction == "after":
                self.scroll_home(animate=False, immediate=True, force=True)
            else:
                self.scroll_end(animate=False, immediate=True, force=True)
        except Exception as error:
            positioned.set_exception(error)
            return
        # Keep inputs blocked until the repositioned layout has actually painted.
        self.call_after_refresh(self.finish_positioning, positioned)
        for drawer in self.query(HistoryActivity):
            if drawer.toggle.expanded and not drawer.built and not drawer.loading:
                drawer.loading = True
                drawer.post_message(HistoryActivity.Requested(drawer))

    def restore_scroll_anchor(self, anchor: tuple[Widget, int] | None) -> None:
        if anchor is not None and anchor[0].is_attached:
            widget, offset = anchor
            self.scroll_to(y=widget.virtual_region.y - offset,
                           animate=False, immediate=True, force=True)

    def finish_positioning(self, positioned: asyncio.Future[None]) -> None:
        if not positioned.done():
            positioned.set_result(None)

    @work(group="history-activity")
    async def on_history_activity_requested(self, event: HistoryActivity.Requested) -> None:
        event.stop()
        drawer = event.drawer
        generation = self.window.generation
        try:
            record = await self.window.activities(self.send, drawer.group_id)
            if generation == self.window.generation and drawer.is_mounted:
                await drawer.populate(record)
        except (OSError, ValueError) as error:
            if drawer.is_mounted:
                await drawer.body.remove_children()
                await drawer.body.mount(Static(Text(f"Could not load activity: {error}. Close and reopen to retry.")))
        finally:
            drawer.loading = False

    @work(group="history-activity")
    async def on_activity_detail_requested(self, event: ActivityDetailRequested) -> None:
        event.stop()
        view = event.view
        generation = self.window.generation
        try:
            detail = await self.window.activity_detail(self.send, view.group_id, view.activity_index)
            if generation == self.window.generation and view.is_attached:
                await view.load_detail(detail)
        except (OSError, ValueError) as error:
            if view.is_attached:
                await view.fail_detail(str(error))
        finally:
            view.detail_loading = False
