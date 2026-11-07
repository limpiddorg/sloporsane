"""Idle time belongs to the open TUI, not persisted session/daemon timestamps."""

import unittest
from unittest.mock import AsyncMock, patch

from sos_tui.app import SosApp
from sos_tui.conversation_recap import ConversationRecap
from sos_tui.message_input import MessageInput
from sos_tui.mode_button import ModeButton
from sos_tui.paste_preview import PastePreview
from sos_tui.recap_idle import RecapIdle


class RecapIdleTest(unittest.TestCase):
    def test_five_full_minutes_and_no_catchup_from_previous_process(self):
        idle = RecapIdle()
        self.assertFalse(idle.due(100000))
        self.assertFalse(idle.due(100299.999))
        self.assertTrue(idle.due(100300))
        idle.mark_requested()
        self.assertFalse(idle.due(200000))
        reopened = RecapIdle()
        self.assertFalse(reopened.due(200000))
        self.assertTrue(reopened.due(200300))

    def test_work_discards_elapsed_time_instead_of_accumulating_idle_minutes(self):
        idle = RecapIdle()
        idle.due(0)
        self.assertFalse(idle.due(299))
        idle.activity()
        idle.pause()
        self.assertFalse(idle.due(1000))
        self.assertFalse(idle.due(1299))
        self.assertTrue(idle.due(1300))


class AutoRecapTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.now = 1000.0
        self.enterContext(patch("sos_tui.app.monotonic", side_effect=lambda: self.now))
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(80, 24)))
        self.app.connected = True
        self.app.session_id = "s"
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        await self.check(0)

    async def check(self, seconds):
        self.now += seconds
        await self.app.check_auto_recap()

    def complete(self, outcome="recap.completed"):
        self.app.handle_event({"type": outcome, "requestId": self.app.recap_request,
                               "info": {"last_history_is_recap": outcome == "recap.completed"},
                               "messages": [{"id": "r1", "role": "assistant", "recap": True,
                                             "content": [{"type": "text", "text": "A quick reminder."}]}]})

    async def test_exact_threshold_preserves_draft_selection_and_local_spinner(self):
        self.entry.text = "Unsent draft"
        self.entry.move_cursor((0, 4))
        selection = self.entry.selection
        await self.check(299.999)
        self.app.client.send.assert_not_awaited()
        await self.check(0.001)
        self.app.client.send.assert_awaited_once_with({"type": "session.recap", "requestId": self.app.recap_request})
        self.assertTrue(self.app.running_recap.loading)
        self.assertFalse(self.app.query_one(ModeButton).busy)
        self.assertEqual(self.entry.text, "Unsent draft")
        self.assertEqual(self.entry.selection, selection)
        self.complete()
        await self.pilot.pause()
        self.assertEqual(self.entry.text, "Unsent draft")
        self.assertEqual(self.entry.selection, selection)
        self.assertFalse(self.app.query_one(ConversationRecap).loading)

    async def test_long_execution_requires_five_new_minutes_after_completion(self):
        await self.check(299)
        self.app.busy = True
        await self.check(3600)
        self.app.client.send.assert_not_awaited()
        self.app.busy = False
        await self.check(0)
        await self.check(299)
        self.app.client.send.assert_not_awaited()
        await self.check(1)
        self.app.client.send.assert_awaited_once()

    async def test_even_work_between_timer_ticks_restarts_countdown(self):
        await self.check(299)
        self.app.busy = True
        self.app.busy = False
        await self.check(1)
        self.app.client.send.assert_not_awaited()
        await self.check(300)
        self.app.client.send.assert_awaited_once()

    async def test_no_repeated_recap_without_new_activity(self):
        await self.check(300)
        self.complete()
        await self.check(3600)
        self.app.client.send.assert_awaited_once()
        self.app.busy = True
        self.app.busy = False
        await self.check(0)
        await self.check(300)
        self.app.client.send.assert_awaited_once()  # Work alone did not change the tail.
        self.app.update_agent_info({"last_history_is_recap": False})
        await self.check(0)
        await self.check(299)
        self.app.client.send.assert_awaited_once()
        await self.check(1)
        self.assertEqual(self.app.client.send.await_count, 2)

    async def test_fresh_tui_blocks_saved_recap_even_on_an_older_page(self):
        self.app.update_agent_info({"last_history_is_recap": True})
        self.app.recap_idle = RecapIdle()
        self.app.history_window.page = {"hasNewer": True, "last": "old-user-message"}
        await self.check(0)
        await self.check(300)
        await self.check(900)
        self.app.client.send.assert_not_awaited()
        # Switching away and back also cannot forget the persisted tail.
        self.app.session_id = "resumed-session"
        await self.check(0)
        await self.check(300)
        self.app.client.send.assert_not_awaited()

    async def test_manual_command_ignores_saved_recap_guard(self):
        self.app.update_agent_info({"last_history_is_recap": True})
        self.entry.text = "/recap"
        await self.pilot.press("enter")
        self.app.client.send.assert_awaited_once_with({"type": "session.recap", "requestId": self.app.recap_request})

    async def test_user_send_restarts_idle_even_if_transport_rejects_it(self):
        await self.check(299)
        self.app.client.send.side_effect = OSError("Unavailable")
        with patch.object(self.app, "prepare_history_for_send", new=AsyncMock(return_value=True)):
            self.entry.text = "A new request"
            await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "message.send")
        self.assertFalse(self.app.busy)
        self.app.client.send.reset_mock(side_effect=True)
        await self.check(1)
        await self.check(299)
        self.app.client.send.assert_not_awaited()
        await self.check(1)
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "session.recap")

    async def test_manual_recap_also_satisfies_current_idle_period(self):
        self.entry.text = "/recap"
        await self.pilot.press("enter")
        self.complete()
        await self.check(3600)
        self.app.client.send.assert_awaited_once()

    async def test_failed_or_cancelled_auto_recap_does_not_retry(self):
        for outcome in ("request.failed", "recap.cancelled"):
            with self.subTest(outcome=outcome):
                self.app.recap_idle.activity()
                await self.check(0)
                await self.check(300)
                before = self.app.client.send.await_count
                self.complete(outcome)
                await self.check(3600)
                self.assertEqual(self.app.client.send.await_count, before)

    async def test_no_daemon_only_empty_session_or_disconnected_catchup(self):
        for field, blocked, restored in (("connected", False, True), ("session_id", None, "s")):
            with self.subTest(field=field):
                setattr(self.app, field, blocked)
                await self.check(3600)
                self.app.client.send.assert_not_awaited()
                setattr(self.app, field, restored)
                await self.check(0)
                await self.check(299)
                self.app.client.send.assert_not_awaited()

    async def test_timer_does_not_query_missing_composer_during_teardown(self):
        await self.entry.remove()
        await self.check(300)
        self.app.client.send.assert_not_awaited()

    async def test_history_and_external_editor_do_not_count_as_idle_time(self):
        for field in ("history_rendering", "cleaning_history", "editing_external"):
            with self.subTest(field=field):
                setattr(self.app, field, True)
                await self.check(3600)
                self.app.client.send.assert_not_awaited()
                setattr(self.app, field, False)
                await self.check(0)
                await self.check(299)
                self.app.client.send.assert_not_awaited()

    async def test_session_switch_gets_its_own_five_minutes(self):
        await self.check(299)
        self.app.session_id = "other"
        await self.check(3600)
        self.app.client.send.assert_not_awaited()
        await self.check(300)
        self.app.client.send.assert_awaited_once()

    async def test_automatic_completion_does_not_steal_popup_focus(self):
        await self.check(300)
        popup = PastePreview("Draft preview", lambda: None)
        await self.app.push_screen(popup)
        await self.pilot.pause()
        focused = self.app.focused
        self.complete()
        await self.pilot.pause()
        self.assertIs(self.app.screen, popup)
        self.assertIs(self.app.focused, focused)

    async def test_popup_defers_auto_generation(self):
        await self.check(299)
        await self.app.push_screen(PastePreview("Preview", lambda: None))
        await self.check(3600)
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("escape")
        await self.check(0)
        await self.check(299)
        self.app.client.send.assert_not_awaited()

    async def test_registered_tui_timer_calls_the_idle_check(self):
        with patch.object(self.app, "request_recap", new=AsyncMock()) as request:
            self.now += 300
            await self.pilot.pause(1.1)
            request.assert_awaited_once_with(automatic=True)
