"""Persistent read-only side conversations, independent of the principal Activity."""

import re
from functools import partial
from typing import Any
from rich.text import Text
from textual.widgets import Static, Collapsible, Rule
from textual.worker import WorkerCancelled

from .chat_collapsible import ChatCollapsible
from .config import MISC_MD, SLOP_COLOR, MUTED_COLOR
from .history_transfer import HistoryTransfer
from .history_viewport import HistoryViewport
from .message_input import MessageInput
from .response_markdown import ResponseMarkdown
from .user_message import UserMessage

BTW_USAGE = "Use /btw {message} to start a new btw conversation or /btw {id} {message} to continue one"


def parse_btw(text: str) -> tuple[str, str]:
    parts = text.strip().split(maxsplit=1)
    if len(parts) != 2:
        raise ValueError(BTW_USAGE)
    body = parts[1].strip()
    first, *rest = body.split(maxsplit=1)
    if re.fullmatch(r"btw-[1-9][0-9]*", first):
        if not rest or not rest[0].strip():
            raise ValueError(BTW_USAGE)
        return first, rest[0]
    return "", body


class BtwUserMessage(UserMessage):
    def compose(self):
        for widget in super().compose():
            if widget.has_class("mode-band"):
                widget.styles.background = MISC_MD
            yield widget


class BtwPanel(ChatCollapsible):
    DEFAULT_CSS = """
    BtwPanel { height: auto; margin: 0 0 0 2; padding: 0; }
    BtwPanel.standalone { margin-left: 0; }
    BtwPanel > CollapsibleTitle,
    BtwPanel > CollapsibleTitle:hover,
    BtwPanel > CollapsibleTitle:focus { padding: 0; color: $misc-md; }
    BtwPanel > Contents { padding: 0 0 0 2; }
    BtwPanel ResponseMarkdown { margin: 0; padding: 0; }
    BtwPanel .btw-status, BtwPanel .btw-reference { height: auto; margin: 0; }
    BtwPanel .btw-reference { text-align: right; }
    BtwPanel Rule.btw-boundary { height: 1; margin: 0; color: $misc-md; }
    """

    def __init__(self, branch: dict[str, Any], messages: list, collapsed: bool = False):
        self.branch_id = branch["id"]
        self.record = None
        super().__init__(title=self.branch_id, collapsed=collapsed,
                         classes="standalone" if branch.get("standalone", False) else "",
                         collapsed_symbol="▸", expanded_symbol="▾")

    async def update_conversation(self, branch: dict[str, Any], messages: list) -> None:
        record = (branch, messages)
        if self.record == record:
            return
        contents = self.query_one(Collapsible.Contents)
        await contents.remove_children()
        await contents.mount(Rule(line_style="solid", classes="btw-boundary"))
        for message in messages:
            text = "\n".join(part["text"] for part in message["content"] if part["type"] == "text")
            if message["role"] == "user":
                await contents.mount(BtwUserMessage(text, "sane"))
            elif message["role"] == "assistant":
                reference = Text(self.branch_id, style=MISC_MD)
                metadata = message.get("metadata")
                await contents.mount(ResponseMarkdown(text, generation=metadata, footer_prefix=reference))
                if metadata is None:
                    await contents.mount(Static(reference, classes="btw-reference"))
        status = branch.get("error") or ("Working…" if branch["status"] == "running" else "")
        notice = Static(Text(status, style=f"bold {SLOP_COLOR}" if branch.get("error") else MUTED_COLOR),
                        classes="btw-status")
        notice.display = bool(status)
        await contents.mount(notice, Rule(line_style="solid", classes="btw-boundary"))
        self.record = record

    def retry(self, retry: dict) -> None:
        text = "Working…"
        if retry["attempt"]:
            state = f'Retrying in {retry["seconds"]}s' if retry["seconds"] else "Reconnecting…"
            cause = retry.get("error") or retry["reason"]
            text = f'{cause}\nRetry {retry["attempt"]}/{retry["limit"]} · {state}'
        status = self.query(".btw-status").first() if self.query(".btw-status") else None
        if status is not None:
            status.update(Text(text, style=f"bold {SLOP_COLOR}" if retry["attempt"] else MUTED_COLOR))


class BtwConversations:
    def __init__(self, app):
        self.app = app
        self.records: dict[str, tuple[dict, list]] = {}
        self.panels: dict[str, BtwPanel] = {}
        self.expanded: set[str] = set()
        self.pending: dict[str, str] = {}
        self.transfers: dict[str, tuple[HistoryTransfer, dict]] = {}
        self.retries: dict[str, dict] = {}
        self.open_pending: set[str] = set()

    def capture(self):
        for id, panel in self.panels.items():
            if panel.parent is not None:
                if panel.collapsed:
                    self.expanded.discard(id)
                else:
                    self.expanded.add(id)

    def collapse(self):
        viewport = self.app.query_one(HistoryViewport)
        if viewport.history_layout.scroll_anchor is not None:
            viewport.history_layout.scroll_anchor = None
            viewport.refresh(layout=True)
        self.app.workers.cancel_group(self.app, "btw-open")
        self.expanded.clear()
        self.open_pending.clear()
        for panel in self.panels.values():
            panel.collapsed = True

    def pause(self):
        self.capture()
        self.app.workers.cancel_group(self.app, "btw-render")

    def reset(self):
        self.app.workers.cancel_group(self.app, "btw-render")
        self.app.workers.cancel_group(self.app, "btw-open")
        self.records.clear()
        self.panels.clear()
        self.expanded.clear()
        self.transfers.clear()
        self.retries.clear()
        self.open_pending.clear()

    def disconnected(self):
        for draft in reversed(list(self.pending.values())):
            self.app.prepend_draft(draft)
        self.pending.clear()
        self.transfers.clear()
        self.retries.clear()
        for id, (branch, messages) in list(self.records.items()):
            if branch["status"] == "running":
                self.records[id] = dict(branch, status="interrupted", error="BTW interrupted: daemon disconnected."), messages
        self.render()

    async def refresh(self):
        if self.app.session_id:
            await self.app.client.send({"type": "btw.list", "sessionId": self.app.session_id,
                                        "requestId": "btw-list"})

    async def submit(self, text: str):
        entry = self.app.query_one(MessageInput)
        try:
            branch, message = parse_btw(text)
            if entry.images:
                raise ValueError("BTW accepts text only; images in the principal context are inherited.")
            if not self.app.session_id:
                raise ValueError("Start a principal conversation before using /btw")
        except ValueError as error:
            self.app.notify(str(error), title="BTW", severity="error", markup=False)
            return
        self.app.request_number += 1
        request = f"btw-send-{self.app.request_number}"
        self.pending[request] = text
        entry.text = ""
        try:
            await self.app.client.send({"type": "btw.send", "requestId": request,
                                        "sessionId": self.app.session_id, "branchId": branch, "text": message})
        except (OSError, ValueError) as error:
            self.receive({"type": "request.failed", "requestId": request, "error": str(error)})

    def receive(self, event: dict) -> bool:
        kind, request = event["type"], event.get("requestId", "")
        if kind == "request.failed" and request in self.pending:
            draft = self.pending.pop(request)
            self.app.prepend_draft(draft)
            self.app.notify(event["error"], title="BTW", severity="error", markup=False)
            return True
        if not kind.startswith("btw."):
            return False
        if event.get("sessionId") != self.app.session_id:
            self.pending.pop(request, None)
            return True
        if kind == "btw.accepted":
            sent = self.pending.pop(request, None)
            entry = self.app.query_one(MessageInput)
            if sent is not None and not entry.text:
                entry.text = f'/btw {event["branchId"]} '
                entry.move_cursor((0, len(entry.text)))
                entry.update_suggestions()
            self.collapse()
            self.expanded.add(event["branchId"])
            self.open_pending.add(event["branchId"])
            panel = self.panels.get(event["branchId"])
            if panel is not None:
                panel.collapsed = False
        elif kind == "btw.started":
            self.transfers[request] = (HistoryTransfer(event), event["branch"])
        elif kind in ("btw.chunk", "btw.finished"):
            transfer, branch = self.transfers[request]
            translated = dict(event, type="session.history_chunk" if kind == "btw.chunk" else "session.attached")
            result = transfer.accept(translated)
            if result is not None:
                del self.transfers[request]
                self.records[branch["id"]] = branch, result["messages"]
                self.retries.pop(branch["id"], None)
                if branch["id"] in self.open_pending:
                    self.open_pending.discard(branch["id"])
                    self.app.run_worker(partial(self.open_branch, branch), group="btw-open", exclusive=True)
                else:
                    self.render()
        elif kind == "btw.retry":
            self.retries[event["branchId"]] = event["retry"]
            panel = self.panels.get(event["branchId"])
            if panel is not None and panel.parent is not None:
                panel.retry(event["retry"])
        return True

    def render(self):
        return self.app.run_worker(self._render, group="btw-render", exclusive=True)

    async def open_branch(self, branch: dict):
        viewport = self.app.query_one(HistoryViewport)
        if self.find_anchor(branch, viewport) is None:
            if not await self.app.load_history_page("before", anchor=branch.get("pageAnchor") or branch["anchor"]):
                return
        try:
            await self.render().wait()
        except WorkerCancelled:
            return
        panel = self.panels.get(branch["id"])
        if panel is not None and panel.parent is not None:
            viewport.scroll_input_number += 1
            viewport.follow_input_number = None
            panel.call_after_refresh(self.prepare_scroll, branch["id"], viewport.scroll_input_number)

    def scroll_target(self, branch_id: str):
        panel = self.panels.get(branch_id)
        if panel is None or panel.parent is None or panel.collapsed:
            return None
        users = list(panel.query(BtwUserMessage))
        if len(users) > 1:
            return users[-1]
        branch = self.records[branch_id][0]
        if branch.get("standalone", False):
            return panel
        viewport = self.app.query_one(HistoryViewport)
        return next((widget for widget, id in viewport.message_ids.items()
                     if id == branch["anchor"] and isinstance(widget, UserMessage)), None)

    def prepare_scroll(self, branch_id: str, input_number: int):
        viewport = self.app.query_one(HistoryViewport)
        if input_number != viewport.scroll_input_number or self.scroll_target(branch_id) is None:
            return
        viewport.history_layout.scroll_anchor = partial(self.scroll_target, branch_id)
        viewport.refresh(layout=True)
        viewport.call_after_refresh(self.position_scroll, branch_id, input_number)

    def position_scroll(self, branch_id: str, input_number: int):
        viewport = self.app.query_one(HistoryViewport)
        target = self.scroll_target(branch_id)
        if target is None or input_number != viewport.scroll_input_number or viewport.scroll_locked:
            return
        viewport.scroll_position_number += 1
        viewport.scroll_to(y=viewport.scroll_y + target.region.y - viewport.content_region.y,
                           animate=False, immediate=True, force=True)

    async def _render(self):
        if self.app.history_rendering:
            return
        viewport = self.app.query_one(HistoryViewport)
        self.capture()
        previous = {}
        for id, (branch, messages) in sorted(self.records.items(), key=lambda item: int(item[0].removeprefix("btw-"))):
            anchor = self.find_anchor(branch, viewport)
            panel = self.panels.get(id)
            if anchor is None:
                if panel is not None and panel.parent is not None:
                    await panel.remove()
                continue
            if panel is None or panel.parent is None:
                panel = BtwPanel(branch, messages, collapsed=id not in self.expanded)
                self.panels[id] = panel
                await viewport.mount(panel, after=previous.get(branch["anchor"], anchor))
            previous[branch["anchor"]] = panel
            with self.app.batch_update():
                await panel.update_conversation(branch, messages)
            if id in self.retries:
                panel.retry(self.retries[id])

    @staticmethod
    def find_anchor(branch: dict, viewport: HistoryViewport):
        if branch.get("standalone", False):
            # Idle branches follow the last display record, including Activities
            # with no assistant text. Resolve the same persisted ID after resume.
            children = list(viewport.children)
            records = {widget: record["id"] for record, widget in viewport.rendered}
            for index in range(len(children) - 1, -1, -1):
                widget = children[index]
                if (viewport.message_ids.get(widget) == branch["anchor"]
                        or getattr(widget, "group_id", None) == branch["anchor"]
                        or records.get(widget) == branch["anchor"]):
                    if index + 1 < len(children) and children[index + 1] is viewport.live_footer:
                        return viewport.live_footer
                    return widget
            return None
        return next((widget for widget, id in viewport.message_ids.items()
                     if id == branch["anchor"] and isinstance(widget, UserMessage)
                     and widget.parent is viewport), None)
