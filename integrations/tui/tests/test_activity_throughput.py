"""Per-activity model throughput excludes tool time and survives history import."""

import unittest
from unittest.mock import AsyncMock, patch

from sos_tui.app import SosApp
from sos_tui.history_viewport import HistoryActivity, HistoryViewport
from sos_tui.turn_activity import ActivityToggle
from sos_tui.mode_button import ModeButton


def call(duration, tokens=None):
    result = {"model": "test-model", "reasoning": "none", "duration_ms": duration}
    if tokens is not None:
        result["output_tokens"] = tokens
    return result


class ActivityThroughputTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(160, 40)))

    async def mount_activity(self, **fields):
        drawer = HistoryActivity({"id": "group", "kinds": [], "terminal": False, "duration_ms": 50000, **fields})
        await self.app.query_one(HistoryViewport).mount(drawer)
        await self.pilot.pause()
        return drawer.toggle

    async def test_weighted_average_uses_model_time_not_activity_time(self):
        toggle = await self.mount_activity()
        toggle.record_model_call(call(1000, 10))
        toggle.record_model_call(call(3000, 90))
        self.assertIn("50s - 25 tok/s", toggle.render().plain)
        toggle.duration_ms = 80000  # Tools and permission waits change elapsed time, not the rate.
        toggle.update_elapsed()
        self.assertIn("1m 20s - 25 tok/s", toggle.render().plain)

    async def test_completed_header_has_no_spinner_before_time_for_any_activity(self):
        toggle = await self.mount_activity(model_call=call(4000, 100))
        toggle.record_activity("tools")
        for terminal in (False, True):
            toggle.is_terminal = terminal
            for frame in (None, 0, 1, 2, 3):
                toggle.set_spinner(frame)
                self.assertEqual(toggle.render().plain, "▸ Activity ▪ 50s - 25 tok/s")

    async def test_spinner_runs_before_time_then_disappears_on_completion(self):
        toggle = await self.mount_activity(model_call=call(4000, 100), terminal=True)
        with patch("sos_tui.turn_activity.monotonic", return_value=150.0):
            toggle.start_generation({"model": "test-model", "reasoning": "none"}, 100.0)
            for frame, character in enumerate(toggle.SPINNER_FRAMES):
                toggle.set_spinner(frame)
                self.assertEqual(toggle.render().plain, f"▸ Activity {character} 50s - 25 tok/s")
            toggle.finish_generation()
        self.assertEqual(toggle.render().plain, "▸ Activity 50s - 25 tok/s")

    async def test_unknown_usage_and_zero_duration_omit_rate_and_separator(self):
        for model_call in (None, call(1000), call(0, 10)):
            fields = {"model_call": model_call} if model_call is not None else {}
            toggle = await self.mount_activity(generation={"model": "test-model", "reasoning": "high"}, **fields)
            self.assertNotIn("tok/s", toggle.render().plain)
            self.assertNotIn(" - ", toggle.render().plain)
        toggle = await self.mount_activity()
        toggle.record_model_call(call(1000, 10))
        toggle.record_model_call(None)
        toggle.record_model_call(call(1000, 20))
        self.assertNotIn("tok/s", toggle.render().plain)
        zero = await self.mount_activity(model_call=call(1000, 0))
        self.assertIn(" - 0 tok/s", zero.render().plain)

    async def test_history_restores_model_reasoning_and_aggregate_without_loading_details(self):
        toggle = await self.mount_activity(model_call=call(4000, 100))
        self.assertIn("50s - 25 tok/s", toggle.render().plain)
        self.assertFalse(toggle.parent.built)
        legacy = await self.mount_activity(generation={"model": "old-model", "reasoning": "high"})
        self.assertIn("50s", legacy.render().plain)
        self.assertNotIn("old-model", legacy.render().plain)
        self.assertNotIn(" - ", legacy.render().plain)

    async def test_live_response_is_counted_once_in_the_activity_waiting_for_it(self):
        self.app.busy = True
        self.app.display_message({"id": "u", "role": "user", "content": [{"type": "text", "text": "Run"}]})
        self.app.display_message({"id": "a1", "role": "assistant", "model_call": call(1000, 10), "content": [
            {"type": "reasoning", "text": "Thinking"}, {"type": "text", "text": "Progress"},
            {"type": "tool_call", "callId": "c1", "toolName": "bash", "arguments": {"command": "first"}}]})
        self.app.display_message({"id": "t1", "role": "tool", "content": [
            {"type": "tool_result", "callId": "c1", "output": "Long tool", "isError": False,
             "metadata": {"output_tokens": 9999, "duration_ms": 8000}}]})
        self.app.display_message({"id": "a2", "role": "assistant", "model_call": call(3000, 90), "content": [
            {"type": "tool_call", "callId": "c2", "toolName": "bash", "arguments": {"command": "second"}}]})
        self.app.display_message({"id": "t2", "role": "tool", "content": [
            {"type": "tool_result", "callId": "c2", "output": "Long tool", "isError": False}]})
        self.app.display_message({"id": "a3", "role": "assistant", "model_call": call(1000, 10),
            "metadata": {"model": "test-model", "reasoning": "none", "duration_ms": 19000},
            "content": [{"type": "text", "text": "Done"}]})
        await self.pilot.pause()
        toggles = list(self.app.query(ActivityToggle))
        self.assertEqual(len(toggles), 2)
        self.assertEqual(toggles[0].output_tokens, 10)
        self.assertNotIn("test-model", toggles[0].render().plain)
        self.assertIn(" - 10 tok/s", toggles[0].render().plain)
        self.assertEqual(toggles[1].output_tokens, 100)
        self.assertEqual(toggles[1].model_duration_ms, 4000)
        self.assertIn("19s - 25 tok/s", toggles[1].render().plain)

    async def test_unknown_activity_time_is_not_replaced_with_model_time(self):
        toggle = await self.mount_activity(duration_ms=None, model_call=call(4000, 100))
        self.assertEqual(toggle.duration_label, "")
        self.assertNotIn("tok/s", toggle.render().plain)

    async def test_new_activity_created_while_away_uses_server_start_checkpoint(self):
        app = self.app
        app.session_id = "saved"
        app.busy = True
        app.query_one(ModeButton).animation_timer.pause()
        app.request_started_at = 100.0
        app.activity_clock = ("old/activity/0", 100.0, {"model": "old", "reasoning": "low"})
        record = {"id": "new/activity/0", "role": "activity", "content": [], "kinds": ["tools"],
                  "terminal": True, "started_ms": 20000, "model_call": call(4000, 100),
                  "generation": {"model": "test-model", "reasoning": "none"}, "activities": []}
        app.history_window.navigate = AsyncMock(return_value={"session": {"id": "saved", "mode": "slop"},
            "page": {"hasNewer": False}, "messages": [record]})
        app.history_window.activities = AsyncMock(return_value=record)
        with patch("sos_tui.turn_activity.monotonic", return_value=150.0):
            self.assertTrue(await app.load_history_page("latest"))
            await self.pilot.pause()
            toggle = app.running_activity.toggle
            self.assertEqual(toggle.started_at, 120.0)
            self.assertEqual(toggle.duration_label, "30s")
            app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(2))
            self.assertIn("| 30s - 25 tok/s", toggle.render().plain)
            self.assertEqual(toggle.model, "test-model")

    async def test_navigation_restores_pending_tool_clock_without_resetting_usage(self):
        app = self.app
        app.session_id = "saved"
        app.busy = True
        app.query_one(ModeButton).animation_timer.pause()
        app.update_agent_info({"model": "test-model", "reasoning": "none"})
        app.display_message({"id": "u", "role": "user", "content": [{"type": "text", "text": "Run"}]})
        app.display_message({"id": "a", "role": "assistant", "model_call": call(4000, 100), "content": [
            {"type": "tool_call", "callId": "pending", "toolName": "bash", "arguments": {"command": "test"}}]})
        app.running_activity.toggle.started_at = 100.0
        await self.pilot.pause()
        record = {"id": "u/activity/0", "role": "activity", "content": [], "kinds": ["tools"],
                  "terminal": True, "model_call": call(4000, 100), "activities": [
                      {"kind": "tool", "toolName": "bash", "callId": "pending", "arguments": {"command": "test"}}]}
        base = {"session": {"id": "saved", "mode": "slop"}}
        app.history_window.navigate = AsyncMock(side_effect=[
            {**base, "page": {"hasNewer": True}, "messages": []},
            {**base, "page": {"hasNewer": False}, "messages": [record]}])
        app.history_window.activities = AsyncMock(return_value=record)
        self.assertTrue(await app.load_history_page("before"))
        self.assertIsNone(app.running_activity)
        app.update_agent_info({"model": "next-model", "reasoning": "high"})
        with patch("sos_tui.turn_activity.monotonic", return_value=150.0):
            self.assertTrue(await app.load_history_page("latest"))
            await self.pilot.pause()
            toggle = app.running_activity.toggle
            self.assertEqual(toggle.started_at, 100.0)
            self.assertEqual(toggle.model, "test-model")
            self.assertEqual(toggle.output_tokens, 100)
            self.assertEqual(toggle.model_duration_ms, 4000)
            self.assertIn("pending", app.pending_tools)
            for index, character in enumerate(toggle.SPINNER_FRAMES):
                app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(index))
                self.assertIn(f"{character} 1 step | 50s - 25 tok/s", toggle.render().plain)
        with patch("sos_tui.turn_activity.monotonic", return_value=151.0):
            app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(1))
            self.assertEqual(toggle.duration_label, "51s")
            app.handle_event({"type": "execution.completed", "metadata": {
                "model": "test-model", "reasoning": "none", "duration_ms": 51000}})
        self.assertIsNone(toggle.started_at)
        self.assertEqual(toggle.render().plain, "▸ Activity ▪ 1 step | 51s - 25 tok/s")

    async def test_step_count_restores_without_loading_or_counting_model_calls(self):
        toggle = await self.mount_activity(count=12, kinds=["tools"], model_call=call(4000, 100))
        self.assertEqual(toggle.render().plain, "▸ Activity ▪ 12 steps | 50s - 25 tok/s")
        toggle.record_model_call(call(4000, 100))
        self.assertEqual(toggle.step_count, 12)
        self.assertFalse(toggle.parent.built)

    async def test_live_steps_count_reasoning_and_tool_once_not_result_or_refusal(self):
        self.app.busy = True
        self.app.display_message({"id": "user", "role": "user", "content": [{"type": "text", "text": "Task"}]})
        self.app.display_message({"id": "call", "role": "assistant", "content": [
            {"type": "reasoning", "text": "Thinking"},
            {"type": "tool_call", "callId": "read", "toolName": "read", "arguments": {"filePath": "x"}}]})
        toggle = self.app.current_activity.toggle
        self.assertEqual(toggle.step_count, 2)
        self.app.display_message({"id": "result", "role": "tool", "content": [
            {"type": "tool_result", "callId": "read", "output": "Contents", "isError": False}]})
        toggle.record_activity("refused")
        self.assertEqual(toggle.step_count, 2)
