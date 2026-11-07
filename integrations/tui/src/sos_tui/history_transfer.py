"""Validate and assemble one ordered session history transfer, message by message."""

import json
from typing import Any


class HistoryTransfer:
    def __init__(self, event: dict[str, Any]) -> None:
        self.request_id = event["requestId"]
        self.session_id = event["sessionId"]
        self.session = event["session"]
        self.page = event.get("page", {})
        self.purpose = event.get("purpose", "page")
        self.group_id = event.get("groupId", "")
        self.activity_index = event.get("activityIndex")
        self.count = event["messageCount"]
        if type(self.count) is not int or self.count < 0 or self.session.get("id") != self.session_id:
            raise ValueError("Invalid session history header")
        self.messages: list[dict[str, Any]] = []
        self.fragments: list[str] = []

    def accept(self, event: dict[str, Any]) -> dict[str, Any] | None:
        if event.get("requestId") != self.request_id or event.get("sessionId") != self.session_id:
            raise ValueError("Mismatched session history transfer")
        if event["type"] == "session.attached":
            if (self.fragments or len(self.messages) != self.count
                    or type(event.get("messageCount")) is not int or event["messageCount"] != self.count):
                raise ValueError("Incomplete session history transfer")
            return {"session": self.session, "messages": self.messages, "page": self.page,
                    "purpose": self.purpose, "groupId": self.group_id, "activityIndex": self.activity_index}
        if (event["type"] != "session.history_chunk" or len(self.messages) >= self.count
                or type(event.get("messageIndex")) is not int or event["messageIndex"] != len(self.messages)
                or type(event.get("partIndex")) is not int or event["partIndex"] != len(self.fragments)
                or not isinstance(event.get("data"), str) or not event["data"]
                or type(event.get("last")) is not bool):
            raise ValueError("Invalid or out-of-order session history fragment")
        self.fragments.append(event["data"])
        if event["last"]:
            message = json.loads("".join(self.fragments))
            if not isinstance(message, dict) or not isinstance(message.get("content"), list) or message.get("role") not in (
                    "system", "user", "assistant", "tool", "activity"):
                raise ValueError("Invalid session history message")
            self.messages.append(message)
            self.fragments.clear()
        return None
