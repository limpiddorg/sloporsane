import json
import unittest
from unittest.mock import AsyncMock, patch

from textual.widgets import Collapsible, Static
from sos_tui.app import SosApp
from sos_tui.btw import BtwPanel, BtwUserMessage, parse_btw
from sos_tui.config import MISC_MD
from sos_tui.response_markdown import GenerationFooter
from sos_tui.history_viewport import HistoryViewport
from sos_tui.message_input import MessageInput
from sos_tui.user_message import UserMessage


class BtwParsingTest(unittest.TestCase):
    def test_new_and_existing_branch_syntax(self):
        self.assertEqual(parse_btw("/btw Why this choice?"), ("", "Why this choice?"))
        self.assertEqual(parse_btw("/btw btw-12 Explain more"), ("btw-12", "Explain more"))
        self.assertEqual(parse_btw("/btw btw-example"), ("", "btw-example"))
        for text in ("/btw", "/btw   ", "/btw btw-1"):
            with self.assertRaises(ValueError) as error:
                parse_btw(text)
            self.assertEqual(str(error.exception),
                             "Use /btw {message} to start a new btw conversation or /btw {id} {message} to continue one")


class BtwUiTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 35)))
        self.app.connected = True
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.send = self.enterContext(patch.object(self.app.client, "send", new_callable=AsyncMock))
        self.app.busy = True
        self.app.session_id = "session"
        self.app.display_message({"id": "principal", "role": "user", "content": [{"type": "text", "text": "Main task"}]})
        await self.pilot.pause()
        self.activity = self.app.running_activity
        self.transfer = 0

    async def branch(self, id="btw-1", status="running", messages=None, accepted=True,
                     standalone=False, anchor="principal", page_anchor=None):
        if accepted:
            self.app.handle_event({"type": "btw.accepted", "sessionId": "session", "branchId": id})
        self.transfer += 1
        request = str(self.transfer)
        messages = messages or [{"id": "question", "role": "user", "content": [{"type": "text", "text": "Explain this"}]}]
        base = {"sessionId": "session", "requestId": request}
        self.app.handle_event({**base, "type": "btw.started", "session": {"id": "session"},
                               "branch": {"id": id, "anchor": anchor, "pageAnchor": page_anchor or anchor,
                                          "standalone": standalone, "status": status, "error": ""},
                               "messageCount": len(messages)})
        for i, message in enumerate(messages):
            self.app.handle_event({**base, "type": "btw.chunk", "messageIndex": i, "partIndex": 0,
                                   "last": True, "data": json.dumps(message)})
        self.app.handle_event({**base, "type": "btw.finished", "messageCount": len(messages)})
        await self.pilot.pause()
        return self.app.btw.panels[id]

    async def test_command_is_sent_during_generation_instead_of_being_queued(self):
        self.entry.text = "/btw Why this approach?"
        await self.app.on_message_input_submitted(MessageInput.Submitted(self.entry.text))
        request = self.send.call_args.args[0]
        self.assertEqual(request["type"], "btw.send")
        self.assertEqual(request["text"], "Why this approach?")
        self.assertEqual(request["branchId"], "")
        self.assertIsNone(self.app.queued_message)
        self.assertTrue(self.app.busy)
        self.assertEqual(self.entry.text, "")

    async def test_creation_without_principal_conversation_is_refused(self):
        self.app.busy = False
        self.app.session_id = None
        self.entry.text = "/btw Question"
        with patch.object(self.app, "notify") as notify:
            await self.app.btw.submit(self.entry.text)
        self.assertEqual(notify.call_args.args[0], "Start a principal conversation before using /btw")
        self.assertEqual(self.entry.text, "/btw Question")
        self.send.assert_not_awaited()

    async def test_accepted_send_prefills_branch_for_each_follow_up(self):
        self.entry.focus()
        for text in ("/btw Question", "/btw btw-12 Follow-up"):
            self.entry.text = text
            await self.app.on_message_input_submitted(MessageInput.Submitted(text))
            request = self.send.call_args.args[0]["requestId"]
            self.app.handle_event({"type": "btw.accepted", "sessionId": "session",
                                   "requestId": request, "branchId": "btw-12"})
            await self.pilot.pause()
            self.assertEqual(self.entry.text, "/btw btw-12 ")
            self.assertEqual(self.entry.cursor_location, (0, len(self.entry.text)))
            self.assertFalse(self.entry.suggestions.display)

    async def test_acceptance_preserves_a_draft_typed_while_waiting(self):
        await self.app.btw.submit("/btw Question")
        request = self.send.call_args.args[0]["requestId"]
        self.entry.text = "Another draft"
        self.app.handle_event({"type": "btw.accepted", "sessionId": "session",
                               "requestId": request, "branchId": "btw-1"})
        self.assertEqual(self.entry.text, "Another draft")

    async def test_erasing_prefix_returns_to_principal_without_reinsertion(self):
        await self.app.btw.submit("/btw Question")
        request = self.send.call_args.args[0]["requestId"]
        self.app.handle_event({"type": "btw.accepted", "sessionId": "session",
                               "requestId": request, "branchId": "btw-1"})
        self.entry.text = ""
        await self.branch(status="completed", accepted=False)
        self.assertEqual(self.entry.text, "")
        self.app.busy = False
        await self.app.on_message_input_submitted(MessageInput.Submitted("Main follow-up"))
        self.assertEqual(self.send.call_args.args[0]["type"], "message.send")

    async def test_arriving_branch_does_not_open_suggestions(self):
        self.entry.focus()
        self.entry.text = "/btw "
        await self.pilot.pause()
        self.assertFalse(self.entry.suggestions.display)
        await self.branch(accepted=False)
        self.assertEqual(self.entry.suggestions.matches, ())
        self.assertFalse(self.entry.suggestions.display)

    async def test_first_btw_positions_principal_user_at_top(self):
        viewport = self.app.query_one(HistoryViewport)
        principal = next(widget for widget in viewport.message_ids if isinstance(widget, UserMessage))
        await viewport.mount(Static("Earlier conversation\n" * 12), before=principal)
        await self.pilot.pause()
        viewport.scroll_home(animate=False, immediate=True)
        await self.branch()
        self.assertEqual(principal.region.y, viewport.content_region.y)

    async def test_follow_up_positions_its_user_at_top_and_main_response_does_not_steal_scroll(self):
        await self.branch()
        messages = [
            {"id": "first", "role": "user", "content": [{"type": "text", "text": "First question"}]},
            {"id": "answer", "role": "assistant", "content": [{"type": "text", "text": "First answer"}]},
            {"id": "second", "role": "user", "content": [{"type": "text", "text": "Second question"}]},
        ]
        panel = await self.branch(messages=messages)
        viewport = self.app.query_one(HistoryViewport)
        # Opening a branch schedules two layout-refresh callbacks before anchoring
        # the scroll; one pilot pause may observe the panel before they both run.
        for _ in range(10):
            if list(panel.query(BtwUserMessage))[-1].region.y == viewport.content_region.y:
                break
            await self.pilot.pause()
        self.assertEqual(list(panel.query(BtwUserMessage))[-1].region.y, viewport.content_region.y)
        self.app.display_message({"id": "main-answer", "role": "assistant",
                                  "content": [{"type": "text", "text": "Principal answer"}]})
        await self.pilot.pause()
        self.assertEqual(list(panel.query(BtwUserMessage))[-1].region.y, viewport.content_region.y)
        messages.append({"id": "reply", "role": "assistant", "content": [{"type": "text", "text": "Second answer"}]})
        await self.branch(messages=messages, status="completed", accepted=False)
        self.assertEqual(list(panel.query(BtwUserMessage))[-1].region.y, viewport.content_region.y)
        viewport.action_scroll_home()
        await self.pilot.wait_for_scheduled_animations()
        await self.pilot.pause()
        position = viewport.scroll_y
        messages = [*messages, {"id": "more", "role": "assistant", "content": [{"type": "text", "text": "More detail"}]}]
        await self.branch(messages=messages, status="completed", accepted=False)
        self.assertEqual(viewport.scroll_y, position)

    async def test_reference_prefixes_generation_footer_on_the_right(self):
        panel = await self.branch(status="completed", messages=[
            {"id": "answer", "role": "assistant", "content": [{"type": "text", "text": "Answer"}],
             "metadata": {"model": "test-model", "reasoning": "high", "duration_ms": 2000}}
        ])
        footer = panel.query_one(GenerationFooter)
        label = footer.render()
        self.assertTrue(str(label).startswith("btw-1 | test-model | "))
        self.assertTrue(str(label).endswith(" | 2s"))
        self.assertEqual(footer.prefix.style, MISC_MD)
        self.assertEqual(footer.styles.text_align, "right")
        self.assertFalse(panel.query(".btw-reference"))

    async def test_misc_boundaries_follow_content_and_hide_when_collapsed(self):
        panel = await self.branch()
        contents = panel.query_one(Collapsible.Contents)
        top, bottom = panel.query(".btw-boundary")
        self.assertIs(contents.children[0], top)
        self.assertIs(contents.children[-1], bottom)
        user = panel.query_one(BtwUserMessage)
        self.assertLess(top.region.y, user.region.y)
        self.assertGreater(bottom.region.y, user.region.bottom - 1)
        for rule in (top, bottom):
            self.assertEqual(rule.styles.color.hex, MISC_MD.upper())
            self.assertEqual(rule.region.x, user.region.x)
            self.assertEqual(rule.region.height, 1)
        original_height = contents.region.height
        await self.branch(status="completed", accepted=False, messages=[
            {"id": "question", "role": "user", "content": [{"type": "text", "text": "Question"}]},
            {"id": "answer", "role": "assistant", "content": [{"type": "text", "text": "Answer"}]}
        ])
        top, bottom = panel.query(".btw-boundary")
        self.assertIs(contents.children[0], top)
        self.assertIs(contents.children[-1], bottom)
        self.assertGreater(contents.region.height, original_height)
        panel.collapsed = True
        await self.pilot.pause()
        self.assertFalse(contents.display)

    async def test_second_branch_folds_first_and_completion_does_not_stop_principal(self):
        first = await self.branch()
        second = await self.branch("btw-2")
        self.assertTrue(first.collapsed)
        self.assertFalse(second.collapsed)
        await self.branch("btw-2", "completed", [
            {"id": "user", "role": "user", "content": [{"type": "text", "text": "Question"}]},
            {"id": "answer", "role": "assistant", "content": [{"type": "text", "text": "Side answer"}]}], accepted=False)
        self.assertTrue(self.app.busy)
        self.assertIs(self.app.running_activity, self.activity)
        self.assertEqual(str(second.query_one(".btw-reference", Static).render()), "btw-2")

    async def test_new_and_existing_branches_can_be_sent_while_idle(self):
        self.app.busy = False
        self.entry.text = "/btw New question"
        await self.app.on_message_input_submitted(MessageInput.Submitted(self.entry.text))
        self.assertEqual(self.send.call_args.args[0]["branchId"], "")
        self.assertEqual(self.entry.text, "")
        self.entry.text = "/btw btw-1 Follow-up"
        await self.app.on_message_input_submitted(MessageInput.Submitted(self.entry.text))
        self.assertEqual(self.send.call_args.args[0]["branchId"], "btw-1")
        self.assertFalse(self.app.busy)

    async def test_idle_branch_can_anchor_after_activity_without_assistant_text(self):
        viewport = self.app.query_one(HistoryViewport)
        viewport.start_generation_footer({"model": "main-model", "reasoning": "high"})
        await self.pilot.pause()
        panel = await self.branch(standalone=True, anchor=self.activity.group_id, page_anchor="principal")
        children = list(viewport.children)
        self.assertEqual(children.index(viewport.live_footer), children.index(self.activity) + 1)
        self.assertEqual(children.index(panel), children.index(viewport.live_footer) + 1)

    async def test_idle_off_page_branch_uses_page_anchor_not_activity_id(self):
        viewport = self.app.query_one(HistoryViewport)
        self.activity.group_id = "off-page"
        async def load(direction, clean=False, anchor=None):
            self.assertEqual(anchor, "principal")
            self.activity.group_id = "saved-activity"
            return True
        with patch.object(self.app, "load_history_page", side_effect=load) as navigate:
            panel = await self.branch(standalone=True, anchor="saved-activity", page_anchor="principal")
            navigate.assert_awaited_once()
            self.assertFalse(panel.collapsed)

    async def test_main_cancellation_does_not_change_branch(self):
        panel = await self.branch()
        self.app.handle_event({"type": "execution.cancelled"})
        await self.pilot.pause()
        self.assertEqual(self.app.btw.records["btw-1"][0]["status"], "running")
        self.assertIn("Working", str(panel.query_one(".btw-status", Static).render()))
        self.assertFalse(self.app.busy)

    async def test_failed_btw_send_restores_draft_without_ending_principal(self):
        self.entry.text = "/btw Question"
        await self.app.on_message_input_submitted(MessageInput.Submitted(self.entry.text))
        request = self.send.call_args.args[0]
        self.entry.text = "Another draft"
        self.app.handle_event({"type": "request.failed", "requestId": request["requestId"], "error": "Failed"})
        self.assertIn("/btw Question", self.entry.text)
        self.assertIn("Another draft", self.entry.text)
        self.assertTrue(self.app.busy)

    async def test_history_remount_keeps_branch_content_and_fold_state(self):
        panel = await self.branch()
        panel.collapsed = True
        self.app.btw.capture()
        await panel.remove()
        self.app.btw.render()
        await self.pilot.pause()
        restored = self.app.btw.panels["btw-1"]
        self.assertTrue(restored.collapsed)
        self.assertEqual(restored.query_one(BtwUserMessage).text, "Explain this")

    async def test_new_principal_send_folds_branches(self):
        panel = await self.branch()
        self.app.busy = False
        await self.app.send_user_message("Next task")
        await self.pilot.pause()
        self.assertTrue(panel.collapsed)

    async def test_other_session_updates_do_not_pollute_current_chat(self):
        self.app.handle_event({"type": "btw.accepted", "sessionId": "other", "branchId": "btw-1"})
        self.assertFalse(self.app.btw.expanded)
        self.assertFalse(self.app.query(BtwPanel))

    async def test_continuing_an_off_page_branch_returns_to_its_anchor(self):
        viewport = self.app.query_one(HistoryViewport)
        anchors = dict(viewport.message_ids)
        viewport.message_ids.clear()
        async def load(direction, clean=False, anchor=None):
            self.assertEqual(direction, "before")
            self.assertEqual(anchor, "principal")
            viewport.message_ids.update(anchors)
            return True
        with patch.object(self.app, "load_history_page", side_effect=load) as navigate:
            panel = await self.branch()
            navigate.assert_awaited_once()
            self.assertFalse(panel.collapsed)

    async def test_retry_and_failure_stay_inside_the_branch(self):
        panel = await self.branch()
        self.app.handle_event({"type": "btw.retry", "sessionId": "session", "branchId": "btw-1",
                               "retry": {"attempt": 2, "limit": 10, "seconds": 2, "reason": "Connection problem"}})
        self.assertIn("Retry 2/10", str(panel.query_one(".btw-status", Static).render()))
        self.assertIsNone(self.app.retry_notice)
        self.assertTrue(self.app.busy)

    async def test_resume_data_starts_collapsed_and_disconnect_stops_branch_status(self):
        panel = await self.branch(accepted=False)
        self.assertTrue(panel.collapsed)
        self.app.btw.disconnected()
        await self.pilot.pause()
        self.assertIn("disconnected", str(panel.query_one(".btw-status", Static).render()))
