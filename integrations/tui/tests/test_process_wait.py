"""Process waiting has its own status line below the Activity contents."""

import unittest
from time import monotonic
from unittest.mock import patch

from rich.color import Color
from sos_tui.app import SosApp
from sos_tui.history_viewport import HistoryViewport
from sos_tui.implementation_step_view import SubstepActivity
from sos_tui.tool_view import ToolView, format_arguments
from sos_tui.turn_activity import TurnActivity


class ProcessWaitTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(140, 30)))
        self.activity = TurnActivity()
        await self.app.query_one(HistoryViewport).mount(self.activity)
        self.toggle = self.activity.toggle
        self.toggle.start_generation({}, monotonic())

    async def add_tool(self, tool="wait", call_id="w", status="Monitoring deployment progress"):
        view = ToolView(call_id, tool, {"command": "monitor", "mainCommand": "gh", "status": status})
        self.activity.add_activity(view, "tools")
        await self.pilot.pause()
        return view

    async def test_wait_keeps_spinner_and_shows_status_below_activity(self):
        view = await self.add_tool()
        self.assertIn(f" {self.toggle.spinner} ", self.toggle.render().plain)
        self.assertNotIn("Monitoring", self.toggle.render().plain)
        self.assertTrue(self.toggle.wait_status.display)
        self.assertIn("Monitoring deployment progress", self.toggle.wait_status.render().plain)
        self.assertEqual(self.toggle.wait_status.region.x, self.toggle.region.x + 2)
        self.assertEqual(self.toggle.wait_status.region.y, self.toggle.region.bottom)
        self.assertTrue(self.toggle.wait_timer._active.is_set())
        self.assertIn("gh", view.title_text().plain)
        self.activity.on_activity_toggle_toggled(self.toggle.Toggled())
        await self.pilot.pause()
        self.assertGreater(self.toggle.wait_status.region.y, self.toggle.region.bottom)
        self.assertEqual(self.toggle.wait_status.region.y, self.activity.body.region.bottom)
        self.assertEqual(self.toggle.wait_status.region.x, self.toggle.region.x + 2)
        view.set_result({"output": "done", "isError": False, "metadata": {"exit": 0}})
        await self.pilot.pause()
        self.assertFalse(self.toggle.wait_status.display)
        self.assertIn(f" {self.toggle.spinner} ", self.toggle.render().plain)
        self.assertFalse(self.toggle.wait_timer._active.is_set())
        self.assertIn("Exit code 0", view.title_text().plain)

    async def test_dots_cycle_in_fixed_width_and_shimmer_is_white_text_only(self):
        await self.add_tool(status="Waiting for CI completion")
        self.toggle.wait_timer.pause()
        phrase = "Waiting for CI completion"
        labels = []
        for elapsed, dots in ((0, ".  "), (.5, ".. "), (1, "..."), (1.5, ".  ")):
            with patch("sos_tui.turn_activity.monotonic", return_value=self.toggle.wait_started + elapsed):
                label = self.toggle.waiting_label()
            self.assertEqual(label.plain, phrase + dots)
            labels.append(label)
        self.assertEqual(len({label.cell_len for label in labels}), 1)
        self.assertNotEqual(labels[0].spans, labels[2].spans)
        highlights = [span for span in labels[2].spans if span.start < len(phrase)]
        self.assertTrue(highlights)
        self.assertTrue(all(span.end <= len(phrase) for span in highlights))
        self.assertTrue(any(min(Color.parse(str(span.style)).get_truecolor()) > 220 for span in highlights))
        for label in labels:
            for span in label.spans:
                self.assertNotIn(" on ", str(span.style))

    async def test_bash_keeps_normal_spinner_without_status(self):
        await self.add_tool("bash")
        self.assertFalse(self.toggle.waiting_processes)
        self.assertFalse(self.toggle.wait_status.display)
        self.assertIsNone(self.toggle.wait_timer)

    async def test_finish_stops_animation_even_without_tool_result(self):
        await self.add_tool()
        self.toggle.finish_generation()
        self.assertFalse(self.toggle.waiting_processes)
        self.assertFalse(self.toggle.wait_timer._active.is_set())
        self.assertFalse(self.toggle.wait_status.display)

    async def test_multiple_waits_finish_independently_and_update_label(self):
        first = await self.add_tool(call_id="first", status="Waiting for CI completion")
        second = await self.add_tool(call_id="second")
        first.set_result({"output": "timeout", "isError": False, "metadata": {"timedOut": True}})
        await self.pilot.pause()
        self.assertEqual(self.toggle.waiting_processes, {"second"})
        self.assertIn("Monitoring deployment progress", self.toggle.wait_status.render().plain)
        self.assertIn("Failed", first.title_text().plain)
        second.set_result({"output": "cancelled", "isError": True,
                           "metadata": {"cancelled": True, "started": True}})
        await self.pilot.pause()
        self.assertFalse(self.toggle.waiting_processes)
        self.assertFalse(self.toggle.wait_status.display)
        self.assertIn("Cancelled", second.title_text().plain)

    async def test_completed_history_never_animates(self):
        view = ToolView("old", "wait", {"mainCommand": "gh", "status": "Waiting for CI completion"})
        view.set_result({"output": "done", "isError": False, "metadata": {"exit": 1}})
        self.toggle.finish_generation()
        self.activity.add_activity(view, "tools")
        await self.pilot.pause()
        self.assertFalse(self.toggle.wait_status.display)
        self.assertIsNone(self.toggle.wait_timer)
        self.assertIn("Exit code 1", view.title_text().plain)

    async def test_collapsed_implementation_waits_without_building_tool_widgets(self):
        activity = SubstepActivity([])
        await self.app.query_one(HistoryViewport).mount(activity)
        activity.toggle.start_generation({}, monotonic())
        messages = [{"content": [{"type": "tool_call", "toolName": "wait", "callId": "ci",
                                 "arguments": {"command": "monitor", "mainCommand": "gh",
                                               "status": "Waiting for CI completion"}}]}]
        activity.append_messages(messages)
        await self.pilot.pause()
        self.assertFalse(activity.tools)
        self.assertTrue(activity.toggle.wait_status.display)
        self.assertIn("Waiting for CI completion", activity.toggle.wait_status.render().plain)
        messages.append({"content": [{"type": "tool_result", "callId": "ci", "output": "done", "isError": False}]})
        activity.append_messages(messages)
        self.assertFalse(activity.toggle.waiting_processes)
        self.assertFalse(activity.toggle.wait_status.display)
        self.assertFalse(activity.toggle.wait_timer._active.is_set())

    async def test_legacy_wait_without_status_has_a_fallback(self):
        view = ToolView("legacy", "wait", {"command": "monitor", "mainCommand": "gh"})
        self.activity.add_activity(view, "tools")
        await self.pilot.pause()
        self.assertIn("Waiting for process completion", self.toggle.wait_status.render().plain)

    def test_arguments_show_wait_default_without_changing_bash(self):
        self.assertIn("3600000 ms", format_arguments({}, "wait").plain)
        self.assertIn("120000 ms", format_arguments({}, "bash").plain)
