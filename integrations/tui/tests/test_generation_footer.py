"""Model settings belong below the final response, not in Activity headers."""

import unittest
from unittest.mock import patch

from sos_tui.app import SosApp
from sos_tui.config import MUTED_COLOR
from sos_tui.history_viewport import HistoryViewport
from sos_tui.response_markdown import GenerationFooter, ResponseMarkdown
from sos_tui.turn_activity import ActivityToggle
from sos_tui.user_message import UserMessage
from sos_tui.message_input import MessageInput
from sos_tui.mode_button import ModeButton


class GenerationFooterTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(120, 40)))
        self.metadata = {"model": "historical-model", "reasoning": "none", "duration_ms": 12000}
        self.clock = self.enterContext(patch("sos_tui.response_markdown.monotonic", return_value=100.0))
        self.app.request_started_at = 88.0

    def message(self, text, **fields):
        return {"id": "final", "role": "assistant", "content": [{"type": "text", "text": text}], **fields}

    def assert_footer(self):
        footers = list(self.app.query(GenerationFooter))
        self.assertEqual(len(footers), 1)
        footer = footers[0]
        self.assertEqual(str(footer.render()), "historical-model | No reasoning | 12s")
        self.assertEqual(footer.styles.text_align, "right")
        self.assertEqual(footer.region.height, 1)
        self.assertEqual(footer.region.right, footer.parent.content_region.right - footer.styles.margin.right)
        self.assertEqual(footer.region.y, footer.parent.children[-2].region.bottom)
        line = footer.render_line(0).text
        self.assertTrue(line.endswith("historical-model | No reasoning | 12s"))
        self.assertTrue(line.startswith(" "))
        for segment in footer.render_line(0):
            if segment.text.strip():
                self.assertEqual(segment.style.color.get_truecolor().hex.lower(), MUTED_COLOR.lower())
        for toggle in self.app.query(ActivityToggle):
            self.assertNotIn("historical-model", toggle.render().plain)
            self.assertNotIn("No reasoning", toggle.render().plain)

    async def test_live_footer_only_on_last_text_and_keeps_historical_settings(self):
        self.app.display_message({"id": "u", "role": "user", "content": [{"type": "text", "text": "Hello"}]})
        self.app.display_message(self.message("Progress", id="progress"))
        await self.pilot.pause()
        self.assertFalse(self.app.query(GenerationFooter))
        final = self.message("First paragraph", metadata=self.metadata)
        final["content"].extend([{"type": "text", "text": "Last paragraph"}, {"type": "text", "text": "  "}])
        self.app.display_message(final)
        await self.pilot.pause()
        self.assert_footer()
        self.app.update_agent_info({"model": "next-model", "reasoning": "high"})
        self.assert_footer()

    async def test_resume_and_repeated_page_load_keep_one_footer(self):
        viewport = self.app.query_one(HistoryViewport)
        result = {"session": {"id": "saved", "mode": "slop"}, "page": {}, "messages": [
            self.message("First paragraph", metadata=self.metadata),
            self.message("Last paragraph", metadata=self.metadata),
        ]}
        for _ in range(2):
            await viewport.replace(result)
            await self.pilot.pause()
            self.assert_footer()

    async def test_old_messages_without_metadata_do_not_invent_footer(self):
        self.app.display_message(self.message("Old response"))
        await self.pilot.pause()
        self.assertFalse(self.app.query(GenerationFooter))
        markdown = self.app.query_one(ResponseMarkdown)
        self.assertIsNone(markdown.generation)

    async def test_footer_replaces_blank_line_before_next_user_live_and_on_resume(self):
        final = self.message("Done", metadata=self.metadata)
        user = {"id": "next", "role": "user", "content": [{"type": "text", "text": "Next"}]}
        self.app.display_message(final)
        await self.pilot.pause()
        self.app.display_message(user)
        await self.pilot.pause()
        self.assertEqual(self.app.query_one(UserMessage).region.y,
                         self.app.query_one(GenerationFooter).region.bottom)
        result = {"session": {"id": "saved", "mode": "slop"}, "page": {}, "messages": [final, user]}
        for _ in range(2):
            await self.app.query_one(HistoryViewport).replace(result)
            await self.pilot.pause()
            self.assertEqual(self.app.query_one(UserMessage).region.y,
                             self.app.query_one(GenerationFooter).region.bottom)

    async def test_running_footer_follows_activity_and_text_without_changing_with_next_selection(self):
        self.app.update_agent_info(self.metadata)
        self.app.busy = True
        turn = self.app.mount_user_turn("Start")
        await self.pilot.pause()
        viewport = self.app.query_one(HistoryViewport)
        footer = viewport.live_footer
        self.assertIsNotNone(footer)
        self.assertEqual(footer.region.y, turn.activity.region.bottom)
        self.assert_footer()
        self.app.query_one(MessageInput).text = "Next draft"
        self.app.update_agent_info({"model": "next-model", "reasoning": "high"})
        self.assert_footer()
        self.app.display_message(self.message("Progress", id="progress"))
        await self.pilot.pause()
        self.assertIs(viewport.children[-1], footer)
        self.assert_footer()
        self.app.display_message({"id": "tools", "role": "assistant", "content": [
            {"type": "tool_call", "callId": "c", "toolName": "bash", "arguments": {"command": "test"}}]})
        await self.pilot.pause()
        self.assertEqual(footer.region.y, self.app.current_activity.region.bottom)
        self.assert_footer()
        self.app.display_message(self.message("Final answer", metadata=self.metadata))
        self.app.handle_event({"type": "execution.completed", "metadata": self.metadata})
        await self.pilot.pause()
        self.assertIsNone(viewport.live_footer)
        self.assert_footer()

    async def test_pending_footer_survives_history_cleanup_and_is_removed_with_rejected_turn(self):
        self.app.update_agent_info(self.metadata)
        self.app.busy = True
        self.app.pending_turn = self.app.mount_user_turn("Pending")
        turn = self.app.pending_turn
        viewport = self.app.query_one(HistoryViewport)
        await viewport.replace({"session": {"id": "saved", "mode": "slop"}, "page": {}, "messages": []},
                               preview=(turn.user, turn.activity))
        await self.pilot.pause()
        self.assert_footer()
        self.app.discard_pending_turn()
        await self.pilot.pause()
        self.assertFalse(self.app.query(GenerationFooter))

    async def test_live_footer_hides_on_older_pages_and_returns_on_latest(self):
        self.app.update_agent_info(self.metadata)
        self.app.busy = True
        self.app.mount_user_turn("Start")
        viewport = self.app.query_one(HistoryViewport)
        result = {"session": {"id": "saved", "mode": "slop"}, "messages": [self.message("Progress")]}
        for has_newer in (True, False):
            await viewport.replace({**result, "page": {"hasNewer": has_newer}})
            await self.pilot.pause()
            self.assertEqual(viewport.live_footer.display, not has_newer)
        self.assert_footer()
        await viewport.replace({**result, "page": {}, "messages": [self.message("Final", metadata=self.metadata)]})
        await self.pilot.pause()
        self.assertIsNone(viewport.live_footer)
        self.assert_footer()

    async def test_authoritative_execution_settings_and_queue_do_not_duplicate_footer(self):
        self.app.update_agent_info({"model": "selected-model", "reasoning": "high"})
        self.app.busy = True
        self.app.mount_user_turn("Start")
        self.app.update_agent_info({"execution_model": "historical-model", "execution_reasoning": "none",
                                    "model": "next-model", "reasoning": "high"})
        self.app.queue_message("Queued request")
        self.app.display_message(self.message("Progress"))
        await self.pilot.pause()
        viewport = self.app.query_one(HistoryViewport)
        footer = viewport.live_footer
        self.assertEqual(str(footer.render()), "historical-model | No reasoning | 12s")
        self.assertIs(viewport.children[-2], footer)
        self.assertEqual(len(self.app.query(GenerationFooter)), 1)
        self.app.take_queued_message()
        self.app.handle_event({"type": "execution.cancelled", "metadata": self.metadata})
        await self.pilot.pause()
        self.assertIs(viewport.children[-1], footer)
        self.assertEqual(str(footer.render()), "historical-model | No reasoning | 12s")
        self.assertEqual(len(self.app.query(GenerationFooter)), 1)

    async def test_total_clock_keeps_running_across_text_and_new_activities(self):
        self.app.update_agent_info(self.metadata)
        self.app.request_started_at = 100.0
        self.app.busy = True
        self.app.mount_user_turn("Start")
        await self.pilot.pause()
        viewport = self.app.query_one(HistoryViewport)
        footer = viewport.live_footer
        self.assertEqual(str(footer.render()), "historical-model | No reasoning | 0s")
        self.app.display_message(self.message("Intermediate response"))
        self.clock.return_value = 119.0
        self.app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(0))
        self.assertTrue(str(footer.render()).endswith(" | 19s"))
        self.app.display_message({"role": "assistant", "content": [
            {"type": "tool_call", "callId": "new", "toolName": "bash", "arguments": {}}]})
        self.app.update_agent_info({"execution_model": "historical-model", "execution_reasoning": "none"})
        self.clock.return_value = 165.0
        self.app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(1))
        self.assertTrue(str(footer.render()).endswith(" | 1m 05s"))
        self.assertEqual(footer.started_at, 100.0)
        final = {**self.metadata, "duration_ms": 67000}
        self.app.display_message(self.message("Final", metadata=final))
        self.app.handle_event({"type": "execution.completed", "metadata": final})
        await self.pilot.pause()
        saved = self.app.query_one(GenerationFooter)
        self.assertEqual(str(saved.render()), "historical-model | No reasoning | 1m 07s")
        self.clock.return_value = 900.0
        saved.update_elapsed()
        self.assertTrue(str(saved.render()).endswith(" | 1m 07s"))

    async def test_hidden_footer_clock_does_not_restart_when_returning_to_latest(self):
        self.app.update_agent_info(self.metadata)
        self.app.busy = True
        self.app.mount_user_turn("Start")
        viewport = self.app.query_one(HistoryViewport)
        result = {"session": {"id": "saved", "mode": "slop"}, "messages": [self.message("Progress")]}
        await viewport.replace({**result, "page": {"hasNewer": True}})
        self.clock.return_value = 128.0
        await viewport.replace({**result, "page": {"hasNewer": False}})
        self.app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(0))
        self.assertTrue(str(viewport.live_footer.render()).endswith(" | 40s"))
        self.assertEqual(viewport.live_footer.started_at, 88.0)

    async def test_stop_without_terminal_metadata_freezes_local_elapsed_time(self):
        self.app.update_agent_info(self.metadata)
        self.app.busy = True
        self.app.mount_user_turn("Start")
        self.clock.return_value = 111.0
        self.app.busy = False
        footer = self.app.query_one(HistoryViewport).live_footer
        self.assertTrue(str(footer.render()).endswith(" | 23s"))
        self.clock.return_value = 200.0
        footer.update_elapsed()
        self.assertTrue(str(footer.render()).endswith(" | 23s"))

    async def test_missing_historical_duration_is_not_invented(self):
        metadata = {"model": "old-model", "reasoning": "high"}
        footer = GenerationFooter(metadata)
        await self.app.query_one(HistoryViewport).mount_live(footer)
        self.assertEqual(str(footer.render()), "old-model | high")
