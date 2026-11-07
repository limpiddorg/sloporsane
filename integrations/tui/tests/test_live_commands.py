"""Commands stay usable while the principal is generating."""

import unittest
from unittest.mock import AsyncMock, patch

from sos_tui.app import SosApp
from sos_tui.config import TITLE_MD
from sos_tui.history_viewport import HistoryViewport
from sos_tui.input_error_notice import InputErrorNotice
from sos_tui.message_input import MessageInput


class LiveCommandsTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 35)))
        self.app.connected = True
        self.app.session_id = "session"
        self.app.busy = True
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.send = self.enterContext(patch.object(self.app.client, "send", new_callable=AsyncMock))

    async def command(self, text):
        self.entry.text = text
        await self.app.on_message_input_submitted(MessageInput.Submitted(text))
        await self.pilot.pause()

    async def test_model_opens_while_running(self):
        await self.command("/model")
        self.assertIsNotNone(self.app.model_screen)
        self.assertEqual(self.send.call_args.args[0]["type"], "model.list")
        self.assertTrue(self.app.busy)

    async def test_permissions_opens_while_running(self):
        await self.command("/permissions")
        self.assertIsNotNone(self.app.permissions_screen)
        self.assertTrue(self.app.busy)

    async def test_commands_remain_available_while_permission_reply_is_pending(self):
        self.app.handle_event({"type": "permission.requested", "permissionId": "p", "callId": "c",
                               "permission": "edit", "tool": "write", "patterns": ["file.txt"]})
        self.app.permission_card.waiting = True
        await self.command("/permissions")
        self.assertIsNotNone(self.app.permissions_screen)

    async def test_new_does_not_send_an_old_queued_message_into_the_new_session(self):
        self.app.queue_message("Old queued task")
        await self.command("/new")
        self.app.handle_event({"type": "execution.completed"})
        await self.pilot.pause()
        self.assertIsNotNone(self.app.queued_message)
        await self.app.reset_session({"info": {"model": "model", "reasoning": "high"}}).wait()
        self.assertIsNone(self.app.queued_message)
        self.assertFalse(self.app.queued_ready)
        self.assertEqual([call.args[0]["type"] for call in self.send.call_args_list], ["session.new"])

    async def test_connect_opens_without_cancelling_on_merely_opening_the_dialog(self):
        await self.command("/connect")
        self.assertIsNotNone(self.app.connect_screen)
        self.assertEqual(self.send.call_args.args[0]["type"], "provider.list")
        self.assertTrue(self.app.busy)

    async def test_resume_can_list_sessions_while_running(self):
        await self.command("/resume")
        self.assertIsNotNone(self.app.resume_screen)
        self.assertEqual(self.send.call_args.args[0]["type"], "session.list")
        self.assertTrue(self.app.busy)

    async def test_new_sends_immediately_without_queueing(self):
        await self.command("/new")
        self.assertEqual(self.send.call_args.args[0]["type"], "session.new")
        self.assertIsNone(self.app.queued_message)
        self.assertIsNotNone(self.app.new_request)

    async def test_summaries_warn_in_yellow_without_being_queued(self):
        for command in ("/compact", "/recap"):
            await self.command(command)
            notice = self.app.query_one(InputErrorNotice)
            self.assertEqual(str(notice.render()), f"Wait for the current execution to finish before using {command}.")
            self.assertEqual(notice.styles.border_left[1].hex, TITLE_MD.upper())
            self.assertEqual(self.entry.text, command)
            self.assertIsNone(self.app.compaction_request)
            self.assertIsNone(self.app.recap_request)
        self.send.assert_not_awaited()

    async def test_selected_model_does_not_relabel_inflight_footer(self):
        viewport = self.app.query_one(HistoryViewport)
        viewport.start_generation_footer({"model": "old", "reasoning": "high"})
        await self.pilot.pause()
        self.app.update_agent_info({"model": "new", "reasoning": "low",
                                    "execution_model": "old", "execution_reasoning": "high"})
        self.assertEqual(self.app.runtime_info["model"], "new")
        self.assertEqual(viewport.live_footer.metadata["model"], "old")
        self.assertEqual(viewport.live_footer.metadata["reasoning"], "high")
