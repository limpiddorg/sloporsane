import unittest
from unittest.mock import AsyncMock, patch

from textual.widgets import Static
from sos_tui.app import SosApp
from sos_tui.history_viewport import HistoryViewport
from sos_tui.message_input import MessageInput
from sos_tui.user_message import UserMessage


class GenerationRetryTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 30)))
        self.app.connected = True
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.send = self.enterContext(patch.object(self.app.client, "send", new_callable=AsyncMock))
        self.entry.text = "Keep this message"
        await self.app.on_message_input_submitted(MessageInput.Submitted(self.entry.text))
        request = self.send.call_args.args[0]
        self.app.handle_event({"type": "message.accepted", "requestId": request["requestId"],
                               "session": {"id": "test", "title": "Retry test"}})
        self.activity = self.app.running_activity
        await self.pilot.pause()

    def retry(self, attempt, seconds, reason="Connection problem", error=""):
        self.app.handle_event({"type": "execution.retry", "retry": {
            "attempt": attempt, "limit": 10, "seconds": seconds, "reason": reason, "error": error}})

    async def test_countdown_is_above_activity_and_updates_in_place(self):
        self.retry(2, 2)
        await self.pilot.pause()
        notice = self.app.retry_notice
        self.assertIsNotNone(notice)
        children = list(self.app.query_one(HistoryViewport).children)
        user = self.app.query_one(UserMessage)
        self.assertLess(children.index(user), children.index(notice))
        self.assertEqual(children.index(notice) + 1, children.index(self.activity))
        self.assertEqual(notice.region.bottom, self.activity.region.y)
        self.assertEqual(notice.styles.margin.bottom, 0)
        self.assertIn("Connection problem\nRetry 2/10 · Retrying in 2s", str(notice.render()))
        self.retry(2, 1)
        self.assertIs(self.app.retry_notice, notice)
        self.assertIn("Retrying in 1s", str(notice.render()))
        self.retry(2, 0)
        self.assertIn("Reconnecting…", str(notice.render()))
        self.assertTrue(self.app.busy)
        self.assertIsNotNone(self.activity.toggle.started_at)
        self.assertEqual(self.entry.text, "")
        self.assertEqual(self.send.call_count, 1)

    async def test_recovery_removes_notice_without_stopping_generation(self):
        self.retry(1, 0)
        await self.pilot.pause()
        self.retry(0, 0)
        await self.pilot.pause()
        self.assertIsNone(self.app.retry_notice)
        self.assertEqual(len(self.app.query(".generation-retry")), 0)
        self.assertTrue(self.app.busy)
        self.assertIsNotNone(self.activity.toggle.started_at)

    async def test_provider_error_is_visible_above_retry_and_can_change(self):
        self.retry(2, 2, "Service temporarily unavailable", "OpenAI HTTP error 503")
        await self.pilot.pause()
        notice = self.app.retry_notice
        self.assertIn("OpenAI HTTP error 503\nRetry 2/10 · Retrying in 2s", str(notice.render()))
        self.assertGreaterEqual(notice.region.height, 2)
        self.retry(3, 5, error="OpenAI connection interrupted")
        await self.pilot.pause()
        self.assertIs(self.app.retry_notice, notice)
        self.assertIn("OpenAI connection interrupted\nRetry 3/10", str(notice.render()))
        self.assertNotIn("503", str(notice.render()))

    async def test_permanent_execution_error_shows_its_cause(self):
        self.app.handle_event({"type": "execution.failed", "error": "OpenAI quota exhausted"})
        await self.pilot.pause()
        error = self.app.query_one(".generation-error", Static)
        self.assertIn("Execution error\nOpenAI quota exhausted", str(error.render()))
        self.assertGreaterEqual(error.region.height, 2)
        self.assertFalse(self.app.busy)

    async def test_history_navigation_defers_retry_rendering(self):
        self.app.history_rendering = True
        self.retry(3, 5)
        self.assertIsNone(self.app.retry_notice)
        self.assertEqual(self.app.retry_status["seconds"], 5)
        self.retry(3, 4)
        self.app.history_rendering = False
        self.app.show_generation_retry(self.app.retry_status)
        await self.pilot.pause()
        self.assertIn("Retrying in 4s", str(self.app.retry_notice.render()))

    async def test_exhaustion_is_below_activity_and_does_not_restore_sent_message(self):
        self.retry(10, 0)
        await self.pilot.pause()
        self.entry.text = "A different draft"
        error = "Generation failed after 10 retries. Your message was kept. Please try again manually."
        self.app.handle_event({"type": "execution.failed", "code": "retries_exhausted", "error": error})
        await self.pilot.pause()
        self.assertFalse(self.app.busy)
        self.assertIsNone(self.activity.toggle.started_at)
        self.assertIsNone(self.app.retry_notice)
        self.assertEqual(self.entry.text, "A different draft")
        self.assertEqual(len(self.app.query(UserMessage)), 1)
        final = [w for w in self.app.query(Static) if error in str(w.render())]
        self.assertEqual(len(final), 1)
        children = list(self.app.query_one(HistoryViewport).children)
        self.assertLess(children.index(self.activity), children.index(final[0]))
        self.assertEqual(self.activity.region.bottom, final[0].region.y)
        self.assertEqual(final[0].styles.margin.bottom, 0)
        self.assertEqual(self.send.call_count, 1)

    async def test_cancellation_clears_countdown(self):
        self.retry(3, 5)
        await self.pilot.pause()
        self.app.handle_event({"type": "execution.cancelled"})
        await self.pilot.pause()
        self.assertIsNone(self.app.retry_notice)
        self.assertIsNone(self.activity.toggle.started_at)
        self.assertEqual(self.entry.text, "")

    async def test_disconnect_clears_countdown(self):
        self.retry(3, 5)
        await self.pilot.pause()
        with patch.object(self.app.client, "connect", new_callable=AsyncMock), \
                patch.object(self.app.client, "close", new_callable=AsyncMock), \
                patch.object(self.app.client, "receive", new_callable=AsyncMock,
                             side_effect=[{"type": "integration.configured"}, OSError("Disconnected")]):
            worker = SosApp.listen(self.app)
            await worker.wait()
        await self.pilot.pause()
        self.assertIsNone(self.app.retry_notice)
