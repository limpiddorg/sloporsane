"""One history window and its in-flight transfers, independent of Textual."""

import asyncio
from collections.abc import Awaitable, Callable
from typing import Any

from .history_transfer import HistoryTransfer


class HistoryWindow:
    def __init__(self) -> None:
        self.session_id: str | None = None
        self.records: list[dict[str, Any]] = []
        self.page: dict[str, Any] = {}
        self.generation = 0
        self.sequence = 0
        self.pending: dict[str, asyncio.Future] = {}
        self.transfers: dict[str, HistoryTransfer] = {}
        self.groups: dict[str, asyncio.Task] = {}
        self.loading = False

    @property
    def latest(self) -> bool:
        return not self.page.get("hasNewer", False)

    def invalidate(self) -> None:
        self.generation += 1
        for future in self.pending.values():
            if not future.done():
                future.cancel()
        self.pending.clear()
        self.transfers.clear()
        for task in self.groups.values():
            task.cancel()
        self.groups.clear()

    def reset(self) -> None:
        self.invalidate()
        self.session_id = None
        self.records = []
        self.page = {}
        self.loading = False

    def clear_activity_cache(self) -> None:
        self.invalidate()
        for record in self.records:
            if record["role"] == "activity":
                record.pop("activities", None)

    def install(self, result: dict[str, Any]) -> None:
        self.session_id = result["session"]["id"]
        self.records = result["messages"]
        self.page = result["page"]

    def accept(self, event: dict[str, Any]) -> bool:
        request_id = event.get("requestId", "")
        if not request_id.startswith("history:"):
            return False
        future = self.pending.get(request_id)
        if future is None or future.done():
            return True  # Evicted page, reset, or cancelled activity request.
        if event["type"] == "request.failed":
            future.set_exception(ValueError(event["error"]))
        elif event["type"] == "session.attach_started":
            if request_id in self.transfers or event["sessionId"] != self.session_id:
                raise ValueError("Unexpected history transfer")
            self.transfers[request_id] = HistoryTransfer(event)
        else:
            transfer = self.transfers.get(request_id)
            if transfer is None:
                raise ValueError("Missing history transfer header")
            result = transfer.accept(event)
            if result is not None:
                future.set_result(result)
        return True

    async def request(self, send: Callable[[dict], Awaitable[None]], **fields: Any) -> dict:
        self.sequence += 1
        request_id = f"history:{self.generation}:{self.sequence}"
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = future
        try:
            await send({"requestId": request_id, "sessionId": self.session_id, **fields})
            return await future
        finally:
            self.pending.pop(request_id, None)
            self.transfers.pop(request_id, None)

    async def navigate(self, send: Callable[[dict], Awaitable[None]], direction: str, clean: bool = False,
                       anchor: str | None = None) -> dict:
        self.invalidate()
        generation = self.generation
        self.loading = True
        try:
            if anchor is None:
                anchor = self.page.get("first" if direction == "before" else "last", "")
            return await self.request(send, type="session.history", direction=direction, anchor=anchor, clean=clean)
        finally:
            if generation == self.generation:
                self.loading = False

    async def activities(self, send: Callable[[dict], Awaitable[None]], group_id: str) -> dict:
        # Live drawers may not have appeared in a window snapshot yet. Their
        # stable group ID is enough to fetch them after the next-send purge.
        record = next((item for item in self.records if item["id"] == group_id), None)
        if record is not None and "activities" in record:
            return record
        generation = self.generation
        task = self.groups.get(group_id)
        if task is None:
            task = asyncio.create_task(self.request(send, type="session.activities", groupId=group_id))
            self.groups[group_id] = task
        try:
            result = await task
            if generation != self.generation:
                raise asyncio.CancelledError
            if result["purpose"] != "activities" or result["groupId"] != group_id or len(result["messages"]) != 1:
                raise ValueError("Invalid activity response")
            loaded = result["messages"][0]
            if loaded["id"] != group_id or loaded.get("role") != "activity" or not isinstance(loaded.get("activities"), list):
                raise ValueError("Mismatched activity group")
            if record is not None:
                record.update(loaded)
                return record
            return loaded
        finally:
            if self.groups.get(group_id) is task:
                self.groups.pop(group_id, None)

    async def activity_detail(self, send: Callable[[dict], Awaitable[None]], group_id: str, index: int) -> dict:
        generation = self.generation
        result = await self.request(send, type="session.activity_detail", groupId=group_id, activityIndex=index)
        if generation != self.generation:
            raise asyncio.CancelledError
        if (result["purpose"] != "activity-detail" or result["groupId"] != group_id
                or type(result.get("activityIndex")) is not int or result["activityIndex"] != index or len(result["messages"]) != 1):
            raise ValueError("Invalid activity detail response")
        record = result["messages"][0]
        items = record.get("activities", [])
        if (record.get("id") != group_id or record.get("role") != "activity" or not isinstance(items, list)
                or len(items) != 1 or not isinstance(items[0], dict) or type(items[0].get("activityIndex")) is not int
                or items[0]["activityIndex"] != index):
            raise ValueError("Mismatched activity detail")
        return items[0]
