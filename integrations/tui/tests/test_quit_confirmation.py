"""Ctrl+C clears a draft, or independently confirms quitting within the Escape timeout."""

import unittest
from unittest.mock import AsyncMock, patch
from rich.color import Color
from confirmation_fixture import confirmation_input

from sos_tui.app import SosApp
from sos_tui.config import SLOP_COLOR
from sos_tui.message_input import MessageInput
from sos_tui.mode_button import ModeButton
from sos_tui.confirmation_bar import ConfirmationBar


class QuitConfirmationTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(80, 24)))
        self.exit_app = self.enterContext(patch.object(self.app, "exit"))
        self.app.connected = True
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.button = self.app.query_one(ModeButton)
        self.confirmation = self.app.query_one(ConfirmationBar)
        await self.pilot.pause()
        await self.enterAsyncContext(confirmation_input())

    async def test_empty_draft_arms_orange_then_second_ctrl_c_quits(self):
        await self.pilot.press("ctrl+c")
        self.assertEqual(self.confirmation.kind, "exit")
        self.assertEqual(str(self.confirmation.label), "Exit ? Confirm with CTRL-C")
        self.assertEqual(self.confirmation.render().spans[0].style.bgcolor, Color.parse(SLOP_COLOR))
        self.assertFalse(self.app.query_one("#mode-actions").display)
        self.exit_app.assert_not_called()
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("ctrl+c")
        self.exit_app.assert_called_once()
        self.assertNotEqual(self.confirmation.kind, "exit")
        self.assertIsNone(self.app.quit_deadline)
        self.assertIsNone(self.app.quit_timer)

    async def test_expiry_uses_escape_duration_and_restores_current_mode_color(self):
        self.assertEqual(self.app.CANCEL_CONFIRM_SECONDS, 3.0)
        with patch.object(self.app, "set_timer", wraps=self.app.set_timer) as schedule:
            await self.pilot.press("ctrl+c", "tab")
        self.assertEqual(schedule.call_args.args[0], self.app.CANCEL_CONFIRM_SECONDS)
        self.assertEqual(self.confirmation.kind, "exit")
        self.assertEqual(self.confirmation.render().spans[0].style.bgcolor, Color.parse(SLOP_COLOR))
        self.app.quit_timer.stop()
        schedule.call_args.args[1]()  # Fire the registered expiry, without waiting three real seconds.
        await self.pilot.pause()
        self.assertNotEqual(self.confirmation.kind, "exit")
        self.assertIsNone(self.app.quit_deadline)
        self.assertIsNone(self.app.quit_timer)
        self.assertEqual(self.button.styles.background.hex, SLOP_COLOR.upper())
        self.assertEqual(str(self.button.label), "Slop")
        await self.pilot.press("ctrl+c")
        self.exit_app.assert_not_called()
        self.assertEqual(self.confirmation.kind, "exit")

    async def test_an_expired_deadline_cannot_quit_even_if_the_timer_has_not_fired(self):
        await self.pilot.press("ctrl+c")
        previous_timer = self.app.quit_timer
        previous_timer.pause()
        self.app.quit_deadline = -1
        await self.pilot.press("ctrl+c")
        self.exit_app.assert_not_called()
        self.assertEqual(self.confirmation.kind, "exit")
        self.assertIsNot(self.app.quit_timer, previous_timer)
        self.assertGreater(self.app.quit_deadline, 0)

    async def test_editing_or_clearing_a_draft_disarms_quit(self):
        await self.pilot.press("ctrl+c", "x")
        self.assertNotEqual(self.confirmation.kind, "exit")
        self.assertIsNone(self.app.quit_deadline)
        await self.pilot.press("backspace", "ctrl+c")
        self.assertEqual(self.confirmation.kind, "exit")
        self.exit_app.assert_not_called()
        # Clearing text wins even before TextArea.Changed has been processed.
        self.entry.text = "Draft\nwith another line"
        self.app.action_clear_or_quit()
        self.assertEqual(self.entry.text, "")
        self.assertNotEqual(self.confirmation.kind, "exit")
        self.exit_app.assert_not_called()

    async def test_submitting_a_message_disarms_quit(self):
        await self.pilot.press("ctrl+c")
        await self.app.on_message_input_submitted(MessageInput.Submitted("New message"))
        self.assertNotEqual(self.confirmation.kind, "exit")
        self.assertIsNone(self.app.quit_deadline)
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "message.send")
        self.exit_app.assert_not_called()

    async def test_escape_and_ctrl_c_cannot_confirm_each_other(self):
        self.app.busy = True
        await self.pilot.press("escape", "ctrl+c")
        self.assertNotEqual(self.confirmation.kind, "cancel")
        self.assertEqual(self.confirmation.kind, "exit")
        self.exit_app.assert_not_called()
        self.app.client.send.assert_not_awaited()
        deadline, timer = self.app.quit_deadline, self.app.quit_timer
        await self.pilot.press("escape")
        self.assertEqual(self.confirmation.kind, "exit")
        self.assertEqual(self.app.quit_deadline, deadline)
        self.assertIs(self.app.quit_timer, timer)
        self.assertIsNone(self.app.cancel_deadline)
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("ctrl+c")
        self.exit_app.assert_called_once()

    async def test_escape_works_normally_after_quit_deadline_even_before_timer_callback(self):
        self.app.busy = True
        await self.pilot.press("ctrl+c")
        self.app.quit_timer.pause()
        self.app.quit_deadline = -1
        await self.pilot.press("escape")
        self.assertEqual(self.confirmation.kind, "cancel")
        self.assertIsNone(self.app.quit_deadline)
        self.app.client.send.assert_not_awaited()
        self.exit_app.assert_not_called()

    async def test_escape_does_not_retract_pending_during_quit_confirmation(self):
        self.app.busy = True
        await self.app.on_message_input_submitted(MessageInput.Submitted("Queued text"))
        await self.app.workers.wait_for_complete()
        await self.pilot.press("ctrl+c", "escape")
        self.assertEqual(self.confirmation.kind, "exit")
        self.assertEqual(self.app.queued_message.text, "Queued text")
        self.assertEqual(self.entry.text, "")
        self.app.reset_quit_confirmation()
        await self.pilot.press("escape")
        self.assertIsNone(self.app.queued_message)
        self.assertEqual(self.entry.text, "Queued text")
        self.assertIsNone(self.app.cancel_deadline)
        self.app.client.send.assert_not_awaited()

    async def test_clicking_the_button_does_not_confirm_quit(self):
        self.app.busy = True
        await self.pilot.press("ctrl+c")
        await self.pilot.click(self.confirmation)
        self.assertNotEqual(self.confirmation.kind, "exit")
        self.assertEqual(self.confirmation.kind, "cancel")
        self.exit_app.assert_not_called()
        self.app.client.send.assert_not_awaited()

    async def test_execution_completion_does_not_clear_an_armed_quit(self):
        self.app.busy = True
        await self.pilot.press("ctrl+c")
        self.app.handle_event({"type": "execution.completed"})
        await self.pilot.pause()
        self.assertEqual(self.confirmation.kind, "exit")
        self.assertEqual(str(self.confirmation.label), "Exit ? Confirm with CTRL-C")
        self.assertEqual(self.confirmation.render().spans[0].style.bgcolor, Color.parse(SLOP_COLOR))
        await self.pilot.press("ctrl+c")
        self.exit_app.assert_called_once()
        self.app.client.send.assert_not_awaited()

    async def test_disconnected_disabled_input_still_requires_quit_confirmation(self):
        self.app.connected = False
        self.entry.disabled = True
        await self.pilot.pause()
        await self.pilot.press("ctrl+c")
        self.assertEqual(self.confirmation.kind, "exit")
        self.exit_app.assert_not_called()
        await self.pilot.press("ctrl+c")
        self.exit_app.assert_called_once()
