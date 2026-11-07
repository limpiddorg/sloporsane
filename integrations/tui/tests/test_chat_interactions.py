"""Focus safety and avoiding redundant work in the chat's interactive widgets."""

from contextlib import ExitStack
import unittest
from unittest.mock import AsyncMock, patch

from textual._xterm_parser import XTermParser
from textual.color import Color
from textual.widgets import Button, Input
from textual.screen import ModalScreen

from sos_tui.agent_info_bar import AgentInfoBar
from sos_tui.app import SosApp
from sos_tui.config import CONTENT_BACKGROUND_COLOR, SANE_COLOR
from sos_tui.chat_collapsible import ChatCollapsible
from sos_tui.history_detail import HistoryReasoning, HistoryToolView
from sos_tui.history_viewport import HistoryCompaction, HistoryViewport
from sos_tui.message_input import MessageInput
from sos_tui.permission_card import PermissionCard
from sos_tui.response_markdown import ResponseMarkdown
from sos_tui.user_message import UserMessage
from sos_tui.mode_button import ModeButton
from sos_tui.objective_plan import ObjectivePlan
from sos_tui.implementation_plan import ImplementationPlan
from sos_tui.question_screen import QuestionScreen
from sos_tui.tool_view import ToolView
from sos_tui.turn_activity import TurnActivity


class ChatInteractionsTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 24)))
        self.app.connected = True
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.viewport = self.app.query_one(HistoryViewport)
        await self.pilot.pause()

    async def until(self, predicate):
        for _ in range(50):
            if predicate():
                return
            await self.pilot.pause(0.01)
        self.fail("Timed out waiting for the UI")

    async def test_sane_control_keeps_draft_without_triggering_plan_generation(self):
        self.entry.text = "Unsent notes"
        await self.pilot.click(self.app.query_one(ModeButton))
        self.app.client.send.assert_not_awaited()
        self.app.update_agent_info({"objective_plan_ready": True})
        self.assertEqual(str(self.app.query_one(ModeButton).label), "Sane")
        await self.pilot.click(self.app.query_one(ModeButton))
        self.app.client.send.assert_not_awaited()
        self.assertEqual(self.entry.text, "Unsent notes")

    async def attach_implementation(self, session_id, *, archive=False):
        request = self.app.client.send.call_args.args[0]
        info = {"discussion_id": "discussion", "implementation_view": session_id != "discussion",
                "implementation_archive": archive and session_id != "discussion", "active_implementation_id": "" if archive else "branch",
                "objective_plan_ready": False, "implementation_plan_ready": False, "implementation_branches": [
                    {"id": "branch", "objective_id": "objective", "frozen": archive}]}
        session = {"id": session_id, "discussion_id": "discussion" if session_id != "discussion" else "",
                   "mode": "sane", "workspace": self.app.workspace, "title": "Test"}
        common = {"requestId": request["requestId"], "sessionId": session_id, "messageCount": 0, "info": info}
        self.app.handle_event(dict(common, type="session.attach_started", session=session, page={}))
        self.app.handle_event(dict(common, type="session.attached"))
        await self.until(lambda: self.app.implementation.request_id is None)
        await self.pilot.pause()

    async def test_implementation_tabs_keep_drafts_and_archive_hides_composer(self):
        self.app.session_id = "discussion"
        self.app.update_agent_info({"discussion_id": "discussion", "objective_plan_ready": True})
        self.entry.text = "Discussion draft"
        await self.app.implementation.request("implementation.open")
        await self.attach_implementation("branch")
        self.assertEqual(self.app.query_one("#tabs").active, "implement")
        self.assertEqual(self.entry.text, "")
        generated = self.app.client.send.call_args.args[0]
        self.assertEqual(generated["type"], "message.send")
        self.assertTrue(generated["implementation_request"])
        self.assertEqual(generated["text"], "Implementation plan request")
        self.app.handle_event({"type": "execution.cancelled", "sessionId": "branch"})
        await self.pilot.pause()
        self.assertEqual(str(self.app.query_one(ModeButton).label), "Sane")
        self.app.action_toggle_mode()
        self.assertEqual(self.app.mode, "sane")
        self.entry.text = "Implementation draft"
        await self.app.implementation.request("implementation.view")
        await self.attach_implementation("discussion")
        self.assertEqual(self.entry.text, "Discussion draft")
        self.assertEqual(self.app.query_one("#tabs").active, "discussion")
        await self.app.implementation.request("implementation.view", "branch")
        await self.attach_implementation("branch", archive=True)
        self.assertFalse(self.app.query_one("#composer").display)
        self.assertTrue(self.entry.disabled)
        self.assertIs(self.app.focused, self.viewport)
        self.assertTrue(self.app.query("#implement"))
        self.assertFalse(self.app.query("#plan"))
        await self.pilot.press("escape")
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "implementation.view")
        self.assertEqual(self.app.client.send.call_args.args[0]["branchId"], "")
        await self.attach_implementation("discussion", archive=True)

    async def test_direct_plan_restore_opens_steps_and_sends_auto_plan_with_chosen_pace(self):
        from sos_tui.implementation_workspace import ImplementationWorkspace
        self.app.session_id = "discussion"
        self.app.update_agent_info({"discussion_id": "discussion", "objective_plan_ready": True})
        self.entry.text = "Keep discussion draft"
        await self.app.implementation.request("implementation.open", objective_id="objective", auto_plan_pace="everything")
        await self.attach_implementation("branch")
        await self.until(lambda: self.app.client.send.call_args.args[0]["type"] == "implementation.auto_plan")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["pace"], "everything")
        self.assertEqual(request["sessionId"], "branch")
        self.assertTrue(self.app.query_one(ImplementationWorkspace).display)
        self.assertTrue(self.app.query_one("#implementation-auto-spinner").display)
        self.assertFalse(self.app.query_one("#discussion-body").display)
        self.assertEqual(self.app.implementation.drafts["discussion"][0], "Keep discussion draft")
        self.assertFalse(self.app.query(ImplementationPlan))
        self.assertFalse(any(call.args[0]["type"] == "message.send" for call in self.app.client.send.await_args_list))

    async def test_implementation_plan_is_only_on_right_until_accepted_and_implement_is_below_it(self):
        self.app.session_id = "branch"
        self.app.update_agent_info({"discussion_id": "discussion", "implementation_view": True,
                                    "active_implementation_id": "branch", "implementation_plan_ready": False})
        await self.pilot.pause()
        self.entry.text = "Unsent implementation notes"
        await self.pilot.click(self.app.query_one(ModeButton))
        self.app.client.send.assert_not_awaited()
        self.assertEqual(self.entry.text, "Unsent implementation notes")
        message = {"id": "implementation", "role": "assistant", "implementation_plan": {"steps": [
            {"title": "Foundation", "objective": "A testable model", "substeps": [
                {"title": "State", "intention": "Represent transitions", "changes": "Define transition rules"}],
             "validation": ["Run transition tests"]}]},
            "content": [{"type": "text", "text": "## Step 1: Foundation\n\n### 1.1 State\n\nRepresent transitions"}]}
        self.app.handle_event({"type": "execution.completed", "sessionId": "branch", "messages": [message],
                               "info": {"implementation_plan_ready": True, "implementation_plan": message}})
        await self.pilot.pause()
        self.assertEqual(str(self.app.query_one(ModeButton).label), "Sane")
        plan = self.app.query_one(ImplementationPlan)
        self.assertEqual(plan.query_one(".plan-heading").render(), "Implementation Plan")
        self.assertEqual(plan.styles.border_left[0], "outer")
        self.assertEqual(plan.styles.border_left[1], Color.parse("#A675DB"))
        self.assertFalse(plan.display)
        self.assertEqual(str(plan.query_one(Button).label), "Implementation")
        await self.viewport.replace({"session": {"id": "branch", "mode": "sane"}, "page": {}, "messages": [message]})
        self.assertEqual(self.app.query_one(ImplementationPlan).query_one(".plan-heading").render(), "Implementation Plan")
        self.assertFalse(self.app.query_one(ImplementationPlan).display)
        await self.pilot.click("#implementation-plan-start")
        from sos_tui.implementation_workspace import ImplementationModeScreen
        self.assertIsInstance(self.app.screen, ImplementationModeScreen)
        await self.pilot.press("escape")
        self.app.client.send.assert_not_awaited()
        self.app.handle_event({"type": "execution.completed", "sessionId": "branch", "info": {"implementation_plan_ready": False}})
        await self.pilot.pause()
        self.assertEqual(str(self.app.query_one(ModeButton).label), "Sane")

    async def test_crucial_choice_uses_modal_and_reply_not_user_message(self):
        self.app.session_id = "s"
        self.app.handle_event({"type": "question.requested", "sessionId": "s", "questionId": "q1",
                               "question": "Which approach?", "options": ["Small", "Large"]})
        await self.until(lambda: isinstance(self.app.screen, QuestionScreen))
        await self.pilot.press("down", "enter")
        await self.until(lambda: self.app.client.send.await_count == 1)
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "question.reply")
        self.assertEqual(self.app.client.send.call_args.args[0]["choice"], 1)
        self.assertEqual(self.entry.text, "")

    async def test_dismissing_choice_rejects_question_without_cancelling_turn(self):
        self.app.session_id = "s"
        self.app.busy = True
        self.app.handle_event({"type": "question.requested", "sessionId": "s", "questionId": "q2",
                               "question": "Which approach?", "options": ["A", "B"]})
        await self.until(lambda: isinstance(self.app.screen, QuestionScreen))
        await self.pilot.press("escape")
        await self.until(lambda: self.app.client.send.await_count == 1)
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "question.reject")
        self.assertEqual(self.app.client.send.call_args.args[0]["questionId"], "q2")
        self.assertIsNone(self.app.cancel_request_id)

    async def test_restored_plan_is_distinct_and_restores_implement(self):
        self.app.session_id = "s"
        self.app.update_agent_info({"objective_plan_ready": True})
        await self.viewport.replace({"session": {"id": "s", "mode": "sane"},
            "page": {"first": "u", "last": "plan", "hasOlder": False, "hasNewer": False},
            "messages": [
                {"id": "u", "role": "user", "metadata": {"mode": "sane", "sane_request": True},
                 "content": [{"type": "text", "text": "Sane Implementation request"}]},
                {"id": "middle", "role": "assistant", "content": [{"type": "text", "text": "Checking scope"}]},
                {"id": "plan", "role": "assistant", "objective_plan": True,
                 "objective_stack": "- Textual: terminal UI",
                 "content": [{"type": "text", "text": "Build a status page."}]},
            ]})
        await self.pilot.pause()
        self.assertEqual(str(self.app.query_one(ModeButton).label), "Sane")
        self.assert_sane_request_style(self.app.query_one(UserMessage))
        plan = self.app.query_one(ObjectivePlan)
        self.assertEqual(plan.query_one(".plan-heading").render(), "Objective Plan")
        self.assertEqual(plan.query_one(".stack-heading").render(), "Stack")
        self.assertEqual([str(markdown._markdown) for markdown in plan.query(ResponseMarkdown)],
                         ["Build a status page.", "- Textual: terminal UI"])
        self.assertEqual(plan.styles.margin.left, 0)
        self.assertEqual(plan.styles.border_left[0], "outer")
        self.assertEqual(plan.query_one(".stack-heading").styles.margin.top, 1)

    async def test_live_plan_renders_separate_stack(self):
        self.app.display_message({"id": "plan", "role": "assistant", "objective_plan": True,
            "objective_stack": "- PyTorch: model training",
            "content": [{"type": "text", "text": "Train and validate the model."}]})
        await self.pilot.pause()
        plan = self.app.query_one(ObjectivePlan)
        self.assertEqual([str(markdown._markdown) for markdown in plan.query(ResponseMarkdown)],
                         ["Train and validate the model.", "- PyTorch: model training"])
        self.assertEqual(plan.query_one(".stack-heading").render(), "Stack")

    async def test_plan_without_stack_hides_stack_heading_live_and_on_reload(self):
        message = {"id": "plan", "role": "assistant", "objective_plan": True,
                   "content": [{"type": "text", "text": "Build a status page."}]}
        self.app.display_message(message)
        await self.pilot.pause()
        self.assertEqual(len(self.app.query(ObjectivePlan)), 1)
        self.assertFalse(self.app.query_one(ObjectivePlan).query(".stack-heading"))
        await self.viewport.replace({"session": {"id": "s", "mode": "sane"},
            "page": {"first": "plan", "last": "plan", "hasOlder": False, "hasNewer": False}, "messages": [message]})
        await self.pilot.pause()
        self.assertFalse(self.app.query_one(ObjectivePlan).query(".stack-heading"))

    def assert_sane_request_style(self, message):
        self.assertTrue(message.has_class("sane-request"))
        self.assertEqual(message.styles.background, Color.parse(CONTENT_BACKGROUND_COLOR))
        self.assertEqual(message.query_one(".mode-band").styles.display, "none")
        label = message.query_one(".user-text")
        self.assertEqual(label.styles.color, Color.parse(SANE_COLOR))
        self.assertTrue(label.styles.text_style.bold)

    async def long_history(self):
        await self.viewport.replace({"session": {"id": "s"}, "page": {}, "messages": [
            {"id": "u", "role": "user", "content": [{"type": "text", "text": "Question"}]},
            {"id": "a", "role": "assistant", "content": [{"type": "text", "text": "\n\n".join(
                f"Paragraph {i}." for i in range(60))}]},
        ]})
        self.viewport.scroll_home(animate=False, immediate=True)
        await self.pilot.pause()

    def request_permission(self):
        self.app.busy = True
        self.app.handle_event({"type": "permission.requested", "permissionId": "p", "callId": "c",
                               "permission": "edit", "tool": "write", "patterns": ["file.txt"],
                               "details": {"filepath": "file.txt", "diff": "-old\n+new"}})
        return self.app.permission_card

    async def terminal_input(self, sequence):
        parser = XTermParser()
        for char in sequence:
            for event in parser.feed(char):
                self.app.post_message(event)
        await self.pilot.pause()

    async def test_app_blur_does_not_refocus_until_terminal_returns(self):
        await self.long_history()
        self.entry.text = "draft"
        self.entry.move_cursor((0, 2))
        position = self.viewport.scroll_y
        await self.terminal_input("\x1b[O")
        self.assertFalse(self.app.app_focus)
        self.assertIsNone(self.app.focused)
        await self.terminal_input("\x1b[I")
        self.assertTrue(self.entry.has_focus)
        self.assertEqual(self.entry.cursor_location, (0, 2))
        self.assertEqual(self.viewport.scroll_y, position)
        await self.pilot.press("x")
        self.assertEqual(self.entry.text, "drxaft")

    async def test_app_focus_recovers_missing_focus_and_accepts_zed_line_feed(self):
        # No widget was focused when the window lost focus, so Textual has no
        # previous widget to restore. The app must choose the composer itself.
        with patch.object(self.app, "focus_composer"):
            self.app.set_focus(None)
            await self.terminal_input("\x1b[O")
        self.assertIsNone(self.app.focused)
        await self.terminal_input("\x1b[Ifirst\nsecond")
        self.assertTrue(self.entry.has_focus)
        self.assertEqual(self.entry.text, "first\nsecond")
        self.app.client.send.assert_not_awaited()
        await self.terminal_input("\r")
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["text"], "first\nsecond")

    async def test_app_focus_preserves_permission_choice(self):
        card = self.request_permission()
        await self.until(card.owns_button_focus)
        await self.pilot.press("left")
        choice = card.query_one("#permission-no", Button)
        for _ in range(2):
            await self.terminal_input("\x1b[O")
            self.assertIsNone(self.app.focused)
            await self.terminal_input("\x1b[I")
            self.assertTrue(choice.has_focus)
        self.app.client.send.assert_not_awaited()

    async def test_app_focus_preserves_modal_input(self):
        class DraftDialog(ModalScreen):
            def compose(self):
                yield Input("modal draft")

        await self.app.push_screen(DraftDialog())
        entry = self.app.screen.query_one(Input)
        entry.focus()
        entry.cursor_position = 4
        await self.terminal_input("\x1b[O")
        await self.terminal_input("\x1b[I")
        self.assertTrue(entry.has_focus)
        self.assertEqual(entry.cursor_position, 4)
        self.assertIs(self.app.focused, entry)
        await self.pilot.press("x")
        self.assertEqual(entry.value, "modaxl draft")

    async def test_app_focus_does_not_reclaim_composer_during_loading_or_external_editor(self):
        for state in ("loading", "disabled", "editor"):
            with self.subTest(state=state):
                token = self.app.begin_history_loading() if state == "loading" else None
                self.entry.disabled = state != "editor"
                self.app.editing_external = state == "editor"
                with patch.object(self.app, "focus_composer"):
                    self.app.set_focus(None)
                    await self.terminal_input("\x1b[O")
                await self.terminal_input("\x1b[I")
                self.assertFalse(self.entry.has_focus)
                self.app.editing_external = False
                if token is not None:
                    self.app.end_history_loading(token)
                self.entry.disabled = False
                self.entry.focus()
                await self.pilot.pause()

    async def test_permission_is_visible_before_focus_and_enter_can_approve(self):
        await self.long_history()
        visibility_at_focus = []
        original = Button.focus
        def focus(button, *args, **kwargs):
            if button.id and button.id.startswith("permission-"):
                visibility_at_focus.append(button.screen.can_view_entire(button))
            return original(button, *args, **kwargs)
        with patch.object(Button, "focus", focus):
            card = self.request_permission()
            await self.until(card.owns_button_focus)
        self.assertTrue(visibility_at_focus)
        self.assertTrue(all(visibility_at_focus))
        self.assertGreater(self.viewport.scroll_y, 0)
        self.assertTrue(self.app.screen.can_view_entire(card.query_one("#permission-yes")))
        await self.pilot.press("enter")
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["decision"], "yes")

    async def test_offscreen_enter_reveals_permission_without_approving(self):
        await self.long_history()
        card = self.request_permission()
        await self.until(card.owns_button_focus)
        button = card.query_one("#permission-yes", Button)
        self.viewport.scroll_home(animate=False, immediate=True)
        await self.pilot.pause()
        self.assertTrue(button.has_focus)
        self.assertFalse(self.app.screen.can_view_entire(button))
        await self.pilot.press("enter")
        self.app.client.send.assert_not_awaited()
        self.assertFalse(card.waiting)
        await self.until(lambda: self.app.screen.can_view_entire(button))

    async def test_retry_reveals_the_previous_permission_choice(self):
        await self.long_history()
        card = self.request_permission()
        await self.until(card.owns_button_focus)
        await self.pilot.press("left", "enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["decision"], "no")
        self.viewport.scroll_home(animate=False, immediate=True)
        await self.pilot.pause()
        self.app.handle_event({"type": "request.failed", "requestId": self.app.permission_reply_id, "error": "Retry"})
        button = card.query_one("#permission-no", Button)
        await self.until(lambda: button.has_focus)
        self.assertTrue(self.app.screen.can_view_entire(button))
        self.assertFalse(card.waiting)

    async def test_dismissed_permission_does_not_take_focus_later(self):
        await self.long_history()
        self.request_permission()
        self.app.handle_event({"type": "permission.dismissed", "permissionId": "p"})
        await self.pilot.pause()
        self.assertEqual(len(self.app.query(PermissionCard)), 0)
        self.assertTrue(self.entry.has_focus)
        self.app.client.send.assert_not_awaited()

    async def test_permission_arrows_reclaim_focus_without_moving_draft_cursor(self):
        self.entry.text = "Next draft"
        self.entry.move_cursor((0, 4))
        card = self.request_permission()
        await self.until(card.owns_button_focus)
        for key, choice in (("left", "no"), ("right", "yes"), ("right", "always"),
                            ("right", "no"), ("right", "yes")):
            # Simulate a click back into the draft, or another widget stealing focus.
            self.entry.focus()
            await self.pilot.pause()
            cursor = self.entry.cursor_location
            await self.pilot.press(key)
            button = card.query_one("#permission-" + choice, Button)
            await self.until(lambda: button.has_focus)
            self.assertEqual(self.entry.text, "Next draft")
            self.assertEqual(self.entry.cursor_location, cursor)
            self.assertEqual(card.selected_button, button.id)
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("enter")
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["decision"], "yes")

    async def test_permission_arrow_reveals_choice_after_scrolling_away(self):
        await self.long_history()
        card = self.request_permission()
        await self.until(card.owns_button_focus)
        await self.pilot.press("right")
        self.entry.focus()
        self.viewport.scroll_home(animate=False, immediate=True)
        await self.pilot.pause()
        self.assertFalse(self.app.screen.can_view_entire(card.query_one("#permission-always")))
        await self.pilot.press("right")
        button = card.query_one("#permission-no", Button)
        await self.until(lambda: button.has_focus)
        self.assertTrue(self.app.screen.can_view_entire(button))
        self.app.client.send.assert_not_awaited()

    async def test_permission_arrows_override_another_history_widgets_bindings(self):
        drawer = ToolView("c", "bash", {})
        await self.viewport.mount(drawer)
        card = self.request_permission()
        await self.until(card.owns_button_focus)
        with patch.object(self.app, "focus_composer"):
            drawer.query_one("CollapsibleTitle").focus()
            await self.pilot.pause()
            await self.pilot.press("left")
            await self.until(lambda: card.query_one("#permission-no").has_focus)
        self.app.client.send.assert_not_awaited()

    async def test_waiting_permission_does_not_navigate_or_send_again(self):
        self.entry.text = "Draft"
        self.entry.move_cursor((0, 3))
        card = self.request_permission()
        await self.until(card.owns_button_focus)
        await self.pilot.press("right", "enter")
        self.assertTrue(card.waiting)
        self.entry.focus()
        await self.pilot.pause()
        cursor = self.entry.cursor_location
        await self.pilot.press("left", "right")
        self.assertEqual(card.selected_button, "permission-always")
        self.assertEqual(self.entry.cursor_location, cursor)
        self.app.client.send.assert_awaited_once()
        self.app.handle_event({"type": "request.failed", "requestId": self.app.permission_reply_id, "error": "Retry"})
        await self.until(card.owns_button_focus)
        self.entry.focus()
        await self.pilot.press("right")
        await self.until(lambda: card.query_one("#permission-no").has_focus)

    async def test_arrows_return_to_composer_after_permission_is_dismissed(self):
        self.entry.text = "Draft"
        self.entry.move_cursor((0, 3))
        card = self.request_permission()
        await self.until(card.owns_button_focus)
        self.app.handle_event({"type": "permission.dismissed", "permissionId": "p"})
        await self.until(lambda: self.entry.has_focus)
        await self.pilot.press("left")
        self.assertEqual(self.entry.cursor_location, (0, 2))
        await self.pilot.press("right")
        self.assertEqual(self.entry.cursor_location, (0, 3))
        self.app.client.send.assert_not_awaited()

    async def test_permission_does_not_intercept_typing_in_the_composer(self):
        card = self.request_permission()
        await self.until(card.owns_button_focus)
        self.entry.focus()
        await self.pilot.press("h", "i")
        self.assertEqual(self.entry.text, "hi")
        self.assertTrue(self.entry.has_focus)
        await self.pilot.press("right")
        await self.until(lambda: card.query_one("#permission-always").has_focus)
        self.assertEqual(self.entry.text, "hi")
        self.app.client.send.assert_not_awaited()

    async def test_deferred_focus_callbacks_tolerate_removed_buttons(self):
        card = self.request_permission()
        await self.until(card.owns_button_focus)
        await card.remove_children()
        card.reveal_choice()
        card.focus_visible_choice()
        card.move_choice(1)
        self.app.client.send.assert_not_awaited()

    async def test_modal_keeps_its_arrows_while_a_permission_is_pending(self):
        class DraftDialog(ModalScreen):
            def compose(self):
                yield Input("modal draft")

        card = self.request_permission()
        await self.until(card.owns_button_focus)
        await self.app.push_screen(DraftDialog())
        entry = self.app.screen.query_one(Input)
        entry.focus()
        entry.cursor_position = 4
        await self.pilot.press("left")
        self.assertEqual(entry.cursor_position, 3)
        self.assertEqual(card.selected_button, "permission-yes")
        self.app.pop_screen()
        await self.pilot.pause()
        self.entry.focus()
        await self.pilot.press("left")
        await self.until(lambda: card.query_one("#permission-no").has_focus)
        self.app.client.send.assert_not_awaited()

    async def test_drawer_clicks_toggle_without_focus_round_trip(self):
        drawer = ToolView("c", "webfetch", {})
        drawer.set_result({"output": "\n\n".join(f"Paragraph {i}." for i in range(100)), "isError": False})
        await self.viewport.mount(drawer)
        await self.pilot.pause()
        title = drawer.query_one("CollapsibleTitle")
        with patch.object(self.app.screen, "_update_focus_styles", wraps=self.app.screen._update_focus_styles) as focus_styles:
            await self.pilot.click(title)
            await self.until(lambda: drawer.rendered)
            blocks = list(drawer.query_one(ResponseMarkdown).children)
            await self.pilot.click(title)
            self.assertTrue(drawer.collapsed)
            await self.pilot.click(title)
            self.assertFalse(drawer.collapsed)
            self.assertEqual(list(drawer.query_one(ResponseMarkdown).children), blocks)
            self.assertTrue(self.entry.has_focus)
            focus_styles.assert_not_called()

    async def test_all_chat_drawer_types_keep_keyboard_access(self):
        drawers = [ChatCollapsible(title="Reasoning"), ToolView("c", "bash", {}), HistoryCompaction("Summary"),
                   HistoryReasoning("g", {"activityIndex": 0}),
                   HistoryToolView("g", {"activityIndex": 1, "toolName": "bash"})]
        await self.viewport.mount_all(drawers)
        await self.pilot.pause()
        for drawer in drawers:
            self.assertFalse(drawer.query_one("CollapsibleTitle").FOCUS_ON_CLICK)
            self.assertTrue(drawer.query_one("CollapsibleTitle").can_focus)
        # Test native keyboard interaction without the app's existing composer-focus policy.
        with patch.object(self.app, "focus_composer"):
            title = drawers[0].query_one("CollapsibleTitle")
            title.focus()
            await self.pilot.pause()
            await self.pilot.press("enter")
            self.assertFalse(drawers[0].collapsed)
            await self.pilot.press("enter")
            self.assertTrue(drawers[0].collapsed)

    async def test_activity_toggle_click_leaves_composer_focus(self):
        activity = TurnActivity()
        await self.viewport.mount(activity)
        await self.pilot.pause()
        with patch.object(self.app.screen, "_update_focus_styles", wraps=self.app.screen._update_focus_styles) as focus_styles:
            await self.pilot.click(activity.toggle)
            self.assertTrue(activity.toggle.expanded)
            self.assertTrue(self.entry.has_focus)
            focus_styles.assert_not_called()

        with patch.object(self.app, "focus_composer"):
            activity.toggle.focus()
            await self.pilot.pause()
            await self.pilot.press("space")
            self.assertFalse(activity.toggle.expanded)

    async def test_agent_info_updates_only_changed_labels(self):
        bar = self.app.query_one(AgentInfoBar)
        info = {"model": "test-model", "reasoning": "medium", "last_turn_used_context": 1000, "total_context": 100000}
        self.app.update_agent_info(info)
        await self.pilot.pause()
        with ExitStack() as stack:
            updates = {id: stack.enter_context(patch.object(bar.query_one("#" + id), "update",
                       wraps=bar.query_one("#" + id).update)) for id in bar.labels}
            reflow = stack.enter_context(patch.object(bar, "reflow", wraps=bar.reflow))
            bar.update_info({})
            bar.update_info(info)
            reflow.assert_not_called()
            for update in updates.values():
                update.assert_not_called()
            bar.update_info({"last_turn_used_context": 1200, "total_context": 100000})
            updates["context-info"].assert_called_once_with("1 200 / 100 000")
            for id in ("model-info", "reasoning-info", "context-percent"):
                updates[id].assert_not_called()
            reflow.assert_called_once()

    async def test_unchanged_runtime_info_skips_bar_but_resize_and_loader_still_reflow(self):
        bar = self.app.query_one(AgentInfoBar)
        info = {"model": "test-model", "reasoning": "medium", "last_turn_used_context": None, "total_context": 100000}
        self.app.update_agent_info(info)
        await self.pilot.pause()
        with patch.object(bar, "update_info", wraps=bar.update_info) as update:
            self.app.update_agent_info({})
            self.app.update_agent_info(info)
            update.assert_not_called()
        with patch.object(bar, "reflow", wraps=bar.reflow) as reflow:
            await self.pilot.resize_terminal(40, 18)
            self.assertTrue(reflow.called)
            bar.set_history_loading(True)
            self.app.update_agent_info({"last_turn_used_context": 2500, "total_context": 100000})
            self.assertFalse(bar.query_one("#context-info").display)
            bar.set_history_loading(False)
            await self.pilot.resize_terminal(200, 24)
            self.assertTrue(bar.query_one("#context-info").display)
            self.assertEqual(bar.labels["context-info"], "2 500 / 100 000")
