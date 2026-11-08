"""Implementation construction, review, validation and archive interactions."""
import copy
import unittest
from unittest.mock import AsyncMock, patch
from sos_tui.app import SosApp
from sos_tui.implementation_workspace import ImplementationWorkspace, ImplementationModeScreen, ReviewInput
from sos_tui.message_input import MessageInput
from sos_tui.question_screen import QuestionScreen
from sos_tui.implementation_code import HistoricalCode, change_zones
from textual.widgets import Button, Static, OptionList, Tree
from sos_tui.config import ALGAE_MD, BOLD_MD, SLOP_COLOR
from sos_tui.agent_info_bar import AgentInfoBar
from sos_tui.mode_button import ModeButton
from sos_tui.implementation_changes_tree import ImplementationChangesTree
from sos_tui.implementation_step_view import ImplementationStepView, SubstepActivity
from async_fixture import wait_until


def state():
    part = {"complete": True, "reviewed": False, "explanation": "A testable state model", "diffs": [
        {"path": "src/state.py", "patch": "--- before\n+++ after\n@@ -1 +1 @@\n-old\n+new\n"},
        {"path": "tests/state.py", "patch": "+test\n"}]}
    return {"status": "waiting", "pace": "step_by_step", "work_step": 1, "work_substep": 0,
            "review_step": 0, "review_substep": 0, "error": "",
            "plan": {"steps": [{"title": "Model", "substeps": [{"title": "State"}], "objective": "Model", "validation": ["Test"]},
                               {"title": "Wire", "substeps": [{"title": "Connect"}], "objective": "Wire", "validation": ["Test"]}]},
            "steps": [{"substeps": [copy.deepcopy(part)], "whole": copy.deepcopy(part)},
                      {"substeps": [dict(part, complete=False)], "whole": dict(part, complete=False)}]}


class ImplementationLogicTest(unittest.TestCase):
    def test_captured_last_substep_counts_before_whole_step_completion(self):
        from sos_tui.implementation_step_view import substep_progress
        parts = [{"started": True, "announced": True, "complete": False, "finalized": True} for _ in range(3)]
        self.assertEqual(substep_progress(parts), (3, 3))
        parts[-1]["finalized"] = False
        self.assertEqual(substep_progress(parts), (2, 3))

    def test_starting_next_substep_does_not_finalize_announced_work(self):
        from sos_tui.implementation_step_view import substep_progress
        parts = [{"complete": False, "announced": True, "finalized": False},
                 {"complete": False, "started": True, "finalized": False}]
        self.assertEqual(substep_progress(parts), (0, 2))
        parts[0]["unnecessary"] = True
        self.assertEqual(substep_progress(parts), (1, 2))
        parts[1]["complete"] = True
        self.assertEqual(substep_progress(parts), (2, 2))

    def test_diff_palette_and_bullets(self):
        zones = change_zones([{"path": "file", "after": "new\n", "patch": "@@ -1 +1 @@\n-old\n+new\n"}])
        self.assertEqual(zones[0].rows[0][2], BOLD_MD)
        self.assertEqual(zones[0].rows[1][2], ALGAE_MD)
        self.assertEqual(ImplementationWorkspace.bullet_description("Existing detail\n- Concrete check\n"),
                         "- Existing detail\n- Concrete check")


class ImplementationWorkspaceTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(120, 35)))
        self.app.connected = True
        self.app.session_id = "branch"
        self.app.client.send = AsyncMock()
        self.workspace = self.app.query_one(ImplementationWorkspace)
        self.workspace.session_id = "branch"
        self.workspace.apply_state(state())
        self.app.update_agent_info({"implementation_view": True, "implementation_execution": True,
                                   "discussion_id": "discussion", "active_implementation_id": "branch",
                                   "implementation_plan": {"id": "accepted", "content": [{"type": "text", "text": "Accepted foundation plan"}]}})
        await self.pilot.pause()
        self.workspace.focus()

    async def test_diff_position_survives_substep_round_trip_and_reordered_files(self):
        value = state()
        value["plan"]["steps"][0]["substeps"].append({"title": "Second"})
        value["steps"][0]["substeps"].append(copy.deepcopy(value["steps"][0]["substeps"][0]))
        self.workspace.apply_state(value)
        await self.pilot.press("enter", "right")
        self.assertEqual(self.workspace.zones[self.workspace.diff_index - 1].path, "tests/state.py")
        await self.pilot.press("down", "up")
        self.assertEqual(self.workspace.diff_index, 2)
        value["steps"][0]["substeps"][0]["diffs"].reverse()
        self.workspace.apply_state(value)
        await self.pilot.press("down", "up")
        self.assertEqual(self.workspace.diff_index, 1)
        self.assertEqual(self.workspace.zones[self.workspace.diff_index - 1].path, "tests/state.py")
        self.app.client.send.assert_not_awaited()

    async def test_activity_clock_starts_offscreen_and_uses_backend_elapsed(self):
        value = state()
        value.update(status="running", work_step=1, work_substep=0, active_duration_ms=42000)
        value["steps"][1]["substeps"][0]["started"] = True
        with patch("sos_tui.implementation_step_view.monotonic", return_value=100):
            self.workspace.apply_state(value)
        timeline = self.workspace.query_one(ImplementationStepView)
        self.assertEqual(timeline.clock[(1, 0)], 58)
        self.workspace.selection = (1, -1)
        self.workspace.show_selection()
        await self.pilot.pause()
        self.assertEqual(timeline.rows[0][2].toggle.started_at, 58)
        value["steps"][1]["validation"] = {"started": True, "complete": False, "activity": []}
        value.update(work_substep=1, active_duration_ms=9000)
        with patch("sos_tui.implementation_step_view.monotonic", return_value=120):
            self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertEqual(timeline.validation_activity.toggle.started_at, 111)
        value["status"] = "interrupted"
        value["steps"][1]["validation"]["duration_ms"] = 9000
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertIsNone(timeline.validation_activity.toggle.started_at)
        self.assertEqual(timeline.validation_activity.toggle.duration_ms, 9000)

    async def test_scroll_to_selection_waits_for_list_layout(self):
        from unittest.mock import PropertyMock
        from textual.containers import VerticalScroll

        listing = self.workspace.query_one("#implementation-steps", OptionList)
        menu = self.workspace.query_one("#implementation-step-list", VerticalScroll)
        self.assertIn(self.workspace.selection, self.workspace.rows)
        with patch.object(menu, "scroll_to_region") as scroll:
            with patch.object(OptionList, "_index_to_line", new_callable=PropertyMock, return_value={}):
                self.workspace.scroll_to_selection()
                scroll.assert_not_called()
            with patch.object(OptionList, "_heights", new_callable=PropertyMock, return_value={}):
                self.workspace.scroll_to_selection()
                scroll.assert_not_called()
            listing._update_lines()
            self.workspace.scroll_to_selection()
            scroll.assert_called_once()

    async def test_auto_plan_shows_empty_steps_and_spinner_then_accepts_plan(self):
        self.workspace.state = None
        self.workspace.selection = None
        self.workspace.rows = []
        self.workspace.query_one(OptionList).clear_options()
        self.app.update_agent_info({"implementation_execution": False, "implementation_plan_ready": False,
            "implementation_plan": None, "auto_plan_pending": True, "auto_plan_running": True, "auto_plan_pace": "everything"})
        await self.pilot.pause()
        self.assertTrue(self.workspace.display)
        self.assertFalse(self.app.query_one("#discussion-body").display)
        self.assertTrue(self.workspace.query_one("#implementation-auto-spinner").display)
        self.assertTrue(self.workspace.query_one("#implementation-show-plan", Button).disabled)
        self.assertTrue(self.app.busy)
        for key in ("tab", "shift+tab", "tab"):
            await self.pilot.press(key)
            self.assertFalse(self.workspace.chat_open)
            self.assertFalse(self.workspace.query_one("#implementation-chat").display)
            self.assertTrue(self.workspace.query_one("#implementation-agent-info").display)
        self.workspace.spinner = 3
        for frame in "|/-\\":
            self.workspace.animate_spinner()
            self.assertEqual(str(self.workspace.query_one("#implementation-auto-spinner", Static).render()), frame)
        self.app.handle_event({"type": "execution.failed", "error": "Offline", "info": {
            "auto_plan_pending": True, "auto_plan_running": False, "auto_plan_error": "Offline"}})
        await self.pilot.pause()
        self.assertFalse(self.workspace.query_one("#implementation-auto-spinner").display)
        self.assertTrue(self.workspace.query_one("#implementation-auto-retry").display)
        await self.pilot.click("#implementation-auto-retry")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "implementation.auto_plan")
        self.assertEqual(self.app.client.send.call_args.args[0]["pace"], "everything")
        self.app.handle_event({"type": "implementation.updated", "sessionId": "branch", "implementation": state(),
            "info": {"auto_plan_pending": False, "auto_plan_running": False, "implementation_execution": True,
                     "implementation_plan_ready": True}})
        await self.pilot.pause()
        self.assertFalse(self.workspace.query_one("#implementation-auto-spinner").display)
        self.assertFalse(self.workspace.query_one("#implementation-show-plan", Button).disabled)
        self.assertTrue(self.workspace.rows)
        accepted = {"id": "accepted", "role": "assistant", "implementation_plan": {"steps": []},
                    "content": [{"type": "text", "text": "Accepted foundation plan"}]}
        with patch.object(self.app, "display_message") as display:
            self.app.handle_event({"type": "execution.completed", "messages": [accepted]})
            display.assert_called_once_with(accepted)
        await self.pilot.click("#implementation-show-plan")
        await self.pilot.pause()
        self.assertTrue(self.workspace.plan_visible)

    async def test_preparation_interruption_stops_spinner_and_exposes_continue(self):
        value = state()
        value.update(status="running", work_step=0)
        value["steps"][0].update(preparing=True)
        value["steps"][0]["whole"]["complete"] = False
        value["steps"][0]["substeps"][0].update(complete=False, diffs=[])
        self.workspace.selection = (0, -1)
        self.workspace.apply_state(value)
        await self.pilot.pause()
        view = self.workspace.query_one(ImplementationStepView)
        self.assertTrue(view.preparation.display)
        self.assertEqual(view.progress_value, (0, 1))
        self.assertIsNone(view.active)
        value.update(status="interrupted", error="Checkout unavailable")
        value["steps"][0]["preparing"] = False
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertFalse(view.preparation.display)
        self.assertFalse(self.app.busy)
        self.assertEqual(str(view.action.label), "Continue")
        self.assertTrue(view.action.display)
        self.assertEqual(self.workspace.selection, (0, -1))

    async def test_click_active_step_only_inspects_without_cancel_confirmation(self):
        value = state()
        value.update(status="running", work_step=0)
        value["steps"][0]["whole"]["complete"] = False
        value["steps"][0]["substeps"][0].update(complete=False, started=True, diffs=[])
        self.workspace.apply_state(value)
        await self.pilot.pause()
        await self.pilot.click("#implementation-steps", offset=(3, 0))
        await self.pilot.pause()
        self.assertEqual(self.workspace.selection, (0, -1))
        self.assertNotIsInstance(self.app.screen, QuestionScreen)
        self.app.client.send.assert_not_awaited()
        self.workspace.focus()
        await self.pilot.press("enter")
        self.assertIsInstance(self.app.screen, QuestionScreen)

    async def test_rework_confirmation_authorizes_current_proposal_only(self):
        from sos_tui.implementation_proposal_card import ImplementationProposalCard
        from sos_tui.permission_card import PermissionCard
        value = state()
        value.update(revision=7, proposed_rework="Use a class for the existing implementation")
        self.workspace.apply_state(value)
        self.workspace.toggle_chat()
        await self.pilot.pause()
        card = self.workspace.query_one(ImplementationProposalCard)
        self.assertIsInstance(card, PermissionCard)
        card.query_one("#permission-yes").focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.assertFalse(self.app.screen.is_modal)
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "implementation.rework")
        self.assertEqual(self.app.client.send.call_args.args[0]["revision"], 7)
        self.assertFalse(self.workspace.chat_open)
        self.assertEqual(self.workspace.selection, (0, -1))
        self.assertTrue(self.workspace.query_one(ImplementationStepView).preparation.display)

    async def test_proposal_arrows_override_empty_review_diff_navigation(self):
        from sos_tui.implementation_proposal_card import ImplementationProposalCard
        self.workspace.toggle_chat()
        value = state(); value.update(revision=8, proposed_rework="Correct the state model")
        self.workspace.apply_state(value)
        card = self.workspace.query_one(ImplementationProposalCard)
        await wait_until(self.pilot, lambda: not card.focus_pending and card.owns_button_focus(),
                         "proposal's initial button focus")
        self.assertTrue(card.query_one("#permission-yes").has_focus)
        entry = self.workspace.query_one(ReviewInput)
        entry.text = ""
        entry.focus()
        before = self.workspace.diff_index
        await self.pilot.press("right")
        self.assertEqual(card.selected_button, "permission-no")
        await wait_until(self.pilot, lambda: card.query_one("#permission-no").has_focus,
                         "proposal's decline focus after revealing the choice")
        self.assertTrue(card.query_one("#permission-no").has_focus)
        self.assertEqual(self.workspace.diff_index, before)
        await self.pilot.press("left")
        self.assertEqual(card.selected_button, "permission-yes")
        await wait_until(self.pilot, lambda: card.query_one("#permission-yes").has_focus,
                         "proposal's acceptance focus after revealing the choice")
        self.assertTrue(card.query_one("#permission-yes").has_focus)
        self.assertEqual(self.workspace.diff_index, before)

    async def test_accept_updates_view_before_reply_and_failure_restores_review(self):
        value = state(); value.update(revision=9, proposed_rework="Correct this step")
        self.workspace.apply_state(value)
        self.workspace.toggle_chat()
        await wait_until(self.pilot, lambda: self.workspace.proposal_card.owns_button_focus()
                         and self.app.screen.can_view_entire(self.workspace.query_one("#permission-yes")),
                         "visible acceptance button after opening review")
        await self.pilot.click("#permission-yes")
        await self.pilot.pause()
        self.assertFalse(self.workspace.chat_open)
        self.assertEqual(self.workspace.selection, (0, -1))
        self.assertTrue(self.workspace.query_one(ImplementationStepView).preparation.display)
        before = self.app.client.send.await_count
        self.workspace.confirm_rework()
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.await_count, before)
        request = self.app.client.send.call_args.args[0]["requestId"]
        revising = copy.deepcopy(value)
        revising.update(status="revising", preparing=True)
        revising["steps"][0]["preparing"] = True
        self.workspace.apply_state(revising)
        self.workspace.receive({"type": "request.failed", "requestId": request, "error": "Checkout unavailable"})
        await self.pilot.pause()
        self.assertTrue(self.workspace.chat_open)
        self.assertFalse(self.workspace.rework_preparing)
        self.assertFalse(self.workspace.query_one(ImplementationStepView).preparation.display)
        self.assertFalse(self.workspace.proposal_card.query_one("#permission-yes", Button).disabled)

    async def test_review_history_shows_incorporation_boundary_without_hiding_old_exchanges(self):
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        await chat.show_records([
            {"id": "old", "role": "user", "content": [{"type": "text", "text": "Original question"}]},
            {"id": "review-integrated-old", "role": "system", "content": [{"type": "text", "text": "Accepted"}]},
            {"id": "new", "role": "user", "content": [{"type": "text", "text": "Next question"}]}], False)
        await self.pilot.pause()
        self.assertEqual(len(chat.rendered_records), 3)
        self.assertIn("New review segment", str(chat.query_one(".review-boundary", Static).render()))

    def versioned_step(self):
        value = state()
        value.update(version_count=3, accepted_plan_markdown="Latest plan")
        message = lambda text: {"role": "assistant", "content": [{"type": "text", "text": text}]}
        built = value["steps"][0]
        built.update(display_revision=3, correction_request="Correction 2", construction_history=[{
            "revision": revision, "title": f"Model {revision}", "explanation": f"Global {revision}",
            "request": f"Correction {revision}", "substeps": [
                {"title": f"Storage {revision}", "explanation": f"Result {revision}", "activity": [message(str(revision))]}],
            "validation_activity": [message(f"Validation {revision}")]}
            for revision in (1, 2)])
        built["substeps"][0].update(explanation="Latest result", activity=[message("Latest work")])
        built["whole"]["explanation"] = "Global latest must not appear"
        self.history_versions = []
        for index, segment in enumerate(built["construction_history"]):
            old = copy.deepcopy(value)
            old.update(status="archived", accepted_plan_markdown=f"Plan {index + 1}")
            old["plan"]["steps"][0].update(title=segment["title"], substeps=[{"title": segment["substeps"][0]["title"]}])
            part = dict(copy.deepcopy(old["steps"][0]["substeps"][0]), **segment["substeps"][0])
            old["steps"][0] = {"whole": dict(built["whole"], explanation=segment["explanation"]), "substeps": [part],
                               "validation": {"complete": True, "activity": segment["validation_activity"]},
                               "correction_request": "" if index == 0 else "Correction 1"}
            self.history_versions.append(old)
        return value

    async def test_step_versions_show_only_selected_activity_and_correct_request(self):
        from sos_tui.implementation_step_view import SubstepTimeline
        value = self.versioned_step()
        self.workspace.apply_state(value)
        self.workspace.version_states = self.history_versions
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        self.assertEqual(self.workspace.version_index, 2)
        self.assertTrue(self.workspace.query_one("#implementation-version-next", Button).disabled)
        self.assertIn("Correction 2", str(timeline.overview.render()))
        self.assertNotIn("Global latest", str(timeline.overview.render()))
        self.assertEqual(str(timeline.query_one(".substep-description", Static).render()), "- Latest result")
        self.assertEqual(len(timeline.query(SubstepTimeline)), 2)
        await self.pilot.click("#implementation-version-previous")
        await self.pilot.pause()
        self.assertEqual(self.workspace.version_index, 1)
        self.assertIn("Correction 1", str(timeline.overview.render()))
        self.assertNotIn("Global 2", str(timeline.overview.render()))
        self.assertEqual(str(timeline.query_one(".substep-title", Static).render()), "1. Storage 2")
        self.assertEqual(timeline.rows[0][2].messages[0]["content"][0]["text"], "2")
        self.assertEqual(timeline.validation_activity.messages[0]["content"][0]["text"], "Validation 2")
        self.assertFalse(timeline.action.display)
        previous = self.workspace.query_one("#implementation-version-previous", Button)
        await wait_until(self.pilot, lambda: not previous.has_class("-active"), "previous-version button click animation")
        await self.pilot.click("#implementation-version-previous")
        await self.pilot.pause()
        self.assertEqual(self.workspace.version_index, 0)
        self.assertTrue(self.workspace.query_one("#implementation-version-previous", Button).disabled)
        self.assertIn("Global 1", str(timeline.overview.render()))
        self.assertNotIn("Correction", str(timeline.overview.render()))
        await self.pilot.click("#implementation-version-next")
        await self.pilot.pause()
        self.assertEqual(self.workspace.version_index, 1)
        self.assertEqual(len(timeline.query(".substep-title")), 1)
        self.assertEqual(len(timeline.query(".step-progress")), 1)

    async def test_live_refresh_keeps_history_selected_and_new_review_selects_latest(self):
        value = self.versioned_step()
        self.workspace.apply_state(value)
        self.workspace.version_states = self.history_versions
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        await self.pilot.click("#implementation-version-previous")
        await self.pilot.pause()
        refreshed = copy.deepcopy(value)
        refreshed["steps"][0]["substeps"][0]["explanation"] = "Updated latest result"
        self.workspace.apply_state(refreshed)
        await self.pilot.pause()
        self.assertEqual(self.workspace.version_index, 1)
        self.assertIn("Result 2", str(timeline.query_one(".substep-description", Static).render()))
        refreshed.update(revision=4, proposed_rework="Third correction")
        self.workspace.apply_state(refreshed)
        # Acceptance is available only on the live version.
        self.workspace.select_version(2)
        self.workspace.confirm_rework()
        await self.pilot.pause()
        self.assertIn("Third correction", str(timeline.overview.render()))
        self.assertNotIn("Global latest", str(timeline.overview.render()))
        self.assertTrue(timeline.preparation.display)
        self.assertFalse(timeline.rows[0][2].messages)
        self.assertTrue(self.workspace.query_one("#implementation-version-next", Button).disabled)
        refreshed.update(version_count=4, status="revising")
        self.workspace.apply_state(refreshed)
        self.assertEqual(self.workspace.version_index, 3)

    async def test_accepted_version_transitions_from_preparing_to_live_activity(self):
        value = state()
        value.update(revision=1, proposed_rework="Rename the field")
        self.workspace.apply_state(value)
        self.workspace.confirm_rework()
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        self.assertTrue(timeline.preparing)
        old = value["steps"][0]
        running = copy.deepcopy(value)
        running.update(status="running", proposed_rework="", work_step=0, version_count=2)
        built = running["steps"][0]
        built.update(display_revision=2, correction_request="Rename the field", construction_history=[{
            "revision": 1, "title": "Model", "explanation": old["whole"]["explanation"],
            "request": "Rename the field", "substeps": [dict(old["substeps"][0], title="State")]}])
        built["whole"].update(complete=False, explanation="")
        built["substeps"][0].update(complete=False, started=True, explanation="", activity=[
            {"role": "assistant", "content": [{"type": "text", "text": "Renaming"}]}])
        self.workspace.apply_state(running)
        await self.pilot.pause()
        self.assertEqual(self.workspace.version_index, 1)
        self.assertFalse(timeline.preparing)
        self.assertIs(timeline.active, timeline.rows[0][2])
        self.assertIn("Rename the field", str(timeline.overview.render()))
        self.assertEqual(len(timeline.rows[0][2].messages), 1)
        self.workspace.version_states = [dict(copy.deepcopy(value), status="archived", proposed_rework="")]
        await self.pilot.click("#implementation-version-previous")
        await self.pilot.pause()
        self.assertIsNone(timeline.active)
        self.assertFalse(timeline.action.display)
        self.assertEqual(self.workspace.live_state["status"], "running")
        self.assertTrue(self.app.busy)
        self.assertEqual(self.app.client.send.await_count, 1)

    async def test_version_navigation_restores_historical_substep_structure(self):
        value = self.versioned_step()
        value["plan"]["steps"][0]["substeps"].append({"title": "Contract"})
        value["steps"][0]["substeps"].append(copy.deepcopy(value["steps"][0]["substeps"][0]))
        self.workspace.apply_state(value)
        self.workspace.version_states = self.history_versions
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        self.assertEqual(len(timeline.rows), 2)
        await self.pilot.click("#implementation-version-previous")
        await self.pilot.pause()
        self.assertEqual(len(timeline.rows), 1)
        await self.pilot.click("#implementation-version-next")
        await self.pilot.pause()
        self.assertEqual(len(timeline.rows), 2)
        self.assertEqual(str(self.workspace.query_one("#implementation-version-label", Static).render()), "Version 3")

    async def test_version_row_navigation_selects_versions_and_shows_centered_overview(self):
        self.workspace.apply_state(self.versioned_step())
        self.workspace.version_states = self.history_versions
        await self.pilot.pause()
        self.workspace.selection = self.workspace.rows[0]
        self.workspace.focus()
        await self.pilot.press("up", "up")
        await self.pilot.pause()
        label = self.workspace.query_one("#implementation-version-label")
        overview = self.workspace.query_one("#implementation-version-overview", Static)
        self.assertTrue(label.has_focus)
        self.assertTrue(overview.display)
        self.assertEqual(str(overview.render()), "Version 3")
        self.assertEqual(overview.styles.content_align_horizontal, "center")
        self.assertEqual(overview.styles.content_align_vertical, "middle")
        await self.pilot.press("left")
        await self.pilot.pause()
        self.assertEqual(self.workspace.version_index, 1)
        self.assertTrue(label.has_focus)
        self.assertEqual(str(overview.render()), "Version 2")
        await self.pilot.press("left", "left")
        await self.pilot.pause()
        self.assertEqual(self.workspace.version_index, 0)
        await self.pilot.press("right")
        await self.pilot.pause()
        self.assertEqual(self.workspace.version_index, 1)
        self.assertEqual(str(overview.render()), "Version 2")
        await self.pilot.press("down")
        await self.pilot.pause()
        self.assertTrue(self.workspace.plan_visible)
        self.assertFalse(overview.display)
        self.app.client.send.assert_not_awaited()

    async def test_initial_step_has_no_version_controls(self):
        timeline = self.workspace.query_one(ImplementationStepView)
        self.assertEqual(self.workspace.version_index, 0)
        self.assertTrue(self.workspace.query_one("#implementation-version-label").disabled)
        self.assertFalse(timeline.query(".version-navigation"))
        self.assertTrue(self.workspace.query_one("#implementation-version-previous", Button).disabled)
        self.assertTrue(self.workspace.query_one("#implementation-version-next", Button).disabled)
        self.assertFalse(self.workspace.query("#implementation-result-history"))
        self.assertEqual(timeline.query_one(".step-construction").styles.align_horizontal, "left")
        self.assertEqual(timeline.query_one(".step-construction").styles.align_vertical, "top")
        self.assertIn("A testable state model", str(timeline.overview.render()))

    async def test_rework_stale_modal_archive_and_running_review_do_not_authorize(self):
        from sos_tui.implementation_proposal_card import ImplementationProposalCard
        from sos_tui.permission_card import PermissionCard
        value = state(); value.update(revision=7, proposed_rework="Correct this step")
        self.workspace.apply_state(value)
        self.workspace.apply_state(dict(value, revision=8))
        self.workspace.on_permission_card_decided(PermissionCard.Decided("7", "yes"))
        await self.pilot.pause()
        self.app.client.send.assert_not_awaited()
        self.workspace.review_running = True
        self.workspace.confirm_rework()
        self.assertFalse(self.app.screen.is_modal)
        self.workspace.review_running = False
        self.workspace.archive = True
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertFalse(self.workspace.query(ImplementationProposalCard))

    async def test_everything_candidate_plan_is_separate_until_accepted_and_escape_returns(self):
        from sos_tui.implementation_proposal_card import ImplementationProposalCard
        from sos_tui.implementation_revision_plan import ImplementationRevisionPlan, RevisionPlanInput
        value = state()
        value.update(pace="everything", status="revision_pending", revision=9,
                     proposed_rework="Launch with uv run snake", proposed_plan_markdown="## Proposed uv packaging",
                     accepted_plan_markdown="## Original plan")
        self.workspace.apply_state(value)
        self.workspace.toggle_chat()
        card = self.workspace.query_one(ImplementationProposalCard)
        await wait_until(self.pilot, lambda: not card.focus_pending and card.owns_button_focus(),
                         "revision proposal layout and focus")
        await self.pilot.click("#permission-yes")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "implementation.revision.open")
        self.assertFalse(self.workspace.revision_page_visible)
        self.assertFalse(self.workspace.can_validate_step(0))
        value = dict(value, revision=10, status="revision_planning", revision_planning=True)
        self.workspace.apply_state(value)
        await self.pilot.pause()
        page = self.workspace.query_one(ImplementationRevisionPlan)
        self.assertTrue(page.is_on_screen)
        self.assertFalse(self.workspace.query_one("#implementation-body").display)
        self.assertLess(page.query_one("#revision-plan-discussion").region.x, page.query_one("#revision-plan-document").region.x)
        self.assertFalse(self.workspace.query(ImplementationProposalCard))
        from sos_tui.response_markdown import ResponseMarkdown
        markdown = page.query_one(ResponseMarkdown)
        self.assertIn("Proposed uv packaging", markdown._markdown)
        self.assertEqual(str(page.query_one("#revision-plan-heading", Static).render()), "Proposed implementation plan")
        self.assertEqual(self.app.client.send.await_count, 1)
        await self.pilot.press("escape")
        self.assertFalse(self.workspace.revision_page_visible)
        await self.pilot.click("#implementation-resume-plan")
        await self.pilot.click("#revision-plan-accept")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "implementation.rework")
        self.assertEqual(self.app.client.send.call_args.args[0]["revision"], 10)
        resumed = state()
        resumed.update(status="running", work_step=1, revision=11, revision_planning=False, version_count=2)
        resumed["steps"][0]["identity"] = "first"
        resumed["steps"][1]["identity"] = "trigger"
        resumed["steps"][1]["display_revision"] = 10
        resumed["plan_versions"] = [{"number": 1, "beforeStep": "first", "markdown": "Original"},
                                   {"number": 2, "beforeStep": "trigger", "markdown": "Accepted revision"}]
        self.workspace.apply_state(resumed)
        await self.pilot.pause()
        self.assertFalse(page.display)
        self.assertEqual(self.workspace.selection, (1, -1))
        self.assertEqual(self.workspace.rows, [(0, -1), (1, -1)])
        self.assertEqual(self.workspace.version_index, 1)

    async def test_reject_proposal_keeps_current_result_and_version(self):
        from sos_tui.implementation_proposal_card import ImplementationProposalCard
        value = state(); value.update(revision=12, proposed_rework="Correction")
        self.workspace.apply_state(value)
        self.workspace.toggle_chat()
        card = self.workspace.query_one(ImplementationProposalCard)
        await wait_until(self.pilot, lambda: not card.focus_pending and card.owns_button_focus(),
                         "proposal's initial button focus")
        await self.pilot.click("#permission-no")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "implementation.rework.reject")
        self.assertEqual(request["revision"], 12)
        self.assertTrue(self.workspace.state["steps"][0]["whole"]["complete"])

    async def test_global_navigation_switches_plan_and_menu_without_interleaved_versions(self):
        value = self.versioned_step()
        value["plan_versions"] = [{"number": 1, "beforeStep": "0", "markdown": "Original plan"},
                                  {"number": 2, "beforeStep": "1", "markdown": "Second plan"}]
        self.workspace.apply_state(value)
        self.workspace.version_states = self.history_versions
        self.assertEqual(self.workspace.rows, [(0, -1), (1, -1)])
        self.workspace.inspect_plan(True)
        self.assertEqual(self.workspace.query_one("#implementation-accepted-plan").query_one("ResponseMarkdown")._markdown, "Latest plan")
        self.workspace.select_version(1)
        await self.pilot.pause()
        self.assertEqual(self.workspace.query_one("#implementation-accepted-plan").query_one("ResponseMarkdown")._markdown, "Plan 2")
        self.assertIn("Model 2", self.workspace.step_prompt(0).plain)
        self.workspace.select_version(0)
        self.assertEqual(self.workspace.query_one("#implementation-accepted-plan").query_one("ResponseMarkdown")._markdown, "Plan 1")
        self.workspace.select_version(2)
        self.assertEqual(self.workspace.query_one("#implementation-accepted-plan").query_one("ResponseMarkdown")._markdown, "Latest plan")
        self.app.client.send.assert_not_awaited()

    async def test_step_proposal_preview_is_scrollable_and_has_only_accept_decline(self):
        from sos_tui.implementation_proposal_card import ImplementationProposalCard
        value = state(); value.update(revision=1, proposed_rework="\n".join(f"- Detail {i}" for i in range(50)))
        self.workspace.apply_state(value)
        self.workspace.toggle_chat()
        await self.pilot.pause()
        card = self.workspace.query_one(ImplementationProposalCard)
        self.assertEqual([str(b.label) for b in card.query(Button)], ["Accept", "Decline"])
        preview = card.query_one(".permission-preview")
        await wait_until(self.pilot, lambda: preview.max_scroll_y > 0, "proposal Markdown layout")
        self.assertLessEqual(preview.size.height, 8)
        self.assertGreater(preview.max_scroll_y, 0)
        preview.scroll_end(animate=False, immediate=True)
        await self.pilot.pause()
        position = preview.scroll_y
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertEqual(preview.scroll_y, position)
        self.assertGreater(position, 0)
        self.assertTrue(all(button.is_on_screen for button in card.query(Button)))

    async def test_step_proposal_preview_renders_markdown_heading_list_and_code(self):
        from sos_tui.implementation_proposal_card import ImplementationProposalCard
        from sos_tui.response_markdown import ResponseMarkdown
        request = "### Changes\n\n- **Preserve** the existing model.\n- Use `snake:main`.\n\n```toml\n[project.scripts]\nsnake = 'snake:main'\n```"
        value = state(); value.update(revision=1, proposed_rework=request)
        self.workspace.apply_state(value)
        self.workspace.toggle_chat()
        await self.pilot.pause()
        document = self.workspace.query_one(ImplementationProposalCard).query_one(ResponseMarkdown)
        await wait_until(self.pilot, lambda: request in document._markdown and bool(document.query("CodeFence")),
                         "deferred proposal Markdown rendering")
        self.assertIn(request, document._markdown)
        self.assertTrue(document.query("MarkdownH3"))
        self.assertTrue(document.query("MarkdownBulletList"))
        self.assertTrue(document.query("CodeFence"))

    async def test_revision_plan_discussion_keeps_new_draft_and_does_not_execute(self):
        from sos_tui.implementation_revision_plan import RevisionPlanInput
        value = state(); value.update(revision=1, revision_planning=True, proposed_plan_markdown="Candidate")
        self.workspace.apply_state(value)
        await self.pilot.pause()
        entry = self.workspace.query_one(RevisionPlanInput)
        entry.text = "Simplify the plan"
        entry.focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "implementation.revision.review")
        self.assertFalse(entry.disabled)
        await self.pilot.press("n", "e", "x", "t", "enter")
        self.assertEqual(entry.text, "next")
        self.assertEqual(self.app.client.send.await_count, 1)

    async def test_revision_plan_shows_activity_while_generating(self):
        from sos_tui.implementation_revision_plan import RevisionPlanInput
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        from sos_tui.turn_activity import TurnActivity
        value = state()
        value.update(revision=1, revision_planning=True, proposed_plan_markdown="Candidate")
        self.workspace.apply_state(value)
        await self.pilot.pause()
        entry = self.workspace.query_one(RevisionPlanInput)
        entry.text = "Revise the implementation plan"
        entry.focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        chat = self.workspace.query_one("#revision-plan-chat", ImplementationReviewChat)
        activity = chat.query_one(TurnActivity)
        self.assertIs(chat.running, activity)
        self.assertTrue(activity.is_on_screen)
        self.assertGreater(activity.region.height, 0)
        self.assertTrue(activity.toggle.is_on_screen)

        records = [
            {"role": "user", "content": [{"type": "text", "text": "Revise the implementation plan"}]},
            {"role": "assistant", "content": [{"type": "text", "text": "\n\n".join(
                f"Planning detail {index}" for index in range(40))}]},
        ]
        await chat.show_records(records, True)
        self.workspace.sync_revision_page()
        await self.pilot.pause()
        self.assertIsNotNone(chat.running)
        self.assertGreater(chat.running.toggle.region.bottom, chat.content_region.y)
        self.assertLessEqual(chat.running.toggle.region.bottom, chat.content_region.bottom)
        self.assertEqual(chat.query_one("ResponseMarkdown")._markdown, records[-1]["content"][0]["text"])
        await chat.show_records(records, False)
        await self.pilot.pause()
        self.assertIsNone(chat.running)

    async def test_revision_history_is_loaded_once_and_whole_view_is_read_only(self):
        value = self.versioned_step()
        self.workspace.apply_state(value)
        await self.pilot.pause()
        await self.pilot.click("#implementation-version-previous")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "implementation.history")
        self.assertTrue(request["all"])
        self.assertTrue(self.workspace.query_one("#implementation-version-previous", Button).disabled)
        self.workspace.receive({"type": "implementation.history", "sessionId": "branch", "requestId": request["requestId"],
                                "implementations": self.history_versions})
        await self.pilot.pause()
        self.assertEqual(self.workspace.version_index, 1)
        self.assertTrue(self.workspace.archive)
        self.assertFalse(self.workspace.can_validate_step(0))
        self.assertFalse(self.workspace.review_available())
        self.workspace.confirm_rework()
        self.workspace.select_version(0)
        self.workspace.select_version(2)
        self.assertFalse(self.workspace.archive)
        self.assertIs(self.workspace.state, value)
        self.assertEqual(self.app.client.send.await_count, 1)

    async def test_revision_pending_steps_are_muted_and_selection_tracks_identity_after_reorder(self):
        value = state(); value["steps"][0]["identity"] = "a"; value["steps"][1]["identity"] = "b"
        self.workspace.apply_state(value)
        self.workspace.selection = (1, -1)
        revised = copy.deepcopy(value)
        revised["steps"].insert(1, copy.deepcopy(value["steps"][1]))
        revised["steps"][1]["identity"] = "new"
        revised["plan"]["steps"].insert(1, copy.deepcopy(value["plan"]["steps"][1]))
        revised.update(status="running", work_step=0)
        self.workspace.apply_state(revised)
        self.assertEqual(self.workspace.selection, (2, -1))
        from sos_tui.config import MUTED_COLOR
        self.assertEqual(self.workspace.step_prompt(2).style, MUTED_COLOR)

    async def test_global_history_restores_removed_steps_diffs_and_snapshot_tree(self):
        value = self.versioned_step()
        for index, step in enumerate(value["steps"]):
            step["identity"] = f"current-{index}"
        old = self.history_versions[1]
        old["plan"]["steps"].append({"title": "Removed step", "objective": "Historical intent",
                                      "substeps": [{"title": "Historical substep"}]})
        built = copy.deepcopy(old["steps"][0])
        built.update(identity="removed", repository_root="/historical/repository", repository_tree="historical-tree")
        built["substeps"][0]["diffs"] = [{"path": "removed.py", "patch": "@@ -1 +1 @@\n-old\n+historical\n"}]
        old["steps"].append(built)
        self.workspace.apply_state(value)
        self.workspace.version_states = self.history_versions
        self.workspace.select_version(1)
        self.workspace.selection = (2, -1)
        self.workspace.show_selection()
        self.workspace.enter()
        await self.pilot.pause()
        self.assertEqual(self.workspace.selection, (2, 0))
        self.assertEqual(self.workspace.zones[0].path, "removed.py")
        self.assertEqual(self.workspace.query_one(ImplementationChangesTree).baseline[-1], "historical-tree")
        self.assertFalse(self.workspace.query_one("#implementation-validate").display)
        self.workspace.select_version(2)
        self.assertEqual(len(self.workspace.state["steps"]), 2)
        self.assertEqual(self.workspace.selection, (1, -1))

    async def test_history_loading_failure_can_retry_and_stale_result_does_not_change_version(self):
        value = self.versioned_step()
        self.workspace.apply_state(value)
        self.workspace.select_version(1)
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.workspace.receive({"type": "request.failed", "requestId": request["requestId"], "error": "History unavailable"})
        self.assertIsNone(self.workspace.requested_version)
        self.assertFalse(self.workspace.query_one("#implementation-version-previous", Button).disabled)
        self.workspace.select_version(1)
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.workspace.apply_state(dict(value, version_count=4))
        self.workspace.receive({"type": "implementation.history", "sessionId": "branch", "requestId": request["requestId"],
                                "implementations": self.history_versions})
        self.assertEqual(self.workspace.version_index, 3)
        self.assertFalse(self.workspace.archive)
        self.assertEqual(self.workspace.version_states, [])

    async def test_history_send_failure_releases_navigation(self):
        self.workspace.apply_state(self.versioned_step())
        self.app.client.send.side_effect = OSError("Disconnected")
        self.workspace.select_version(1)
        await self.pilot.pause()
        self.assertIsNone(self.workspace.requested_version)
        self.assertFalse(self.workspace.query_one("#implementation-version-previous", Button).disabled)
        self.assertEqual(self.workspace.version_index, 2)

    async def test_frozen_implementation_stays_read_only_when_returning_to_latest_version(self):
        value = self.versioned_step()
        self.workspace.archive = True
        self.workspace.apply_state(value)
        self.workspace.version_states = self.history_versions
        self.workspace.select_version(1)
        self.workspace.select_version(2)
        self.assertTrue(self.workspace.archive)
        self.assertFalse(self.workspace.can_validate_step(0))
        self.assertFalse(self.workspace.review_available())
        self.app.client.send.assert_not_awaited()

    async def test_rework_starts_fresh_visual_progress_without_previous_step_rows(self):
        from sos_tui.config import MISC_MD
        from sos_tui.implementation_step_view import StepProgress
        value = state()
        value["steps"][0]["validation"] = {"started": True, "complete": True, "activity": []}
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        previous_activity = timeline.rows[0][2]
        timeline.clock[(0, 0)] = 1.0
        revised = copy.deepcopy(value)
        revised.update(status="running", work_step=0, work_substep=0)
        built = revised["steps"][0]
        built.update(display_revision=7, previous_versions=[{"revision": 6, "plan_number": 1, "title": "Model", "substeps": ["State"]}])
        built["whole"].update(complete=False, reviewed=False, explanation="", diffs=[], activity=[])
        built["validation"] = {"started": False, "complete": False, "activity": []}
        for part in built["substeps"]:
            part.update(complete=False, reviewed=False, started=False, announced=False, finalized=False, unnecessary=False, explanation="", diffs=[], activity=[])
        self.workspace.apply_state(revised)
        await self.pilot.pause()
        self.assertEqual(str(timeline.query_one(".step-progress-count").render()), "0/1")
        self.assertFalse(timeline.query_one(StepProgress).validating)
        self.assertIsNot(previous_activity, timeline.rows[0][2])
        self.assertGreater(timeline.clock[(0, 0)], 1.0)
        self.assertFalse(timeline.validation_activity.display)
        self.assertTrue(timeline.rows[0][2].display)
        self.assertFalse(timeline.rows[0][3].display)
        listing = self.workspace.query_one("#implementation-steps", OptionList)
        self.assertEqual(self.workspace.rows, [(0, -1), (1, -1)])
        option = listing.get_option_at_index(0)
        self.assertFalse(option.disabled)
        self.assertNotIn("strike", str(option.prompt.style))
        self.assertEqual(self.workspace.step_prompt(0).style, MISC_MD)
        self.app.client.send.assert_not_awaited()

    async def test_revised_substeps_only_appear_after_completion_and_expansion(self):
        value = state()
        value.update(status="running", work_step=0, work_substep=0)
        built = value["steps"][0]
        built.update(identity="model", display_revision=3,
                     previous_versions=[{"revision": 1, "plan_number": 1, "title": "Old model", "substeps": ["Old state"]}])
        built["whole"].update(complete=False, explanation="", diffs=[])
        fresh = {"complete": False, "reviewed": False, "started": False, "announced": False, "finalized": False, "explanation": "", "diffs": [], "activity": []}
        built["substeps"] = [copy.deepcopy(fresh), copy.deepcopy(fresh)]
        value["plan"]["steps"][0]["substeps"] = [{"title": "Revised state"}, {"title": "UV launch"}]
        value["plan_versions"] = [{"number": 1, "beforeStep": "model", "markdown": "Old plan"},
                                  {"number": 2, "beforeStep": "model", "markdown": "Revised plan"}]
        self.workspace.apply_state(value)
        await self.pilot.pause()
        collapsed_rows = [(0, -1), (1, -1)]
        self.assertEqual(self.workspace.rows, collapsed_rows)
        # Even a stale expansion flag must not expose an unfinished step.
        self.workspace.expanded.add(0)
        self.workspace.refresh_rows()
        self.assertEqual(self.workspace.rows, collapsed_rows)
        self.workspace.expanded.clear()
        timeline = self.workspace.query_one(ImplementationStepView)
        titles = [str(title.render()) for _, title, _, _ in timeline.rows]
        self.assertTrue(any("Revised state" in title for title in titles))
        self.assertTrue(any("UV launch" in title for title in titles))
        self.assertEqual(str(timeline.query_one(".step-progress-count").render()), "0/2")

        completed = copy.deepcopy(value)
        completed["status"] = "waiting"
        completed["steps"][0]["whole"]["complete"] = True
        for part in completed["steps"][0]["substeps"]:
            part.update(complete=True, diffs=[{"path": "state.py", "patch": "+new"}])
        self.workspace.apply_state(completed)
        await self.pilot.pause()
        self.assertEqual(self.workspace.rows, collapsed_rows)
        self.workspace.focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.assertEqual(self.workspace.rows, collapsed_rows[:-1] + [(0, 0), (0, 1), (0, 2), (1, -1)])
        listing = self.workspace.query_one("#implementation-steps", OptionList)
        for index, title in ((1, "Revised state"), (2, "UV launch")):
            prompt = listing.get_option_at_index(index).prompt
            self.assertTrue(prompt.plain.startswith("  " + title))
            self.assertFalse(listing.get_option_at_index(index).disabled)

    async def test_everything_rework_collapses_reset_steps_and_keeps_successors_muted_without_strike(self):
        from sos_tui.config import MUTED_COLOR
        value = state()
        value.update(pace="everything", status="completed", work_step=2)
        for index, step in enumerate(value["steps"]):
            step["identity"] = f"step-{index}"
            step["whole"]["complete"] = True
            step["substeps"][0]["complete"] = True
        self.workspace.apply_state(value)
        self.workspace.expanded.update({0, 1})
        self.workspace.refresh_rows()
        revised = copy.deepcopy(value)
        revised.update(status="running", work_step=0)
        for step in revised["steps"]:
            step["display_revision"] = 5
            step["whole"]["complete"] = False
            step["substeps"][0]["complete"] = False
        revised["steps"][0]["previous_versions"] = [{"revision": 4, "title": "Model", "substeps": ["State"]}]
        self.workspace.apply_state(revised)
        await self.pilot.pause()
        self.assertFalse(self.workspace.expanded)
        self.assertEqual(self.workspace.rows, [(0, -1), (1, -1)])
        listing = self.workspace.query_one("#implementation-steps", OptionList)
        self.assertNotIn("strike", str(listing.get_option_at_index(0).prompt.style))
        successor = listing.get_option_at_index(1).prompt
        self.assertEqual(successor.style, MUTED_COLOR)
        self.assertNotIn("strike", str(successor.style))
        self.workspace.apply_state(copy.deepcopy(revised))
        self.assertEqual(self.workspace.rows, [(0, -1), (1, -1)])
        self.assertEqual(self.workspace.selection, (0, -1))
        self.app.client.send.assert_not_awaited()

    async def test_review_draft_remains_editable_during_generation_and_acceptance(self):
        await self.pilot.press("enter", "tab")
        entry = self.workspace.query_one(ReviewInput)
        entry.text = "First question"
        await self.pilot.press("enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertTrue(self.workspace.review_running)
        self.assertFalse(entry.disabled)
        self.assertEqual(entry.text, "")
        await self.pilot.press("n", "e", "x", "t", "enter")
        self.assertEqual(entry.text, "next")
        self.assertEqual(self.app.client.send.await_count, 1)
        self.workspace.receive({"type": "implementation.review.accepted", "requestId": request["requestId"]})
        self.assertEqual(entry.text, "next")

    async def test_review_compaction_splits_activity_keeps_spinner_and_restores_checkpoint(self):
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        from sos_tui.chat_collapsible import CompactionDrawer
        from sos_tui.turn_activity import TurnActivity
        from sos_tui.response_markdown import ResponseMarkdown
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        records = [
            {"role": "user", "content": [{"type": "text", "text": "Explain"}]},
            {"role": "assistant", "content": [{"type": "reasoning", "text": "Before"}]},
            {"id": "compaction-status-2", "role": "system", "content": [{"type": "text", "text": "started"}]}]
        await chat.show_records(records, True)
        await self.pilot.pause()
        drawer = chat.query_one(CompactionDrawer)
        self.assertTrue(drawer.running)
        self.assertIsNone(chat.running)
        self.assertIsNone(chat.query_one(TurnActivity).toggle.started_at)
        self.assertIn(drawer.styles.border.top[0], ("", "none"))
        self.assertEqual(drawer.query_one("CollapsibleTitle").region.x,
                         chat.query_one(TurnActivity).toggle.region.x)
        records += [{"role": "assistant", "compaction": {"mode": "sane"},
                     "content": [{"type": "text", "text": "Checkpoint summary"}]}]
        await chat.show_records(records, True)
        await self.pilot.pause()
        resumed = chat.running
        self.assertIsNotNone(resumed)
        self.assertIsNotNone(resumed.toggle.started_at)
        self.assertEqual(resumed.toggle.step_count, 0)
        records += [{"role": "assistant", "content": [{"type": "reasoning", "text": "After"}]}]
        await chat.show_records(records, True)
        await self.pilot.pause()
        self.assertFalse(drawer.running)
        self.assertIs(chat.running, resumed)
        self.assertEqual(len(chat.query(TurnActivity)), 2)
        self.assertIs(chat.running, chat.query(TurnActivity).last())
        self.assertEqual(len(chat.query(CompactionDrawer)), 1)
        drawer.collapsed = False
        await self.pilot.pause()
        self.assertEqual(drawer.query_one(ResponseMarkdown).source, "Checkpoint summary")
        await chat.reset_history()
        await chat.show_records(records, False)
        self.assertEqual(len(chat.query(TurnActivity)), 2)
        self.assertFalse(chat.query_one(CompactionDrawer).running)

    async def test_failed_review_compaction_stops_spinner_and_next_attempt_has_its_own_drawer(self):
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        from sos_tui.chat_collapsible import CompactionDrawer
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        records = [{"id": "compaction-status-1", "role": "system", "content": [{"type": "text", "text": "started"}]},
                   {"id": "compaction-status-2", "role": "system", "content": [{"type": "text", "text": "failed\nProvider error"}]}]
        await chat.show_records(records, False)
        await self.pilot.pause()
        self.assertFalse(chat.query_one(CompactionDrawer).running)
        records.append({"id": "compaction-status-3", "role": "system", "content": [{"type": "text", "text": "started"}]})
        await chat.show_records(records, True)
        await self.pilot.pause()
        drawers = list(chat.query(CompactionDrawer))
        self.assertEqual(len(drawers), 2)
        self.assertFalse(drawers[0].running)
        self.assertTrue(drawers[1].running)

    async def test_review_can_start_with_compaction_without_empty_activity(self):
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        from sos_tui.chat_collapsible import CompactionDrawer
        from sos_tui.turn_activity import TurnActivity
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        chat.start("Question", {})
        await self.pilot.pause()
        await chat.show_records([
            {"role": "user", "content": [{"type": "text", "text": "Question"}]},
            {"id": "compaction-status-1", "role": "system", "content": [{"type": "text", "text": "started"}]}], True)
        await self.pilot.pause()
        self.assertFalse(chat.query(TurnActivity))
        self.assertTrue(chat.query_one(CompactionDrawer).running)
        await chat.show_records(chat.rendered_records, False)
        self.assertFalse(chat.query_one(CompactionDrawer).running)

    async def test_construction_and_validation_compactions_split_lazy_activities(self):
        from sos_tui.chat_collapsible import CompactionDrawer
        from sos_tui.turn_activity import TurnActivity
        value = state()
        value.update(status="running", work_step=0, work_substep=0)
        value["steps"][0]["whole"]["complete"] = False
        started = {"id": "compaction-status-1", "role": "system", "content": [{"type": "text", "text": "started"}]}
        before = {"role": "assistant", "content": [{"type": "reasoning", "text": "Before"}]}
        after = {"role": "assistant", "content": [{"type": "reasoning", "text": "After"}]}
        checkpoint = {"role": "assistant", "compaction": {"mode": "sane"},
                      "content": [{"type": "text", "text": "Saved context"}]}
        part = value["steps"][0]["substeps"][0]
        part.update(started=True, complete=False, activity=[before, started])
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        activity = timeline.rows[0][2]
        drawer = activity.query_one(CompactionDrawer)
        self.assertTrue(drawer.running)
        self.assertEqual(drawer.query_one("CollapsibleTitle").region.x, activity.toggle.region.x)
        part["activity"] += [checkpoint]
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertIsNotNone(activity.toggle.started_at)
        self.assertEqual(activity.toggle.step_count, 0)
        part["activity"] += [after]
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertEqual(len(activity.query(TurnActivity)), 2)
        self.assertFalse(drawer.running)
        self.assertIsNotNone(activity.toggle.started_at)
        value["steps"][0]["validation"] = {"started": True, "complete": False,
            "activity": [before, started, checkpoint, after]}
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertEqual(len(timeline.validation_activity.query(TurnActivity)), 2)
        self.assertEqual(len(timeline.validation_activity.query(CompactionDrawer)), 1)
        self.assertEqual(len(activity.query(CompactionDrawer)), 1)

    async def test_compaction_completion_after_step_navigation_does_not_touch_removed_drawer(self):
        from sos_tui.chat_collapsible import CompactionDrawer
        from sos_tui.response_markdown import ResponseMarkdown
        value = state()
        value["steps"][0]["substeps"][0]["activity"] = [{"id": "compaction-status-1", "role": "system",
            "content": [{"type": "text", "text": "started"}]}]
        self.workspace.apply_state(value)
        await self.pilot.pause()
        drawer = self.workspace.query_one(CompactionDrawer)
        drawer.set_result("Saved checkpoint")
        self.workspace.selection = (1, -1)
        self.workspace.show_selection()
        await self.pilot.pause()
        self.assertFalse(drawer.is_attached)
        # Reproduce the deferred callback arriving after the step's teardown.
        await drawer.finish("Saved checkpoint")
        self.assertFalse(drawer.query(ResponseMarkdown))
        self.workspace.selection = (0, -1)
        value["steps"][0]["substeps"][0]["activity"].append({"id": "checkpoint", "role": "assistant",
            "compaction": {"mode": "sane"}, "content": [{"type": "text", "text": "Saved checkpoint"}]})
        self.workspace.apply_state(value)
        await self.pilot.pause()
        current = self.workspace.query_one(CompactionDrawer)
        await current.finish("Saved checkpoint")
        await current.finish("Saved checkpoint")
        self.assertEqual(len(current.query(ResponseMarkdown)), 1)

    async def test_compaction_result_before_mount_is_rendered_once(self):
        from sos_tui.chat_collapsible import CompactionDrawer
        from sos_tui.response_markdown import ResponseMarkdown
        drawer = CompactionDrawer()
        await drawer.finish("Checkpoint before mount")
        await self.workspace.query_one(ImplementationStepView).mount(drawer)
        await self.pilot.pause()
        drawer.set_result("Checkpoint before mount")
        await self.pilot.pause()
        self.assertEqual(len(drawer.query(ResponseMarkdown)), 1)

    async def test_review_metadata_and_repeated_snapshots_do_not_create_trailing_activity(self):
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        from sos_tui.turn_activity import TurnActivity
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        records = [{"role": "user", "content": [{"type": "text", "text": "Explain"}]}]
        for i in range(20):
            records.append({"role": "assistant", "content": [{"type": "tool_call", "callId": str(i),
                "toolName": "read", "arguments": {"filePath": "model.py"}}]})
            records.append({"role": "tool", "content": [{"type": "tool_result", "callId": str(i),
                "output": "model", "isError": False}]})
        await chat.show_records(records, True)
        records = records + [{"role": "assistant", "content": [{"type": "text", "text": "Answer"}]}]
        await chat.show_records(records, True)
        records = records + [{"role": "assistant", "content": [], "model_call": {
            "model": "test", "reasoning": "none", "duration_ms": 1000, "output_tokens": 50},
            "metadata": {"model": "test", "reasoning": "none", "duration_ms": 2000}}]
        await chat.show_records(records, False)
        await chat.show_records(records, False)
        await self.pilot.pause()
        self.assertEqual(len(chat.query(TurnActivity)), 1)
        self.assertEqual(chat.query_one(TurnActivity).toggle.step_count, 20)
        self.assertIsNone(chat.running)
        self.assertIs(chat.children[-1], chat.query_one("ResponseMarkdown"))

    async def test_finished_step_and_substep_replace_progress_with_performance(self):
        value = state()
        stats = {"duration_ms": 2500, "output_tokens": 100, "model": "test", "reasoning": "none"}
        value["steps"][0]["whole"].update(duration_ms=7000, model_statistics=stats)
        value["steps"][0]["substeps"][0].update(duration_ms=4000, model_statistics=stats)
        self.workspace.apply_state(value)
        self.assertEqual(self.workspace.step_prompt(0).plain.splitlines()[1], "  7s | 40 tok/s")
        from sos_tui.config import MUTED_COLOR, TITLE_MD
        prompt = self.workspace.step_prompt(0)
        self.assertEqual(prompt.get_style_at_offset(self.app.console, len(prompt.plain) - 1).color.get_truecolor().hex.upper(), TITLE_MD.upper())
        await self.pilot.press("enter")
        listing = self.workspace.query_one("#implementation-steps", OptionList)
        self.assertIn("4s | 40 tok/s", listing.get_option_at_index(1).prompt.plain)
        prompt = listing.get_option_at_index(1).prompt
        self.assertEqual(prompt.get_style_at_offset(self.app.console, len(prompt.plain) - 1).color.get_truecolor().hex.upper(), TITLE_MD.upper())
        prompt = listing.get_option_at_index(0).prompt
        self.assertEqual(prompt.get_style_at_offset(self.app.console, len(prompt.plain) - 1).color.get_truecolor().hex.upper(), MUTED_COLOR.upper())
        await self.pilot.press("up")
        prompt = listing.get_option_at_index(0).prompt
        self.assertEqual(prompt.get_style_at_offset(self.app.console, len(prompt.plain) - 1).color.get_truecolor().hex.upper(), TITLE_MD.upper())
        prompt = listing.get_option_at_index(1).prompt
        self.assertEqual(prompt.get_style_at_offset(self.app.console, len(prompt.plain) - 1).color.get_truecolor().hex.upper(), MUTED_COLOR.upper())

    async def test_main_step_titles_do_not_repeat_menu_statistics_and_keep_activity(self):
        value = state()
        stats = {"duration_ms": 2500, "output_tokens": 100, "model": "test", "reasoning": "none"}
        value["steps"][0]["whole"].update(duration_ms=7000, model_statistics=stats)
        part = value["steps"][0]["substeps"][0]
        part.update(duration_ms=4000, model_statistics=stats, activity=[
            {"role": "assistant", "content": [{"type": "tool_call", "callId": "read-state",
             "toolName": "read", "arguments": {"filePath": "src/state.py"}}]},
            {"role": "tool", "content": [{"type": "tool_result", "callId": "read-state", "output": "State", "isError": False}]}])
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        overview = str(timeline.query_one(".step-overview").render())
        title = str(timeline.query_one(".substep-title").render())
        self.assertNotIn("7s", overview)
        self.assertNotIn("4s", title)
        self.assertNotIn("tok/s", overview + title)
        self.assertIn("Model", overview)
        self.assertIn("A testable state model", overview)
        self.assertEqual(title, "1. State")
        activity = timeline.query_one(".substep-construction").query_one(SubstepActivity)
        self.assertTrue(activity.display)
        self.assertEqual(len(activity.messages), 2)

    async def test_plan_button_uses_full_width_row_and_left_aligned_label(self):
        from textual.color import Color
        from sos_tui.config import SANE_COLOR, DEPTH_MD
        button = self.workspace.query_one("#implementation-show-plan", Button)
        self.assertNotEqual(button.styles.background, Color.parse(SANE_COLOR))
        sidebar = self.workspace.query_one("#implementation-sidebar")
        self.assertLessEqual(abs((button.region.x + button.region.right) - (sidebar.region.x + sidebar.region.right)), 2)
        self.assertEqual(button.region.height, 3)
        self.assertEqual(button.region.width, sidebar.content_size.width)
        self.assertEqual(str(button.label), "Implementation plan")
        self.assertEqual(button.styles.content_align_horizontal, "left")
        self.assertEqual(button.styles.text_align, "left")
        await self.pilot.hover(button)
        await self.pilot.pause()
        self.assertNotEqual(button.styles.background, sidebar.styles.background)
        await self.pilot.click(button)
        await self.pilot.pause()
        self.assertEqual(button.styles.background, Color.parse(DEPTH_MD))

    async def test_global_version_navigation_is_above_plan_and_has_no_separate_background(self):
        navigation = self.workspace.query_one("#implementation-version-navigation")
        plan = self.workspace.query_one("#implementation-show-plan")
        label = self.workspace.query_one("#implementation-version-label", Static)
        self.assertLess(navigation.region.y, plan.region.y)
        self.assertEqual(navigation.styles.align_horizontal, "left")
        self.assertEqual(label.styles.background.a, 0)
        self.assertEqual(str(label.render()), "Version 1")
        self.assertEqual(label.styles.text_align, "center")
        self.assertTrue(label.styles.text_style.bold)
        sidebar = self.workspace.query_one("#implementation-sidebar")
        self.assertLessEqual(abs((label.region.x + label.region.right)
                                 - (sidebar.region.x + sidebar.region.right)), 1)
        from sos_tui.config import CONTENT_BACKGROUND_COLOR, DEPTH_MD
        for selector in ("#implementation-version-previous", "#implementation-version-next"):
            arrow = self.workspace.query_one(selector, Button)
            self.assertEqual(arrow.styles.background.hex.lower(), "#3a4655")
            arrow.disabled = False
            await self.pilot.hover(arrow)
            await self.pilot.pause()
            self.assertEqual(arrow.styles.background.hex.lower(), DEPTH_MD.lower())
            await self.pilot.hover(label)
            await self.pilot.pause()
            self.assertEqual(arrow.styles.background.hex.lower(), "#3a4655")

    async def test_plan_and_conclusion_share_full_width_message_rows_and_version_input_background(self):
        from sos_tui.config import CONTENT_BACKGROUND_COLOR, INPUT_BACKGROUND_COLOR
        await self.pilot.hover("#implementation-version-label")
        self.workspace.focus()
        await self.pilot.pause()
        navigation = self.workspace.query_one("#implementation-version-navigation")
        self.assertEqual(navigation.styles.background.hex, INPUT_BACKGROUND_COLOR)
        plan = self.workspace.query_one("#implementation-show-plan", Button)
        conclusion = self.workspace.query_one("#implementation-conclusion", Button)
        for button in (plan, conclusion):
            self.assertEqual(button.styles.background.hex, CONTENT_BACKGROUND_COLOR)
            self.assertEqual(button.styles.text_align, "left")
            self.assertTrue(button.styles.text_style.bold)
            self.assertEqual(button.parent.styles.padding.left, 0)
            self.assertEqual(button.parent.styles.padding.right, 0)
            self.assertEqual(button.region.width, button.parent.region.width)
            self.assertEqual(button.region.height, 3)
            self.assertEqual(button.region.width, plan.region.width)
            self.assertEqual(button.region.x, plan.region.x)
        self.workspace.selection = self.workspace.rows[-1]
        conclusion.disabled = False
        self.workspace.navigate(1)
        await self.pilot.pause()
        self.assertTrue(self.workspace.conclusion_visible)
        self.assertTrue(self.workspace.query_one("#implementation-accepted-plan").display)
        self.workspace.navigate(-1)
        await self.pilot.pause()
        self.assertFalse(self.workspace.conclusion_visible)

    async def test_page_keys_target_visible_content_or_open_review_regardless_of_focus(self):
        from contextlib import ExitStack
        selectors = ("#implementation-diff", "#implementation-step-view",
                     "#implementation-accepted-plan", "#implementation-review-history")
        cases = (
            (False, False, False, False, "#implementation-diff"),
            (False, False, False, True, "#implementation-step-view"),
            (True, False, False, False, "#implementation-accepted-plan"),
            (False, True, False, False, "#implementation-accepted-plan"),
            (False, False, True, True, "#implementation-review-history"),
        )
        for plan, conclusion, review, step, target in cases:
            with self.subTest(target=target), ExitStack() as stack:
                self.workspace.plan_visible = plan
                self.workspace.conclusion_visible = conclusion
                self.workspace.chat_open = review
                self.workspace.query_one("#implementation-chat").display = review
                self.workspace.query_one("#implementation-step-view").display = step
                calls = {selector: (stack.enter_context(patch.object(self.workspace.query_one(selector), "action_page_up")),
                                    stack.enter_context(patch.object(self.workspace.query_one(selector), "action_page_down")))
                         for selector in selectors}
                self.workspace.query_one("#implementation-show-plan").focus()
                await self.pilot.press("pageup", "pagedown")
                for selector, (up, down) in calls.items():
                    self.assertEqual(up.call_count, int(selector == target))
                    self.assertEqual(down.call_count, int(selector == target))

    async def test_navigating_to_plan_opens_it_without_enter_and_down_returns_to_first_step(self):
        self.workspace.selection = self.workspace.rows[0]
        self.workspace.focus()
        self.workspace.show_selection()
        await self.pilot.press("up")
        await self.pilot.pause()
        self.assertTrue(self.workspace.plan_visible)
        self.assertTrue(self.workspace.query_one("#implementation-show-plan").has_focus)
        self.assertTrue(self.workspace.query_one("#implementation-accepted-plan").display)
        self.assertFalse(self.workspace.query_one("#implementation-step-view").display)
        await self.pilot.press("down")
        await self.pilot.pause()
        self.assertFalse(self.workspace.plan_visible)
        self.assertEqual(self.workspace.selection, self.workspace.rows[0])
        self.assertFalse(self.workspace.query_one("#implementation-accepted-plan").display)
        self.assertTrue(self.workspace.query_one("#implementation-step-view").display)

    async def test_active_step_shows_two_substep_words_ellipsis_and_spinner(self):
        value = state()
        value.update(status="running", work_step=0, work_substep=0)
        value["plan"]["steps"][0]["substeps"][0]["title"] = "Define status transitions and invariants"
        value["steps"][0]["whole"]["complete"] = False
        value["steps"][0]["substeps"][0]["complete"] = False
        self.workspace.apply_state(value)
        prompt = self.workspace.step_prompt(0)
        self.assertEqual(prompt.plain.splitlines()[1], "  [0/1] Define status... |")
        self.workspace.animate_spinner()
        self.assertTrue(self.workspace.step_prompt(0).plain.endswith("Define status... /"))

    async def test_keyboard_expands_selects_then_validates_without_selecting_future(self):
        self.assertTrue(self.workspace.display)
        self.assertFalse(self.app.query_one("#chat").display)
        self.assertFalse(self.app.query_one("#composer").display)
        self.assertFalse(self.workspace.query("#implementation-discuss, #implementation-controls"))
        self.assertFalse(self.workspace.query_one("#implementation-status").display)
        self.assertEqual(self.workspace.selection, (0, -1))
        await self.pilot.press("enter")
        self.assertEqual(self.workspace.selection, (0, 0))
        await self.pilot.press("enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "implementation.validate")
        self.assertEqual((request["step"], request["substep"]), (0, 0))
        await self.pilot.press("down")
        self.assertEqual(self.workspace.selection, (0, 1))
        await self.pilot.press("right")
        self.assertEqual(self.workspace.diff_index, 0) # Validate has no code views.

    async def test_chat_arrows_edit_and_ctrl_arrows_navigate_enter_only_sends_chat(self):
        await self.pilot.press("enter", "tab")
        self.assertTrue(self.workspace.chat_open)
        entry = self.workspace.query_one(ReviewInput)
        entry.text = "Explain this"
        entry.move_cursor((0, 12))
        self.assertIs(self.app.focused, entry)
        await self.pilot.press("left")
        self.assertEqual(entry.cursor_location[1], 11)
        self.assertEqual(self.workspace.diff_index, 1)
        await self.pilot.press("ctrl+right")
        self.assertEqual(self.workspace.diff_index, 2)
        await self.pilot.press("enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "implementation.review")
        self.assertEqual(request["text"], "Explain this")
        self.assertEqual((request["path"], request["hunk"]), ("tests/state.py", 0))
        await self.pilot.press("tab")
        self.assertFalse(self.workspace.chat_open)

    async def test_review_is_limited_to_first_unvalidated_step_without_locking_future_diffs(self):
        value = state()
        value["pace"] = "everything"
        value["steps"][1]["whole"]["complete"] = True
        value["steps"][1]["substeps"][0]["complete"] = True
        self.workspace.apply_state(value)
        self.workspace.selection = (1, 0)
        self.workspace.expanded.add(1)
        self.workspace.show_selection()
        await self.pilot.pause()
        self.assertTrue(self.workspace.zones)
        with patch.object(self.app, "notify") as notify:
            self.workspace.toggle_chat()
            self.workspace.open_review_command()
            self.assertEqual(notify.call_count, 2)
            self.assertIn("first unvalidated step", notify.call_args.args[0])
        self.assertFalse(self.workspace.chat_open)
        self.workspace.review_records = [{"role": "user", "content": [{"type": "text", "text": "OLD_REVIEW"}]}]
        value = copy.deepcopy(value)
        value["steps"][0]["whole"]["reviewed"] = True
        value["review_step"] = 1
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertEqual(self.workspace.review_records, [])
        self.assertTrue(self.workspace.review_available())
        self.workspace.toggle_chat()
        self.assertTrue(self.workspace.chat_open)

    async def test_review_composer_grows_and_shrinks_with_multiline_draft(self):
        self.workspace.toggle_chat()
        await self.pilot.pause()
        entry = self.workspace.query_one(ReviewInput)
        composer = self.workspace.query_one("#implementation-review-composer")
        indicator = self.workspace.query_one("#implementation-review-mode")
        entry.focus()
        await self.pilot.press("a", "shift+enter", "b", "shift+enter", "c")
        await self.pilot.pause()
        self.assertEqual(entry.text, "a\nb\nc")
        self.assertEqual(entry.region.height, 3)
        self.assertEqual(composer.region.height, 3)
        self.assertEqual(indicator.region.height, 3)
        entry.text = "\n".join(["line"] * 10)
        await self.pilot.pause()
        self.assertEqual(entry.region.height, entry.MAX_VISIBLE_LINES)
        self.assertEqual(composer.region.height, entry.MAX_VISIBLE_LINES)
        entry.text = ""
        await self.pilot.pause()
        self.assertEqual(entry.region.height, 1)
        self.assertEqual(composer.region.height, 1)
        entry.text = "word " * 40
        await self.pilot.pause()
        self.assertGreater(entry.region.height, 1)
        self.assertEqual(composer.region.height, entry.region.height)
        await self.pilot.resize_terminal(80, 35)
        await self.pilot.pause()
        self.assertEqual(entry.region.height, min(entry.MAX_VISIBLE_LINES, entry.wrapped_document.height))
        self.assertEqual(composer.region.height, entry.region.height)

    async def test_chat_moves_info_above_composer_and_keeps_sane_indicator(self):
        from sos_tui.config import MODE_COLORS
        diff_bar = self.workspace.query_one("#implementation-agent-info", AgentInfoBar)
        chat_bar = self.workspace.query_one("#implementation-review-agent-info", AgentInfoBar)
        self.app.update_agent_info({"model": "test-model", "reasoning": "high",
                                    "last_turn_used_context": 100, "total_context": 32768})
        await self.pilot.press("enter", "tab")
        await self.pilot.pause()
        self.assertFalse(diff_bar.display)
        self.assertTrue(chat_bar.is_on_screen)
        self.assertIs(self.app.active_info_bar(), chat_bar)
        composer = self.workspace.query_one("#implementation-review-composer")
        self.assertEqual(chat_bar.region.bottom, composer.region.y)
        self.assertEqual(chat_bar.labels, diff_bar.labels)
        self.assertTrue(chat_bar.query_one(ModeButton).display)
        self.assertEqual(str(chat_bar.query_one(ModeButton).label), "Sane")
        indicator = self.workspace.query_one("#implementation-review-mode")
        self.app.set_mode("slop")
        await self.pilot.pause()
        self.assertEqual(indicator.styles.background.hex.lower(), MODE_COLORS["sane"].lower())
        self.assertEqual(indicator.region.width, 1)
        self.assertEqual(indicator.region.x, composer.region.x)
        await self.pilot.press("tab")
        await self.pilot.pause()
        self.assertTrue(diff_bar.is_on_screen)
        self.assertFalse(chat_bar.is_on_screen)
        self.assertIs(self.app.active_info_bar(), diff_bar)

    async def test_enter_toggles_substep_escape_does_not_and_archive_cannot_validate(self):
        value = state()
        value["steps"][0]["substeps"][0]["reviewed"] = True
        self.workspace.apply_state(value)
        await self.pilot.press("enter", "escape")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.await_count, 0)
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "implementation.unvalidate")
        self.app.client.send.reset_mock()
        self.workspace.archive = True
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.await_count, 0)

    async def test_mode_popup_has_only_two_paces_and_escape_does_not_start(self):
        self.workspace.choose_mode()
        await self.pilot.pause()
        self.assertIsInstance(self.app.screen, ImplementationModeScreen)
        self.assertEqual(self.app.screen.options, ["Step by step", "Everything"])
        await self.pilot.press("escape")
        self.assertNotIsInstance(self.app.screen, QuestionScreen)
        self.assertEqual(self.app.client.send.await_count, 0)
        self.workspace.choose_mode()
        await self.pilot.press("down", "enter")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["pace"], "everything")

    async def test_validate_confirmation_cancel_and_permanent_summary(self):
        await self.pilot.press("enter", "down", "enter")
        self.assertIsInstance(self.app.screen, QuestionScreen)
        self.assertEqual(self.app.client.send.await_count, 0)
        await self.pilot.press("escape")
        self.assertEqual(self.app.client.send.await_count, 0)
        await self.pilot.press("enter", "enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual((request["type"], request["substep"]), ("implementation.validate", 1))
        value = state()
        value["steps"][0]["whole"]["reviewed"] = True
        value["steps"][0]["substeps"][0]["reviewed"] = True
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertNotIn((0, 1), self.workspace.rows)
        self.assertEqual(self.workspace.selection, (0, -1))
        self.assertFalse(self.workspace.query_one("#implementation-validate").display)
        text = "\n".join(str(widget.render()) for widget in self.workspace.query_one(ImplementationStepView).query(Static))
        self.assertIn("State", text)
        self.assertNotIn("[x]", text)
        self.assertIn("A testable state model", text)

    async def test_spinner_preserves_list_scroll_selection_and_options(self):
        value = state()
        value["status"] = "running"
        value["work_step"] = 15
        value["steps"] = [copy.deepcopy(value["steps"][0]) for _ in range(30)]
        value["plan"]["steps"] = [copy.deepcopy(value["plan"]["steps"][0]) for _ in range(30)]
        self.workspace.apply_state(value)
        self.workspace.selection = (15, -1)
        self.workspace.refresh_rows()
        await self.pilot.pause()
        listing = self.workspace.query_one("#implementation-steps", OptionList)
        listing.scroll_to(y=10, animate=False, force=True)
        await self.pilot.pause()
        scroll = listing.scroll_y
        option = listing.get_option_at_index(15)
        for _ in range(10):
            self.workspace.animate_spinner()
        await self.pilot.pause()
        self.assertEqual(listing.scroll_y, scroll)
        self.assertIs(listing.get_option_at_index(15), option)
        self.assertEqual(self.workspace.selection, (15, -1))

    async def test_expanding_step_scrolls_new_selection_into_outer_menu(self):
        value = state()
        definition = value["plan"]["steps"][0]
        definition["substeps"] = [{"title": f"Substep {i} with a long wrapped label"} for i in range(24)]
        built = value["steps"][0]
        built["substeps"] = [copy.deepcopy(built["substeps"][0]) for _ in definition["substeps"]]
        value["plan"]["steps"] = [copy.deepcopy(definition) for _ in range(20)]
        value["steps"] = [copy.deepcopy(built) for _ in range(20)]
        self.workspace.apply_state(value)
        self.workspace.selection = (19, -1)
        self.workspace.show_selection()
        await self.pilot.resize_terminal(90, 18)
        await self.pilot.pause()
        await self.pilot.press("enter")
        await self.pilot.pause()
        listing = self.workspace.query_one("#implementation-steps", OptionList)
        menu = self.workspace.query_one("#implementation-step-list")
        index = self.workspace.rows.index(self.workspace.selection)
        top = listing.region.y + listing.styles.gutter.top + listing._index_to_line[index] - int(listing.scroll_y)
        self.assertGreaterEqual(top, menu.content_region.y)
        self.assertLessEqual(top + listing._heights[index], menu.content_region.bottom)
        self.assertEqual(listing.scroll_y, 0)
        self.assertGreaterEqual(listing.size.height, sum(listing._heights.values()))
        conclusion = self.workspace.query_one("#implementation-conclusion")
        self.assertGreaterEqual(conclusion.region.y, listing.region.bottom)

    async def test_step_navigation_scrolls_outer_menu_to_selected_wrapped_row(self):
        value = state()
        value["steps"] = [copy.deepcopy(value["steps"][0]) for _ in range(20)]
        value["plan"]["steps"] = [copy.deepcopy(value["plan"]["steps"][0]) for _ in range(20)]
        for definition in value["plan"]["steps"]:
            definition["title"] = "A long step title wrapping across several menu lines"
        self.workspace.apply_state(value)
        await self.pilot.pause()
        menu = self.workspace.query_one("#implementation-step-list")
        listing = self.workspace.query_one("#implementation-steps", OptionList)
        self.assertGreater(menu.max_scroll_y, 0)

        def assert_visible():
            index = self.workspace.rows.index(self.workspace.selection)
            top = listing.region.y + listing.styles.gutter.top + listing._index_to_line[index] - int(listing.scroll_y)
            bottom = top + listing._heights[index]
            self.assertGreaterEqual(top, menu.content_region.y)
            self.assertLessEqual(bottom, menu.content_region.bottom)

        for _ in range(15):
            await self.pilot.press("down")
            assert_visible()
        self.assertEqual(self.workspace.selection, (15, -1))
        self.assertGreater(menu.scroll_y, 0)
        # A state update must not undo a manual scroll at unchanged selection.
        menu.scroll_home(animate=False, immediate=True, force=True)
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertEqual(menu.scroll_y, 0)
        await self.pilot.press("up")
        assert_visible()
        for _ in range(14):
            await self.pilot.press("up")
            assert_visible()
        self.assertEqual(self.workspace.selection, (0, -1))

    async def test_substep_navigation_scrolls_menu_even_from_review_chat(self):
        value = state()
        definition = value["plan"]["steps"][0]
        definition["substeps"] = [{"title": f"Substep {index} with a wrapped menu label"} for index in range(18)]
        built = value["steps"][0]
        built["substeps"] = [copy.deepcopy(built["substeps"][0]) for _ in definition["substeps"]]
        self.workspace.apply_state(value)
        await self.pilot.press("enter", "tab")
        menu = self.workspace.query_one("#implementation-step-list")
        listing = self.workspace.query_one("#implementation-steps", OptionList)
        for _ in range(14):
            await self.pilot.press("down")
        self.assertEqual(self.workspace.selection, (0, 14))
        self.assertTrue(self.workspace.chat_open)
        self.assertGreater(menu.scroll_y, 0)
        index = self.workspace.rows.index(self.workspace.selection)
        top = listing.region.y + listing.styles.gutter.top + listing._index_to_line[index] - int(listing.scroll_y)
        self.assertGreaterEqual(top, menu.content_region.y)
        self.assertLessEqual(top + listing._heights[index], menu.content_region.bottom)

    async def test_direct_zones_and_progressive_historical_context(self):
        value = state()
        after = "\n".join(f"line {i}" for i in range(200)) + "\n"
        diff = {"path": "model.py", "after": after, "before": after,
                "patch": "@@ -60 +60 @@\n-old first\n+line 59\n@@ -150 +150 @@\n-old second\n+line 149\n"}
        value["steps"][0]["substeps"][0]["diffs"] = [diff]
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        self.assertEqual(self.workspace.diff_index, 1)
        self.assertEqual(len(self.workspace.zones), 2)
        self.assertEqual([zone.hunk for zone in self.workspace.zones], [0, 1])
        code = self.workspace.query_one(HistoricalCode)
        await self.pilot.pause()
        self.assertGreater(code.start, 0)
        self.assertLess(code.end, 200)
        old_start, old_end = code.start, code.end
        code.scroll_end(animate=False)
        await self.pilot.pause()
        self.assertGreater(code.end, old_end)
        code.scroll_home(animate=False)
        await self.pilot.pause()
        self.assertLess(code.start, old_start)
        await self.pilot.press("right")
        self.assertEqual(self.workspace.diff_index, 2)
        await self.pilot.press("right")
        self.assertEqual(self.workspace.diff_index, 2)
        await self.pilot.press("left", "left")
        self.assertEqual(self.workspace.diff_index, 1)
        self.assertIsNotNone(code.zone)

    async def test_three_diffs_stop_at_both_ends_including_with_chat_open(self):
        value = state()
        value["steps"][0]["substeps"][0]["diffs"].append({"path": "third.py", "patch": "+third\n"})
        self.workspace.apply_state(value)
        await self.pilot.press("enter", "left")
        self.assertEqual(self.workspace.diff_index, 1)
        await self.pilot.press("right", "right", "right")
        self.assertEqual(self.workspace.diff_index, 3)
        self.assertEqual(str(self.workspace.query_one("#implementation-diff-count", Static).render()), "3/3")
        await self.pilot.press("tab", "ctrl+right")
        self.assertEqual(self.workspace.diff_index, 3)
        await self.pilot.press("ctrl+left", "ctrl+left", "ctrl+left")
        self.assertEqual(self.workspace.diff_index, 1)
        self.assertEqual(str(self.workspace.query_one("#implementation-diff-count", Static).render()), "1/3")

    async def test_empty_review_arrows_navigate_diffs_without_leaving_composer(self):
        await self.pilot.press("enter", "tab")
        entry = self.workspace.query_one(ReviewInput)
        self.assertTrue(entry.has_focus)
        self.assertEqual(entry.text, "")
        await self.pilot.press("right", "right")
        self.assertEqual(self.workspace.diff_index, 2)
        await self.pilot.press("left", "left")
        self.assertEqual(self.workspace.diff_index, 1)
        self.assertTrue(self.workspace.chat_open)
        self.assertTrue(entry.has_focus)
        self.assertEqual(entry.text, "")
        await self.pilot.press("h", "i")
        self.assertEqual(entry.text, "hi")
        await self.pilot.press("left")
        self.assertEqual(entry.cursor_location, (0, 1))
        await self.pilot.press("right")
        self.assertEqual(entry.cursor_location, (0, 2))
        self.assertEqual(self.workspace.diff_index, 1)
        entry.text = " "
        entry.move_cursor((0, 0))
        await self.pilot.press("right")
        self.assertEqual(entry.cursor_location, (0, 1))
        self.assertEqual(self.workspace.diff_index, 1)
        entry.text = ""
        await self.pilot.press("right")
        self.assertEqual(self.workspace.diff_index, 2)
        self.assertTrue(entry.has_focus)
        self.app.client.send.assert_not_awaited()

    async def test_chat_up_down_keeps_item_navigation_and_left_edits(self):
        await self.pilot.press("enter", "tab", "down")
        self.assertEqual(self.workspace.selection, (0, 1))
        self.assertIsInstance(self.app.focused, ReviewInput)

    async def test_filename_context_and_tree_follow_the_zone(self):
        await self.pilot.press("enter")
        filename = self.workspace.query_one("#implementation-file", Static)
        counter = self.workspace.query_one("#implementation-diff-count", Static)
        context = self.workspace.query_one("#implementation-agent-info", AgentInfoBar)
        hint = self.workspace.query_one("#implementation-hint", Static)
        tree = self.workspace.query_one(Tree)
        self.assertEqual(str(filename.render()), "src/state.py")
        self.assertEqual(str(counter.render()), "1/2")
        self.assertLess(counter.region.y, filename.region.y)
        self.assertNotIn("1/2", str(hint.render()))
        self.assertFalse(self.workspace.query("#implementation-context"))
        self.assertEqual(tree.cursor_node.data, 1)
        self.assertLess(filename.region.y, context.region.y)
        self.assertLess(hint.region.y, context.region.y)
        await self.pilot.press("right")
        self.assertEqual(str(filename.render()), "tests/state.py")
        self.assertEqual(str(counter.render()), "2/2")
        self.assertEqual(tree.cursor_node.data, 2)
        value = state()
        value["steps"][0]["substeps"][0]["diffs"][1]["path"] = "/outside/repo/file.py"
        self.workspace.apply_state(value)
        self.assertEqual(str(filename.render()), "/outside/repo/file.py")
        value["steps"][0]["repository_root"] = "/outside/repo"
        self.workspace.apply_state(value)
        self.assertEqual(str(filename.render()), "file.py")
        value["steps"][0]["repository_root"] = "/other/repo"
        self.workspace.apply_state(value)
        self.assertEqual(str(filename.render()), "/outside/repo/file.py")

    async def test_chronological_diff_navigation_tree_and_review_keep_original_hunk_identity(self):
        from test_implementation_diff_order import ordered_diffs
        value = state()
        value["steps"][0]["substeps"][0]["diffs"] = ordered_diffs()
        value["steps"][0]["whole"]["diffs"] = ordered_diffs()
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        code = self.workspace.query_one(HistoricalCode)
        self.assertEqual((code.zone.path, code.zone.hunk), ("z.py", 1))
        await self.pilot.press("right")
        self.assertEqual((code.zone.path, code.zone.hunk), ("a.py", 0))
        await self.pilot.press("right")
        self.assertEqual((code.zone.path, code.zone.hunk), ("z.py", 0))
        self.workspace.selection = (0, -1)
        self.workspace.show_selection()
        tree = self.workspace.query_one(ImplementationChangesTree)
        tree.select_node(tree.files["z.py"])
        await self.pilot.pause()
        self.assertEqual((code.zone.path, code.zone.hunk), ("z.py", 1))
        self.assertEqual(self.workspace.diff_index, 1)
        await self.pilot.press("tab")
        self.workspace.query_one(ReviewInput).text = "Explain this first contribution"
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual((request["type"], request["path"], request["hunk"]), ("implementation.review", "z.py", 1))

    def scoped_changes_state(self):
        value = state()
        first = value["steps"][0]["substeps"][0]
        first["diffs"] = [{"path": "src/state.py", "patch": "-old\n"}]
        second = copy.deepcopy(first)
        second["diffs"] = [
            {"path": "src/state.py", "patch": "+new\n"},
            {"path": "src/second.py", "patch": "+second\n"}]
        value["steps"][0]["substeps"].append(second)
        value["plan"]["steps"][0]["substeps"].append({"title": "Extend"})
        value["steps"][0]["whole"]["diffs"] = [
            {"path": "src/state.py", "patch": "-old\n+new\n"}, second["diffs"][1]]
        return value

    async def test_changes_scope_and_signs_follow_selected_substep(self):
        value = self.scoped_changes_state()
        self.workspace.apply_state(value)
        tree = self.workspace.query_one(ImplementationChangesTree)
        self.assertEqual(tree.changed_paths, {"src/state.py", "src/second.py"})
        self.assertEqual(tree.files["src/state.py"].label.plain, "+- state.py")
        await self.pilot.press("enter")
        self.assertEqual(self.workspace.selection, (0, 0))
        self.assertEqual(tree.changed_paths, {"src/state.py"})
        self.assertEqual(tree.files["src/state.py"].label.plain, "- state.py")
        self.assertNotIn("src/second.py", tree.files)
        await self.pilot.press("down")
        self.assertEqual(self.workspace.selection, (0, 1))
        self.assertEqual(tree.changed_paths, {"src/state.py", "src/second.py"})
        self.assertEqual(tree.files["src/state.py"].label.plain, "+ state.py")
        self.workspace.apply_state(copy.deepcopy(value))
        self.assertEqual(self.workspace.selection, (0, 1))
        self.assertEqual(tree.files["src/state.py"].label.plain, "+ state.py")

    async def test_step_changes_click_opens_owner_and_first_hunk_without_taking_focus(self):
        value = self.scoped_changes_state()
        value["steps"][0]["substeps"][1]["diffs"][0].update(
            after="new\nkeep\nlast\n", patch="@@ -1 +1 @@\n-old\n+new\n@@ -3 +3 @@\n-before\n+last\n")
        self.workspace.apply_state(value)
        tree = self.workspace.query_one(ImplementationChangesTree)
        await self.pilot.pause()
        focused = self.app.focused
        node = tree.files["src/second.py"]
        await self.pilot.click(tree, offset=(10, tree.content_region.y - tree.region.y + node.line - int(tree.scroll_y)))
        await self.pilot.pause()
        self.assertEqual(self.workspace.selection, (0, 1))
        self.assertIn(0, self.workspace.expanded)
        self.assertEqual(self.workspace.diff_index, 3)
        self.assertEqual(self.workspace.query_one(HistoricalCode).zone.path, "src/second.py")
        self.assertFalse(tree.has_focus)
        self.assertIs(self.app.focused, focused)
        self.workspace.selection = (0, -1)
        self.workspace.show_selection()
        tree.select_node(tree.files["src/state.py"])
        await self.pilot.pause()
        self.assertEqual(self.workspace.selection, (0, 0))
        self.assertEqual(self.workspace.diff_index, 1)
        self.assertEqual(self.workspace.query_one(HistoricalCode).zone.path, "src/state.py")
        self.app.client.send.assert_not_awaited()

    async def test_changes_keeps_other_substep_files_as_plain_snapshot_neighbors(self):
        value = self.scoped_changes_state()
        value["steps"][0].update(repository_root="/repo", repository_tree="baseline")
        self.workspace.apply_state(value)
        await self.reply_directory([{"name": "src", "directory": True}])
        await self.reply_directory([{"name": "state.py", "directory": False},
                                    {"name": "second.py", "directory": False}], path="src")
        await self.pilot.press("enter")
        tree = self.workspace.query_one(ImplementationChangesTree)
        self.assertEqual(tree.changed_paths, {"src/state.py"})
        self.assertEqual(tree.files["src/second.py"].label.plain, "second.py")
        self.assertIsNone(tree.files["src/second.py"].data)

    async def test_changes_does_not_open_substeps_before_step_finishes(self):
        value = self.scoped_changes_state()
        value["steps"][0]["whole"]["complete"] = False
        value.update(status="running", work_step=0)
        self.workspace.apply_state(value)
        tree = self.workspace.query_one(ImplementationChangesTree)
        self.assertEqual(tree.changed_paths, {"src/state.py", "src/second.py"})
        tree.select_node(tree.files["src/second.py"])
        await self.pilot.pause()
        self.assertEqual(self.workspace.selection, (0, -1))
        self.assertNotIn(0, self.workspace.expanded)

    async def test_step_heading_is_left_aligned_and_wraps_without_horizontal_overflow(self):
        value = state()
        value["steps"][0]["whole"]["explanation"] = "- " + "Concrete detail " * 25
        self.workspace.apply_state(value)
        await self.pilot.pause()
        code = self.workspace.query_one(ImplementationStepView)
        content = code.query_one(".step-overview", Static)
        self.assertGreater(content.size.height, 3)
        self.assertEqual(code.max_scroll_x, 0)
        self.assertGreaterEqual(content.region.y, code.region.y)
        self.assertFalse(self.workspace.query_one("#implementation-file").display)
        self.assertFalse(self.workspace.query_one("#implementation-diff-count").display)
        await self.pilot.press("enter", "down")
        self.assertFalse(code.display)
        self.assertTrue(self.workspace.query_one(HistoricalCode).has_class("description"))

    async def test_agent_context_stays_inside_diff_column_and_survives_chat_split(self):
        self.app.update_agent_info({"model": "provider/model", "reasoning": "high",
                                    "last_turn_used_context": 1000, "total_context": 10000})
        await self.pilot.pause()
        bar = self.app.active_info_bar()
        column = self.workspace.query_one("#implementation-diff-column")
        hint = self.workspace.query_one("#implementation-hint")
        self.assertFalse(self.app.query_one("#agent-info").display)
        self.assertIs(bar, self.workspace.query_one("#implementation-agent-info", AgentInfoBar))
        self.assertEqual(bar.labels["model-info"], "provider/model")
        self.assertGreaterEqual(bar.region.x, column.region.x)
        self.assertLessEqual(bar.region.right, column.region.right)
        self.assertLess(hint.region.y, bar.region.y)
        await self.pilot.press("enter", "tab")
        await self.pilot.pause()
        self.assertLessEqual(bar.region.right, column.region.right)
        bar.show_confirmation("cancel", None, 3)
        await self.pilot.pause()
        self.assertTrue(bar.query_one("#confirmation-bar").display)

    async def test_counter_is_left_aligned_bold_with_full_width_background_and_heading_has_no_arrows(self):
        hint = self.workspace.query_one("#implementation-hint", Static)
        self.assertNotIn("←/→", str(hint.render()))

        await self.pilot.press("enter")
        counter = self.workspace.query_one("#implementation-diff-count")
        column = self.workspace.query_one("#implementation-diff-column")
        self.assertEqual(counter.region.width, column.region.width)
        self.assertEqual(counter.styles.content_align_horizontal, "left")
        self.assertTrue(counter.styles.text_style.bold)
        self.assertNotEqual(counter.styles.background.a, 0)
        await self.pilot.press("down")
        self.assertNotIn("←/→", str(hint.render()))

    async def test_enter_on_local_execution_button_does_not_validate_a_substep(self):
        await self.pilot.press("enter")
        self.app.busy = True
        button = self.workspace.query_one("#implementation-agent-info").query_one(ModeButton)
        button.display = True
        button.focus()
        with patch.object(self.app, "request_execution_cancel", new_callable=AsyncMock) as cancel:
            await self.pilot.press("enter")
            await self.pilot.pause()
            cancel.assert_awaited_once()
        self.assertEqual(self.app.client.send.await_count, 0)

    async def reply_directory(self, entries, *, path=""):
        await self.pilot.pause()
        request = next(call.args[0] for call in reversed(self.app.client.send.call_args_list)
                       if call.args[0]["type"] == "implementation.directory" and call.args[0]["path"] == path)
        self.workspace.receive({**request, "entries": entries})
        await self.pilot.pause()
        return request

    async def test_base_tree_loads_root_and_only_final_changed_folder_neighbors(self):
        value = state()
        built = value["steps"][0]
        built.update(repository_root="/repo", repository_tree="baseline")
        original = copy.deepcopy(built["whole"]["diffs"])
        built["whole"]["diffs"] = []
        self.workspace.apply_state(value)
        await self.reply_directory([{"name": name, "directory": directory} for name, directory in
                                   (("README.md", False), ("LICENSE", False), ("docs", True), ("src", True), ("tests", True))])
        tree = self.workspace.query_one(ImplementationChangesTree)
        self.assertEqual(set(tree.files), {"README.md", "LICENSE"})
        self.assertTrue(all(not folder.is_expanded for folder in tree.folders.values()))
        self.assertIn("README.md", [str(node.label) for node in tree.root.children])
        built["whole"]["diffs"] = original
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertTrue(tree.folders["src"].is_expanded)
        self.assertTrue(tree.folders["tests"].is_expanded)
        self.assertFalse(tree.folders["docs"].is_expanded)
        self.assertEqual(str(tree.files["src/state.py"].label), "+- state.py")
        self.assertEqual(str(tree.files["tests/state.py"].label), "+ state.py")
        await self.pilot.press("enter")
        self.assertIs(tree.cursor_node, tree.files["src/state.py"])
        await self.pilot.press("right")
        self.assertIs(tree.cursor_node, tree.files["tests/state.py"])
        self.assertTrue(tree.center_scroll)
        self.assertNotIn("src/other.py", tree.files)
        self.assertNotIn("docs/guide.md", tree.files)
        self.assertEqual([call.args[0]["path"] for call in self.app.client.send.call_args_list], ["", "src", "tests"])
        value["steps"][1]["whole"]["diffs"] = []
        self.workspace.selection = (1, -1)
        self.workspace.diff_index = 0
        self.workspace.refresh_rows()
        self.workspace.show_selection()
        await self.pilot.pause()
        self.assertEqual(set(tree.files), {"README.md", "LICENSE"})
        self.assertTrue(all(not folder.is_expanded for folder in tree.folders.values()))

    async def test_code_has_colored_addition_and_deletion_glyphs(self):
        await self.pilot.press("enter")
        text = self.workspace.query_one("#implementation-patch", Static).render()
        self.assertIn("-  old", str(text))
        self.assertIn("+  new", str(text))
        self.assertTrue(any(span.style.foreground and span.style.foreground.hex == SLOP_COLOR and "-" in str(text)[span.start:span.end] for span in text.spans))
        self.assertTrue(any(span.style.foreground and span.style.foreground.hex == ALGAE_MD and "+" in str(text)[span.start:span.end] for span in text.spans))

    async def test_directory_replies_are_cached_and_stale_snapshots_are_ignored(self):
        value = state()
        value["steps"][0].update(repository_root="/repo", repository_tree="old")
        self.workspace.apply_state(value)
        await self.pilot.pause()
        old = next(call.args[0] for call in self.app.client.send.call_args_list if call.args[0].get("path") == "")
        self.assertEqual(old["type"], "implementation.directory")
        self.assertEqual(old["path"], "")
        for _ in range(5):
            self.workspace.show_selection()
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.await_count, 3)
        value["steps"][0]["repository_tree"] = "new"
        self.workspace.apply_state(value)
        await self.pilot.pause()
        current = next(call.args[0] for call in reversed(self.app.client.send.call_args_list) if call.args[0].get("path") == "")
        self.assertEqual(current["tree"], "new")
        self.workspace.receive({**old, "entries": [{"name": "stale.py", "directory": False}]})
        tree = self.workspace.query_one(ImplementationChangesTree)
        self.assertNotIn("stale.py", tree.files)
        self.workspace.receive({**current, "entries": [{"name": "docs", "directory": True}]})
        tree.folders["docs"].expand()
        await self.pilot.pause()
        folder = self.app.client.send.call_args.args[0]
        self.assertEqual(folder["path"], "docs")
        self.workspace.receive({"type": "request.failed", "requestId": folder["requestId"], "error": "Unavailable"})
        self.assertIn("docs", tree.errors)
        calls = self.app.client.send.await_count
        self.workspace.show_selection()
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.await_count, calls)
        tree.folders["docs"].collapse()
        await self.pilot.pause()
        tree.folders["docs"].expand()
        await self.pilot.pause()
        await self.reply_directory([{"name": "guide.md", "directory": False}], path="docs")
        self.assertEqual(str(tree.folders["docs"].label), "docs")
        self.assertIn("docs/guide.md", tree.files)

    async def test_changed_file_stays_visible_when_many_neighbors_are_hidden(self):
        value = state()
        built = value["steps"][0]
        built.update(repository_root="/repo", repository_tree="baseline")
        diff = {"path": "file-050.py", "patch": "@@ -1 +1 @@\n-old\n+new\n"}
        built["whole"]["diffs"] = [diff]
        built["substeps"][0]["diffs"] = [diff]
        self.workspace.apply_state(value)
        await self.reply_directory([{"name": f"file-{index:03}.py", "directory": False} for index in range(100)])
        await self.pilot.press("enter")
        await self.pilot.pause()
        tree = self.workspace.query_one(ImplementationChangesTree)
        self.assertIs(tree.cursor_node, tree.files["file-050.py"])
        self.assertGreaterEqual(tree.cursor_line, tree.scroll_y)
        self.assertLess(tree.cursor_line, tree.scroll_y + tree.content_size.height)
        self.assertEqual(len(tree._tree_lines), 8)  # Root, five entries, marker and Show all.

    async def test_changes_footer_toggle_scrolls_with_tree_without_stealing_step_focus(self):
        value = state()
        value["steps"][0].update(repository_root="/repo", repository_tree="baseline")
        self.workspace.apply_state(value)
        await self.reply_directory([{"name": "src", "directory": True}, {"name": "tests", "directory": True}])
        await self.reply_directory([{"name": "state.py", "directory": False}] +
                                   [{"name": f"neighbor-{i}.py", "directory": False} for i in range(30)], path="src")
        tree = self.workspace.query_one(ImplementationChangesTree)
        focused = self.app.focused
        selection, diff = self.workspace.selection, self.workspace.diff_index
        for expanded in (True, False):
            tree.scroll_to_node(tree.toggle_node, animate=False)
            await self.pilot.pause()
            self.assertIs(tree._tree_lines[-1].node, tree.toggle_node)
            await self.pilot.click(tree, offset=(2, tree.content_region.y - tree.region.y +
                                                tree.toggle_node.line - int(tree.scroll_y)))
            await self.pilot.pause()
            self.assertEqual(tree.show_all, expanded)
            self.assertIs(self.app.focused, focused)
            self.assertEqual((self.workspace.selection, self.workspace.diff_index), (selection, diff))
        self.assertTrue(all(call.args[0]["type"] == "implementation.directory"
                            for call in self.app.client.send.call_args_list))

    async def test_arrows_stay_in_steps_even_from_tree_and_chat(self):
        value = state()
        value["steps"][0].update(repository_root="/repo", repository_tree="baseline")
        self.workspace.apply_state(value)
        await self.reply_directory([{"name": "README.md", "directory": False}, {"name": "docs", "directory": True}])
        tree = self.workspace.query_one(ImplementationChangesTree)
        self.assertFalse(tree.can_focus)
        focused = self.app.focused
        await self.pilot.click(tree, offset=(2, 0))
        self.assertIs(self.app.focused, focused)
        tree.root.expand()
        tree.move_cursor(tree.folders["docs"])
        tree.action_select_cursor()
        await self.pilot.pause()
        self.assertTrue(tree.folders["docs"].is_expanded)
        await self.pilot.press("tab", "down")
        self.assertEqual(self.workspace.navigation_target, "implementation-steps")
        self.assertEqual(self.workspace.selection, (1, -1))
        self.assertNotEqual(str(tree.cursor_node.label), "guide.md")

    async def test_explicitly_unnecessary_substeps_are_disabled_struck_and_retained_in_summary(self):
        value = state()
        built = value["steps"][0]
        built["substeps"].insert(0, dict(built["substeps"][0], unnecessary=True, diffs=[], explanation="No distinct file changes in this step"))
        value["plan"]["steps"][0]["substeps"].insert(0, {"title": "Empty work"})
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        self.assertEqual(self.workspace.selection, (0, 1))
        option = self.workspace.query_one(OptionList).get_option_at_index(1)
        self.assertTrue(option.disabled)
        self.assertIn("strike", str(option.prompt.style))
        self.assertIn("Empty work", self.workspace.summary(0).plain)
        self.assertIn("No longer necessary given the changes in previous steps.", self.workspace.summary(0).plain)
        self.assertNotIn("No distinct file changes", self.workspace.summary(0).plain)
        built["substeps"][1]["reviewed"] = True
        self.workspace.apply_state(value)
        await self.pilot.press("down", "enter")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["substep"], 2)
        self.assertNotIsInstance(self.app.screen, QuestionScreen)

    async def test_summary_is_one_left_aligned_block_even_without_corrections(self):
        value = state()
        value["steps"][0]["whole"].update(reviewed=True, explanation="Short\nA considerably longer bullet")
        self.workspace.apply_state(value)
        await self.pilot.pause()
        code = self.workspace.query_one(ImplementationStepView)
        patch = code.query_one(".step-overview")
        self.assertEqual(patch.styles.text_align, "left")
        self.assertEqual(code.query_one(".step-construction").styles.align_horizontal, "left")

    async def test_single_implement_tab_keeps_frozen_plan_and_steps_in_same_session(self):
        from textual.widgets import Tabs
        tabs = self.app.query_one(Tabs)
        self.assertEqual([str(tab.label) for tab in tabs.query("Tab")],
                         ["Discussion", "Implement"])
        await self.pilot.click("#implementation-show-plan")
        await self.pilot.pause()
        self.assertTrue(self.workspace.display)
        self.assertFalse(self.app.query_one("#chat").display)
        self.assertFalse(self.app.query_one("#composer").display)
        self.assertFalse(self.app.query_one("#implementation-plan-panel").display)
        self.assertTrue(self.workspace.query_one("#implementation-accepted-plan").display)
        self.assertEqual(self.workspace.query_one(ImplementationChangesTree).files, {})
        plan_view = self.workspace.query_one("#implementation-accepted-plan")
        self.assertGreaterEqual(plan_view.region.x, self.workspace.query_one("#implementation-main").region.x)
        self.assertIn("Accepted foundation plan", plan_view.query_one("ResponseMarkdown")._markdown)
        button = self.workspace.query_one("#implementation-show-plan")
        sidebar = self.workspace.query_one("#implementation-sidebar")
        self.assertLessEqual(abs((button.region.x + button.region.right) - (sidebar.region.x + sidebar.region.right)), 2)
        self.workspace.apply_state(state())
        self.assertTrue(plan_view.display)
        self.assertEqual(self.workspace.query_one(ImplementationChangesTree).files, {})
        await self.pilot.click("#implementation-steps", offset=(3, 0))
        await self.pilot.pause()
        self.assertTrue(self.workspace.display)
        self.assertFalse(self.workspace.plan_visible)
        self.assertEqual(self.app.session_id, "branch")
        self.assertEqual(self.app.client.send.await_count, 0)

    async def test_keyboard_opens_plan_above_first_step_and_returns_to_selection(self):
        selection = self.workspace.selection
        await self.pilot.press("up")
        self.assertTrue(self.workspace.query_one("#implementation-show-plan").has_focus)
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.assertTrue(self.workspace.display)
        self.assertTrue(self.workspace.query_one("#implementation-accepted-plan").display)
        await self.pilot.press("down")
        await self.pilot.pause()
        self.assertTrue(self.workspace.display)
        self.assertEqual(self.workspace.selection, selection)
        self.app.client.send.assert_not_awaited()

    async def test_discussion_is_read_only_during_review_without_losing_step_or_draft(self):
        from sos_tui.discussion_preview import DiscussionPreview
        selection = self.workspace.selection
        self.workspace.review_running = True
        self.workspace.query_one(ReviewInput).text = "Next review draft"
        await self.app.implementation.select_tab("discussion")
        await self.pilot.pause()
        self.assertTrue(self.app.query_one(DiscussionPreview).display)
        self.assertFalse(self.workspace.display)
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "session.history")
        await self.app.implementation.select_tab("implement")
        self.assertTrue(self.workspace.display)
        self.assertEqual(self.workspace.selection, selection)
        self.assertEqual(self.workspace.query_one(ReviewInput).text, "Next review draft")
        self.assertTrue(self.workspace.review_running)

    async def test_discussion_stays_frozen_and_locked_between_steps_and_during_generation(self):
        import json
        from sos_tui.discussion_preview import DiscussionPreview
        entry = self.app.query_one("#message", MessageInput)
        entry.text = "Implementation draft"
        self.app.busy = True
        await self.app.implementation.select_tab("discussion")
        await self.pilot.pause()
        preview = self.app.query_one(DiscussionPreview)
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "session.history")
        common = {"requestId": request["requestId"], "sessionId": "discussion"}
        self.app.handle_event(dict(common, type="session.attach_started", session={"id": "discussion", "mode": "sane"},
                                   messageCount=1, page={}))
        message = {"id": "parent", "role": "assistant", "content": [{"type": "text", "text": "Frozen discussion"}]}
        self.app.handle_event(dict(common, type="session.history_chunk", messageIndex=0, partIndex=0,
                                   last=True, data=json.dumps(message)))
        self.app.handle_event(dict(common, type="session.attached", messageCount=1))
        await self.pilot.pause()
        self.assertTrue(entry.disabled)
        self.assertFalse(preview.query_one("#discussion-preview-open").display)
        notice = preview.query_one("#discussion-locked-composer")
        self.assertTrue(notice.visible)
        self.assertIn("Finish the current implementation", str(notice.render()))
        self.assertFalse(self.app.query_one("#agent-info").query_one(ModeButton).busy)
        self.assertIs(self.app.active_info_bar(), self.workspace.query_one("#implementation-agent-info"))
        await self.pilot.press("x", "enter")
        self.assertEqual(entry.text, "Implementation draft")
        self.app.client.send.assert_awaited_once()
        viewport = preview.query_one("HistoryViewport")
        old_widgets = list(viewport.children)
        self.app.busy = False  # Waiting for human validation still locks Discussion.
        self.app.set_status("Ready")
        self.assertTrue(entry.disabled)
        await self.pilot.click("#implement")
        self.assertFalse(preview.display)
        self.assertEqual(entry.text, "Implementation draft")
        self.assertFalse(entry.disabled)
        await self.pilot.click("#discussion")
        await self.pilot.pause()
        self.assertTrue(preview.display)
        self.assertTrue(entry.disabled)
        self.assertEqual(list(viewport.children), old_widgets)
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.session_id, "branch")

    async def test_discussion_does_not_advance_generation_activity(self):
        self.app.query_one("#chat").start_generation_footer({"model": "test"})
        self.app.busy = True
        await self.app.implementation.select_tab("discussion")
        await self.pilot.pause()
        footer = self.app.query_one("#chat").live_footer
        with patch.object(footer, "update_elapsed") as elapsed:
            self.app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(1))
            elapsed.assert_not_called()
        self.app.busy = False

    async def test_review_has_shared_styles_user_messages_and_live_activity(self):
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        from sos_tui.turn_activity import TurnActivity
        from sos_tui.user_message import UserMessage
        from sos_tui.response_markdown import ResponseMarkdown
        await self.pilot.press("enter", "tab")
        entry = self.workspace.query_one(ReviewInput)
        entry.text = "Explain this"
        await self.pilot.press("enter")
        await self.pilot.pause()
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        self.assertIsInstance(chat.children[0], UserMessage)
        self.assertIsInstance(chat.children[1], TurnActivity)
        self.assertIsNotNone(chat.running.toggle.started_at)
        self.assertEqual(chat.styles.background, self.app.query_one("#chat").styles.background)
        records = [{"role": "user", "content": [{"type": "text", "text": "Explain this"}]},
                   {"role": "assistant", "content": [{"type": "reasoning", "text": "Thinking"},
                     {"type": "tool_call", "callId": "read-1", "toolName": "read", "arguments": {"filePath": "model.py"}}]},
                   {"role": "tool", "content": [{"type": "tool_result", "callId": "read-1", "output": "model", "isError": False}]},
                   {"role": "assistant", "content": [{"type": "text", "text": "Explanation"}]}]
        await chat.show_records(records, False)
        await self.pilot.pause()
        self.assertIsNone(chat.running)
        activity = chat.query_one(TurnActivity)
        self.assertTrue(activity.toggle.has_reasoning)
        self.assertTrue(activity.toggle.has_reads)
        self.assertEqual(activity.toggle.step_count, 2)
        self.assertTrue(chat.query(ResponseMarkdown))

    async def test_review_history_does_not_add_empty_activity_after_answer(self):
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        from sos_tui.turn_activity import TurnActivity
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        records = [
            {"role": "user", "content": [{"type": "text", "text": "Explain this"}]},
            {"role": "assistant", "content": [{"type": "text", "text": "Here is the explanation."}]},
            {"role": "assistant", "content": [], "metadata": {"model": "test", "reasoning": "none", "duration_ms": 1}},
        ]
        await chat.show_records(records, False)
        await self.pilot.pause()
        self.assertEqual(len(chat.query(TurnActivity)), 1)
        self.assertEqual(len(chat.query("ResponseMarkdown")), 1)
        response = chat.query_one("ResponseMarkdown")
        records.append({"role": "assistant", "content": [{"type": "reasoning", "text": "More context"}]})
        await chat.show_records(records, True)
        self.assertIs(chat.query_one("ResponseMarkdown"), response)

    async def test_review_history_refreshes_are_serialized(self):
        import asyncio
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        first = [{"role": "user", "content": [{"type": "text", "text": "Question"}]}]
        second = first + [{"role": "assistant", "content": [{"type": "text", "text": "Answer"}]}]
        await asyncio.gather(chat.show_records(first, True), chat.show_records(second, False))
        await self.pilot.pause()
        self.assertEqual([widget.render().plain for widget in chat.query(".user-text")], ["Question"])
        self.assertEqual(len(chat.query("ResponseMarkdown")), 1)
        self.assertIsNone(chat.running)

    async def test_review_label_survives_runtime_info_updates(self):
        await self.pilot.press("enter", "tab")
        self.app.update_agent_info({"model": "updated-model", "reasoning": "high"})
        await self.pilot.pause()
        button = self.workspace.query_one("#implementation-review-agent-info").query_one(ModeButton)
        self.assertEqual(str(button.label), "Sane")

    async def test_review_history_can_be_restored_after_switching_sessions(self):
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        records = [{"role": "user", "content": [{"type": "text", "text": "Question"}]},
                   {"role": "assistant", "content": [{"type": "text", "text": "Answer"}]}]
        await chat.show_records(records, False)
        await chat.reset_history()
        await chat.show_records(records, False)
        await self.pilot.pause()
        self.assertEqual(len(chat.query("UserMessage")), 1)
        self.assertEqual(len(chat.query("ResponseMarkdown")), 1)

    async def test_close_is_reenabled_after_review_and_click_sends_real_request(self):
        import json
        value = state()
        value.update(status="completed", work_step=2)
        for step in value["steps"]:
            step["whole"].update(complete=True, reviewed=True)
        self.workspace.review_running = True
        self.workspace.apply_state(value)
        close = self.workspace.query_one("#implementation-close", Button)
        self.assertTrue(close.disabled)
        self.workspace.receive({"type": "btw.started", "requestId": "review-end", "sessionId": "branch",
            "session": {"id": "branch"}, "messageCount": 1,
            "branch": {"review": True, "status": "completed"}})
        self.workspace.receive({"type": "btw.chunk", "requestId": "review-end", "sessionId": "branch",
            "messageIndex": 0, "partIndex": 0, "last": True,
            "data": json.dumps({"role": "assistant", "content": [{"type": "text", "text": "Answer"}]})})
        self.workspace.receive({"type": "btw.finished", "requestId": "review-end", "sessionId": "branch", "messageCount": 1})
        await self.pilot.pause()
        self.assertFalse(close.disabled)
        await self.pilot.click(close)
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "implementation.close")
        self.assertEqual(request["sessionId"], "discussion")

    async def test_review_chat_follows_new_text_only_while_reader_is_at_bottom(self):
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        await self.pilot.press("enter", "tab")
        await self.pilot.pause()
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        question = {"role": "user", "content": [{"type": "text", "text": "Question"}]}
        answer = {"role": "assistant", "content": [{"type": "text", "text": "\n\n".join(f"Line {i}" for i in range(45))}]}
        await chat.show_records([question, answer], False)
        await wait_until(self.pilot, lambda: chat.max_scroll_y > 0 and chat.scroll_y == chat.max_scroll_y,
                         "review Markdown layout and initial follow")
        self.assertGreater(chat.max_scroll_y, 0)
        self.assertTrue(chat.follow_tail)
        self.assertEqual(chat.scroll_y, chat.max_scroll_y)
        chat.scroll_home(animate=False, immediate=True, force=True)
        await self.pilot.pause()
        self.assertFalse(chat.follow_tail)
        followup = {"role": "assistant", "content": [{"type": "text", "text": "\n".join(f"More {i}" for i in range(20))}]}
        await chat.show_records([question, answer, followup], False)
        await self.pilot.pause()
        self.assertEqual(chat.scroll_y, 0)
        self.assertFalse(chat.follow_tail)
        chat.scroll_end(animate=False, immediate=True, force=True)
        await self.pilot.pause()
        self.assertTrue(chat.follow_tail)

    async def test_validation_advances_only_after_successful_reply(self):
        await self.pilot.press("enter", "enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(self.workspace.selection, (0, 0))
        value = state()
        value["steps"][0]["substeps"][0]["reviewed"] = True
        self.workspace.receive({"type": "implementation.updated", "sessionId": "branch", "requestId": request["requestId"], "implementation": value})
        self.assertEqual(self.workspace.selection, (0, 1))

    async def test_import_conflict_uses_slop_notice_without_advancing_validation(self):
        from sos_tui.input_error_notice import InputErrorNotice
        await self.pilot.press("enter", "down", "enter", "enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual((request["type"], request["substep"]), ("implementation.validate", 1))
        selection = self.workspace.selection
        message = "Conflicting workspace changes: src/state.py. Nothing was imported; the step remains unvalidated."
        with patch.object(self.app, "notify") as notify:
            self.workspace.receive({"type": "request.failed", "requestId": request["requestId"], "error": message})
            await self.pilot.pause()
            notify.assert_not_called()
        notice = self.app.screen.query_one(InputErrorNotice)
        self.assertTrue(notice.display)
        self.assertEqual(notice.styles.border_left[1].hex.lower(), SLOP_COLOR.lower())
        self.assertIn(message, str(notice.render()))
        self.assertEqual(self.workspace.selection, selection)
        self.assertFalse(self.workspace.state["steps"][0]["whole"]["reviewed"])
        self.assertNotIn(request["requestId"], self.workspace.pending_validation)

    async def test_review_cancel_button_animates_and_cancels_only_review(self):
        value = state()
        value.update(status="running", work_step=1)
        self.workspace.apply_state(value)
        await self.pilot.press("enter", "tab")
        entry = self.workspace.query_one(ReviewInput)
        entry.text = "Explain this step"
        await self.pilot.press("enter")
        await self.pilot.pause()
        button = self.workspace.query_one("#implementation-review-agent-info").query_one(ModeButton)
        self.assertTrue(button.is_on_screen)
        self.assertTrue(button.busy)
        self.assertEqual(button.mode, "sane")
        from sos_tui.config import SANE_COLOR
        self.assertEqual(button.styles.background.hex.lower(), SANE_COLOR.lower())
        self.assertEqual(str(button.label), "Cancel")
        frame_time = button.last_frame
        await self.pilot.pause(.1)
        self.assertGreater(button.last_frame, frame_time)
        await self.pilot.click(button)
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "implementation.review.cancel")
        self.assertFalse(any(call.args[0]["type"] == "execution.cancel" for call in self.app.client.send.call_args_list))
        self.assertNotIsInstance(self.app.screen, QuestionScreen)

    async def test_idle_review_mode_button_shows_centered_sane_label(self):
        from sos_tui.config import SANE_COLOR
        await self.pilot.press("enter", "tab")
        await self.pilot.pause()
        button = self.workspace.query_one("#implementation-review-agent-info").query_one(ModeButton)
        self.assertTrue(button.is_on_screen)
        self.assertEqual(str(button.label), "Sane")
        self.assertFalse(button.busy)
        self.assertEqual(button.styles.content_align[0], "center")
        self.assertEqual(button.styles.background.hex.lower(), SANE_COLOR.lower())
        self.assertEqual(button.styles.color.hex.lower(), "#000000")

    async def test_continue_is_below_interrupted_activity_and_resumes(self):
        from sos_tui.config import MISC_MD
        value = state()
        value.update(status="interrupted", work_step=0)
        value["steps"][0]["whole"]["complete"] = False
        value["steps"][0]["substeps"][0].update(complete=False, started=True, diffs=[])
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        button = timeline.query_one(".implementation-continue")
        activity = timeline.query_one(SubstepActivity)
        self.assertEqual(str(button.label), "Continue")
        self.assertEqual(button.parent.children[-1], button)
        self.assertGreaterEqual(button.region.y, activity.region.bottom)
        self.assertEqual(button.styles.background.hex.lower(), MISC_MD.lower())
        self.assertEqual(button.styles.color.hex.lower(), "#ffffff")
        button.focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "implementation.resume")
        self.assertTrue(button.disabled)
        self.workspace.receive({"type": "request.failed", "requestId": request["requestId"], "error": "Retry later"})
        self.assertFalse(button.disabled)
        value["status"] = "running"
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertFalse(timeline.query(".implementation-continue"))
        self.workspace.archive = True
        value["status"] = "interrupted"
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertFalse(timeline.query(".implementation-continue"))

    async def test_resume_from_fresh_ui_restores_step_content_and_diffs(self):
        value = state()
        value.update(status="interrupted", work_step=1)
        value["steps"][1]["substeps"][0].update(complete=False, started=True, diffs=[],
            activity=[{"id": "working", "role": "assistant", "content": [
                {"type": "text", "text": "Preserved construction activity"}]}])
        # A fresh process has neither an attached branch nor construction state.
        self.app.session_id = None
        self.app.runtime_info.clear()
        self.workspace.session_id = None
        self.workspace.state = None
        self.app.client.send.reset_mock()
        await self.app.on_message_input_submitted(MessageInput.Submitted("/resume"))
        await self.pilot.pause()
        await self.app.send_resume_request("session.attach", "discussion")
        request = self.app.client.send.call_args.args[0]
        info = {"discussion_id": "discussion", "implementation_view": True,
            "implementation_execution": True, "active_implementation_id": "branch",
            "implementation_archive": False, "auto_plan_pending": False}
        common = {"requestId": request["requestId"], "sessionId": "branch", "messageCount": 0, "info": info}
        self.app.handle_event(dict(common, type="session.attach_started", page={}, session={
            "id": "branch", "discussion_id": "discussion", "workspace": self.app.workspace,
            "title": "Interrupted implementation", "mode": "sane"}))
        self.app.handle_event(dict(common, type="session.attached"))
        await self.app.workers.wait_for_complete()
        await self.pilot.pause()
        requests = [call.args[0] for call in self.app.client.send.call_args_list]
        self.assertTrue(any(item["type"] == "implementation.state" and item["sessionId"] == "branch" for item in requests))
        self.app.handle_event({"type": "implementation.updated", "sessionId": "branch", "implementation": value})
        await self.pilot.pause()
        self.assertTrue(self.workspace.is_on_screen)
        timeline = self.workspace.query_one(ImplementationStepView)
        self.assertEqual(timeline.step_index, 1)
        self.assertTrue(timeline.is_on_screen)
        self.assertEqual(timeline.rows[0][2].messages[0]["id"], "working")
        self.assertTrue(timeline.query_one(".implementation-continue").is_on_screen)
        self.workspace.selection = (0, -1)
        self.workspace.show_selection()
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.assertEqual(self.workspace.selection, (0, 0))
        self.assertTrue(self.workspace.zones)
        self.assertTrue(self.workspace.query_one(HistoricalCode).is_on_screen)

    async def test_reopening_interrupted_run_selects_current_step_and_exposes_continue(self):
        for status in ("interrupted", "failed"):
            with self.subTest(status=status):
                await self.workspace.attach(f"resume-{status}")
                value = state()
                value.update(status=status, work_step=1)
                value["steps"][1]["substeps"][0].update(complete=False, started=True, diffs=[])
                self.workspace.apply_state(value)
                await self.pilot.pause()
                self.assertEqual(self.workspace.selection, (1, -1))
                listing = self.workspace.query_one(OptionList)
                self.assertEqual(listing.highlighted, self.workspace.rows.index((1, -1)))
                timeline = self.workspace.query_one(ImplementationStepView)
                self.assertEqual(timeline.step_index, 1)
                button = timeline.query_one(".implementation-continue")
                self.assertTrue(button.display)
                self.assertEqual(str(button.label), "Continue")
                await self.pilot.click(button)
                await self.pilot.pause()
                self.assertEqual(self.app.client.send.call_args.args[0]["type"], "implementation.resume")

                # Reading a previous step must remain possible during state updates.
                self.workspace.selection = (0, -1)
                self.workspace.show_selection()
                value["error"] = "Updated diagnostic"
                self.workspace.apply_state(value)
                await self.pilot.pause()
                self.assertEqual(self.workspace.selection, (0, -1))

    async def test_reopening_archive_or_built_step_does_not_select_resume_target(self):
        for archive, complete in ((True, False), (False, True)):
            with self.subTest(archive=archive, complete=complete):
                await self.workspace.attach(f"inspect-{archive}-{complete}", archive=archive)
                value = state()
                value.update(status="interrupted", work_step=1)
                value["steps"][1]["whole"]["complete"] = complete
                self.workspace.apply_state(value)
                await self.pilot.pause()
                self.assertEqual(self.workspace.selection, (0, -1))

    async def test_chat_without_review_never_cancels_construction(self):
        value = state()
        value.update(status="running", work_step=1)
        self.workspace.apply_state(value)
        await self.pilot.press("enter", "tab")
        await self.pilot.pause()
        button = self.workspace.query_one("#implementation-review-agent-info").query_one(ModeButton)
        self.assertTrue(button.is_on_screen)
        self.assertEqual(str(button.label), "Sane")
        self.assertFalse(button.busy)
        with patch.object(self.app, "request_execution_cancel", new_callable=AsyncMock) as cancel:
            await self.workspace.escape()
            cancel.assert_not_awaited()

    async def test_step_cancel_requires_confirmation_and_becomes_continue(self):
        value = state()
        value.update(status="running", work_step=0)
        value["steps"][0]["whole"]["complete"] = False
        value["steps"][0]["substeps"][0].update(complete=False, started=True, diffs=[])
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        button = timeline.query_one(".implementation-cancel")
        activity = timeline.query_one(SubstepActivity)
        self.assertEqual(button.parent.children[-1], button)
        self.assertGreaterEqual(button.region.y, activity.region.bottom)
        self.assertFalse(timeline.query(".implementation-continue"))
        await self.pilot.click(button)
        await self.pilot.pause()
        self.assertIsInstance(self.app.screen, QuestionScreen)
        options = self.app.screen.query_one(OptionList)
        self.assertEqual(options.highlighted, 0)
        self.assertEqual(str(options.get_option_at_index(0).prompt), "Keep running")
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("escape")
        await self.pilot.pause()
        self.app.client.send.assert_not_awaited()
        await self.pilot.click(button)
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.app.client.send.assert_not_awaited()
        await self.pilot.click(button)
        await self.pilot.press("down", "enter")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "execution.cancel")
        value["status"] = "interrupted"
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertFalse(timeline.query(".implementation-cancel"))
        self.assertTrue(timeline.query_one(".implementation-continue").is_on_screen)

    async def test_step_navigation_does_not_render_twice_or_send_requests(self):
        with patch.object(self.workspace, "show_item_view", wraps=self.workspace.show_item_view) as render:
            await self.pilot.press("down")
            await self.pilot.pause()
            self.assertEqual(self.workspace.selection, (1, -1))
            render.assert_called_once()
            self.app.client.send.assert_not_awaited()

    async def test_step_overview_does_not_fingerprint_diffs_repository_or_tool_payloads(self):
        timeline = self.workspace.query_one(ImplementationStepView)
        value = copy.deepcopy(self.workspace.state)
        built = value["steps"][0]
        # These values cannot be JSON serialized and are irrelevant to the overview.
        built["repository_files"] = [object()]
        built["substeps"][0]["diffs"] = [object()]
        with patch.object(timeline, "update_step", wraps=timeline.update_step) as update:
            await timeline.show_step(0, value["plan"]["steps"][0], built, False, {},
                                     self.workspace.bullet_description)
            update.assert_not_called()
        built["substeps"][0]["activity"] = [{"role": "tool", "content": [
            {"type": "tool_result", "callId": "absent", "result": object()}]}]
        await timeline.show_step(0, value["plan"]["steps"][0], built, False, {}, self.workspace.bullet_description)
        self.assertEqual(len(timeline.rows[0][2].messages), 1)

    async def test_step_navigation_defers_collapsed_activity_details_until_opened(self):
        from sos_tui.implementation_step_view import ToolView
        value = state()
        messages = [{"role": "assistant", "content": [
            {"type": "tool_call", "callId": f"call-{i}", "toolName": "bash", "arguments": {"command": "true"}},
            {"type": "tool_result", "callId": f"call-{i}", "result": "done", "isError": False}]} for i in range(100)]
        for step in value["steps"]:
            step["substeps"][0]["activity"] = messages
            step["validation"] = {"activity": messages, "complete": True}
        with patch("sos_tui.implementation_step_view.ToolView", wraps=ToolView) as tool_views:
            self.workspace.apply_state(value)
            await self.pilot.pause()
            for index in (1, 0, 1):
                self.workspace.selection = (index, -1)
                self.workspace.show_selection()
                await self.pilot.pause()
                timeline = self.workspace.query_one(ImplementationStepView)
                self.assertEqual(timeline.step_index, index)
                self.assertEqual(timeline.rows[0][2].toggle.step_count, 100)
                self.assertEqual(timeline.validation_activity.toggle.step_count, 100)
                self.assertFalse(timeline.rows[0][2].body.children)
            tool_views.assert_not_called()
            activity = timeline.rows[0][2]
            activity.toggle.focus()
            await self.pilot.press("enter")
            await self.pilot.pause()
            self.assertTrue(activity.toggle.expanded)
            self.assertEqual(tool_views.call_count, 100)
            self.assertEqual(len(activity.body.children), 100)
            self.assertTrue(all(view.result is not None for view in activity.body.children))
            await self.pilot.press("enter", "enter")
            self.assertEqual(tool_views.call_count, 100)
            self.assertEqual(activity.toggle.step_count, 100)

    async def test_deferred_activity_keeps_counters_statistics_and_late_tool_results(self):
        messages = [{"role": "assistant", "model_call": {"model": "test-model", "reasoning": "high",
            "duration_ms": 1000, "output_tokens": 20}, "elapsed_ms": 500,
            "content": [{"type": "text", "text": "Checking"}, {"type": "reasoning", "text": "Think"},
                {"type": "tool_call", "toolName": "read", "callId": "read-1", "arguments": {"filePath": "a.py"}},
                {"type": "tool_call", "toolName": "skill", "callId": "skill-1", "arguments": {"name": "check"}},
                {"type": "tool_call", "toolName": "report_implementation_progress", "callId": "report", "arguments": {}}]}]
        value = state()
        value["steps"][0]["substeps"][0]["activity"] = messages
        self.workspace.apply_state(value)
        await self.pilot.pause()
        activity = self.workspace.query_one(ImplementationStepView).rows[0][2]
        self.assertEqual(activity.toggle.step_count, 4)
        self.assertTrue(activity.toggle.has_reasoning)
        self.assertTrue(activity.toggle.has_reads)
        self.assertTrue(activity.toggle.has_skills)
        self.assertFalse(activity.toggle.has_tools)
        self.assertEqual(activity.toggle.model_call_count, 1)
        self.assertEqual(activity.toggle.output_tokens, 20)
        self.assertFalse(activity.body.children)
        activity.toggle.focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.assertEqual(len(activity.body.children), 4)
        await self.pilot.press("enter")
        messages.append({"role": "tool", "elapsed_ms": 2500, "content": [
            {"type": "tool_result", "callId": "skill-1", "result": "Loaded", "isError": False}]})
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertEqual(activity.rendered_messages, 1)
        self.assertEqual(activity.toggle.duration_ms, 2000)
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.assertEqual(activity.rendered_messages, 2)
        self.assertEqual(activity.toggle.step_count, 4)
        self.assertEqual(activity.toggle.model_call_count, 1)
        self.assertEqual(len(activity.body.children), 4)
        self.assertNotIn("skill-1", activity.tools)
        self.assertEqual(activity.body.children[-1].result["result"], "Loaded")

    async def test_live_updates_preserve_activity_widgets_and_style_step_text(self):
        from sos_tui.config import MISC_MD, DEPTH_MD
        value = state()
        value.update(status="running", work_step=0)
        value["steps"][0]["whole"]["complete"] = False
        part = value["steps"][0]["substeps"][0]
        part.update(complete=False, started=True, announced=True, explanation="- Provisional description", diffs=[],
                    activity=[{"role": "assistant", "content": [{"type": "text", "text": "First message"}]}])
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        activity = timeline.query_one(SubstepActivity)
        activity.toggle.focus()
        await self.pilot.press("enter")
        part["activity"].append({"role": "assistant", "content": [{"type": "text", "text": "Second message"}]})
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertIs(timeline.query_one(SubstepActivity), activity)
        self.assertTrue(activity.toggle.expanded)
        self.assertIs(self.app.focused, activity.toggle)
        self.assertEqual(activity.toggle.step_count, 2)
        self.assertIn("1. State", str(timeline.query_one(".substep-title").render()))
        overview = timeline.query_one(".step-overview").render()
        self.assertTrue(any(span.style.foreground and span.style.foreground.hex.lower() == MISC_MD.lower() for span in overview.spans))
        button = timeline.query_one(".implementation-cancel")
        self.assertGreaterEqual(button.region.y, timeline.query_one(".substep-description").region.bottom)
        from sos_tui.config import DEPTH_MD
        self.assertEqual(button.styles.background.hex.lower(), DEPTH_MD.lower())

    async def test_unnecessary_substep_is_visible_and_struck_during_construction(self):
        value = state()
        value.update(status="running", work_step=0)
        value["steps"][0]["whole"]["complete"] = False
        value["steps"][0]["substeps"][0].update(complete=False, unnecessary=True, announced=False,
            explanation="Already implemented in the current code", diffs=[], activity=[])
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        title = timeline.query_one(".substep-title").render()
        self.assertIn("1. State", title.plain)
        self.assertTrue(any("strike" in str(span.style) for span in title.spans))
        self.assertIn("No longer necessary", str(timeline.query_one(".substep-description").render()))
        self.assertFalse(timeline.query_one(SubstepActivity).display)
        self.assertEqual(str(timeline.query_one(".step-progress-count").render()), "1/1")
        self.workspace.expanded.add(0)
        self.workspace.refresh_rows()
        self.assertNotIn((0, 0), self.workspace.rows)
        self.assertFalse(value["steps"][0]["substeps"][0]["complete"])

    async def test_validate_uses_blue_background_and_white_text(self):
        from sos_tui.config import DEPTH_MD
        await self.pilot.press("enter", "down")
        await self.pilot.pause()
        button = self.workspace.query_one("#implementation-validate")
        self.assertTrue(button.is_on_screen)
        self.assertEqual(button.styles.background.hex.lower(), DEPTH_MD.lower())
        self.assertEqual(button.styles.color.hex.lower(), "#ffffff")

    async def test_step_page_retains_textual_scroll_animation(self):
        value = state()
        value["steps"][0]["whole"]["explanation"] = "\n".join(f"- Detail {i}" for i in range(100))
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        self.assertGreater(timeline.max_scroll_y, 0)
        timeline.scroll_end(animate=True, speed=10000)
        await self.pilot.pause(.2)
        self.assertGreater(timeline.scroll_y, 0)

    async def test_review_previous_step_during_construction_retains_scroll_animation(self):
        from sos_tui.implementation_review_chat import ImplementationReviewChat
        value = state()
        value.update(status="running", work_step=1)
        self.workspace.apply_state(value)
        await self.pilot.press("enter", "tab")
        entry = self.workspace.query_one(ReviewInput)
        entry.text = "Explain step one"
        await self.pilot.press("enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "implementation.review")
        self.assertEqual(request["step"], 0)
        chat = self.workspace.query_one("#implementation-review-history", ImplementationReviewChat)
        await chat.show_records([{"role": "user", "content": [{"type": "text", "text": "Explain step one"}]},
            {"role": "assistant", "content": [{"type": "text", "text": "\n\n".join(f"Detail {i}" for i in range(100))}]}], False)
        await self.pilot.pause()
        self.assertGreater(chat.max_scroll_y, 0)
        chat.scroll_end(animate=True, speed=10000)
        await self.pilot.pause(.2)
        self.assertGreater(chat.scroll_y, 0)
        self.assertEqual(self.workspace.state["work_step"], 1)

    async def test_live_step_render_coalesces_pending_updates(self):
        import asyncio
        timeline = self.workspace.query_one(ImplementationStepView)
        entered, release = asyncio.Event(), asyncio.Event()
        rendered = []

        async def slow_render(step, plan, built, *args):
            rendered.append(built["whole"]["explanation"])
            entered.set()
            await release.wait()

        with patch.object(timeline, "show_step", side_effect=slow_render):
            first = state()
            first["steps"][0]["whole"]["explanation"] = "First"
            self.workspace.apply_state(first)
            await entered.wait()
            for i in range(20):
                value = state()
                value["steps"][0]["whole"]["explanation"] = f"Update {i}"
                self.workspace.apply_state(value)
            release.set()
            await self.pilot.pause()
            self.assertEqual(rendered[0], "First")
            self.assertTrue(rendered[1:])
            self.assertTrue(all(value == "Update 19" for value in rendered[1:]))
            self.assertFalse(self.workspace.step_rendering)

    async def test_live_step_announcements_show_activity_without_early_validation(self):
        value = state()
        value.update(status="running", work_step=0)
        built = value["steps"][0]
        built["whole"]["complete"] = False
        built["substeps"][0].update(complete=False, started=True, announced=True, explanation="- Provisional model", diffs=[],
            activity=[{"role": "assistant", "content": [{"type": "text", "text": "I am checking transitions"}]}])
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        activity = timeline.query_one(SubstepActivity)
        self.assertEqual(activity.toggle.step_count, 1)
        self.assertIsNotNone(activity.toggle.started_at)
        self.assertIn("Provisional model", str(timeline.query_one(".substep-description").render()))
        activity.toggle.focus()
        await self.pilot.press("enter")
        self.assertTrue(activity.toggle.expanded)
        self.workspace.expanded.add(0)
        self.workspace.refresh_rows()
        self.assertNotIn((0, 0), self.workspace.rows)
        self.assertEqual(self.app.client.send.await_count, 0)
        built["whole"]["complete"] = True
        built["substeps"][0]["complete"] = True
        built["substeps"][0]["diffs"] = state()["steps"][0]["substeps"][0]["diffs"]
        value["status"] = "waiting"
        self.workspace.apply_state(value)
        self.workspace.selection = (0, 1)
        self.workspace.refresh_rows()
        self.workspace.show_selection()
        await self.pilot.pause()
        self.assertFalse(timeline.display)
        self.assertFalse(self.workspace.query(SubstepActivity).first().is_on_screen)

    async def test_live_updates_preserve_selected_diff_options_and_tree_nodes(self):
        value = state()
        value.update(status="running", work_step=1)
        value["steps"][0].update(repository_root="/repo", repository_tree="baseline")
        self.workspace.apply_state(value)
        await self.reply_directory([{"name": "docs", "directory": True}])
        await self.pilot.press("enter", "right")
        listing = self.workspace.query_one(OptionList)
        option = listing.get_option_at_index(listing.highlighted)
        tree = self.workspace.query_one(ImplementationChangesTree)
        node = tree.files["tests/state.py"]
        tree.folders["docs"].expand()
        code = self.workspace.query_one(HistoricalCode)
        zone = code.zone
        with patch.object(listing, "clear_options", wraps=listing.clear_options) as clear, \
                patch.object(code, "show_zone", wraps=code.show_zone) as show:
            for update in range(8):
                value["steps"][1]["whole"]["explanation"] = f"Live update {update}"
                self.workspace.apply_state(copy.deepcopy(value))
                self.workspace.animate_spinner()
                await self.pilot.pause()
            clear.assert_not_called()
            show.assert_not_called()
        self.assertEqual(self.workspace.selection, (0, 0))
        self.assertEqual(self.workspace.diff_index, 2)
        self.assertIs(listing.get_option_at_index(listing.highlighted), option)
        self.assertIs(tree.files["tests/state.py"], node)
        self.assertTrue(tree.folders["docs"].is_expanded)
        self.assertIs(code.zone, zone)
        counter = self.workspace.query_one("#implementation-diff-count")
        self.assertEqual(counter.styles.content_align_horizontal, "left")

    async def test_enter_on_running_step_cancels_without_expanding(self):
        value = state()
        value.update(status="running", work_step=0)
        value["steps"][0]["whole"]["complete"] = False
        value["steps"][0]["substeps"][0].update(complete=False, started=True)
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        self.assertIsInstance(self.app.screen, QuestionScreen)
        self.assertNotIn(0, self.workspace.expanded)
        self.assertNotIn((0, 0), self.workspace.rows)
        await self.pilot.press("down", "enter")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "execution.cancel")

    async def test_future_step_does_not_expand_or_cancel(self):
        value = state()
        value.update(status="running", work_step=0)
        value["steps"][0]["whole"]["complete"] = False
        self.workspace.apply_state(value)
        await self.pilot.press("down", "enter")
        self.assertEqual(self.workspace.selection, (1, -1))
        self.assertNotIn(1, self.workspace.expanded)
        self.assertNotIsInstance(self.app.screen, QuestionScreen)
        self.app.client.send.assert_not_awaited()

    async def test_planned_substep_titles_are_visible_before_their_activity_starts(self):
        value = state()
        value.update(status="running", work_step=0)
        built = value["steps"][0]
        built["whole"]["complete"] = False
        built["substeps"][0].update(complete=False, started=True, diffs=[])
        for title in ("Wire the model", "Run the checks"):
            value["plan"]["steps"][0]["substeps"].append({"title": title})
            built["substeps"].append({"complete": False, "reviewed": False, "explanation": "", "diffs": []})
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        self.assertEqual([str(title.render()) for _, title, _, _ in timeline.rows],
                         ["1. State", "2. Wire the model", "3. Run the checks"])
        for row, _, activity, description in timeline.rows[1:]:
            self.assertTrue(row.display)
            self.assertFalse(activity.display)
            self.assertFalse(description.display)
        self.assertTrue(timeline.rows[0][2].display)
        self.assertEqual(str(timeline.progress_count.render()), "0/3")
        second_activity = timeline.rows[1][2]
        built["substeps"][0]["announced"] = True
        built["substeps"][1].update(started=True, activity=[
            {"role": "assistant", "content": [{"type": "text", "text": "Connecting the model"}]}])
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertIs(timeline.rows[1][2], second_activity)
        self.assertTrue(second_activity.display)
        self.assertFalse(timeline.rows[2][2].display)
        self.assertEqual(str(timeline.progress_count.render()), "0/3")
        built["substeps"][0]["finalized"] = True
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertEqual(str(timeline.progress_count.render()), "1/3")

    async def test_progress_between_overview_and_substeps_retains_finished_work_without_diffs(self):
        value = state()
        value.update(status="running", work_step=0)
        built = value["steps"][0]
        built["whole"]["complete"] = False
        part = built["substeps"][0]
        part.update(complete=False, started=True, diffs=[], activity=[
            {"role": "assistant", "content": [{"type": "tool_call", "callId": "edit-1",
                "toolName": "edit", "arguments": {"filePath": "state.py"}}]}])
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        progress = timeline.query_one(".step-progress-bar", Static)
        count = timeline.query_one(".step-progress-count")
        row = timeline.query_one(".substep-construction")
        activity = timeline.query_one(SubstepActivity)
        self.assertEqual(str(count.render()), "0/1")
        self.assertTrue(str(progress.render()).startswith("["))
        self.assertNotIn("#", str(progress.render()))
        self.assertIn("-", str(progress.render()))
        self.assertLess(progress.parent.region.width, timeline.region.width)
        self.assertGreaterEqual(count.region.y, timeline.overview.region.bottom)
        self.assertLess(count.region.y, row.region.y)
        self.assertLess(count.region.x, progress.region.x)
        part["announced"] = True
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertEqual(str(count.render()), "0/1")
        self.assertNotIn("#", str(progress.render()))
        self.assertIn("-", str(progress.render()))
        value["status"] = "waiting"
        built["whole"]["complete"] = part["complete"] = True
        part["explanation"] = "The edit was merged into the final implementation."
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertEqual(str(count.render()), "1/1")
        self.assertIn("#", str(progress.render()))
        self.assertTrue(row.display)
        self.assertIs(timeline.query_one(SubstepActivity), activity)
        self.assertIn("State", self.workspace.summary(0).plain)
        await self.pilot.press("enter")
        self.assertNotIn((0, 0), self.workspace.rows)
        self.assertEqual(self.workspace.selection, (0, -1))
        await self.pilot.press("down")
        self.assertEqual(self.workspace.selection, (0, 1))

    async def test_final_validation_collapses_step_and_selects_next_progress(self):
        await self.pilot.press("enter", "down", "enter", "enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        value = state()
        value["steps"][0]["whole"]["reviewed"] = True
        value["status"] = "running"
        value["work_step"] = 1
        value["steps"][1]["preparing"] = True
        with patch.object(self.workspace, "show_item_view", wraps=self.workspace.show_item_view) as show:
            self.workspace.receive({"type": "implementation.updated", "sessionId": "branch",
                "requestId": request["requestId"], "implementation": value})
            self.assertTrue(show.called)
            self.assertTrue(all(call.args[0] == 1 for call in show.call_args_list))
        await self.pilot.pause()
        self.assertEqual(self.workspace.selection, (1, -1))
        self.assertNotIn(0, self.workspace.expanded)
        self.assertTrue(self.workspace.query_one(ImplementationStepView).display)
        self.assertIn("Wire", str(self.workspace.query_one(ImplementationStepView).overview.render()))
        timeline = self.workspace.query_one(ImplementationStepView)
        self.assertTrue(timeline.preparation.display)
        self.assertEqual(len(timeline.rows), len(value["plan"]["steps"][1]["substeps"]))
        self.assertIsNone(self.workspace.validating_step)
        # Startup failure remains on the selected successor, with Continue available.
        value["status"] = "interrupted"
        value["error"] = "Preparation failed"
        value["steps"][1]["preparing"] = False
        self.workspace.receive({"type": "implementation.updated", "sessionId": "branch", "implementation": value})
        await self.pilot.pause()
        self.assertEqual(self.workspace.selection, (1, -1))
        self.assertFalse(timeline.preparation.display)

    async def test_final_validation_waits_for_import_and_blocks_double_submission(self):
        await self.pilot.press("enter", "down", "enter", "enter")
        await self.pilot.pause()
        self.assertEqual(self.workspace.selection, (0, 1))
        button = self.workspace.query_one("#implementation-validate", Button)
        self.assertTrue(button.disabled)
        self.assertEqual(str(button.label), "Applying changes...")
        self.assertFalse(self.workspace.state["steps"][0]["whole"]["reviewed"])
        calls = self.app.client.send.await_count
        self.workspace.validate_step(0)
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.await_count, calls)
        request = self.app.client.send.call_args.args[0]
        self.workspace.receive({"type": "request.failed", "requestId": request["requestId"], "error": "Import conflict"})
        await self.pilot.pause()
        self.assertEqual(self.workspace.selection, (0, 1))
        self.assertFalse(button.disabled)
        self.assertEqual(str(button.label), "Validate")

    async def test_final_validation_send_failure_restores_button(self):
        await self.pilot.press("enter", "down")
        with patch.object(self.app.client, "send", side_effect=OSError("Disconnected")):
            self.workspace.validating_step = 0
            self.workspace.command("implementation.validate", step=0, substep=1)
            await self.pilot.pause()
        self.assertIsNone(self.workspace.validating_step)
        self.assertFalse(self.workspace.pending_validation)
        self.assertEqual(self.workspace.selection, (0, 1))

    async def test_close_in_conclusion_is_focused_after_final_validation(self):
        value = state()
        value.update(status="completed", work_step=2)
        for step in value["steps"]:
            step["whole"]["complete"] = True
        self.workspace.apply_state(copy.deepcopy(value))
        self.assertFalse(self.workspace.query_one("#implementation-close").display)
        for step in value["steps"]:
            step["whole"]["reviewed"] = True
        self.workspace.apply_state(value)
        await self.pilot.pause()
        close = self.workspace.query_one("#implementation-close", Button)
        self.assertTrue(close.display)
        self.assertTrue(close.has_focus)
        self.assertTrue(self.workspace.conclusion_visible)
        document = self.workspace.query_one("#implementation-accepted-plan")
        self.assertEqual(close.parent, document)
        from sos_tui.response_markdown import ResponseMarkdown
        self.assertGreaterEqual(close.region.y, document.query_one(ResponseMarkdown).region.bottom)
        conclusion = self.workspace.query_one("#implementation-conclusion", Button)
        self.assertEqual(conclusion.region.y, self.workspace.query_one("#implementation-steps").region.bottom)
        await self.app.implementation.sync()
        self.assertTrue(close.has_focus)
        from sos_tui.config import DEPTH_MD
        self.assertEqual(close.styles.background.hex.lower(), DEPTH_MD.lower())
        with patch.object(self.app.implementation, "request", new_callable=AsyncMock) as request:
            await self.pilot.press("enter")
            await self.pilot.pause()
            request.assert_awaited_once_with("implementation.close")
        self.app.runtime_info.update(active_implementation_id="", implementation_view=False,
                                     implementation_execution=False, implementation_archive=False)
        await self.app.implementation.sync()
        self.assertFalse(self.app.query_one("#tabs").query("#implement, #plan"))

    async def test_conclusion_shows_global_recap_and_caveats_without_diffs_or_validation(self):
        self.assertTrue(self.workspace.query_one("#implementation-conclusion", Button).disabled)
        value = state()
        value.update(status="completed", work_step=2,
                     conclusion="Feature implemented.\n\n14 pre-existing failures reproduced on the baseline; targeted tests pass.")
        for step in value["steps"]:
            step["whole"]["complete"] = True
        self.workspace.apply_state(value)
        self.workspace.selection = (1, -1)
        self.workspace.navigate(1)
        await self.pilot.pause()
        self.assertTrue(self.workspace.conclusion_visible)
        self.assertTrue(self.workspace.query_one("#implementation-conclusion").has_focus)
        self.assertFalse(self.workspace.query_one("#implementation-close").display)
        await self.pilot.press("enter")
        self.assertTrue(self.workspace.conclusion_visible)
        from sos_tui.response_markdown import ResponseMarkdown
        document = self.workspace.query_one("#implementation-accepted-plan")
        self.assertIn("14 pre-existing failures", document.query_one(ResponseMarkdown)._markdown)
        self.assertFalse(self.workspace.query_one("#implementation-validate").display)
        self.assertFalse(self.workspace.query_one(ImplementationStepView).display)
        self.assertEqual(self.workspace.zones, [])
        self.workspace.apply_state(copy.deepcopy(value))
        self.assertTrue(self.workspace.conclusion_visible)
        self.workspace.navigate(-1)
        self.assertFalse(self.workspace.conclusion_visible)
        self.assertEqual(self.workspace.selection, (1, -1))
        self.assertTrue(self.workspace.query_one(ImplementationStepView).display)
        self.app.client.send.assert_not_awaited()

    async def test_archive_conclusion_falls_back_to_preserved_step_explanations(self):
        value = state()
        value.update(status="completed", work_step=2)
        for step in value["steps"]:
            step["whole"].update(complete=True, reviewed=True)
        self.workspace.archive = True
        self.workspace.apply_state(value)
        self.workspace.open_conclusion()
        from sos_tui.response_markdown import ResponseMarkdown
        text = self.workspace.query_one("#implementation-accepted-plan").query_one(ResponseMarkdown)._markdown
        self.assertIn("## Model", text)
        self.assertIn("## Wire", text)
        self.assertIn("A testable state model", text)
        self.assertFalse(self.workspace.query_one("#implementation-close").display)

    async def test_conclusion_between_last_step_and_close_remains_readable_until_archive(self):
        value = state()
        value.update(status="completed", work_step=2, conclusion="Delivered feature. Checks and caveats.")
        for step in value["steps"]:
            step["whole"].update(complete=True, reviewed=True)
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertTrue(self.workspace.query_one("#implementation-close").has_focus)
        await self.pilot.press("up")
        self.assertTrue(self.workspace.conclusion_visible)
        await self.pilot.press("down")
        self.assertTrue(self.workspace.query_one("#implementation-close").has_focus)
        with patch.object(self.app.implementation, "request", new_callable=AsyncMock) as request:
            await self.pilot.press("enter")
            await self.pilot.pause()
            request.assert_awaited_once_with("implementation.close")
        self.workspace.archive = True
        self.workspace.apply_state(value)
        self.assertTrue(self.workspace.conclusion_visible)
        value["steps"][-1]["whole"]["complete"] = False
        value.update(status="running", work_step=1)
        self.workspace.apply_state(value)
        self.assertFalse(self.workspace.conclusion_visible)
        self.assertTrue(self.workspace.query_one("#implementation-conclusion").disabled)

    async def test_final_validation_focuses_close_with_long_conclusion(self):
        value = state()
        value.update(status="completed", work_step=2,
                     conclusion="\n\n".join(f"Paragraph {i}. Checks and caveats." for i in range(80)))
        for step in value["steps"]:
            step["whole"]["complete"] = True
        value["steps"][0]["whole"]["reviewed"] = True
        self.workspace.apply_state(value)
        value["steps"][1]["whole"]["reviewed"] = True
        self.workspace.apply_state(value)
        self.workspace.advance_validated_step(1)
        await self.pilot.pause()
        close = self.workspace.query_one("#implementation-close", Button)
        self.assertTrue(self.workspace.conclusion_visible)
        self.assertTrue(close.has_focus)
        # Default focus is required, even when Close is below the viewport.
        await self.pilot.press("up", "up")
        self.assertFalse(self.workspace.conclusion_visible)
        self.assertFalse(close.display)

    async def test_future_step_has_no_validate_until_previous_is_reviewed(self):
        value = state()
        value.update(status="completed", work_step=2)
        value["steps"][1]["whole"]["complete"] = True
        value["steps"][1]["substeps"][0]["complete"] = True
        self.workspace.expanded.add(1)
        self.workspace.apply_state(value)
        self.assertNotIn((1, 1), self.workspace.rows)
        self.workspace.validate_step(1)
        await self.pilot.pause()
        self.app.client.send.assert_not_awaited()
        value["steps"][0]["whole"]["reviewed"] = True
        self.workspace.apply_state(value)
        self.assertIn((1, 1), self.workspace.rows)

    async def test_plan_chat_ignores_construction_messages_even_when_selected(self):
        await self.app.implementation.show_plan(True)
        with patch.object(self.app, "display_message") as display:
            self.app.display_live_messages([{"role": "assistant", "content": [{"type": "text", "text": "Building a step"}]}])
            display.assert_not_called()

    async def test_progress_width_and_misc_color_survive_step_switch(self):
        from sos_tui.config import MISC_MD
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        width = len(str(timeline.progress.render()))
        self.assertEqual(timeline.progress.styles.color.hex, MISC_MD)
        self.assertEqual(timeline.progress_count.styles.color.hex, MISC_MD)
        await self.pilot.press("down")
        await self.pilot.pause()
        self.assertEqual(len(str(timeline.progress.render())), width)

    async def test_live_state_refresh_does_not_redraw_an_unchanged_diff(self):
        value = state()
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        await self.pilot.pause()
        code = self.workspace.query_one(HistoricalCode)
        with patch.object(code, "show_zone", wraps=code.show_zone) as show_zone:
            self.workspace.apply_state(copy.deepcopy(value))
            await self.pilot.pause()
            show_zone.assert_not_called()

    async def test_step_menu_colors_follow_construction_and_validation_state(self):
        from sos_tui.config import ALGAE_MD, MISC_MD, MUTED_COLOR
        value = state()
        self.workspace.apply_state(value)
        self.assertEqual(str(self.workspace.step_prompt(0).style), "white")
        self.assertEqual(str(self.workspace.step_prompt(1).style), MUTED_COLOR)
        value.update(status="running", work_step=1)
        value["steps"][1]["substeps"][0].update(complete=False, started=True)
        self.workspace.apply_state(value)
        self.assertIn("[0/1]", self.workspace.step_prompt(1).plain)
        self.assertEqual(self.workspace.step_prompt(1).plain.splitlines()[1], "  [0/1] Connect... |")
        self.assertEqual(str(self.workspace.step_prompt(1).style), MISC_MD)
        value["steps"][0]["whole"]["reviewed"] = True
        value["steps"][0]["substeps"][0]["reviewed"] = True
        self.workspace.expanded.add(0)
        self.workspace.apply_state(value)
        self.assertEqual(str(self.workspace.step_prompt(0).style), ALGAE_MD)
        option = self.workspace.query_one(OptionList).get_option_at_index(1)
        self.assertNotIn("[x]", option.prompt.plain)
        self.assertNotIn("[ ]", option.prompt.plain)
        self.assertEqual(str(option.prompt.style), ALGAE_MD)

    async def test_reviewed_step_content_keeps_normal_colors_but_summary_is_algae(self):
        from sos_tui.config import ALGAE_MD
        value = state()
        value["steps"][0]["substeps"][0]["reviewed"] = True
        value["steps"][0]["whole"]["reviewed"] = True
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        title = timeline.rows[0][1].render()
        description = timeline.rows[0][3].render()
        for text in (title, description, timeline.overview.render()):
            self.assertFalse(any(ALGAE_MD.lower() in str(span.style).lower() for span in text.spans))
        self.assertIn(ALGAE_MD, str(self.workspace.summary(0).spans))

    async def test_review_suggestions_are_visible_and_take_priority_over_tab_and_escape(self):
        self.app.runtime_info["skills"] = [{"name": "zebra", "description": "Review skill"}]
        await self.pilot.press("slash")
        await self.pilot.pause()
        entry = self.workspace.query_one(ReviewInput)
        menu = entry.suggestions
        self.assertEqual(menu.matches[0], "/zebra")
        self.assertTrue(menu.visible)
        self.assertGreater(menu.region.height, 0)
        self.assertGreater(menu.region.width, 0)
        await self.pilot.press("tab")
        self.assertEqual(entry.text, "/zebra")
        self.assertTrue(self.workspace.chat_open)
        self.assertTrue(entry.has_focus)
        self.workspace.review_running = True
        self.app.client.send.reset_mock()
        await self.pilot.press("escape")
        self.assertFalse(menu.display)
        self.assertTrue(self.workspace.chat_open)
        self.assertEqual(entry.text, "/zebra")
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("escape")
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "implementation.review.cancel")

    async def test_slash_opens_review_and_offers_only_supported_native_commands(self):
        await self.pilot.press("slash")
        await self.pilot.pause()
        entry = self.workspace.query_one(ReviewInput)
        self.assertTrue(self.workspace.chat_open)
        self.assertTrue(entry.has_focus)
        self.assertEqual(entry.text, "/")
        self.assertEqual(set(entry.suggestions.matches), {"/connect", "/model", "/permissions", "/mcps"})
        await self.pilot.press("slash")
        self.assertEqual(entry.text, "//")
        entry.text = "Keep this draft"
        entry.move_cursor(entry.document.end)
        self.workspace.toggle_chat()
        await self.pilot.press("slash")
        self.assertEqual(entry.text, "Keep this draft/")
        self.assertTrue(entry.has_focus)

    async def test_review_native_commands_use_review_composer_and_reject_unsafe_commands(self):
        await self.pilot.press("slash")
        entry = self.workspace.query_one(ReviewInput)
        main = self.app.query_one("#message", MessageInput)
        main.text = "Preserve discussion draft"
        entry.text = "/undo"
        with patch.object(self.app, "notify") as notify:
            await self.pilot.press("enter")
            notify.assert_called()
        self.assertEqual(entry.text, "/undo")
        self.app.client.send.assert_not_awaited()
        entry.text = "/model"
        with patch.object(self.app, "open_settings_command", new_callable=AsyncMock) as settings:
            await self.pilot.press("enter")
            settings.assert_awaited_once_with("/model", entry)
        self.assertEqual(main.text, "Preserve discussion draft")

    async def test_review_skills_complete_and_send_raw_input_only_to_review(self):
        self.app.runtime_info["skills"] = [{"name": "brief", "description": "Be brief", "argument-hint": "message..."}]
        await self.pilot.press("slash", "b")
        entry = self.workspace.query_one(ReviewInput)
        self.assertIn("/brief", entry.suggestions.matches)
        entry.text = "/brief explain this"
        await self.pilot.press("enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "implementation.review")
        self.assertEqual(request["skills"], [{"name": "brief", "input": "explain this"}])
        self.assertEqual(entry.text, "")

    async def test_loading_spinner_stops_on_state_or_failure(self):
        self.workspace.session_id = ""
        await self.workspace.attach("branch")
        self.assertTrue(self.workspace.loading)
        self.assertFalse(self.workspace.query_one("#implementation-content").display)
        spinner = self.workspace.query_one("#implementation-loading", Static)
        old = str(spinner.render())
        self.workspace.animate_spinner()
        self.assertNotEqual(str(spinner.render()), old)
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.workspace.receive({"type": "request.failed", "requestId": request["requestId"], "error": "Cannot load"})
        self.assertFalse(self.workspace.loading)
        self.assertIn("Cannot load", str(self.workspace.query_one("#implementation-status").render()))
        self.workspace.show_loading(True)
        self.workspace.apply_state(state())
        self.assertFalse(spinner.display)
        self.assertTrue(self.workspace.query_one("#implementation-content").display)

    async def test_reopening_same_implementation_refreshes_state_and_shows_spinner_immediately(self):
        await self.app.implementation.request("implementation.view", "branch")
        self.assertTrue(self.workspace.loading)
        self.assertTrue(self.workspace.query_one("#implementation-loading").display)
        from sos_tui.history_loading_bar import HistoryLoadingBar
        self.assertFalse(any(bar.display for bar in self.app.query(HistoryLoadingBar)))
        self.app.implementation.finish()
        await self.workspace.attach("branch")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "implementation.state")
        self.workspace.receive({"type": "implementation.updated", "sessionId": "branch", "requestId": request["requestId"],
                                "implementation": state()})
        self.assertFalse(self.workspace.loading)

    async def test_long_step_menu_has_only_outer_scrollbar(self):
        from textual.containers import VerticalScroll
        value = state()
        value["plan"]["steps"] = [copy.deepcopy(value["plan"]["steps"][0]) for _ in range(40)]
        value["steps"] = [copy.deepcopy(value["steps"][0]) for _ in range(40)]
        self.workspace.apply_state(value)
        await self.pilot.pause()
        menu = self.workspace.query_one("#implementation-step-list", VerticalScroll)
        listing = self.workspace.query_one("#implementation-steps", OptionList)
        self.assertTrue(menu.show_vertical_scrollbar)
        self.assertFalse(listing.show_vertical_scrollbar)
        self.workspace.selection = (39, -1)
        self.workspace.show_selection()
        await self.pilot.pause()
        self.assertGreater(menu.scroll_y, 0)
        self.assertEqual(listing.scroll_y, 0)

    async def test_individual_validation_refreshes_sidebar_color_without_changing_title(self):
        from sos_tui.config import ALGAE_MD
        value = state()
        self.workspace.expanded.add(0)
        self.workspace.apply_state(value)
        listing = self.workspace.query_one(OptionList)
        before = listing.get_option_at_index(1).prompt.plain
        self.assertEqual(str(listing.get_option_at_index(1).prompt.style), "white")
        value = copy.deepcopy(value)
        value["steps"][0]["substeps"][0]["reviewed"] = True
        self.workspace.apply_state(value)
        self.assertFalse(value["steps"][0]["whole"]["reviewed"])
        self.assertEqual(listing.get_option_at_index(1).prompt.plain, before)
        self.assertEqual(str(listing.get_option_at_index(1).prompt.style), ALGAE_MD)

    async def test_short_context_window_extends_until_bottom_is_reachable(self):
        value = state()
        source = "\n".join(f"line {i}" for i in range(140))
        value["steps"][0]["substeps"][0]["diffs"] = [
            {"path": "file.txt", "after": source, "patch": "@@ -1 +1 @@\n-old\n+line 0"}]
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        await self.pilot.resize_terminal(120, 100)
        await self.pilot.pause()
        code = self.workspace.query_one(HistoricalCode)
        self.assertGreater(code.end, code.WINDOW)
        for _ in range(5):
            code.scroll_end(animate=False)
            await self.pilot.pause()
        self.assertEqual(code.end, len(code.zone.rows))

    async def test_large_diff_is_wrapped_once_at_the_visible_panes_width(self):
        value = state()
        source = "\n".join(f"line {i}" for i in range(200))
        value["steps"][0]["substeps"][0]["diffs"] = [
            {"path": "file.txt", "after": source, "patch": "@@ -60 +60 @@\n-old\n+line 59"}]
        self.workspace.apply_state(value)
        code = self.workspace.query_one(HistoricalCode)
        with patch.object(code, "render_window", wraps=code.render_window) as render:
            await self.pilot.press("enter")
            await self.pilot.pause()
            self.assertEqual(render.call_count, 1)
        self.assertEqual(code.rendered_width, code.content_size.width)
        self.assertGreater(code.scroll_y, 0)

    async def test_technical_validation_is_last_unnumbered_misc_activity_without_description(self):
        from sos_tui.config import MISC_MD
        value = state()
        value.update(status="running", work_step=0)
        built = value["steps"][0]
        built["whole"].update(complete=False, explanation="Global explanation only")
        built["substeps"][0].update(complete=False, started=True, announced=True)
        built["validation"] = {"started": True, "complete": False, "activity": [
            {"role": "assistant", "content": [{"type": "tool_call", "callId": "check",
                "toolName": "bash", "arguments": {"command": "run-tests"}}]}]}
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        row = timeline.query_one(".step-validation")
        title = row.query_one(".validation-title", Static).render()
        self.assertEqual(title.plain, "Validating the step")
        self.assertTrue(any(MISC_MD.lower() in str(span.style).lower() for span in title.spans))
        self.assertGreaterEqual(row.region.y, timeline.rows[-1][0].region.bottom)
        self.assertFalse(row.query(".substep-description"))
        self.assertIs(timeline.active, timeline.validation_activity)
        self.assertIsNone(timeline.rows[0][2].toggle.started_at)
        self.assertIn("Global explanation only", timeline.overview.render().plain)
        activity = timeline.validation_activity
        built["validation"]["activity"].append({"role": "tool", "content": [
            {"type": "tool_result", "callId": "check", "result": "Passed", "isError": False}]})
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertIs(timeline.validation_activity, activity)
        self.assertEqual(len(activity.messages), 2)

    async def test_human_validation_menu_and_summary_do_not_repeat_global_description(self):
        value = state()
        built = value["steps"][0]
        built["whole"]["explanation"] = "Global-only result"
        built["substeps"][0]["explanation"] = "Concrete substep and checks"
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        listing = self.workspace.query_one(OptionList)
        self.assertEqual(listing.get_option_at_index(2).prompt.plain, "  Human validation")
        self.workspace.selection = (0, 1)
        self.workspace.show_selection()
        await self.pilot.pause()
        text = self.workspace.query_one(HistoricalCode).query_one(Static).render().plain
        self.assertIn("Concrete substep and checks", text)
        self.assertNotIn("Global-only result", text)
        self.assertIn("Global-only result", self.workspace.summary(0).plain)

    async def test_human_validation_titles_stay_coral_and_only_validated_content_turns_green(self):
        from sos_tui.config import ALGAE_MD, BOLD_MD
        from rich.style import Style
        value = state()
        plan, built = value["plan"]["steps"][0], value["steps"][0]
        built["substeps"][0]["explanation"] = "Concrete substep and checks"
        for reviewed in (False, True):
            built["whole"]["reviewed"] = reviewed
            built["substeps"][0]["reviewed"] = reviewed
            self.workspace.apply_state(value)
            text = self.workspace.summary(0, include_overview=False)
            first_title = "1. " + plan["substeps"][0]["title"]
            self.assertTrue(text.plain.startswith(plan["title"] + "\n\n" + first_title + "\n"))
            self.assertNotIn("─", text.plain[:text.plain.index(first_title)])
            for title in (plan["title"], first_title):
                style = text.get_style_at_offset(self.app.console, text.plain.index(title))
                self.assertEqual(style.color, Style(color=BOLD_MD).color)
            content = text.get_style_at_offset(self.app.console, text.plain.index("Concrete substep"))
            self.assertEqual(content.color, Style(color=ALGAE_MD).color if reviewed else None)

    async def test_full_progress_reflects_during_validation_and_stops_when_finished(self):
        value = state()
        value.update(status="running", work_step=0)
        built = value["steps"][0]
        built["whole"]["complete"] = False
        self.workspace.apply_state(value)
        await self.pilot.pause()
        timeline = self.workspace.query_one(ImplementationStepView)
        self.assertNotIn("-", timeline.progress.render().plain)
        self.assertFalse(timeline.progress.validating)

        built["validation"] = {"started": True, "complete": False, "activity": []}
        # A targeted repair may temporarily clear a substep's final completion.
        built["substeps"][0]["complete"] = False
        self.workspace.apply_state(value)
        await self.pilot.pause()
        progress = timeline.progress
        self.assertTrue(progress.validating)
        self.assertEqual(progress.size.height, 2)
        self.assertEqual(progress.parent.size.height, 2)
        self.assertEqual(timeline.progress_count.region.y, progress.region.y + 1)
        self.assertEqual(str(timeline.progress_count.render()), "1/1")
        rendered = progress.render()
        self.assertEqual(rendered.plain.splitlines()[0].strip(), "Validating")
        self.assertNotIn("-", rendered.plain.splitlines()[1])
        started = progress.started_at
        with patch("sos_tui.implementation_step_view.monotonic", return_value=started + .75):
            middle = progress.render()
        with patch("sos_tui.implementation_step_view.monotonic", return_value=started + 1.05):
            later = progress.render()
        self.assertEqual(middle.plain, later.plain)
        self.assertNotEqual(middle.spans, later.spans)
        await self.pilot.resize_terminal(95, 35)
        await self.pilot.pause()
        resized = progress.render().plain.splitlines()
        self.assertEqual(len(resized[0]), len(resized[1]))
        self.assertEqual(len(resized[1]), progress.size.width)
        self.assertNotIn("-", resized[1])
        self.assertEqual(progress.started_at, started)

        value["status"] = "interrupted"
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertFalse(progress.validating)
        self.assertFalse(progress.timer._active.is_set())
        self.assertNotIn("-", progress.render().plain)
        value["status"] = "running"
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertTrue(progress.validating)
        built["whole"]["complete"] = True
        built["validation"]["complete"] = True
        value["status"] = "waiting"
        self.workspace.apply_state(value)
        await self.pilot.pause()
        self.assertFalse(progress.validating)
        self.assertFalse(progress.timer._active.is_set())
        self.assertEqual(progress.size.height, 1)
        self.assertNotIn("Validating", progress.render().plain)
        self.assertNotIn("-", progress.render().plain)

    async def test_large_diff_opening_in_tall_pane_does_not_load_in_successive_frames(self):
        value = state()
        source = "\n".join(f"line {i}" for i in range(3000))
        value["steps"][0]["substeps"][0]["diffs"] = [
            {"path": "file.txt", "after": source, "patch": "@@ -1 +1 @@\n-old\n+line 0"}]
        await self.pilot.resize_terminal(120, 100)
        self.workspace.apply_state(value)
        await self.pilot.pause()
        code = self.workspace.query_one(HistoricalCode)
        with patch.object(code, "render_window", wraps=code.render_window) as render:
            await self.pilot.press("enter")
            await self.pilot.pause()
            self.assertEqual(render.call_count, 1)
        self.assertGreater(code.max_scroll_y, 5)

    async def test_substep_option_keeps_indentation_on_every_wrapped_line(self):
        value = state()
        value["plan"]["steps"][0]["substeps"][0]["title"] = "A long substep title " * 8
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        from textual.visual import visualize, Visual
        listing = self.workspace.query_one(OptionList)
        prompt = listing.get_option_at_index(1).prompt
        visual = visualize(listing, prompt)
        lines = Visual.to_strips(listing, visual, 22, None, listing.visual_style)
        self.assertGreater(len(lines), 2)
        self.assertTrue(all(line.text.startswith("  ") for line in lines))
        self.assertTrue(all(line.cell_length <= 22 for line in lines))

    async def test_diff_position_is_set_before_the_first_refresh(self):
        from sos_tui.implementation_code import ChangeZone
        await self.pilot.press("enter")
        await self.pilot.pause()
        code = self.workspace.query_one(HistoricalCode)
        zone = ChangeZone("file.txt", [(i + 1, f"line {i}", "") for i in range(240)], 120, "file.txt:120")
        code.show_zone(zone)
        self.assertGreater(code.scroll_y, 0)
        self.assertEqual(code.scroll_y, code.document.offsets[zone.center])

    async def test_wrapped_code_preserves_syntax_and_scrolls_past_window_edges(self):
        from sos_tui.code_colors import ADDED_BACKGROUND
        from sos_tui.config import MISC_MD
        value = state()
        source = "\n".join("def function_name(): return " + str(i) + " # " + "detail " * 25 for i in range(160))
        diff = {"path": "model.py", "after": source, "patch": "@@ -60 +60 @@\n-old\n+" + source.splitlines()[59]}
        value["steps"][0]["substeps"][0]["diffs"] = [diff]
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        await self.pilot.pause()
        code = self.workspace.query_one(HistoricalCode)
        rendered = code.query_one(Static).render()
        self.assertEqual(code.max_scroll_x, 0)
        self.assertGreater(code.offsets[1], 1)
        self.assertTrue(any(span.style.foreground and span.style.foreground.hex == MISC_MD for span in rendered.spans))
        self.assertTrue(any(span.style.background and span.style.background.hex == ADDED_BACKGROUND for span in rendered.spans))
        self.assertEqual(code.styles.padding.left, 0)
        end = code.end
        code.scroll_end(animate=False)
        await self.pilot.pause()
        self.assertGreater(code.end, end)
        await self.pilot.resize_terminal(85, 25)
        await self.pilot.pause()
        self.assertEqual(code.max_scroll_x, 0)

    async def test_diff_explanation_defaults_open_and_toggles_without_capturing_review_typing(self):
        value = state()
        value["steps"][0]["substeps"][0]["diffs"][0]["hunk_explanations"] = ["Explain the model change"]
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        await self.pilot.pause()
        panel = self.workspace.query_one("#implementation-explanation-panel")
        text = self.workspace.query_one("#implementation-explanation", Static)
        self.assertTrue(panel.display)
        self.assertEqual(text.render().plain, "Explain the model change")
        self.assertLessEqual(panel.size.height, 6)
        self.assertLess(panel.region.y, self.workspace.query_one(HistoricalCode).region.y)
        hint = self.workspace.query_one("#implementation-explanation-hint", Static)
        filename = self.workspace.query_one("#implementation-file")
        header = self.workspace.query_one("#implementation-diff-header")
        self.assertTrue(hint.display)
        self.assertEqual(hint.render().plain, "E Toggle explanation")
        self.assertEqual(hint.region.y, filename.region.y)
        self.assertEqual(hint.region.right, header.content_region.right)
        self.assertLessEqual(filename.region.right, hint.region.x)
        self.assertNotIn("Explanation", self.workspace.query_one("#implementation-hint", Static).render().plain)
        await self.pilot.press("e")
        self.assertFalse(panel.display)
        await self.pilot.press("E")
        self.assertTrue(panel.display)
        await self.pilot.press("tab", "e")
        self.assertTrue(self.workspace.chat_open)
        self.assertTrue(panel.display)
        self.assertEqual(self.workspace.query_one(ReviewInput).text, "e")
        self.assertFalse(hint.display)

    async def test_diff_explanation_tracks_navigation_and_is_hidden_on_summary_and_plan(self):
        value = state()
        part = value["steps"][0]["substeps"][0]
        source = "\n".join(f"line {i}" for i in range(30))
        part["diffs"] = [{"path": "file.txt", "after": source,
                          "patch": "@@ -2 +2 @@\n-old\n+line 1\n@@ -20 +20 @@\n-old\n+line 19",
                          "hunk_explanations": ["First explanation", "Second explanation"]}]
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        await self.pilot.pause()
        panel = self.workspace.query_one("#implementation-explanation-panel")
        text = self.workspace.query_one("#implementation-explanation", Static)
        self.assertEqual(text.render().plain, "First explanation")
        await self.pilot.press("right")
        self.assertEqual(text.render().plain, "Second explanation")
        part["diffs"][0]["hunk_explanations"][1] = "Complete revised second explanation"
        self.workspace.apply_state(value)
        self.assertEqual(text.render().plain, "Complete revised second explanation")
        self.workspace.selection = (0, -1)
        self.workspace.show_selection()
        self.assertFalse(panel.display)
        self.assertFalse(self.workspace.query_one("#implementation-diff-header").display)
        self.workspace.inspect_plan(True)
        self.assertFalse(panel.display)
        self.assertFalse(self.workspace.query_one("#implementation-diff-header").display)

    async def test_diff_header_keeps_hint_on_the_right_when_filepath_is_long_and_resized(self):
        value = state()
        path = "src/" + "long-directory/" * 12 + "implementation.py"
        value["steps"][0]["substeps"][0]["diffs"][0]["path"] = path
        self.workspace.apply_state(value)
        await self.pilot.press("enter")
        for width in (120, 90):
            await self.pilot.resize_terminal(width, 35)
            await self.pilot.pause()
            filename = self.workspace.query_one("#implementation-file")
            hint = self.workspace.query_one("#implementation-explanation-hint", Static)
            header = self.workspace.query_one("#implementation-diff-header")
            self.assertEqual(header.size.height, 1)
            self.assertEqual(hint.region.y, filename.region.y)
            self.assertEqual(hint.region.right, header.content_region.right)
            self.assertLessEqual(filename.region.right, hint.region.x)
            self.assertIn("…", filename.render().plain)
            self.assertTrue(filename.render().plain.endswith("implementation.py"))

    async def test_historical_diffs_without_explanations_have_an_explicit_fallback(self):
        self.workspace.apply_state(state())
        await self.pilot.press("enter")
        self.assertTrue(self.workspace.query_one("#implementation-explanation-panel").display)
        self.assertEqual(self.workspace.query_one("#implementation-explanation", Static).render().plain,
                         "No explanation available. This change may come from a terminal command.")
