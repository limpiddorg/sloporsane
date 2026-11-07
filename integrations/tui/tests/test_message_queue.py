"""One local next-turn slot, sent only after the complete current execution."""

import asyncio
from contextlib import nullcontext
import os
from pathlib import Path
import subprocess
import threading
import unittest
from unittest.mock import AsyncMock, patch
from confirmation_fixture import confirmation_input

from sos_tui.app import SosApp
from sos_tui.config import MISC_MD
from sos_tui.history_viewport import HistoryViewport
from sos_tui.message_input import MessageInput
from sos_tui.confirmation_bar import ConfirmationBar
from sos_tui.queued_message import QueuedMessage
from sos_tui.user_message import UserMessage


class MessageQueueTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(90, 28)))
        self.app.connected = True
        self.app.session_id = "s"
        self.app.history_window.session_id = "s"
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.viewport = self.app.query_one(HistoryViewport)
        self.app.busy = True
        self.app.display_live_messages([{"id": "current", "role": "user", "content": [{"type": "text", "text": "Current turn"}]}])
        await self.settle()

    async def settle(self):
        await self.app.workers.wait_for_complete()
        await self.pilot.pause()

    async def enqueue(self, text):
        self.entry.text = text
        await self.app.on_message_input_submitted(MessageInput.Submitted(text))
        await self.settle()

    def assistant(self, id, content):
        self.app.handle_event({"type": "provider.message_arrived", "messages": [
            {"id": id, "role": "assistant", "content": content}]})

    async def test_submissions_merge_into_one_literal_blue_pending_block(self):
        await self.enqueue("First [bold] literal")
        queued = self.app.queued_message
        await self.enqueue("Second line")
        self.assertIs(self.app.queued_message, queued)
        self.assertEqual(queued.text, "First [bold] literal\nSecond line")
        self.assertEqual(queued.content.plain, "[Pending]\nFirst [bold] literal\nSecond line")
        self.assertEqual(queued.content.spans[0].style, MISC_MD)
        self.assertEqual(len(self.app.query(QueuedMessage)), 1)
        self.assertIs(self.viewport.children[-1], queued)
        self.assertEqual(self.entry.text, "")
        self.assertEqual(len(self.app.query(UserMessage)), 1)
        self.assertNotIn(queued, self.viewport.message_ids)
        self.app.client.send.assert_not_awaited()

    async def test_all_intermediate_content_stays_before_pending_until_terminal_event(self):
        await self.enqueue("Next turn")
        queued = self.app.queued_message
        for index in range(3):
            self.assistant(f"text-{index}", [{"type": "text", "text": f"Intermediate {index}"}])
            self.assistant(f"tool-{index}", [{"type": "tool_call", "toolName": "read", "callId": str(index),
                                             "arguments": {"filePath": "file.txt"}}])
            self.app.handle_event({"type": "harness.message_arrived", "messages": [{"role": "tool", "content": [
                {"type": "tool_result", "callId": str(index), "output": "Tool error", "isError": True}]}]})
            await self.settle()
            self.assertIs(self.viewport.children[-1], queued)
            self.app.client.send.assert_not_awaited()
        self.assistant("final", [{"type": "text", "text": "Final answer"}])
        await self.settle()
        self.app.client.send.assert_not_awaited()
        self.app.handle_event({"type": "execution.completed"})
        await self.settle()
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["text"], "Next turn")
        self.assertEqual(len(self.app.query(QueuedMessage)), 0)

    async def test_send_uses_current_mode_preserves_new_draft_and_reuses_echo(self):
        await self.enqueue("One")
        await self.enqueue("Two")
        self.entry.text = "Unsent next draft"
        self.app.set_mode("slop")
        self.app.handle_event({"type": "execution.completed"})
        self.app.dispatch_queued_message()  # A second scheduling attempt cannot send it again.
        await self.settle()
        self.app.client.send.assert_awaited_once()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual((request["type"], request["text"], request["mode"]), ("message.send", "One\nTwo", "slop"))
        self.assertEqual(self.entry.text, "Unsent next draft")
        preview = self.app.pending_turn.user
        self.app.handle_event({"type": "message.accepted", "requestId": request["requestId"], "session": {"id": "s", "title": "Chat"}})
        self.app.handle_event({"type": "harness.message_arrived", "messages": [
            {"id": "next", "role": "user", "content": [{"type": "text", "text": "One\nTwo"}]}]})
        await self.settle()
        self.assertEqual(len(self.app.query(UserMessage)), 2)
        self.assertIs(list(self.app.query(UserMessage))[-1], preview)
        self.assertEqual(preview.mode, "slop")
        self.assertEqual(self.entry.text, "Unsent next draft")

    async def test_completion_before_pending_has_painted_still_sends_one_merged_message(self):
        await self.app.on_message_input_submitted(MessageInput.Submitted("One"))
        await self.app.on_message_input_submitted(MessageInput.Submitted("Two"))
        self.app.handle_event({"type": "execution.completed"})
        await self.settle()
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["text"], "One\nTwo")
        self.assertEqual(len(self.app.query(QueuedMessage)), 0)
        self.assertEqual(len(self.app.query(UserMessage)), 2)

    async def test_submission_after_dispatch_is_queued_for_the_following_turn(self):
        await self.enqueue("Next turn")
        self.app.handle_event({"type": "execution.completed"})
        await self.app.on_message_input_submitted(MessageInput.Submitted("Following turn"))
        await self.settle()
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["text"], "Next turn")
        self.assertEqual(self.app.queued_message.text, "Following turn")
        self.assertIs(self.viewport.children[-1], self.app.queued_message)

    @confirmation_input()
    async def test_escape_restores_pending_and_requires_two_fresh_escapes_to_cancel(self):
        await self.enqueue("Pending one")
        await self.enqueue("Pending two")
        self.entry.text = "Current draft"
        await self.app.request_execution_cancel()
        await self.pilot.press("escape")
        self.assertEqual(self.entry.text, "Pending one\nPending two\nCurrent draft")
        self.assertIsNone(self.app.queued_message)
        self.assertIsNone(self.app.cancel_deadline)
        self.assertIsNone(self.app.query_one(ConfirmationBar).kind)
        self.assertTrue(self.app.busy)
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("escape")
        self.app.client.send.assert_not_awaited()
        self.assertEqual(self.app.query_one(ConfirmationBar).kind, "cancel")
        await self.pilot.press("escape")
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "execution.cancel")

    async def test_execution_error_or_cancellation_restores_queue_without_sending(self):
        for kind in ("execution.failed", "execution.cancelled"):
            with self.subTest(kind=kind):
                self.app.busy = True
                await self.enqueue("Queued")
                self.entry.text = "Draft"
                self.app.handle_event({"type": kind, "error": "Stopped"})
                await self.settle()
                self.assertEqual(self.entry.text, "Queued\nDraft")
                self.assertIsNone(self.app.queued_message)
                self.assertFalse(self.app.busy)
                self.app.client.send.assert_not_awaited()

    async def test_refused_initial_send_preserves_original_queue_and_draft_in_order(self):
        self.app.pending_text = "Original send"
        self.app.pending_request = "request"
        await self.enqueue("Queued")
        self.entry.text = "Draft"
        self.app.handle_event({"type": "request.failed", "requestId": "request", "error": "Rejected"})
        await self.settle()
        self.assertEqual(self.entry.text, "Original send\n\nQueued\nDraft")
        self.assertIsNone(self.app.queued_message)
        self.app.client.send.assert_not_awaited()

    async def test_failed_automatic_send_restores_text_and_preserves_new_draft(self):
        await self.enqueue("Queued")
        self.entry.text = "Draft"
        self.app.client.send.side_effect = OSError("Disconnected")
        self.app.handle_event({"type": "execution.completed"})
        await self.settle()
        self.assertEqual(self.entry.text, "Queued\n\nDraft")
        self.assertFalse(self.app.busy)
        self.assertIsNone(self.app.pending_turn)
        self.assertEqual(len(self.app.query(UserMessage)), 1)

    async def test_pending_survives_page_replacement_and_is_not_a_historical_message(self):
        await self.enqueue("Queued")
        queued = self.app.queued_message
        old = {"session": {"id": "s"}, "page": {"hasNewer": True}, "messages": [
            {"id": "old", "role": "user", "content": [{"type": "text", "text": "Old turn"}]}]}
        await self.viewport.replace(old, "before")
        await self.pilot.pause()
        self.assertTrue(queued.is_attached)
        self.assertFalse(queued.display)
        await self.viewport.replace({**old, "page": {"hasNewer": False}}, "after")
        await self.pilot.pause()
        self.assertTrue(queued.display)
        self.assertIs(self.viewport.children[-1], queued)
        self.assertNotIn(queued, self.viewport.message_ids)
        self.assertNotIn(queued, [widget for _, widget in self.viewport.rendered])

    async def test_completion_waits_for_navigation_and_escape_can_still_retract(self):
        await self.enqueue("Queued")
        self.app.history_rendering = True
        self.app.handle_event({"type": "execution.completed"})
        self.assertTrue(self.app.queued_ready)
        self.app.client.send.assert_not_awaited()
        await self.app.action_request_cancel()
        self.app.history_rendering = False
        self.app.dispatch_queued_message()
        await self.settle()
        self.assertEqual(self.entry.text, "Queued")
        self.assertIsNone(self.app.queued_message)
        self.app.client.send.assert_not_awaited()

    async def test_completion_during_navigation_dispatches_when_the_page_is_ready(self):
        await self.enqueue("Queued")
        entered, release = asyncio.Event(), asyncio.Event()
        async def navigate(*args, **kwargs):
            entered.set()
            await release.wait()
            return {"session": {"id": "s"}, "page": {"hasNewer": False}, "messages": [
                {"id": "current", "role": "user", "content": [{"type": "text", "text": "Current turn"}]}]}
        with patch.object(self.app.history_window, "navigate", navigate):
            navigating = asyncio.create_task(self.app.load_history_page("before"))
            try:
                await asyncio.wait_for(entered.wait(), 2)
                self.entry.text = "Unsent draft"
                self.app.handle_event({"type": "execution.completed"})
                self.app.client.send.assert_not_awaited()
            finally:
                release.set()
                self.assertTrue(await navigating)
        await self.settle()
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["text"], "Queued")
        self.assertEqual(self.entry.text, "Unsent draft")

    async def test_queue_is_not_evicted_with_old_history_widgets(self):
        await self.enqueue("Queued")
        queued = self.app.queued_message
        self.viewport.WINDOW_SIZE = 2
        for index in range(4):
            self.assistant(str(index), [{"type": "text", "text": f"Intermediate {index}"}])
            await self.settle()
        self.assertTrue(queued.is_attached)
        self.assertIs(self.viewport.children[-1], queued)
        self.assertEqual(len(self.viewport.message_ids), 2)
        self.assertIs(self.app.queued_message, queued)

    async def test_slash_commands_are_not_queued(self):
        await self.enqueue("/new")
        self.assertEqual(self.entry.text, "/new")
        self.assertIsNone(self.app.queued_message)
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "session.new")

    async def test_enter_while_a_permission_reply_is_pending_does_not_queue_the_draft(self):
        self.app.handle_event({"type": "permission.requested", "permissionId": "p", "callId": "c",
                               "permission": "edit", "tool": "write", "patterns": ["file.txt"]})
        self.app.permission_card.waiting = True
        await self.enqueue("Keep this draft")
        self.assertEqual(self.entry.text, "Keep this draft")
        self.assertIsNone(self.app.queued_message)
        self.app.client.send.assert_not_awaited()
        self.app.handle_event({"type": "permission.dismissed", "permissionId": "p"})
        await self.enqueue("Keep this draft")
        self.assertEqual(self.app.queued_message.text, "Keep this draft")

    async def test_completion_during_external_edit_sends_queue_without_overwriting_editor(self):
        await self.enqueue("Queued")
        self.entry.text = "New draft in editor"
        entered, release = threading.Event(), threading.Event()
        def editor(command, **kwargs):
            entered.set()
            release.wait(timeout=5)
            Path(command[-1]).write_text("Edited draft", encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        with patch.dict(os.environ, {"EDITOR": "test-editor"}), patch.object(self.app, "suspend", side_effect=nullcontext), \
                patch("sos_tui.app.subprocess.run", side_effect=editor):
            editing = asyncio.create_task(self.app.action_edit_external())
            try:
                self.assertTrue(await asyncio.to_thread(entered.wait, 2))
                self.app.handle_event({"type": "execution.completed"})
                for _ in range(100):
                    if self.app.client.send.await_count:
                        break
                    await asyncio.sleep(0.01)
                self.app.client.send.assert_awaited_once()
                self.assertEqual(self.app.client.send.call_args.args[0]["text"], "Queued")
                self.assertFalse(editing.done())
            finally:
                release.set()
                await asyncio.wait_for(editing, 3)
        await self.settle()
        self.assertEqual(self.entry.text, "Edited draft")
