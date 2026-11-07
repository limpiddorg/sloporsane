"""Manual recap command, persistent card rendering and failure handling."""

import unittest
from unittest.mock import AsyncMock, patch

from textual.widgets import Static

from sos_tui.app import SosApp
from sos_tui.config import CONTENT_BACKGROUND_COLOR, MISC_MD
from sos_tui.conversation_recap import ConversationRecap
from sos_tui.history_viewport import HistoryViewport
from sos_tui.message_input import MessageInput
from sos_tui.mode_button import ModeButton
from sos_tui.user_message import UserMessage


def recap(identifier="r1"):
    return {"id": identifier, "role": "assistant", "recap": True,
            "content": [{"type": "text", "text": "The fix is ready.\nRun [tests] next."}]}


class ConversationRecapTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(80, 24)))
        self.app.connected = True
        self.app.session_id = "s"
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.chat = self.app.query_one(HistoryViewport)

    async def request(self):
        self.entry.text = "/recap"
        await self.pilot.press("enter")
        await self.pilot.pause()
        return self.app.recap_request

    async def test_command_generates_a_card_without_a_user_turn_or_activity(self):
        request = await self.request()
        self.app.client.send.assert_awaited_once_with({"type": "session.recap", "requestId": request})
        self.assertTrue(self.app.busy)
        pending = self.chat.query_one(ConversationRecap)
        self.assertIs(pending, self.app.running_recap)
        self.assertTrue(pending.loading)
        self.assertIsNotNone(pending.spinner_timer)
        self.assertFalse(self.app.query_one(ModeButton).busy)
        self.assertFalse(self.app.history_loads)
        self.assertNotIn(pending, self.chat.message_ids)
        self.assertIsNone(self.app.pending_turn)
        self.assertIsNone(self.app.current_activity)
        self.assertEqual(self.entry.text, "")
        self.app.handle_event({"type": "recap.started", "requestId": request})
        self.entry.text = "Unsent draft"
        self.app.handle_event({"type": "recap.completed", "requestId": request, "messages": [recap()]})
        await self.pilot.pause()
        self.assertFalse(self.app.busy)
        self.assertIsNone(self.app.recap_request)
        self.assertEqual(self.entry.text, "Unsent draft")
        self.assertTrue(self.entry.has_focus)
        card = self.chat.query_one(ConversationRecap)
        self.assertIs(card, pending)
        self.assertFalse(card.loading)
        self.assertIsNone(card.spinner_timer)
        self.assertIsNone(self.app.running_recap)
        self.assertEqual(card.styles.background.hex, CONTENT_BACKGROUND_COLOR.upper())
        band = card.query_one(".recap-band")
        self.assertEqual(band.styles.border_left[0], "outer")
        self.assertEqual(band.styles.border_left[1].hex, MISC_MD)
        self.assertEqual(band.render_lines(band.region.reset_offset)[0].text, "▌")
        self.assertEqual(card.query_one(".recap-text").styles.color.hex, MISC_MD)
        heading = card.query_one(".recap-heading", Static)
        self.assertEqual(heading.styles.color.hex, MISC_MD)
        self.assertTrue(heading.styles.text_style.bold)
        self.assertEqual(str(heading.render()), "Conversation Recap")
        self.assertIn("[tests]", str(card.query_one(".recap-text", Static).render()))
        self.assertTrue(self.chat.rendered[-1][0]["recap"])
        self.assertEqual(self.app.history_window.page["last"], "r1")

    async def test_multiple_recaps_render_identically_after_history_reload(self):
        records = [recap("r1"), recap("r2")]
        await self.chat.replace({"session": {"id": "s"}, "messages": records,
                                 "page": {"first": "r1", "last": "r2", "hasOlder": False, "hasNewer": False}})
        await self.pilot.pause()
        self.assertEqual(len(self.chat.query(ConversationRecap)), 2)
        self.assertEqual(list(self.chat.message_ids.values()), ["r1", "r2"])
        self.assertFalse(self.chat.query("ResponseMarkdown"))

    async def test_failure_and_cancellation_leave_no_recap_and_keep_draft(self):
        for outcome in ("request.failed", "recap.cancelled"):
            with self.subTest(outcome=outcome):
                request = await self.request()
                pending = self.app.running_recap
                self.entry.text = "Draft"
                with patch.object(self.app, "append_chat") as append_chat:
                    self.app.handle_event({"type": outcome, "requestId": request, "error": "Unavailable"})
                    if outcome == "request.failed":
                        append_chat.assert_not_called()
                await self.pilot.pause()
                self.assertFalse(self.app.busy)
                self.assertEqual(self.entry.text, "Draft")
                self.assertFalse(self.chat.query(ConversationRecap))
                self.assertIsNone(pending.spinner_timer)
                self.assertIsNone(self.app.running_recap)

    async def test_empty_session_and_busy_guard_do_not_send(self):
        self.app.session_id = None
        await self.request()
        self.app.client.send.assert_not_awaited()
        self.app.session_id = "s"
        self.app.busy = True
        await self.request()
        self.app.client.send.assert_not_awaited()

    async def test_send_failure_releases_busy_state(self):
        self.app.client.send.side_effect = OSError("Disconnected")
        await self.request()
        self.assertFalse(self.app.busy)
        self.assertIsNone(self.app.recap_request)

    async def test_escape_twice_requests_cancellation(self):
        await self.request()
        await self.pilot.press("escape")
        self.assertEqual(self.app.client.send.await_count, 1)
        await self.pilot.press("escape")
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "execution.cancel")

    async def test_queued_message_waits_for_recap_and_draft_survives(self):
        request = await self.request()
        pending = self.chat.query_one(ConversationRecap)
        self.entry.text = "Continue when ready"
        await self.pilot.press("enter")
        self.assertIsNotNone(self.app.queued_message)
        children = list(self.chat.children)
        self.assertLess(children.index(pending), children.index(self.app.queued_message))
        self.assertTrue(pending.loading)
        self.assertFalse(self.app.query_one(ModeButton).busy)
        self.entry.text = "A later draft"
        self.app.handle_event({"type": "recap.completed", "requestId": request, "messages": [recap()]})
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "message.send")
        self.assertEqual(self.app.client.send.call_args.args[0]["text"], "Continue when ready")
        self.assertEqual(self.entry.text, "A later draft")
        self.assertEqual(len(self.chat.query(ConversationRecap)), 1)
        self.assertIs(self.chat.query_one(ConversationRecap), pending)
        self.assertFalse(pending.loading)
        children = list(self.chat.children)
        self.assertLess(children.index(pending), children.index(self.chat.query_one(UserMessage)))
        self.assertTrue(self.app.query_one(ModeButton).busy)

    async def test_loading_spinner_animates_inside_card_only(self):
        await self.request()
        card = self.app.running_recap
        card.spinner_timer.pause()
        body = card.query_one(".recap-text", Static)
        before = str(body.render())
        card.advance_spinner()
        self.assertNotEqual(str(body.render()), before)
        band = card.query_one(".recap-band")
        self.assertEqual(band.styles.border_left[0], "outer")
        self.assertEqual(band.styles.border_left[1].hex, MISC_MD)
        self.assertEqual(band.render_lines(band.region.reset_offset)[0].text, "▌")
        self.assertEqual(card.query_one(".recap-text").styles.color.hex, MISC_MD)
        self.assertEqual(str(card.query_one(".recap-heading", Static).render()), "Conversation Recap")
        self.assertFalse(self.app.query_one(ModeButton).busy)

    async def test_card_follows_previous_turn_footer(self):
        self.chat.start_generation_footer({"model": "test", "reasoning": "low", "duration_ms": 100})
        footer = self.chat.live_footer
        footer.finish_generation()
        self.app.display_message(recap())
        await self.pilot.pause()
        children = list(self.chat.children)
        self.assertLess(children.index(footer), children.index(self.chat.query_one(ConversationRecap)))
