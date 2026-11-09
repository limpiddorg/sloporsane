"""Historical Objective Plan drawers at the end of Discussion turns."""
import unittest
from unittest.mock import AsyncMock, patch
from async_fixture import wait_until

from textual.widgets import Button, Collapsible

from sos_tui.app import SosApp
from sos_tui.history_viewport import HistoryViewport
from sos_tui.message_input import MessageInput
from sos_tui.objective_drawer import ObjectivePlanDrawer
from sos_tui.objective_workspace import ObjectiveWorkspace, ImplementationPlanWorkspace
from sos_tui.response_markdown import GenerationFooter, ResponseMarkdown


def version(number, text):
    return {"id": f"objective-{number}", "role": "assistant", "objective_plan": True,
            "objective_stack": "- Existing stack", "content": [{"type": "text", "text": text}]}


def message(id, role="assistant", text="Reply"):
    return {"id": id, "role": role, "content": [{"type": "text", "text": text}]}


class ObjectiveDrawerTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(120, 35)))
        self.app.connected = True
        self.app.session_id = "discussion"
        self.app.client.send = AsyncMock()
        self.chat = self.app.query_one(HistoryViewport)
        self.app.update_agent_info({"discussion_id": "discussion", "objective_plans": [], "objective_plan_turns": []})

    async def restore(self, records, turns, plans):
        self.app.update_agent_info({"objective_plans": plans, "objective_plan_turns": turns})
        await self.chat.replace({"session": {"id": "discussion", "mode": "sane"}, "page": {}, "messages": records})
        await self.pilot.pause()

    async def open(self, drawer):
        drawer.collapsed = False
        await self.pilot.pause()

    async def test_no_plan_means_no_drawer_or_split(self):
        await self.restore([message("u", "user"), message("a")], [], [])
        self.assertFalse(self.chat.query(ObjectivePlanDrawer))
        self.assertFalse(self.app.query_one(ObjectiveWorkspace).display)
        self.assertGreater(self.chat.size.width, 110)

    async def test_mode_switch_keeps_historical_sane_drawer_and_its_expansion_visible(self):
        plan = version(1, "Current objective")
        await self.restore([message("a")], [{"message_id": "a", "objective_id": plan["id"], "updated": True}], [plan])
        drawer = self.chat.query_one(ObjectivePlanDrawer)
        await self.open(drawer)
        self.app.set_mode("slop")
        await self.pilot.pause()
        self.assertTrue(drawer.display)
        self.app.set_mode("sane")
        await self.pilot.pause()
        self.assertTrue(drawer.display)
        self.assertFalse(drawer.collapsed)
        self.assertIn("Current objective", drawer.query_one(ResponseMarkdown)._markdown)

    async def test_slop_turns_and_completed_objective_only_affect_future_drawers(self):
        first, updated, next_plan = (version(1, "First objective"), version(2, "Updated objective"),
                                     version(3, "Next objective"))
        records = [message("u1", "user"), first, message("a1")]
        turns = [{"message_id": "a1", "objective_id": first["id"], "updated": True}]
        await self.restore(records, turns, [first])
        original = self.chat.query_one(ObjectivePlanDrawer)
        await self.open(original)

        self.app.set_mode("slop")
        self.app.display_live_messages([message("u2", "user"), message("a2")])
        await self.pilot.pause()
        self.assertEqual(list(self.chat.query(ObjectivePlanDrawer)), [original])
        self.assertTrue(original.display)
        self.assertFalse(original.collapsed)

        self.app.set_mode("sane")
        self.app.display_live_messages([message("u3", "user"), message("a3")])
        turns.append({"message_id": "a3", "objective_id": first["id"], "updated": False})
        self.app.update_agent_info({"objective_plan_turns": turns})
        await self.pilot.pause()
        carried = list(self.chat.query(ObjectivePlanDrawer))[-1]
        self.assertEqual(carried.selected_id, first["id"])
        self.assertEqual(str(carried.title), "Objective plan")

        self.app.display_live_messages([message("u4", "user"), updated, message("a4")])
        turns.append({"message_id": "a4", "objective_id": updated["id"], "updated": True})
        self.app.update_agent_info({"objective_plans": [first, updated], "objective_plan_turns": turns})
        await self.pilot.pause()
        historical = list(self.chat.query(ObjectivePlanDrawer))
        self.assertEqual([drawer.selected_id for drawer in historical], [first["id"], first["id"], updated["id"]])
        self.assertTrue(historical[-1].has_class("updated"))

        self.app.update_agent_info({"objective_plan_ready": False, "implementation_branches": [
            {"id": "implementation", "objective_id": updated["id"]}]})
        self.app.display_live_messages([message("u5", "user"), message("a5")])
        await self.pilot.pause()
        self.assertEqual(list(self.chat.query(ObjectivePlanDrawer)), historical)
        link = historical[-1].query_one(".objective-link", Button)
        self.assertTrue(link.display)
        self.assertEqual(str(link.label), "Implementation")
        self.assertFalse(original.collapsed)
        self.assertIn("First objective", original.query_one(ResponseMarkdown)._markdown)

        self.app.display_live_messages([message("u6", "user"), next_plan, message("a6")])
        turns.append({"message_id": "a6", "objective_id": next_plan["id"], "updated": True})
        self.app.update_agent_info({"objective_plans": [first, updated, next_plan], "objective_plan_turns": turns})
        await self.pilot.pause()
        drawers = list(self.chat.query(ObjectivePlanDrawer))
        self.assertEqual(drawers[:-1], historical)
        self.assertEqual(drawers[-1].selected_id, next_plan["id"])

    async def test_one_frozen_drawer_after_last_assistant_per_turn_and_updated_only_when_published(self):
        first, second = version(1, "First plan"), version(2, "Second plan")
        records = [message("u1", "user"), message("intermediate"), first, message("a1"),
                   message("u2", "user"), message("a2"), message("u3", "user"), second, message("a3")]
        turns = [{"message_id": id, "objective_id": plan["id"], "updated": updated}
                 for id, plan, updated in (("a1", first, True), ("a2", first, False), ("a3", second, True))]
        await self.restore(records, turns, [first, second])
        drawers = list(self.chat.query(ObjectivePlanDrawer))
        self.assertEqual([str(drawer.title) for drawer in drawers],
                         ["Objective plan · updated", "Objective plan", "Objective plan · updated"])
        self.assertTrue(all(drawer.collapsed and not drawer.built for drawer in drawers))
        from textual.color import Color
        for drawer, color in zip(drawers, ("sane", "muted", "sane")):
            expected = Color.parse(self.app.get_css_variables()[color])
            self.assertEqual(drawer.query_one(".objective-separator").styles.color, expected)
            self.assertEqual(drawer.query_one("CollapsibleTitle").styles.color, expected)
        children = list(self.chat.children)
        for drawer, id in zip(drawers, ("a1", "a2", "a3")):
            anchor = next(widget for record, widget in self.chat.rendered if record["id"] == id)
            self.assertIs(children[children.index(anchor) + 1], drawer)
        await self.open(drawers[0])
        self.assertIn("First plan", drawers[0].query_one(ResponseMarkdown)._markdown)
        self.assertIn("Existing stack", drawers[0].query_one(ResponseMarkdown)._markdown)
        self.app.update_agent_info({"objective_plans": [first, second, version(3, "Third plan")]})
        await self.pilot.pause()
        self.assertFalse(drawers[0].collapsed)
        self.assertNotIn("Third plan", drawers[0].query_one(ResponseMarkdown)._markdown)
        self.app.query_one(MessageInput).text = "Discussion draft"
        button = drawers[0].query_one(".objective-implement", Button)
        # The draft edit and Markdown both reflow before hit testing is reliable.
        await self.pilot.pause()
        button.scroll_visible(animate=False, immediate=True)
        await self.pilot.pause()
        await wait_until(self.pilot, lambda: self.app.screen.can_view_entire(button)
                         and self.app.screen.get_widget_at(*button.region.offset)[0] is button,
                         "historical plan's implementation button to be clickable")
        self.assertTrue(await self.pilot.click(button))
        await wait_until(self.pilot, lambda: self.app.client.send.called,
                         "historical plan's implementation request")
        sent = self.app.client.send.call_args.args[0]
        self.assertEqual((sent["type"], sent["objectiveId"]), ("implementation.open", first["id"]))
        self.assertEqual(self.app.query_one(MessageInput).text, "Discussion draft")

    async def test_live_completion_before_mount_places_drawer_after_response(self):
        plan = version(1, "Live plan")
        await self.restore([], [], [])
        self.app.busy = True
        self.app.display_live_messages([message("u", "user"), plan, message("a")])
        self.assertFalse(self.chat.query(ObjectivePlanDrawer))
        self.app.handle_event({"type": "execution.completed", "info": {"objective_plans": [plan],
            "objective_plan_turns": [{"message_id": "a", "objective_id": plan["id"], "updated": True}]}})
        await self.pilot.pause()
        drawer = self.chat.query_one(ObjectivePlanDrawer)
        self.assertEqual(str(drawer.title), "Objective plan · updated")
        anchor = next(widget for record, widget in self.chat.rendered if record["id"] == "a")
        children = list(self.chat.children)
        self.assertIs(children[children.index(anchor) + 1], drawer)

    async def test_page_without_original_plan_and_replacement_keep_correct_drawer(self):
        plan = version(1, "Historical plan")
        records = [message("u", "user"), message("a")]
        turns = [{"message_id": "a", "objective_id": plan["id"], "updated": False}]
        await self.restore(records, turns, [plan])
        drawer = self.chat.query_one(ObjectivePlanDrawer)
        await self.open(drawer)
        await self.restore(records, turns, [plan])
        self.assertIs(self.chat.query_one(ObjectivePlanDrawer), drawer)
        self.assertFalse(drawer.collapsed)
        await self.restore([message("earlier")], turns, [plan])
        self.assertFalse(self.chat.query(ObjectivePlanDrawer))

    async def test_drawer_precedes_generation_metadata_after_restore_and_live_sync(self):
        plan = version(1, "Plan")
        answer = message("a")
        answer["metadata"] = {"model": "test-model", "reasoning": "high", "duration_ms": 1200}
        turns = [{"message_id": "a", "objective_id": plan["id"], "updated": True}]
        for live in (False, True):
            await self.restore([answer], [] if live else turns, [plan])
            if live:
                self.app.update_agent_info({"objective_plan_turns": turns})
                await self.pilot.pause()
            response = next(widget for record, widget in self.chat.rendered if record["id"] == "a")
            drawer = self.chat.query_one(ObjectivePlanDrawer)
            footer = self.chat.query_one(GenerationFooter)
            children = list(self.chat.children)
            self.assertEqual(children[children.index(response):children.index(response) + 3],
                             [response, drawer, footer])
            self.assertFalse(response.query(GenerationFooter))
            self.assertGreaterEqual(drawer.region.y, response.region.bottom)
            self.assertGreaterEqual(footer.region.y, drawer.region.bottom)
            await self.open(drawer)
            self.assertGreaterEqual(footer.region.y, drawer.region.bottom)
            await self.restore([], [], [])

    async def test_direct_action_chooses_pace_and_keeps_focus_and_objective(self):
        from sos_tui.implementation_workspace import ImplementationModeScreen
        plan = version(1, "Plan")
        await self.restore([message("a")], [{"message_id": "a", "objective_id": plan["id"], "updated": True}], [plan])
        drawer = self.chat.query_one(ObjectivePlanDrawer)
        await self.pilot.click(drawer.query_one("CollapsibleTitle"))
        await self.pilot.pause()
        self.assertTrue(self.app.query_one(MessageInput).has_focus)
        buttons = list(drawer.query(".objective-actions Button"))
        self.assertTrue(all(not button.can_focus for button in drawer.query(Button)))
        self.assertEqual(buttons[0].size, buttons[1].size)
        with patch.object(self.app.implementation, "request", new_callable=AsyncMock) as request:
            await self.pilot.click(buttons[1])
            self.assertIsInstance(self.app.screen, ImplementationModeScreen)
            await self.pilot.press("escape")
            request.assert_not_awaited()
            await self.pilot.click(buttons[1])
            await self.pilot.press("down", "enter")
            request.assert_awaited_once_with("implementation.open", "", objective_id=plan["id"], auto_plan_pace="everything")

    async def test_preview_drawer_has_no_actions_and_keeps_full_width(self):
        from sos_tui.discussion_preview import DiscussionPreview
        plan = version(1, "Historical objective")
        self.app.session_id = "branch"
        self.app.update_agent_info({"implementation_view": True, "active_implementation_id": "branch",
            "implementation_branches": [{"id": "branch", "objective_id": plan["id"]}],
            "objective_plans": [plan], "objective_plan_turns": [
                {"message_id": "a", "objective_id": plan["id"], "updated": True}]})
        await self.pilot.pause()
        await self.pilot.click("#discussion")
        preview = self.app.query_one(DiscussionPreview)
        viewport = preview.query_one(HistoryViewport)
        await viewport.replace({"session": {"id": "discussion", "mode": "sane"}, "page": {},
                                "messages": [message("a")]})
        drawer = viewport.query_one(ObjectivePlanDrawer)
        await self.open(drawer)
        preview.sync()
        self.assertFalse(drawer.query_one(".objective-actions").display)
        self.assertFalse(drawer.query_one(".objective-link").display)
        self.assertGreater(preview.size.width, 110)
        self.assertFalse(preview.query_one("#discussion-preview-open").display)
        self.assertIn("Historical objective", drawer.query_one(ResponseMarkdown)._markdown)

    async def test_direct_defaults_to_step_by_step(self):
        plan = version(1, "Plan")
        await self.restore([message("a")], [{"message_id": "a", "objective_id": plan["id"], "updated": True}], [plan])
        drawer = self.chat.query_one(ObjectivePlanDrawer)
        await self.open(drawer)
        with patch.object(self.app.implementation, "request", new_callable=AsyncMock) as request:
            await self.pilot.click(drawer.query_one(".objective-auto-implement", Button))
            await self.pilot.press("enter")
            request.assert_awaited_once_with("implementation.open", "", objective_id=plan["id"], auto_plan_pace="step_by_step")

    async def test_busy_and_existing_branch_actions(self):
        plan = version(1, "Plan")
        await self.restore([message("a")], [{"message_id": "a", "objective_id": plan["id"], "updated": False}], [plan])
        drawer = self.chat.query_one(ObjectivePlanDrawer)
        await self.open(drawer)
        self.app.busy = True
        await self.pilot.pause()
        self.assertTrue(all(button.disabled for button in drawer.query(Button)))
        self.app.busy = False
        self.app.update_agent_info({"active_implementation_id": "branch", "implementation_branches": [
            {"id": "branch", "objective_id": plan["id"]}]})
        await self.pilot.pause()
        with patch.object(self.app.implementation, "request", new_callable=AsyncMock) as request:
            await self.pilot.click(drawer.query_one(".objective-link", Button))
            request.assert_awaited_once_with("implementation.view", "branch")
        self.assertFalse(drawer.query_one(".objective-actions").display)

    async def test_header_link_opens_existing_implementation_without_expanding(self):
        plan = version(1, "Plan")
        await self.restore([message("a")], [{"message_id": "a", "objective_id": plan["id"], "updated": True}], [plan])
        drawer = self.chat.query_one(ObjectivePlanDrawer)
        link = drawer.query_one(".objective-link", Button)
        self.assertFalse(link.display)
        self.app.update_agent_info({"implementation_branches": [
            {"id": "branch", "objective_id": plan["id"]}]})
        await self.pilot.pause()
        title = drawer.query_one("CollapsibleTitle")
        self.assertTrue(link.display)
        self.assertEqual(str(link.label), "Implementation")
        self.assertEqual(link.region.y, title.region.y)
        self.assertEqual(link.region.right, drawer.query_one(".objective-header").region.right)
        with patch.object(self.app.implementation, "request", new_callable=AsyncMock) as request:
            await self.pilot.click(link)
            request.assert_awaited_once_with("implementation.view", "branch")
        self.assertTrue(drawer.collapsed)
        self.assertFalse(drawer.built)
        self.assertTrue(self.app.query_one(MessageInput).has_focus)

    async def test_unlaunched_plan_keeps_actions_below_green_document(self):
        from textual.color import Color
        plan = version(1, "Plan")
        await self.restore([message("a")], [{"message_id": "a", "objective_id": plan["id"], "updated": True}], [plan])
        drawer = self.chat.query_one(ObjectivePlanDrawer)
        await self.open(drawer)
        document = drawer.query_one(".objective-document")
        sane = Color.parse(self.app.get_css_variables()["sane"])
        separator = drawer.query_one(".objective-separator")
        header = drawer.query_one(".objective-header")
        self.assertEqual(separator.styles.color, sane)
        self.assertEqual(separator.region.bottom, header.region.y)
        self.assertEqual(separator.size.width, drawer.content_size.width)
        self.assertEqual(separator.region.x, self.chat.content_region.x)
        self.assertEqual(header.region.x, separator.region.x + 1)
        self.assertEqual(drawer.query_one("CollapsibleTitle").styles.color, sane)
        self.assertEqual(drawer.styles.margin.left, 0)
        self.assertEqual(document.styles.border_left, ("outer", sane))
        self.assertGreater(document.styles.background.a, 0)
        self.assertTrue(drawer.query_one(".objective-actions").display)
        self.assertGreater(drawer.query_one(".objective-actions").region.y,
                           drawer.query_one(ResponseMarkdown).region.y)
        self.assertFalse(drawer.query_one(".objective-link").display)

