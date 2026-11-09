"""Last-turn scroll limits and following assistant text without stealing the view."""

import asyncio
import unittest
from unittest.mock import AsyncMock, patch
from textual import events
from async_fixture import wait_until

from sos_tui.app import SosApp
from sos_tui.history_viewport import HistoryViewport
from sos_tui.message_input import MessageInput
from sos_tui.response_markdown import ResponseMarkdown
from sos_tui.user_message import UserMessage
from sos_tui.paste_preview import PastePreview


class ChatScrollTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(80, 24)))
        self.app.connected = True
        self.app.session_id = "s"
        self.app.history_window.session_id = "s"
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.viewport = self.app.query_one(HistoryViewport)
        self.sequence = 0

    def message(self, role, text):
        self.sequence += 1
        return {"id": str(self.sequence), "role": role, "content": [{"type": "text", "text": text}]}

    async def seed_history(self, turns=2):
        messages = []
        for i in range(turns):
            messages.extend([self.message("user", f"Old question {i}"),
                             self.message("assistant", "\n\n".join(f"Old answer {i}, paragraph {j}." for j in range(12)))])
        result = {"session": {"id": "s"}, "page": {"hasOlder": False, "hasNewer": False}, "messages": messages}
        await self.viewport.replace(result)
        await self.pilot.pause()
        return result

    async def submit(self, text="New question"):
        position = self.viewport.scroll_position_number
        await self.app.on_message_input_submitted(MessageInput.Submitted(text))
        await self.app.workers.wait_for_complete()
        await wait_until(self.pilot, lambda: self.viewport.scroll_position_number > position,
                         "pending turn's post-mount scroll")
        await self.pilot.pause()
        return self.app.pending_turn.user

    async def accept(self, user):
        self.app.handle_event({"type": "message.accepted", "requestId": self.app.pending_request,
                               "session": {"id": "s", "title": "Chat"}})
        self.app.display_live_messages([self.message("user", user.text)])
        await self.pilot.pause()

    async def reply(self, paragraphs=30):
        self.app.display_live_messages([self.message("assistant", "\n\n".join(f"New paragraph {i}." for i in range(paragraphs)))])
        await self.app.workers.wait_for_complete()
        await self.pilot.pause()

    async def test_page_keys_scroll_chat_while_composer_keeps_focus_and_selection(self):
        await self.seed_history()
        self.entry.text = "First line\nSecond line\nThird line"
        self.entry.move_cursor((1, 3))
        selection = self.entry.selection
        self.viewport.scroll_end(animate=False, immediate=True)
        await self.pilot.pause()
        bottom = self.viewport.scroll_y
        self.assertGreater(bottom, 0)
        await self.pilot.press("pageup")
        await self.pilot.pause()
        self.assertLess(self.viewport.scroll_y, bottom)
        middle = self.viewport.scroll_y
        await self.pilot.press("pagedown")
        await self.pilot.pause()
        self.assertGreater(self.viewport.scroll_y, middle)
        self.assertTrue(self.entry.has_focus)
        self.assertEqual(self.entry.selection, selection)
        self.assertEqual(self.entry.text, "First line\nSecond line\nThird line")

    async def test_page_keys_work_when_history_has_focus(self):
        await self.seed_history()
        self.viewport.scroll_home(animate=False, immediate=True)
        self.viewport.focus()
        await self.pilot.pause()
        await self.pilot.press("pagedown")
        await self.pilot.pause()
        middle = self.viewport.scroll_y
        self.assertGreater(middle, 0)
        await self.pilot.press("pageup")
        await self.pilot.pause()
        self.assertLess(self.viewport.scroll_y, middle)

    async def test_page_keys_respect_history_loading_lock(self):
        await self.seed_history()
        self.viewport.scroll_end(animate=False, immediate=True)
        self.viewport.set_history_loading(True)
        await self.pilot.pause()
        position = self.viewport.scroll_y
        await self.pilot.press("pageup", "pagedown")
        await self.pilot.pause()
        self.assertEqual(self.viewport.scroll_y, position)
        self.assertTrue(self.entry.has_focus)

    async def test_page_keys_in_popup_do_not_scroll_chat(self):
        await self.seed_history()
        self.viewport.scroll_end(animate=False, immediate=True)
        await self.pilot.pause()
        position = self.viewport.scroll_y
        await self.app.push_screen(PastePreview("\n".join(f"Line {i}" for i in range(100)), lambda: None))
        popup = self.app.screen.query_one(".paste-preview-content")
        popup.focus()
        await self.pilot.pause()
        await self.pilot.press("pagedown")
        await self.pilot.pause()
        self.assertGreater(popup.scroll_y, 0)
        await self.pilot.press("pageup")
        await self.pilot.pause()
        self.assertEqual(self.viewport.scroll_y, position)

    async def test_send_moves_to_top_before_acceptance_and_echo_does_not_move_it_again(self):
        await self.seed_history()
        self.viewport.scroll_home(animate=False, immediate=True)
        self.entry.text = "A multiline\ndraft\nshrinks on send"
        await self.pilot.pause()
        user = await self.submit(self.entry.text)
        self.assertEqual(self.app.client.send.await_count, 1)
        self.assertEqual(user.region.y, self.viewport.content_region.y)
        self.assertTrue(self.viewport.is_vertical_scroll_end)
        old_answer = list(self.app.query(ResponseMarkdown))[-1]
        self.assertLessEqual(old_answer.region.bottom, self.viewport.content_region.y)
        self.viewport.action_scroll_up()
        # Capture the reading position after the user's scroll has finished,
        # not an intermediate animation frame that acceptance may outlive.
        await asyncio.wait_for(self.pilot.wait_for_scheduled_animations(), timeout=2)
        await self.pilot.pause()
        self.assertLess(self.viewport.scroll_y, self.viewport.max_scroll_y)
        y = user.region.y
        await self.accept(user)
        self.assertIs(list(self.app.query(UserMessage))[-1], user)
        await wait_until(self.pilot, lambda: user.region.y == y, "accepted turn's stable reading position")
        self.assertEqual(user.region.y, y)
        self.assertEqual(len(self.app.query(UserMessage)), 3)

    async def test_send_immediately_after_recap_completion_positions_new_question(self):
        self.app.update_agent_info({"model": "test-model", "reasoning": "high"})
        await self.seed_history()
        await self.app.request_recap()
        self.app.handle_event({"type": "recap.completed", "requestId": self.app.recap_request,
                               "messages": [{**self.message("assistant", "\n".join(
                                   "Recap paragraph with enough text to wrap across several terminal columns. " * 3
                                   for _ in range(20))), "recap": True}]})
        user = await self.submit()
        self.assertEqual(user.region.y, self.viewport.content_region.y)
        self.assertTrue(self.viewport.is_vertical_scroll_end)

    async def test_response_preserves_reading_position_until_user_returns_to_bottom(self):
        await self.seed_history()
        user = await self.submit()
        await self.accept(user)
        self.viewport.scroll_home(animate=False, immediate=True)
        await self.pilot.pause()
        anchor = list(self.app.query(UserMessage))[0]
        y = anchor.region.y
        await self.reply()
        self.assertEqual(anchor.region.y, y)
        self.assertFalse(self.viewport.is_vertical_scroll_end)
        self.viewport.scroll_end(animate=False, immediate=True)
        await self.pilot.pause()
        await self.reply()
        await wait_until(self.pilot, lambda: self.viewport.is_vertical_scroll_end, "response's final follow scroll")
        self.assertTrue(self.viewport.is_vertical_scroll_end)

    async def test_activity_growth_does_not_follow_and_collapse_reduces_scroll_limit(self):
        await self.seed_history()
        user = await self.submit()
        await self.accept(user)
        drawer = self.app.current_activity
        drawer.toggle.action_toggle()
        await self.pilot.pause()
        y = self.viewport.scroll_y
        for i in range(35):
            self.app.display_message({"role": "assistant", "content": [{"type": "tool_call", "toolName": "read",
                                      "callId": str(i), "arguments": {"filePath": f"file-{i}.txt"}}]})
        await self.pilot.pause()
        self.assertEqual(self.viewport.scroll_y, y)
        self.assertFalse(self.viewport.is_vertical_scroll_end)
        self.viewport.scroll_end(animate=False, immediate=True)
        await self.pilot.pause()
        self.assertEqual(drawer.region.bottom, self.viewport.content_region.bottom)
        drawer.toggle.action_toggle()
        await self.pilot.pause()
        self.assertEqual(user.region.y, self.viewport.content_region.y)
        self.assertTrue(self.viewport.is_vertical_scroll_end)

    async def test_scrolling_during_markdown_mount_cancels_pending_follow(self):
        await self.seed_history()
        user = await self.submit()
        await self.accept(user)
        entered, release = asyncio.Event(), asyncio.Event()
        original = ResponseMarkdown._on_mount
        async def delayed_mount(widget, event):
            entered.set()
            await release.wait()
            await original(widget, event)
        with patch.object(ResponseMarkdown, "_on_mount", delayed_mount):
            self.app.display_live_messages([self.message("assistant", "\n\n".join("A paragraph." for _ in range(30)))])
            try:
                await asyncio.wait_for(entered.wait(), 2)
                self.viewport._on_mouse_scroll_up(events.MouseScrollUp(self.viewport, 0, 0, 0, -1, 0, False, False, False))
            finally:
                release.set()
            await self.app.workers.wait_for_complete()
            await self.pilot.pause()
        self.assertFalse(self.viewport.is_vertical_scroll_end)
        self.assertGreater(user.region.y, self.viewport.content_region.y)

    async def activity(self, count=1):
        self.app.display_message({"role": "assistant", "content": [
            {"type": "tool_call", "toolName": "read", "callId": f"call-{i}",
             "arguments": {"filePath": f"file-{i}.txt"}} for i in range(count)]})
        await self.pilot.pause()

    async def test_activity_growth_preserves_follow_intent_but_scrolling_away_cancels_it(self):
        await self.seed_history()
        user = await self.submit()
        await self.accept(user)
        self.app.current_activity.toggle.action_toggle()
        await self.pilot.pause()
        position = self.viewport.scroll_y
        await self.activity(35)
        self.assertEqual(self.viewport.scroll_y, position)
        self.assertFalse(self.viewport.is_vertical_scroll_end)
        await self.reply()
        await wait_until(self.pilot, lambda: self.viewport.is_vertical_scroll_end,
                         "reply follows the bottom after activity growth")
        self.assertTrue(self.viewport.is_vertical_scroll_end)
        await self.activity()
        self.viewport.action_scroll_up()
        # Capture the settled position, not an intermediate animation frame.
        await asyncio.wait_for(self.pilot.wait_for_scheduled_animations(), timeout=2)
        position = self.viewport.scroll_y
        await self.reply()
        await asyncio.wait_for(self.pilot.wait_for_scheduled_animations(), timeout=2)
        self.assertEqual(self.viewport.scroll_y, position)
        self.assertFalse(self.viewport.is_vertical_scroll_end)
        self.viewport.scroll_end(animate=False, immediate=True)
        await self.pilot.pause()
        await self.activity()
        await self.reply()
        await wait_until(self.pilot, lambda: self.viewport.is_vertical_scroll_end,
                         "reply resumes following after returning to the bottom")
        self.assertTrue(self.viewport.is_vertical_scroll_end)

    async def test_reading_back_in_a_long_user_message_disables_follow(self):
        await self.seed_history()
        user = await self.submit("\n".join(f"Question line {i}" for i in range(40)))
        await self.accept(user)
        self.viewport.action_scroll_up()
        await self.pilot.pause()
        position = self.viewport.scroll_y
        await self.activity()
        await self.reply()
        self.assertEqual(self.viewport.scroll_y, position)
        self.assertFalse(self.viewport.is_vertical_scroll_end)

    async def test_eviction_keeps_a_retained_reading_anchor(self):
        await self.seed_history(turns=3)
        self.viewport.WINDOW_SIZE = 6
        anchor = list(self.app.query(UserMessage))[1]
        self.viewport.scroll_to(y=anchor.virtual_region.y, animate=False, immediate=True)
        await self.pilot.pause()
        y = anchor.region.y
        await self.reply()
        self.assertEqual(len(self.viewport.message_ids), 6)
        self.assertTrue(anchor.is_attached)
        self.assertEqual(anchor.region.y, y)
        self.assertTrue(self.app.history_window.page["hasOlder"])

    async def test_latest_page_extent_updates_on_pagination_and_resize(self):
        messages = [self.message("user", "Earlier"), self.message("assistant", "Earlier answer"),
                    self.message("user", "Latest"), self.message("assistant", "Short answer")]
        result = {"session": {"id": "s"}, "page": {"hasNewer": False}, "messages": messages}
        await self.viewport.replace(result)
        await self.pilot.pause()
        user = list(self.app.query(UserMessage))[-1]
        self.assertEqual(user.region.y, self.viewport.content_region.y)
        await self.viewport.replace({**result, "page": {"hasNewer": True}}, "before")
        await self.pilot.pause()
        self.assertEqual(self.viewport.max_scroll_y, 0)
        await self.viewport.replace(result, "after")
        await self.pilot.resize_terminal(60, 16)
        self.viewport.scroll_end(animate=False, immediate=True)
        await self.pilot.pause()
        self.assertEqual(user.region.y, self.viewport.content_region.y)

    async def test_rejected_later_turn_removes_preview_and_keeps_previous_history(self):
        await self.seed_history()
        user = await self.submit()
        self.app.handle_event({"type": "request.failed", "requestId": self.app.pending_request, "error": "Rejected"})
        await self.pilot.pause()
        self.assertFalse(user.is_attached)
        self.assertEqual(len(self.app.query(UserMessage)), 2)
        self.assertEqual(self.entry.text, "New question")
        self.assertIsNone(self.app.pending_turn)

    async def test_send_from_old_page_shows_preview_while_latest_page_is_loading(self):
        latest = await self.seed_history()
        await self.viewport.replace({**latest, "page": {"hasNewer": True}})
        entered, release = asyncio.Event(), asyncio.Event()
        async def navigate(*args, **kwargs):
            entered.set()
            await release.wait()
            return latest
        with patch.object(self.app.history_window, "navigate", navigate):
            position = self.viewport.scroll_position_number
            text = "\n".join(f"Send from history, line {i}" for i in range(40))
            task = asyncio.create_task(self.app.on_message_input_submitted(MessageInput.Submitted(text)))
            try:
                await asyncio.wait_for(entered.wait(), 2)
                await self.app.workers.wait_for_complete()
                await wait_until(self.pilot, lambda: self.viewport.scroll_position_number > position,
                                 "history preview's post-mount scroll")
                await self.pilot.pause()
                user, activity = self.app.pending_turn.user, self.app.pending_turn.activity
                self.assertLess(user.region.y, self.viewport.content_region.y)
                self.assertTrue(self.viewport.is_vertical_scroll_end)
                self.app.client.send.assert_not_awaited()
            finally:
                release.set()
                await task
        await self.pilot.pause()
        self.assertIs(self.app.pending_turn.user, user)
        self.assertIs(self.app.current_activity, activity)
        self.assertTrue(activity.built)
        self.assertLess(user.region.y, self.viewport.content_region.y)
        self.assertTrue(self.viewport.is_vertical_scroll_end)
        self.app.client.send.assert_awaited_once()
        await self.accept(user)
        self.assertEqual(len(self.app.query(UserMessage)), 3)

    async def test_old_page_load_failure_removes_preview_without_sending(self):
        result = await self.seed_history()
        await self.viewport.replace({**result, "page": {"hasNewer": True}})
        with patch.object(self.app.history_window, "navigate", side_effect=ValueError("History unavailable")):
            await self.app.on_message_input_submitted(MessageInput.Submitted("Keep this draft"))
        await self.app.workers.wait_for_complete()
        await self.pilot.pause()
        self.app.client.send.assert_not_awaited()
        self.assertEqual(len(self.app.query(UserMessage)), 2)
        self.assertEqual(self.entry.text, "Keep this draft")
        self.assertIsNone(self.app.pending_turn)
        self.assertFalse(self.app.busy)
        self.assertFalse(self.entry.disabled)
