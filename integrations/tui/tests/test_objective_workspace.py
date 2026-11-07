"""Implementation planning and read-only Discussion previews."""
import unittest
from unittest.mock import AsyncMock, patch

from textual.widgets import Button
from sos_tui.app import SosApp
from sos_tui.history_viewport import HistoryViewport
from sos_tui.message_input import MessageInput
from sos_tui.mode_button import ModeButton
from sos_tui.objective_workspace import ObjectiveWorkspace, ImplementationPlanWorkspace
from sos_tui.response_markdown import ResponseMarkdown
from sos_tui.implementation_workspace import ImplementationModeScreen


def version(number, text):
    return {"id": f"objective-{number}", "role": "assistant", "objective_plan": True,
            "objective_stack": "- Existing stack", "content": [{"type": "text", "text": text}]}


class ObjectiveWorkspaceTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(120, 35)))
        self.app.connected = True
        self.app.session_id = "discussion"
        self.app.client.send = AsyncMock()
        self.panel = self.app.query_one(ObjectiveWorkspace)
        self.app.update_agent_info({"discussion_id": "discussion", "objective_plans": []})
        await self.pilot.pause()

    async def test_implementation_plan_uses_same_split_and_sane_separator(self):
        from sos_tui.config import SANE_COLOR
        from textual.color import Color
        plan = {"id": "implementation-plan", "role": "assistant", "implementation_plan": {"steps": []},
                "content": [{"type": "text", "text": "## Step 1: Tested foundation"}]}
        self.app.session_id = "branch"
        self.app.update_agent_info({"implementation_view": True, "active_implementation_id": "branch",
                                   "implementation_plan": plan, "implementation_plan_ready": True})
        await self.pilot.pause()
        panel = self.app.query_one(ImplementationPlanWorkspace)
        chat = self.app.query_one(HistoryViewport)
        self.assertFalse(self.panel.display)
        self.assertTrue(panel.display)
        self.assertAlmostEqual(chat.size.width, panel.size.width, delta=2)
        self.assertLessEqual(chat.region.right, panel.region.x)
        self.assertEqual(panel.styles.border_left[1], Color.parse(SANE_COLOR))
        self.assertIn("Tested foundation", panel.query_one(ResponseMarkdown)._markdown)
        self.app.update_agent_info({"implementation_plan_ready": False})
        self.assertIn("Tested foundation", panel.query_one(ResponseMarkdown)._markdown)

    async def test_implementation_panel_interactions_keep_chat_input_focus(self):
        entry = self.app.query_one(MessageInput)
        entry.text = "Draft"
        await self.pilot.press("end")
        self.app.session_id = "branch"
        self.app.update_agent_info({"implementation_view": True, "active_implementation_id": "branch",
                                   "implementation_plan": version(3, "Build foundation"), "implementation_plan_ready": True})
        await self.pilot.pause()
        await self.pilot.click(self.app.query_one(ImplementationPlanWorkspace).query_one(ResponseMarkdown))
        await self.pilot.pause()
        self.assertTrue(entry.has_focus)
        await self.pilot.click(self.app.query_one(ModeButton))
        await self.pilot.pause()
        self.assertTrue(entry.has_focus)
        self.app.client.send.assert_not_awaited()

    async def test_restored_history_hides_unaccepted_versions_and_shows_accepted_plan_in_sane_green(self):
        from textual.color import Color
        from sos_tui.config import SANE_COLOR
        from sos_tui.objective_plan import ObjectivePlan
        from sos_tui.implementation_plan import ImplementationPlan
        viewport = self.app.query_one(HistoryViewport)
        first, second = version(1, "First"), version(2, "Second")
        self.app.update_agent_info({"implementation_branches": [{"id": "branch", "objective_id": first["id"], "frozen": False}]})
        await viewport.replace({"session": {"id": "discussion", "mode": "sane"}, "page": {}, "messages": [first, second]})
        await self.pilot.pause()
        cards = list(viewport.query(ObjectivePlan))
        self.assertTrue(cards[0].display)
        self.assertEqual(str(cards[0].query_one(Button).label), "Implementation")
        self.assertFalse(cards[1].display)
        self.assertEqual(cards[0].styles.border_left[1], Color.parse(SANE_COLOR))
        old = {"id": "old-plan", "role": "assistant", "implementation_plan": {"steps": []},
               "content": [{"type": "text", "text": "Old implementation"}]}
        accepted = dict(old, id="accepted", content=[{"type": "text", "text": "Accepted implementation"}])
        self.app.session_id = "branch"
        self.app.update_agent_info({"implementation_view": True, "implementation_execution": True,
                                   "implementation_plan": accepted, "active_implementation_id": "branch"})
        await viewport.replace({"session": {"id": "branch", "mode": "sane"}, "page": {}, "messages": [old, accepted]})
        await self.pilot.pause()
        cards = list(viewport.query(ImplementationPlan))
        self.assertFalse(cards[0].display)
        self.assertTrue(cards[1].display)
        self.assertEqual(cards[1].styles.border_left[1], Color.parse(SANE_COLOR))
        self.assertEqual(str(cards[1].query_one(Button).label), "Implementation")

    async def test_busy_implementation_can_read_discussion_without_switching_or_cancelling(self):
        from sos_tui.discussion_preview import DiscussionPreview
        self.app.session_id = "branch"
        self.app.update_agent_info({"implementation_view": True, "active_implementation_id": "branch"})
        await self.pilot.pause()
        self.app.busy = True
        self.app.query_one(MessageInput).text = "Implementation draft"
        await self.pilot.click("#discussion")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "session.history")
        self.assertEqual(request["sessionId"], "discussion")
        self.assertTrue(request["requestId"].startswith("preview:"))
        self.assertEqual(self.app.session_id, "branch")
        self.assertTrue(self.app.busy)
        preview = self.app.query_one(DiscussionPreview)
        self.assertTrue(preview.display)
        self.assertFalse(self.app.query_one("#composer").display)
        common = {"requestId": request["requestId"], "sessionId": "discussion"}
        self.app.handle_event(dict(common, type="session.attach_started", session={"id": "discussion", "mode": "sane"},
                                   messageCount=1, page={}))
        import json
        message = {"id": "discussion-message", "role": "assistant", "content": [{"type": "text", "text": "Parent discussion"}]}
        self.app.handle_event(dict(common, type="session.history_chunk", messageIndex=0, partIndex=0,
                                   last=True, data=json.dumps(message)))
        self.app.handle_event(dict(common, type="session.attached", messageCount=1))
        await self.pilot.pause()
        self.assertEqual(preview.window.session_id, "discussion")
        self.assertIn("Parent discussion", preview.query_one(ResponseMarkdown)._markdown)
        self.app.display_live_messages([{"id": "branch-message", "role": "assistant",
                                        "content": [{"type": "text", "text": "Implementation response"}]}])
        self.assertNotIn("Implementation response", preview.query_one(ResponseMarkdown)._markdown)
        self.app.client.send.assert_awaited_once()
        await self.pilot.click("#implement")
        await self.pilot.pause()
        self.assertFalse(preview.display)
        self.assertTrue(self.app.query_one("#composer").display)
        self.assertEqual(self.app.query_one(MessageInput).text, "Implementation draft")
        self.app.client.send.assert_awaited_once()
        self.app.busy = False

    async def test_construction_start_opens_steps_in_existing_implement_tab(self):
        self.app.session_id = "branch"
        self.app.update_agent_info({"implementation_view": True, "active_implementation_id": "branch"})
        await self.pilot.pause()
        self.assertTrue(self.app.query_one("#composer").display)
        self.app.update_agent_info({"implementation_execution": True})
        await self.pilot.pause()
        self.assertEqual(self.app.query_one("#tabs").active, "implement")
        self.assertTrue(self.app.query_one("ImplementationWorkspace").display)
        self.assertFalse(self.app.query_one("#composer").display)
        self.assertFalse(self.app.query("#plan"))

    async def test_idle_planning_keeps_branch_attached_and_shows_accepted_objective_in_discussion(self):
        from sos_tui.discussion_preview import DiscussionPreview
        from textual.widgets import Tabs
        first, second = version(1, "Accepted objective"), version(2, "Unaccepted objective")
        self.app.session_id = "branch"
        self.app.update_agent_info({"implementation_view": True, "active_implementation_id": "branch",
                                   "objective_plans": [first, second],
                                   "implementation_branches": [{"id": "branch", "objective_id": first["id"]}]})
        await self.pilot.pause()
        self.assertFalse(self.app.busy)
        await self.pilot.click("#discussion")
        await self.pilot.pause()
        preview = self.app.query_one(DiscussionPreview)
        self.assertEqual(self.app.session_id, "branch")
        self.assertTrue(preview.display)
        self.assertFalse(preview.query_one("#discussion-preview-open").display)
        self.assertFalse(self.panel.display)
        self.assertGreater(preview.size.width, 100)
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "session.history")
        self.app.busy = True
        self.app.update_agent_info({"implementation_plan_ready": True})
        await self.pilot.pause()
        self.assertEqual(self.app.query_one(Tabs).active, "discussion")
        self.assertTrue(preview.display)
        await self.pilot.click("#implement")
        await self.pilot.pause()
        self.assertEqual(self.app.session_id, "branch")
        self.assertFalse(preview.display)
        self.assertFalse(self.panel.display)
        self.assertTrue(self.app.query_one(ImplementationPlanWorkspace).display)
        self.app.client.send.assert_awaited_once()
        self.app.busy = False

    async def test_accept_implementation_plan_starts_first_step_and_keeps_plan_in_diff_area(self):
        from sos_tui.implementation_workspace import ImplementationModeScreen, ImplementationWorkspace
        from test_implementation_workspace import state
        self.app.session_id = "branch"
        plan = {"id": "accepted", "role": "assistant", "implementation_plan": {"steps": []},
                "content": [{"type": "text", "text": "Accepted implementation"}]}
        self.app.update_agent_info({"implementation_view": True, "active_implementation_id": "branch",
                                   "implementation_plan": plan, "implementation_plan_ready": True})
        await self.pilot.pause()
        await self.pilot.click("#implementation-plan-start")
        self.assertIsInstance(self.app.screen, ImplementationModeScreen)
        await self.pilot.press("enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "implementation.start")
        self.assertEqual(request["pace"], "step_by_step")
        value = state()
        value.update(status="running", work_step=0, work_substep=0)
        value["steps"][0]["whole"]["complete"] = False
        value["steps"][0]["substeps"][0].update(complete=False, diffs=[])
        value["steps"][0]["preparing"] = True
        self.app.handle_event({"type": "implementation.updated", "requestId": request["requestId"],
                               "sessionId": "branch", "implementation": value,
                               "info": {"implementation_execution": True}})
        await self.pilot.pause()
        workspace = self.app.query_one(ImplementationWorkspace)
        self.assertTrue(workspace.display)
        self.assertEqual(workspace.selection, (0, -1))
        self.assertFalse(self.app.query_one(ImplementationPlanWorkspace).display)
        from sos_tui.implementation_step_view import ImplementationStepView
        step_view = workspace.query_one(ImplementationStepView)
        self.assertTrue(step_view.preparation.display)
        self.assertIn("Preparing implementation", str(workspace.step_prompt(0)))
        self.assertIsNone(step_view.active)
        value["steps"][0]["preparing"] = False
        self.app.handle_event({"type": "implementation.updated", "sessionId": "branch", "implementation": value})
        await self.pilot.pause()
        self.assertFalse(step_view.preparation.display)
        self.assertIsNotNone(step_view.active)
        self.assertEqual(workspace.selection, (0, -1))
        await self.pilot.click("#implementation-show-plan")
        await self.pilot.pause()
        self.assertTrue(workspace.display)
        self.assertTrue(workspace.query_one("#implementation-accepted-plan").display)
        self.assertEqual(workspace.selection, (0, -1))

    async def test_live_plan_messages_are_hidden_until_acceptance(self):
        from sos_tui.objective_plan import ObjectivePlan
        from sos_tui.implementation_plan import ImplementationPlan
        objective = version(1, "Objective")
        self.app.display_message(objective)
        await self.pilot.pause()
        card = self.app.query_one(HistoryViewport).query_one(ObjectivePlan)
        self.assertFalse(card.display)
        self.app.update_agent_info({"implementation_branches": [{"id": "branch", "objective_id": objective["id"]}]})
        self.assertTrue(card.display)
        plan = {"id": "accepted", "role": "assistant", "implementation_plan": {"steps": []},
                "content": [{"type": "text", "text": "Implementation"}]}
        self.app.session_id = "branch"
        self.app.update_agent_info({"implementation_view": True, "implementation_plan": plan,
                                   "active_implementation_id": "branch"})
        self.app.display_message(plan)
        await self.pilot.pause()
        card = self.app.query_one(HistoryViewport).query_one(ImplementationPlan)
        self.assertFalse(card.display)
        self.app.update_agent_info({"implementation_execution": True})
        self.assertTrue(card.display)
        await self.pilot.pause()
