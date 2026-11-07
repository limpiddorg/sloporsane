"""Skill references in prose and protected pastes remain ordinary messages."""

import unittest
from unittest.mock import AsyncMock, patch

from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput


SKILLS = [{"name": "check-api", "description": "Check routes.",
           "arguments": ["directory", "note"], "argument-hint": "[directory] [note]"}]


class SkillSubmissionTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(110, 35)))
        self.app.connected = True
        self.app.session_id = "s"
        self.app.client.send = AsyncMock()
        self.app.update_agent_info({"skills": SKILLS})
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        # A mocked daemon never completes the turn. Stop its spinner before
        # Textual dismantles the screen during test cleanup.
        self.addCleanup(setattr, self.app, "busy", False)

    async def draft(self, text):
        self.entry.load_text(text)
        self.entry.move_cursor(self.entry.document.end)
        await self.pilot.pause()

    async def assert_plain_submission(self, text):
        with patch('sos_tui.app.parse_skill_commands') as parser:
            await self.pilot.press('enter')
        parser.assert_not_called()
        self.app.client.send.assert_awaited_once()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['type'], 'message.send')
        self.assertEqual(request['text'], text)
        self.assertNotIn('skill', request)
        self.assertEqual(self.entry.text, '')

    async def test_inline_required_skill_without_arguments_is_plain_text(self):
        text = 'Please use /check-api'
        await self.draft(text)
        await self.assert_plain_submission(text)

    async def test_multiple_inline_references_do_not_parse_free_form_arguments(self):
        text = 'Use /check-api on routes then /check-api with "unfinished quotes'
        await self.draft(text)
        await self.assert_plain_submission(text)

    async def test_initial_invocation_after_spaces_sends_structured_arguments(self):
        await self.draft('  \t/check-api "src/my routes" "explain"')
        await self.pilot.press('enter')
        self.app.client.send.assert_awaited_once()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['text'], '  \t/check-api "src/my routes" "explain"')
        self.assertEqual(request['skill'], {'name': 'check-api', 'input': '"src/my routes" "explain"'})

    async def test_empty_input_is_accepted(self):
        await self.draft('  /check-api')
        await self.pilot.press('enter')
        self.assertEqual(self.app.client.send.call_args.args[0]['skill'], {'name': 'check-api', 'input': ''})

    async def test_skill_without_schema_sends_literal_free_form_input(self):
        self.app.update_agent_info({"skills": [{"name": "caveman", "description": "Brief replies."}]})
        text = '/caveman status "unfinished $(touch marker)'
        await self.draft(text)
        await self.pilot.press('enter')
        self.app.client.send.assert_awaited_once()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['text'], text)
        self.assertEqual(request['skill'], {
            'name': 'caveman', 'input': 'status "unfinished $(touch marker)'})

    async def test_stacked_skills_are_sent_together_with_shared_input(self):
        self.app.update_agent_info({"skills": SKILLS + [{"name": "caveman", "description": "Brief replies."}]})
        text = '/caveman /check-api src/http'
        await self.draft(text)
        self.assertEqual(self.entry.suggestion, '')
        await self.pilot.press('enter')
        self.app.client.send.assert_awaited_once()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['text'], text)
        self.assertNotIn('skill', request)
        self.assertEqual(request['skills'], [
            {'name': 'caveman', 'input': 'src/http'},
            {'name': 'check-api', 'input': 'src/http'}])

    async def test_stacked_skills_allow_empty_shared_input(self):
        self.app.update_agent_info({"skills": SKILLS + [{"name": "caveman"}]})
        await self.draft('/caveman /check-api')
        await self.pilot.press('enter')
        self.assertEqual(self.app.client.send.call_args.args[0]['skills'],
                         [{'name': name, 'input': ''} for name in ('caveman', 'check-api')])

    async def test_reference_in_quote_is_not_an_invocation(self):
        text = '> /check-api\nPlease check the quoted request'
        await self.draft(text)
        await self.assert_plain_submission(text)

    async def test_short_pasted_initial_reference_is_not_an_invocation(self):
        text = '/check-api'
        self.entry.paste_text(text)
        await self.pilot.pause()
        self.assertEqual(self.entry.command_text, '')
        await self.assert_plain_submission(text)

    async def test_compacted_initial_reference_sends_expanded_payload_without_invocation(self):
        text = '/check-api\nRequired argument deliberately omitted\nKeep this literal'
        self.entry.paste_text(text)
        await self.pilot.pause()
        self.assertEqual(len(self.entry.paste_blocks), 1)
        self.assertEqual(self.entry.command_text, '')
        await self.assert_plain_submission(text)

    async def test_multiline_request_keeps_text_and_defers_argument_interpretation(self):
        text = 'Check my routes\nwith /check-api\nThe directory is src/http and note=explain it'
        await self.draft(text)
        await self.assert_plain_submission(text)
