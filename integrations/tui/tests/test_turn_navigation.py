import json
import base64
import unittest
from unittest.mock import AsyncMock, patch

from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput
from sos_tui.input_error_notice import InputErrorNotice


class TurnNavigationTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 30)))
        self.app.connected = True
        self.app.session_id = "session"
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()

    async def submit(self, text):
        self.entry.text = text
        await self.pilot.press("enter")
        await self.pilot.pause()

    def transfer(self, draft):
        request = self.app.turn_navigation.request_id
        self.app.handle_event({"type": "session.attach_started", "requestId": request,
            "sessionId": "session", "session": {"id": "session", "workspace": "/work", "title": "Test"},
            "purpose": "navigation", "messageCount": 1, "page": {}})
        self.app.handle_event({"type": "session.history_chunk", "requestId": request,
            "sessionId": "session", "messageIndex": 0, "partIndex": 0,
            "last": True, "data": json.dumps(draft)})
        self.app.handle_event({"type": "session.attached", "requestId": request,
            "sessionId": "session", "messageCount": 1})

    async def test_undo_restores_draft_only_after_complete_transfer(self):
        render = self.enterContext(patch.object(self.app, "render_restored_session", new_callable=AsyncMock))
        await self.submit("/undo")
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "session.undo")
        self.assertTrue(self.entry.disabled)
        self.transfer({"id": "user", "role": "user", "content": [{"type": "text", "text": "Original request"}]})
        await self.app.workers.wait_for_complete()
        self.assertEqual(self.entry.text, "Original request")
        self.assertEqual(render.call_args.args[0]["messages"], [])
        self.assertFalse(self.app.busy)
        self.assertFalse(self.entry.disabled)

    async def test_redo_clears_draft_and_does_not_send_a_user_message(self):
        self.enterContext(patch.object(self.app, "render_restored_session", new_callable=AsyncMock))
        await self.submit("/redo")
        self.transfer({"id": "user", "role": "user", "content": [{"type": "text", "text": "Original request"}]})
        await self.app.workers.wait_for_complete()
        self.assertEqual(self.entry.text, "")
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "session.redo")

    async def test_conflict_keeps_history_and_command(self):
        render = self.enterContext(patch.object(self.app, "render_restored_session", new_callable=AsyncMock))
        await self.submit("/undo")
        self.app.handle_event({"type": "request.failed", "requestId": self.app.turn_navigation.request_id,
                               "error": "Conflict: file.txt"})
        await self.pilot.pause()
        render.assert_not_awaited()
        self.assertEqual(self.entry.text, "/undo")
        self.assertIn("file.txt", str(self.app.query_one(InputErrorNotice).render()))
        self.assertFalse(self.entry.disabled)

    async def test_undo_restores_image_payload_without_duplicating_text(self):
        self.enterContext(patch.object(self.app, "render_restored_session", new_callable=AsyncMock))
        pixels = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII="
        await self.submit("/undo")
        text = "Look at [Pasted Image] please"
        self.transfer({"id": "user", "role": "user", "content": [{"type": "text", "text": text}],
            "images": [{"data": pixels, "name": "pixel.png", "origin": "clipboard"}]})
        await self.app.workers.wait_for_complete()
        self.assertEqual(self.entry.text, text)
        self.assertEqual(len(self.entry.images), 1)
        self.assertEqual(self.entry.images[0].pixels, base64.b64decode(pixels))

    async def test_complete_transfer_really_replaces_viewport(self):
        self.enterContext(patch.object(self.app.btw, "refresh", new_callable=AsyncMock))
        self.app.append_chat("Old assistant", "This should disappear")
        await self.pilot.pause()
        await self.submit("/undo")
        self.transfer({"id": "user", "role": "user", "content": [{"type": "text", "text": "Restored"}]})
        await self.app.workers.wait_for_complete()
        self.assertEqual(self.entry.text, "Restored")
        self.assertFalse(self.app.busy)
        self.assertEqual(self.app.history_window.records, [])

    async def test_busy_and_empty_sessions_do_not_send_navigation(self):
        self.app.busy = True
        await self.submit("/undo")
        self.app.client.send.assert_not_awaited()
        self.app.busy = False
        self.app.session_id = None
        await self.submit("/redo")
        self.app.client.send.assert_not_awaited()
