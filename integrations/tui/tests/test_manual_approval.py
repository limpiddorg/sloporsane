"""Manual authorization never rewrites or executes the historical tool call."""

import unittest
from unittest.mock import AsyncMock, patch

from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput
from textual.widgets import Button

from sos_tui.config import ALGAE_MD, BOLD_MD, DEPTH_MD, MUTED_COLOR, SANE_COLOR, SLOP_COLOR, TEXT_COLOR
from sos_tui.history_viewport import HistoryActivity, HistoryViewport
from sos_tui.manual_approval import APPROVAL_BACKGROUND, APPROVAL_HOVER_BACKGROUND, ManualApprovalView
from sos_tui.tool_view import ToolView


class ManualApprovalTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 40)))
        self.app.connected = True
        self.app.session_id = "session"
        self.app.client.send = AsyncMock()
        self.arguments = {"command": "echo test > file"}
        self.result = {"type": "tool_result", "callId": "call", "output": "Refused", "isError": True,
                       "metadata": {"permissions": [{"id": "1", "decision": "ai_refused"}], "refused_message_id": "result"}}
        self.app.display_message({"id": "u", "role": "user", "content": [{"type": "text", "text": "Try"}]})
        self.app.display_message({"id": "a", "role": "assistant", "content": [
            {"type": "tool_call", "callId": "call", "toolName": "bash", "arguments": self.arguments}]})
        self.app.display_message({"id": "result", "role": "tool", "content": [self.result]})
        await self.pilot.pause()
        self.drawer = self.app.current_activity
        self.drawer.toggle.action_toggle()
        await self.pilot.pause()
        self.row = self.drawer.query_one(ManualApprovalView)
        self.row.collapsed = False
        await self.pilot.pause()

    async def test_approval_is_separate_and_waits_for_server_acknowledgement(self):
        self.assertEqual(self.row.title, "bash • AI")
        self.assertTrue(self.drawer.separator.display)
        self.assertTrue(self.drawer.refusals.display)
        self.assertEqual(str(self.drawer.refusals_heading.content), "Refused tools")
        self.assertTrue(self.drawer.refusals_heading.display)
        self.assertTrue(self.drawer.history_heading.display)
        self.assertEqual(str(self.drawer.history_heading.content), "History")
        history_segment = next(segment for segment in self.drawer.history_heading.render_line(0) if "History" in segment.text)
        self.assertEqual(history_segment.style.color.get_truecolor().hex.lower(), BOLD_MD.lower())
        self.assertEqual(self.drawer.history_heading.content_region.x, self.drawer.refusals_heading.content_region.x)
        self.assertEqual(self.drawer.history_heading.region.y, self.drawer.separator.region.bottom)
        self.assertEqual(self.drawer.body.region.y, self.drawer.history_heading.region.bottom)
        self.assertTrue(self.drawer.toggle.has_refused)
        self.assertTrue(any(str(span.style) == SLOP_COLOR for span in self.drawer.toggle.render().spans))
        self.assertEqual(self.drawer.separator.styles.color.hex.lower(), MUTED_COLOR.lower())
        self.assertEqual(self.row.query_one("CollapsibleTitle").styles.color.hex.lower(), SLOP_COLOR.lower())
        self.assertEqual(self.drawer.separator.region.x, self.row.query_one("CollapsibleTitle").content_region.x)
        self.assertEqual(self.drawer.separator.region.x, self.drawer.refusals_heading.content_region.x)
        self.assertEqual(self.drawer.separator.region.x, self.drawer.history_heading.content_region.x)
        self.assertEqual(self.drawer.separator.region.right, self.app.screen.size.width)
        self.assertEqual(self.drawer.separator.render_line(0).text,
                         "─" * (self.app.screen.size.width - self.drawer.separator.region.x))
        self.assertLess(self.drawer.separator.region.y, self.drawer.body.region.y)
        self.app.client.send.assert_not_awaited()
        await self.pilot.click(self.row.button)
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "permission.manual")
        self.assertEqual(request["refusedMessageId"], "result")
        self.assertEqual(request["sessionId"], "session")
        self.assertTrue(request["approved"])
        self.assertFalse(self.row.approved)
        self.assertTrue(self.row.button.disabled)
        self.app.handle_event({"type": "permission.manual_updated", "requestId": request["requestId"],
                               "sessionId": "session", "states": {"result": True}})
        await self.pilot.pause()
        self.assertTrue(self.row.approved)
        self.assertIn("Human", self.row.title)
        self.assertEqual(str(self.row.button.label), "Revoke approval")
        self.assertEqual(self.row.query_one("CollapsibleTitle").styles.color.hex.lower(), SANE_COLOR.lower())
        self.assertTrue(any(str(span.style) == SLOP_COLOR for span in self.drawer.toggle.render().spans))
        original = self.drawer.body.query_one(ToolView)
        self.assertIn(" • AI • ", original.title)
        self.assertEqual(original.result, self.result)
        self.assertEqual(self.app.client.send.await_count, 1)
        await self.pilot.click(self.row.button)
        await self.pilot.pause()
        self.assertFalse(self.app.client.send.call_args.args[0]["approved"])
        request = self.app.client.send.call_args.args[0]
        self.app.handle_event({"type": "permission.manual_updated", "requestId": request["requestId"],
                               "sessionId": "session", "states": {"result": False}})
        await self.pilot.pause()
        self.assertEqual(str(self.row.button.label), "Approve")
        self.assertEqual(self.row.query_one("CollapsibleTitle").styles.color.hex.lower(), SLOP_COLOR.lower())

    async def test_save_failure_restores_confirmed_toggle_state(self):
        await self.pilot.click(self.row.button)
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.app.handle_event({"type": "request.failed", "requestId": request["requestId"], "error": "Cannot save"})
        await self.pilot.pause()
        self.assertFalse(self.row.approved)
        self.assertEqual(str(self.row.button.label), "Approve")
        self.assertFalse(self.row.button.disabled)
        self.assertIn("Cannot save", str(self.row.error.content))
        self.assertEqual(self.app.client.send.await_count, 1)

    async def test_run_confidence_stays_visible_when_approved_and_revoked(self):
        item = {"toolName": "bash", "arguments": self.arguments, "result": {
            **self.result, "metadata": {**self.result["metadata"], "permissions": [
                {"decision": "ai_accepted", "confidence": 0.91},
                {"decision": "ai_refused", "confidence": 0.02}]}}}
        row = ManualApprovalView(item)
        await self.drawer.refusals.mount(row)
        self.assertEqual(row.title, "bash • AI 2%")
        row.set_approved(True)
        self.assertEqual(row.title, "bash • Human (AI 2%)")
        await self.pilot.pause()
        self.assertEqual(row.query_one("CollapsibleTitle").styles.color.hex.lower(), SANE_COLOR.lower())
        row.set_approved(False)
        self.assertEqual(row.title, "bash • AI 2%")
        await self.pilot.pause()
        self.assertEqual(row.query_one("CollapsibleTitle").styles.color.hex.lower(), SLOP_COLOR.lower())

    async def test_replay_displays_real_tool_and_exact_arguments_not_wrapper(self):
        self.app.display_message({"role": "assistant", "content": [{"type": "tool_call", "callId": "replay",
            "toolName": "rerun_approved_action", "arguments": {"id": "approval-1"}}]})
        self.app.handle_event({"type": "permission.resolved", "callId": "replay", "permissionId": "p",
                               "decision": "manually_approved", "tool": "bash", "details": {"arguments": self.arguments}})
        pending = self.app.pending_tools["replay"]
        self.assertEqual(pending.title, "bash • Human • Pending")
        self.app.display_message({"role": "tool", "content": [{"type": "tool_result", "callId": "replay", "output": "OK",
            "isError": False, "metadata": {"exit": 0, "approved_action": {
                "id": "approval-1", "tool": "bash", "arguments": self.arguments},
                "permissions": [{"decision": "manually_approved"}, {"decision": "manually_approved"}]}}]})
        await self.pilot.pause()
        replay = next(tool for tool in self.app.query(ToolView) if tool.call_id == "replay")
        self.assertEqual(replay.title, "bash • Human • Completed · Exit code 0")
        self.assertEqual(replay.arguments, self.arguments)
        self.assertIn(self.arguments["command"], str(replay.arguments_view.content))

    async def test_history_restores_approval_and_loads_request_details_lazily(self):
        item = {"kind": "tool", "toolName": "bash", "callId": "call", "activityIndex": 0,
                "result": {**self.result, "metadata": {**self.result["metadata"], "manually_approved": True}}}
        drawer = HistoryActivity({"id": "old/activity/0", "kinds": ["tools"]})
        await self.app.query_one(HistoryViewport).mount(drawer)
        await drawer.populate({"activities": [item]})
        row = drawer.query_one(ManualApprovalView)
        self.assertTrue(row.approved)
        self.assertFalse(row.details_loaded)
        self.assertIsNone(row.button)
        self.app.history_window.activity_detail = AsyncMock(return_value={**item, "arguments": self.arguments})
        drawer.toggle.action_toggle()
        await self.pilot.pause()
        row.collapsed = False
        await self.pilot.pause()
        self.assertTrue(row.details_loaded)
        self.assertIn("echo test > file", str(row.arguments_view.content))
        self.assertEqual(str(row.button.label), "Revoke approval")
        self.assertEqual(row.query_one("CollapsibleTitle").styles.color.hex.lower(), SANE_COLOR.lower())
        self.assertIn(" • AI • ", drawer.body.query_one(ToolView).title)

    async def test_legacy_refusal_is_visible_but_cannot_grant_unknown_action(self):
        item = {"kind": "tool", "toolName": "bash", "callId": "call", "arguments": self.arguments,
                "result": {**self.result, "metadata": {"permissions": [{"decision": "ai_refused"}]}}}
        row = ManualApprovalView(item)
        self.assertFalse(row.approved)
        self.assertIsNone(row.button)
        await self.drawer.refusals.mount(row)
        row.collapsed = False
        await self.pilot.pause()
        self.assertTrue(row.button.disabled)

    async def test_collapsing_activity_hides_both_sections_and_separator(self):
        self.drawer.toggle.action_toggle()
        await self.pilot.pause()
        self.assertFalse(self.drawer.body.display)
        self.assertFalse(self.drawer.refusals.display)
        self.assertFalse(self.drawer.separator.display)
        self.assertFalse(self.drawer.refusals_heading.display)
        self.assertFalse(self.drawer.history_heading.display)
        self.assertIsNone(self.row.button)

    async def test_next_message_purge_hides_and_releases_manual_rows(self):
        await self.app.query_one(HistoryViewport).purge_activities()
        self.assertFalse(self.drawer.refusals.display)
        self.assertFalse(self.drawer.separator.display)
        self.assertFalse(self.drawer.refused_views)
        self.assertFalse(self.drawer.history_heading.display)
        self.assertEqual(len(self.drawer.refusals.children), 0)

    async def test_history_heading_is_absent_without_refused_tools(self):
        drawer = HistoryActivity.live("normal")
        await self.app.query_one(HistoryViewport).mount(drawer)
        drawer.toggle.action_toggle()
        await self.pilot.pause()
        self.assertTrue(drawer.body.display)
        self.assertFalse(drawer.history_heading.display)
        self.assertFalse(drawer.refusals_heading.display)
        self.assertFalse(drawer.separator.display)

    async def test_button_is_destroyed_on_collapse_and_recreated_with_current_state(self):
        previous = self.row.button
        self.row.collapsed = True
        await self.pilot.pause()
        self.assertIsNone(self.row.button)
        self.assertEqual(len(self.row.query(Button)), 0)
        self.row.set_approved(True)
        self.row.collapsed = False
        await self.pilot.pause()
        self.assertIsNot(self.row.button, previous)
        self.assertEqual(str(self.row.button.label), "Revoke approval")
        self.assertEqual(len(self.row.query(Button)), 1)

    async def test_reopening_during_save_keeps_button_disabled(self):
        await self.pilot.click(self.row.button)
        self.row.collapsed = True
        await self.pilot.pause()
        self.row.collapsed = False
        await self.pilot.pause()
        self.assertTrue(self.row.button.disabled)
        self.assertEqual(self.app.client.send.await_count, 1)

    async def test_empty_metadata_and_error_leave_no_space_below_button(self):
        self.assertFalse(self.row.context_view.display)
        self.assertFalse(self.row.error.display)
        self.assertEqual(self.row.arguments_view.region.bottom, self.row.button.region.y)
        self.assertEqual(self.row.button.region.height, 1)
        self.assertEqual(self.row.button.region.bottom, self.row.region.bottom)
        self.assertEqual(self.row.region.bottom, self.drawer.separator.region.y)

    async def test_unloaded_history_summary_already_shows_refusal_marker(self):
        drawer = HistoryActivity({"id": "unloaded", "kinds": ["tools", "refused"]})
        self.assertTrue(drawer.toggle.has_refused)
        self.assertFalse(drawer.built)
        self.assertFalse(drawer.refused_views)

    async def test_button_colors_stay_contrasted_on_hover_focus_and_press(self):
        # Disable automatic composer focus to isolate button styling.
        self.app.query_one(MessageInput).disabled = True
        button = self.row.button
        def assert_rendered(background, foreground):
            segments = list(button.render_line(0))
            visible = [segment for segment in segments if segment.text.strip()]
            self.assertTrue(visible)
            self.assertEqual("".join(segment.text for segment in segments).strip(), str(button.label))
            for segment in visible:
                self.assertEqual(segment.style.color.triplet.hex.lower(), foreground.lower())
                self.assertEqual(segment.style.bgcolor.triplet.hex.lower(), background.lower())
                self.assertTrue(segment.style.bold)
                self.assertFalse(segment.style.underline)
            self.assertEqual(button.region.height, 1)
            self.assertEqual(button.content_region.height, 1)

        for approved, foreground in ((False, ALGAE_MD), (True, SLOP_COLOR)):
            self.row.set_approved(approved)
            self.app.set_focus(None)
            await self.pilot.hover(self.drawer.toggle)
            await self.pilot.pause()
            assert_rendered(APPROVAL_BACKGROUND, foreground)
            await self.pilot.hover(button)
            await self.pilot.pause()
            assert_rendered(APPROVAL_HOVER_BACKGROUND, foreground)
            button.focus()
            await self.pilot.pause()
            assert_rendered(DEPTH_MD, TEXT_COLOR)
            await self.pilot.hover(self.drawer.toggle)
            await self.pilot.pause()
            assert_rendered(DEPTH_MD, TEXT_COLOR)
            self.app.set_focus(None)
            await self.pilot.pause()
            assert_rendered(APPROVAL_BACKGROUND, foreground)
            size = button.region.size
            await self.pilot.click(button)
            await self.pilot.pause()
            self.assertTrue(button.disabled)
            self.assertFalse(button.has_class("-active"))
            self.assertEqual(button.active_effect_duration, 0)
            self.assertEqual(button.region.size, size)
            assert_rendered(APPROVAL_BACKGROUND, foreground)
