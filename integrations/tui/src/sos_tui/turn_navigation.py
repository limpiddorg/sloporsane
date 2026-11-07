"""Undo/redo requests replace history only after a complete daemon transfer."""

import base64

from .history_transfer import HistoryTransfer
from .images import snapshot
from .message_input import MessageInput


class TurnNavigation:
    def __init__(self, app):
        self.app = app
        self.request_id = None
        self.transfer = None
        self.redo = False

    async def request(self, redo: bool) -> None:
        app = self.app
        command = "/redo" if redo else "/undo"
        if self.request_id is not None:
            return
        if app.busy or app.pending_request is not None:
            await app.show_input_error(f"Wait for the current execution to finish before using {command}.", warning=True)
            return
        if app.session_id is None:
            await app.show_input_error(f"Start a conversation before using {command}.", warning=True)
            return
        app.request_number += 1
        self.request_id = str(app.request_number)
        self.redo = redo
        app.busy = True
        app.query_one(MessageInput).disabled = True
        app.set_status("Redoing…" if redo else "Undoing…")
        try:
            await app.client.send({"type": "session.redo" if redo else "session.undo", "requestId": self.request_id})
        except (OSError, ValueError) as error:
            self.receive({"type": "request.failed", "requestId": self.request_id, "error": str(error)})

    def receive(self, event: dict) -> bool:
        if self.request_id is None or event.get("requestId") != self.request_id:
            return False
        if event["type"] == "request.failed":
            self.reset()
            self.app.call_later(self.app.show_input_error, event["error"], warning=True)
        elif event["type"] == "session.attach_started":
            if self.transfer is not None or event.get("sessionId") != self.app.session_id or event.get("purpose") != "navigation":
                raise ValueError("Unexpected undo/redo history transfer")
            self.transfer = HistoryTransfer(event)
        elif event["type"] in ("session.history_chunk", "session.attached"):
            if self.transfer is None:
                raise ValueError("Undo/redo history transfer has not started")
            restored = self.transfer.accept(event)
            if restored is not None:
                self.app.run_worker(self.finish(restored), group="turn-navigation", exclusive=True)
        return True

    async def finish(self, restored: dict) -> None:
        if not restored["messages"]:
            raise ValueError("Undo/redo draft record is missing")
        draft = restored["messages"].pop(0)
        text = "" if self.redo else "".join(part.get("text", "") for part in draft["content"] if part.get("type") == "text")
        images = () if self.redo else tuple(snapshot(base64.b64decode(image["data"], validate=True),
            image["name"], image["origin"]) for image in draft.get("images", []))
        try:
            await self.app.render_restored_session(restored)
            entry = self.app.query_one(MessageInput)
            entry.text = ""
            if images:
                entry.restore_images(text, images)
            else:
                entry.text = text
            entry.move_cursor(entry.document.end)
        finally:
            self.reset()

    def reset(self) -> None:
        self.request_id = None
        self.transfer = None
        self.app.busy = False
        self.app.query_one(MessageInput).disabled = not self.app.connected
        self.app.focus_composer()
        self.app.set_status("Ready" if self.app.connected else "Disconnected")
