"""Confirmation replaces one full information row and shares its action deadline."""

import unittest
from unittest.mock import AsyncMock, patch

from rich.color import Color
from confirmation_fixture import confirmation_input

from sos_tui.agent_info_bar import AgentInfoBar
from sos_tui.app import SosApp
from sos_tui.config import BACKGROUND_COLOR, SLOP_COLOR
from sos_tui.confirmation_bar import ConfirmationBar
from sos_tui.history_loading_bar import HistoryLoadingBar
from sos_tui.message_input import MessageInput
from sos_tui.mode_button import ModeButton


class ConfirmationBarTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(80, 24)))
        self.app.connected = True
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.info = self.app.query_one(AgentInfoBar)
        self.bar = self.app.query_one(ConfirmationBar)
        self.mode = self.app.query_one(ModeButton)
        self.app.update_agent_info({"model": "test-model", "reasoning": "medium",
                                    "last_turn_used_context": 1200, "total_context": 100000})
        await self.pilot.pause()

        await self.enterAsyncContext(confirmation_input())

    async def test_replaces_the_entire_row_without_moving_chat_or_composer(self):
        regions = {id: self.app.query_one("#" + id).region for id in ("chat", "composer", "agent-info", "session-bar")}
        self.app.action_clear_or_quit()
        await self.pilot.pause()
        self.assertEqual(self.bar.region, self.info.region)
        self.assertEqual(self.bar.region.width, 80)
        self.assertEqual(self.bar.region.height, 1)
        self.assertEqual(self.bar.content_size.width, 80)
        self.assertEqual(self.bar.render().plain, "Exit ? Confirm with CTRL-C".center(80))
        for id in ("model-details", "context-details", "mode-actions"):
            self.assertFalse(self.info.query_one("#" + id).display)
        for id, region in regions.items():
            self.assertEqual(self.app.query_one("#" + id).region, region)
        self.assertTrue(self.entry.has_focus)
        self.assertFalse(self.bar.can_focus)

    async def test_fill_is_full_half_then_empty_at_the_deadline(self):
        with patch("sos_tui.confirmation_bar.monotonic", return_value=100.0) as clock:
            self.info.show_confirmation("exit", 101.0, 1.0)
            await self.pilot.pause()
            for now, expected_fill in ((100.0, 80), (100.5, 40), (101.0, 0), (102.0, 0)):
                with self.subTest(now=now):
                    clock.return_value = now
                    text = self.bar.render()
                    colors = [text.get_style_at_offset(self.app.console, column).bgcolor for column in range(80)]
                    self.assertEqual(colors, [Color.parse(SLOP_COLOR)] * expected_fill +
                                     [Color.parse(BACKGROUND_COLOR)] * (80 - expected_fill))
                    self.assertEqual(text.plain, "Exit ? Confirm with CTRL-C".center(80))

    async def test_deadline_is_shared_with_the_action_and_not_restarted_by_other_updates(self):
        self.app.action_clear_or_quit()
        deadline = self.app.quit_deadline
        self.assertEqual(self.bar.deadline, deadline)
        self.assertEqual(self.bar.duration, self.app.CANCEL_CONFIRM_SECONDS)
        self.app.busy = True
        self.app.handle_event({"type": "execution.completed"})
        self.app.set_mode("slop")
        self.assertEqual(self.bar.deadline, deadline)
        self.assertEqual(self.bar.kind, "exit")
        with patch("sos_tui.confirmation_bar.monotonic", return_value=deadline - self.app.CANCEL_CONFIRM_SECONDS / 2):
            self.assertEqual(self.bar.remaining, 0.5)

    async def test_hidden_info_updates_reappear_with_current_mode_on_expiry(self):
        self.app.action_clear_or_quit()
        self.app.update_agent_info({"model": "updated-model", "reasoning": "high",
                                    "last_turn_used_context": 2500, "total_context": 200000})
        self.app.set_mode("slop")
        await self.pilot.pause()
        self.assertFalse(self.info.query_one("#model-details").display)
        self.assertFalse(self.info.query_one("#context-details").display)
        self.app.reset_quit_confirmation()
        await self.pilot.pause()
        self.assertFalse(self.bar.display)
        self.assertTrue(self.info.query_one("#model-details").display)
        self.assertTrue(self.info.query_one("#context-details").display)
        self.assertTrue(self.info.query_one("#mode-actions").display)
        self.assertEqual(self.info.query_one("#model-info").content, "updated-model")
        self.assertEqual(self.info.query_one("#reasoning-info").content, " | high")
        self.assertEqual(self.info.query_one("#context-info").content, "2 500 / 200 000")
        self.assertEqual(self.mode.styles.background.hex, SLOP_COLOR.upper())
        phase = self.mode.phase
        await self.pilot.pause(0.1)
        self.assertEqual(self.mode.phase, phase)  # The shared clock is paused again at rest.

    async def test_confirmation_hides_the_history_loader_and_restores_it(self):
        self.info.set_history_loading(True)
        self.info.show_confirmation("exit", 101.0, 1.0)
        await self.pilot.pause()
        self.assertFalse(self.info.query_one("#context-details").display)
        self.assertTrue(self.bar.display)
        self.info.hide_confirmation("exit")
        await self.pilot.pause()
        self.assertTrue(self.info.query_one(HistoryLoadingBar).display)
        self.assertTrue(self.info.query_one("#context-details").display)
        self.info.set_history_loading(False)
        await self.pilot.pause()
        self.assertFalse(self.info.query_one(HistoryLoadingBar).display)
        self.assertTrue(self.info.query_one("#context-info").display)

    async def test_resize_keeps_full_width_and_one_line_without_restarting_countdown(self):
        self.info.show_confirmation("cancel", 101.0, 1.0)
        for width in (120, 24, 1):
            with self.subTest(width=width):
                await self.pilot.resize_terminal(width, 24)
                self.assertEqual(self.bar.region.width, width)
                self.assertEqual(self.bar.region.height, 1)
                self.assertEqual(self.bar.region, self.info.region)
                self.assertEqual(self.bar.render().plain, "Cancel Execution ? Confirm with ESC"[:width].center(width))
                self.assertEqual(self.bar.deadline, 101.0)

    async def test_confirmation_shares_the_pulse_clock_and_ticks_only_repaint(self):
        self.app.busy = True
        await self.app.request_execution_cancel()
        await self.pilot.pause()
        with patch.object(self.info, "reflow", wraps=self.info.reflow) as reflow, \
                patch.object(self.bar, "refresh", wraps=self.bar.refresh) as refresh, \
                patch.object(self.mode, "post_message") as post:
            self.mode.spinner_frame = -1
            self.mode.tick()
            messages = [call.args[0] for call in post.call_args_list]
            self.assertTrue(any(isinstance(message, ModeButton.SpinnerFrame) for message in messages))
            self.assertTrue(any(isinstance(message, ModeButton.ConfirmationFrame) for message in messages))
            self.info.on_mode_button_confirmation_frame(ModeButton.ConfirmationFrame())
            refresh.assert_called_once()
            reflow.assert_not_called()
        with patch.object(self.bar, "refresh", wraps=self.bar.refresh) as repaint:
            await self.pilot.pause(0.15)
            self.assertGreater(repaint.call_count, 0)  # The hidden mode button still drives the visible countdown.
