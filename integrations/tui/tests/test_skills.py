"""Skill commands send raw input and display literal author-supplied hints."""

import unittest
from io import StringIO
from rich.console import Console
from unittest.mock import AsyncMock, patch

from sos_tui.app import SosApp
from sos_tui.command_suggestions import CommandSuggestions
from sos_tui.skill_details import SkillDetails
from textual.widgets import Static
from sos_tui.message_input import MessageInput
from sos_tui.config import ALGAE_MD, BOLD_MD, TITLE_MD, MUTED_COLOR
from sos_tui.history_viewport import HistoryActivity, HistoryViewport
from sos_tui.tool_view import ToolView
from textual.color import Color
from sos_tui.skills import (parse_skill_command, parse_skill_commands, skill_command_chain,
                            is_skill_command_position, skill_commands, skill_reference_spans)

def option_command(option):
    output = StringIO()
    Console(file=output, width=120, color_system=None).print(option.prompt)
    return output.getvalue().rstrip().removesuffix("Skill").rstrip()


SKILLS = [{"name": "check-api", "description": "Check HTTP routes.", "arguments": ["directory", "note"],
           "argument-hint": "[directory] [note]", "scripts": ["scripts/check.py", "scripts/nested/report.sh"]}]

MULTI_OPTIONAL = {"name": "my-test-skill", "arguments": ["message", "name", "note"],
                  "argument-hint": "[message] [name] [note]"}


class FreeFormSkillTest(unittest.TestCase):
    def test_missing_or_empty_schema_accepts_literal_tail(self):
        for skill in ({"name": "caveman"}, {"name": "caveman", "arguments": {}}):
            for value in ('status', 'ultra', "l'utilisateur", '"unfinished',
                          'name=value $(touch marker) \\path\nnext line  '):
                with self.subTest(skill=skill, value=value):
                    self.assertEqual(parse_skill_command('  /caveman ' + value, [skill]),
                                     {'name': 'caveman', 'input': value})

    def test_no_input_serializes_an_empty_string(self):
        self.assertEqual(parse_skill_command('/caveman  ', [{"name": "caveman"}]),
                         {'name': 'caveman', 'input': ''})


class StackedSkillsTest(unittest.TestCase):
    skills = [{"name": "caveman"}, {"name": "review"}] + SKILLS

    def test_consecutive_skills_share_the_exact_tail(self):
        tail = '"src/my routes" "note"  '
        result = parse_skill_commands('  /caveman /review /check-api ' + tail, self.skills)
        self.assertEqual([item['name'] for item in result], ['caveman', 'review', 'check-api'])
        self.assertEqual([item['input'] for item in result], [tail] * 3)

    def test_native_unknown_quoted_and_nonleading_tokens_stop_expansion(self):
        for tail in ('/new', '/unknown /review', '"/review"', 'please /review'):
            with self.subTest(tail=tail):
                self.assertEqual(parse_skill_commands('/caveman ' + tail, self.skills),
                                 [{'name': 'caveman', 'input': tail}])
        self.assertEqual(parse_skill_commands('Please use /caveman /review', self.skills), [])

    def test_six_skill_limit_keeps_seventh_and_remainder_as_input(self):
        result = parse_skill_commands('/caveman ' * 7 + 'status', self.skills)
        self.assertEqual(len(result), 6)
        self.assertEqual([item['input'] for item in result], ['/caveman status'] * 6)

    def test_no_required_or_extra_argument_validation(self):
        self.assertEqual(parse_skill_commands('/caveman /check-api', self.skills),
                         [{'name': 'caveman', 'input': ''}, {'name': 'check-api', 'input': ''}])
        self.assertEqual(parse_skill_commands('/check-api one two three', self.skills)[0]['input'], 'one two three')

    def test_declared_names_do_not_change_shared_raw_input(self):
        result = parse_skill_commands('/check-api /review "src/my routes"', self.skills)
        self.assertEqual(result, [{'name': name, 'input': '"src/my routes"'} for name in ('check-api', 'review')])

    def test_empty_and_single_skill_keep_the_same_shapes(self):
        self.assertEqual(parse_skill_commands('/caveman /review', self.skills), [
            {'name': 'caveman', 'input': ''}, {'name': 'review', 'input': ''}])
        self.assertEqual(parse_skill_commands('/caveman status', self.skills),
                         [parse_skill_command('/caveman status', self.skills)])

    def test_argument_help_only_applies_to_leading_skill_positions(self):
        self.assertTrue(is_skill_command_position('', self.skills))
        self.assertTrue(is_skill_command_position('/caveman ', self.skills))
        self.assertFalse(is_skill_command_position('Please use ', self.skills))
        self.assertFalse(is_skill_command_position('/caveman status ', self.skills))
        self.assertFalse(is_skill_command_position('/caveman ' * 6, self.skills))
        chain, tail = skill_command_chain('/caveman /check-api src', self.skills)
        self.assertEqual([item['name'] for item in chain], ['caveman', 'check-api'])
        self.assertEqual(tail, 'src')


class SkillReferenceTest(unittest.TestCase):
    def test_initial_and_inline_references_exclude_arguments(self):
        for prefix in ("", "  \t", "Please use ", "Première ligne\nPuis "):
            text = prefix + '/check-api src "unfinished'
            with self.subTest(prefix=prefix):
                self.assertEqual(skill_reference_spans(text, SKILLS),
                                 ((len(prefix), len(prefix) + len('/check-api')),))

    def test_multiple_references_keep_original_multiline_offsets(self):
        text = "Use /check-api src\nthen /my-test-skill hello and /check-api."
        expected = ((4, 14), (24, 38), (49, 59))
        self.assertEqual(skill_reference_spans(text, SKILLS + [MULTI_OPTIONAL]), expected)
        self.assertEqual([text[start:end] for start, end in expected],
                         ['/check-api', '/my-test-skill', '/check-api'])

    def test_surrounding_punctuation_is_not_part_of_reference(self):
        for left, right in (("(", ")"), ("[", "]"), ("`", "`"), ('"', '"'),
                            ("'", "'"), ("", ","), ("", "."), ("", ":"),
                            ("", ";"), ("", "!"), ("", "?")):
            text = left + '/check-api' + right
            with self.subTest(text=text):
                self.assertEqual(skill_reference_spans(text, SKILLS),
                                 ((len(left), len(left) + len('/check-api')),))

    def test_nearby_names_are_matched_in_full_regardless_of_catalog_order(self):
        skills = [{"name": name} for name in ('check', 'check-api', 'check-api-extra')]
        text = '/check /check-api /check-api-extra /check-api-unknown'
        expected = ((0, 6), (7, 17), (18, 34))
        self.assertEqual(skill_reference_spans(text, skills), expected)
        self.assertEqual(skill_reference_spans(text, list(reversed(skills))), expected)

    def test_rejects_partial_names_and_path_fragments(self):
        for text in ('/check-ap', '/check-api-extra', '/check-api_extra',
                     '/check-api2', '/check-apié', 'word/check-api', 'é/check-api',
                     '_/check-api', '-/check-api', './check-api', '//check-api',
                     'src/check-api', r'folder\/check-api', '/check-api/src',
                     r'/check-api\src', '/check-api.py', 'https://check-api'):
            with self.subTest(text=text):
                self.assertEqual(skill_reference_spans(text, SKILLS), ())

    def test_unknown_references_and_empty_inputs(self):
        self.assertEqual(skill_reference_spans('/unknown /CHECK-API //check-api', SKILLS), ())
        self.assertEqual(skill_reference_spans('/check-api', []), ())
        self.assertEqual(skill_reference_spans('', SKILLS), ())

    def test_detection_uses_catalog_names_without_argument_validation(self):
        with patch('sos_tui.skills.parse_skill_command') as parser:
            self.assertEqual(skill_reference_spans('/check-api', SKILLS), ((0, 10),))
            self.assertEqual(skill_reference_spans('/check-api', [{"name": "check-api"}]), ((0, 10),))
        parser.assert_not_called()


class SkillCommandTest(unittest.TestCase):
    def test_transport_never_parses_or_validates_arguments(self):
        for value in ('', 'one two three extra', 'name=A name=B', '"unfinished', "l'utilisateur", '$(touch marker)'):
            with self.subTest(value=value):
                self.assertEqual(parse_skill_command('/check-api ' + value, SKILLS),
                                 {'name': 'check-api', 'input': value})

    def test_native_commands_cannot_be_shadowed(self):
        collision = [{"name": "new"}]
        self.assertEqual(skill_commands(collision), {})
        self.assertIsNone(parse_skill_command('/new extra', collision))
        self.assertIsNone(parse_skill_command('//check-api', SKILLS))


class SkillUiTest(unittest.IsolatedAsyncioTestCase):
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

    async def draft(self, text):
        self.entry.text = text
        self.entry.move_cursor((0, len(text)))
        await self.pilot.pause()

    async def test_completion_shows_description_and_arguments_without_changing_command_id(self):
        await self.draft('/check')
        menu = self.app.query_one(CommandSuggestions)
        self.assertEqual(menu.matches, ('/check-api',))
        self.assertEqual(option_command(menu.options[0]), '/check-api [directory] [note]')
        self.assertNotIn('=<required>', option_command(menu.options[0]))
        details = self.app.query_one(SkillDetails)
        self.assertTrue(details.display)
        await self.pilot.press('tab')
        self.assertEqual(menu.selected_command, '/check-api')
        self.assertTrue(self.entry.has_focus)
        self.assertTrue(details.display)
        self.assertEqual(details.region.height, self.app.size.height // 2)
        text = details.query_one('#skill-body', Static).render().plain
        self.assertEqual(details.query_one('#skill-name', Static).render().plain, 'check-api')
        for value in ('Check HTTP routes.', '- scripts/check.py', '- scripts/nested/report.sh'):
            self.assertIn(value, text)
        self.app.client.send.assert_not_awaited()

    async def test_stacked_completion_shows_skill_arguments_but_not_native_commands(self):
        self.app.update_agent_info({'skills': SKILLS + [{'name': 'caveman', 'description': 'Brief.'}]})
        await self.draft('/caveman /')
        menu = self.app.query_one(CommandSuggestions)
        self.assertIn('/check-api', menu.matches)
        self.assertNotIn('/new', menu.matches)
        await self.draft('/caveman /check')
        details = self.app.query_one(SkillDetails)
        self.assertEqual(details.query_one('#skill-name', Static).render().plain, 'check-api')
        await self.pilot.press('tab')
        self.assertEqual(self.entry.text, '/caveman /check-api')
        self.assertIn('[directory] [note]', self.entry.suggestion)
        self.assertTrue(self.entry.has_focus)
        await self.draft('Please use /check')
        self.assertNotIn('Arguments', details.query_one('#skill-body', Static).render().plain)

    async def test_skill_rows_show_only_names_and_shift_tab_cycles_details(self):
        self.app.update_agent_info({'skills': [{**SKILLS[0], 'name': 'z-first', 'source': 'workspace'},
            {'name': 'z-last', 'description': 'Other description', 'source': 'user', 'input': ''}]})
        await self.draft('/z')
        menu = self.app.query_one(CommandSuggestions)
        self.assertEqual([option_command(option) for option in menu.options], ['/z-first [directory] [note]', '/z-last'])
        mode = self.app.mode
        for name in ('z-last', 'z-first', 'z-last'):
            await self.pilot.press('shift+tab')
            self.assertEqual(menu.selected_command, f'/{name}')
            self.assertEqual(self.app.query_one(SkillDetails).query_one('#skill-name', Static).render().plain, name)
            self.assertTrue(self.entry.has_focus)
            self.assertEqual(self.entry.text, f'/{name}')
            self.assertEqual(self.app.mode, mode)
        self.app.client.send.assert_not_awaited()

    async def test_skill_argument_preview_follows_selection_and_typed_values(self):
        for draft, expected in (
            ('/', 'check-api [directory] [note]'),
            ('/check-api', ' [directory] [note]'),
            ('/check-api ', '[directory] [note]'),
            ('/check-api src', ''),
            ('/check-api "src/my ', ''),
            ('/check-api "src/my http" ', ''),
            ('/check-api src "a note"', ''),
        ):
            await self.draft(draft)
            self.assertEqual(self.entry.suggestion, expected)
            self.assertEqual(self.entry.text, draft)
        await self.draft('/ch')
        await self.pilot.press('tab')
        self.assertEqual(self.entry.suggestion, ' [directory] [note]')
        self.assertIn('/check-api [directory] [note]', self.entry.render_line(0).text)
        await self.pilot.press('left')
        self.assertEqual(self.entry.suggestion, '')
        await self.pilot.press('right')
        self.assertEqual(self.entry.text, '/check-api')
        self.assertEqual(self.entry.suggestion, ' [directory] [note]')
        # Keep a real blur: the app normally restores composer focus automatically.
        with patch.object(self.app, 'focus_composer'):
            self.entry.blur()
            await self.pilot.pause()
            self.assertFalse(self.entry.has_focus)
            self.assertEqual(self.entry.suggestion, '')
        self.app.client.send.assert_not_awaited()

    async def test_argument_hints_are_never_submitted(self):
        await self.draft('/check-api src')
        self.assertEqual(self.entry.suggestion, '')
        await self.pilot.press('enter')
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['text'], '/check-api src')
        self.assertEqual(request['skill']['input'], 'src')

    async def test_inline_completion_previews_description_without_arguments(self):
        await self.draft('Please use /ch')
        menu = self.app.query_one(CommandSuggestions)
        details = self.app.query_one(SkillDetails)
        self.assertEqual(menu.matches, ('/check-api',))
        self.assertTrue(details.display)
        body = details.query_one('#skill-body', Static).render().plain
        self.assertIn('Check HTTP routes.', body)
        self.assertNotIn('Arguments', body)
        self.assertEqual(self.entry.suggestion, '')
        await self.pilot.press('tab')
        self.assertEqual(self.entry.text, 'Please use /check-api')
        self.assertEqual(self.entry.suggestion, '')
        await self.pilot.press('enter')
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['text'], 'Please use /check-api')
        self.assertNotIn('skill', request)

    async def test_completion_at_cursor_preserves_both_sides_and_undo(self):
        text = 'First line\nUse (/ch) then keep this /check-api'
        self.entry.load_text(text)
        self.entry.move_cursor((1, 8))
        await self.pilot.pause()
        self.assertEqual(self.app.query_one(CommandSuggestions).matches, ('/check-api',))
        await self.pilot.press('tab')
        self.assertEqual(self.entry.text, text.replace('(/ch)', '(/check-api)'))
        self.assertEqual(self.entry.cursor_location, (1, 15))
        self.entry.action_undo()
        self.assertEqual(self.entry.text, text)

    async def test_inline_cycling_preserves_surrounding_text(self):
        self.app.update_agent_info({'skills': [
            {**SKILLS[0], 'name': 'check-one'}, {**SKILLS[0], 'name': 'check-two'}]})
        await self.draft('Use /ch after')
        self.entry.move_cursor((0, 7))
        await self.pilot.pause()
        for key, name in (('tab', 'check-one'), ('tab', 'check-two'), ('shift+tab', 'check-one')):
            await self.pilot.press(key)
            self.assertEqual(self.entry.text, f'Use /{name} after')
            self.assertEqual(self.entry.suggestion, '')

    async def test_no_inline_native_commands_paths_urls_or_old_prefix(self):
        menu = self.app.query_one(CommandSuggestions)
        for text in ('Use /new', 'src/ch', 'https://ch', '//ch', 'word/ch', r'folder\ch', '/unknown'):
            await self.draft(text)
            self.assertFalse(menu.display, text)

    async def test_inline_completion_after_compact_paste_preserves_payload(self):
        self.entry.paste_text('First line\nSecond line\nThird line')
        self.entry.insert(' use /ch')
        await self.pilot.pause()
        await self.pilot.press('tab')
        self.assertEqual(self.entry.expanded_text, 'First line\nSecond line\nThird line use /check-api')
        self.assertEqual(len(self.entry.paste_blocks), 1)

    async def test_named_optional_preview_and_submission(self):
        self.app.update_agent_info({'skills': [MULTI_OPTIONAL]})
        await self.draft('/my-test-skill')
        self.assertEqual(self.entry.suggestion, ' [message] [name] [note]')
        await self.draft('/my-test-skill hello note="A note"')
        self.assertEqual(self.entry.suggestion, '')
        await self.pilot.press('enter')
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['text'], '/my-test-skill hello note="A note"')
        self.assertEqual(request['skill']['input'], 'hello note="A note"')

    async def test_single_argument_prose_preview_and_submission(self):
        skill = {"name": "work", "argument-hint": "[request...]"}
        self.app.update_agent_info({"skills": [skill]})
        await self.draft('/work')
        self.assertEqual(self.entry.suggestion, ' [request...]')
        text = "/work Modifie l'utilisateur sans guillemets"
        await self.draft(text)
        self.assertEqual(self.entry.suggestion, '')
        await self.pilot.press('enter')
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['text'], text)
        self.assertEqual(request['skill']['input'], "Modifie l'utilisateur sans guillemets")

    async def test_name_equals_values_remain_literal(self):
        await self.draft('/check-api hello name=A name=B')
        await self.pilot.press('enter')
        self.assertEqual(self.app.client.send.call_args.args[0]['skill']['input'], 'hello name=A name=B')

    async def test_selected_skill_inserts_only_name_and_allows_empty_input(self):
        await self.draft('/ch')
        await self.pilot.press('tab')
        self.assertEqual(self.entry.text, '/check-api')
        self.assertEqual(self.entry.suggestion, ' [directory] [note]')
        await self.pilot.press('enter')
        self.assertEqual(self.app.client.send.call_args.args[0]['skill'], {'name': 'check-api', 'input': ''})

    async def test_hint_is_literal_display_text_not_an_argument_schema(self):
        self.app.update_agent_info({'skills': [{**SKILLS[0], 'argument-hint': '[bold]custom [anything...]'}]})
        await self.draft('/check')
        details = self.app.query_one(SkillDetails)
        self.assertEqual(details.query_one('#skill-name', Static).render().plain, 'check-api')
        self.assertNotIn('Arguments', details.query_one('#skill-body', Static).render().plain)
        self.assertEqual(option_command(self.app.query_one(CommandSuggestions).options[0]),
                         '/check-api [bold]custom [anything...]')

    async def test_catalog_refresh_updates_hint_without_changing_command_names(self):
        await self.draft('/check')
        self.app.update_agent_info({'skills': [{**SKILLS[0], 'argument-hint': '[new hint...]'}]})
        await self.pilot.pause()
        menu = self.app.query_one(CommandSuggestions)
        self.assertEqual(menu.matches, ('/check-api',))
        self.assertEqual(option_command(menu.options[0]), '/check-api [new hint...]')
        self.assertEqual(self.entry.text, '/check')

    async def test_argument_descriptions_follow_declaration_order_without_required_flags(self):
        self.app.update_agent_info({'skills': [{**SKILLS[0], 'arguments': ['zebra', 'alpha', 'unexplained'],
            'argument-descriptions': {'alpha': 'Second [bold]literal.', 'zebra': 'First description.'}}]})
        await self.draft('/check')
        details = self.app.query_one(SkillDetails)
        text = details.query_one('#skill-body', Static).render()
        for value in ('Arguments', '1. zebra\nFirst description.', '2. alpha\nSecond [bold]literal.', '3. unexplained'):
            self.assertIn(value, text.plain)
        self.assertLess(text.plain.index('1. zebra'), text.plain.index('2. alpha'))
        self.assertLess(text.plain.index('Arguments'), text.plain.index('Scripts'))
        self.assertTrue(text.get_style_at_offset(text.plain.index('Arguments')).bold)
        self.assertFalse(text.get_style_at_offset(text.plain.index('[bold]literal')).bold)
        self.assertNotIn('(required)', text.plain)
        self.assertNotIn('(optional)', text.plain)
        self.app.client.send.assert_not_awaited()

    async def test_description_only_labels_are_displayed_and_empty_descriptions_hide_section(self):
        await self.draft('/check')
        details = self.app.query_one(SkillDetails)
        details.show_skill({'name': 'raw', 'description': 'Raw input.',
                            'argument-descriptions': {'request': 'An entire sentence.'}})
        self.assertIn('1. request\nAn entire sentence.', details.query_one('#skill-body', Static).render().plain)
        details.show_skill({'name': 'caveman', 'argument-hint': 'status|off|message...',
                            'argument-descriptions': {'status|off|message...': 'Choose status, off, or a task.'}})
        self.assertEqual(details.query_one('#skill-name', Static).render().plain, 'caveman')
        self.assertIn('Arguments\n\n1. status|off|message...\nChoose status, off, or a task.',
                      details.query_one('#skill-body', Static).render().plain)
        for extra in ({}, {'argument-descriptions': {}}):
            details.show_skill({**SKILLS[0], **extra})
            self.assertNotIn('Arguments', details.query_one('#skill-body', Static).render().plain)

    async def test_hint_delimiters_are_preserved_and_neither_hint_nor_descriptions_are_sent(self):
        self.app.update_agent_info({'skills': [{**SKILLS[0], 'argument-hint': '<directory> [note]',
            'argument-descriptions': {'directory': 'Where to inspect.'}}]})
        await self.draft('/check-api')
        self.assertEqual(self.entry.suggestion, ' <directory> [note]')
        self.assertEqual(option_command(self.app.query_one(CommandSuggestions).options[0]),
                         '/check-api <directory> [note]')
        await self.pilot.press('enter')
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['text'], '/check-api')
        self.assertEqual(request['skill'], {'name': 'check-api', 'input': ''})

    async def test_details_use_yellow_separator_and_only_headings_are_bold(self):
        await self.draft('/check')
        await self.pilot.press('tab')
        details = self.app.query_one(SkillDetails)
        self.assertEqual(details.styles.border_bottom[1], Color.parse(TITLE_MD))
        self.assertEqual(details.styles.border.spacing.top, 1)
        self.assertEqual(details.styles.border_top[1], Color.parse(TITLE_MD))
        self.assertEqual(details.styles.border.spacing.left, 0)
        self.assertEqual(details.styles.border.spacing.right, 0)
        menu = self.app.query_one(CommandSuggestions)
        self.assertEqual(details.styles.scrollbar_size_vertical, menu.styles.scrollbar_size_vertical)
        self.assertEqual(details.styles.scrollbar_color, menu.styles.scrollbar_color)
        self.assertEqual(details.styles.scrollbar_background, menu.styles.scrollbar_background)
        text = details.query_one('#skill-body', Static).render()
        self.assertTrue(details.query_one('#skill-name', Static).render().get_style_at_offset(0).bold)
        for value in ('Scripts',):
            self.assertTrue(text.get_style_at_offset(text.plain.index(value)).bold)
        for value in ('Check HTTP routes.', 'scripts/check.py'):
            self.assertFalse(text.get_style_at_offset(text.plain.index(value)).bold)

    async def test_details_follow_selection_and_hide_outside_skills(self):
        self.app.update_agent_info({"skills": SKILLS + [{"name": "z-other", "description": "[bold]literal", "arguments": {}, "scripts": []}]})
        await self.draft('/')
        details = self.app.query_one(SkillDetails)
        menu = self.app.query_one(CommandSuggestions)
        await self.pilot.press('tab', 'tab')
        self.assertEqual(menu.selected_command, '/z-other')
        self.assertIn('[bold]literal', details.query_one('#skill-body', Static).render().plain)
        self.assertEqual(details.query_one('#skill-body', Static).render().plain, '\n[bold]literal')
        await self.draft('/check')
        await self.pilot.click(menu, offset=(3, 0))
        self.assertIn('check-api', details.query_one('#skill-name', Static).render().plain)
        self.assertTrue(self.entry.has_focus)
        await self.pilot.resize_terminal(80, 24)
        await self.pilot.pause()
        self.assertEqual(details.region.height, 12)
        await self.draft('/')
        self.assertEqual(details.query_one('#skill-name', Static).render().plain, 'check-api')
        for text in ('/missing', '/check-api directory=src', ''):
            await self.draft(text)
            self.assertFalse(details.display)

    async def test_resource_categories_are_ordered_literal_and_hide_when_empty(self):
        await self.draft('/check')
        await self.pilot.press('tab')
        details = self.app.query_one(SkillDetails)
        details.show_skill({**SKILLS[0], 'references': ['references/nested/guide.md'],
                            'assets': ['assets/[bold]template.md']})
        text = details.query_one('#skill-body', Static).render()
        headings = ('Scripts', 'References', 'Assets')
        offsets = [text.plain.index(heading) for heading in headings]
        self.assertEqual(offsets, sorted(offsets))
        for offset in offsets:
            self.assertTrue(text.get_style_at_offset(offset).bold)
        for path in ('references/nested/guide.md', 'assets/[bold]template.md'):
            self.assertIn(f'- {path}', text.plain)
            self.assertFalse(text.get_style_at_offset(text.plain.index(path)).bold)
        details.show_skill({'name': 'empty', 'description': 'Empty skill.', 'input': '',
                            'scripts': [], 'references': [], 'assets': []})
        self.assertEqual(details.query_one('#skill-body', Static).render().plain, '\nEmpty skill.')
        self.app.client.send.assert_not_awaited()

    async def test_resource_errors_are_visible_instead_of_hidden_as_empty(self):
        await self.draft('/check')
        await self.pilot.press('tab')
        details = self.app.query_one(SkillDetails)
        for category in ('scripts', 'references', 'assets'):
            details.show_skill({'name': 'broken', 'description': 'Broken listing.',
                                f'{category}_error': 'Symlink refused'})
            text = details.query_one('#skill-body', Static).render().plain
            self.assertIn(f'{category.title()}\nUnavailable: Symlink refused', text)
            for other in ('scripts', 'references', 'assets'):
                if other != category:
                    self.assertNotIn(other.title(), text)

    async def test_optional_metadata_layout_styles_and_literal_values(self):
        self.app.update_agent_info({'skills': [{**SKILLS[0], 'source': 'workspace',
            'license': 'MIT', 'compatibility': 'Requires Python 3.11+ [bold].',
            'assets': ['assets/template.md'],
            'metadata': {'author': 'river-fox', 'version': '1.0', 'literal': '[bold]text'}}]})
        await self.draft('/check')
        await self.pilot.press('tab')
        details = self.app.query_one(SkillDetails)
        title_widget = details.query_one('#skill-name', Static)
        title = title_widget.render()
        self.assertEqual(title.plain, 'check-api [MIT]')
        self.assertTrue(title.get_style_at_offset(0).bold)
        self.assertEqual(title.get_style_at_offset(0).foreground, Color.parse(ALGAE_MD))
        license_style = title.get_style_at_offset(title.plain.index('[MIT]'))
        self.assertEqual(license_style.foreground, Color.parse(MUTED_COLOR))
        self.assertFalse(license_style.bold)
        compatibility = details.query_one('#skill-compatibility', Static)
        self.assertTrue(compatibility.display)
        self.assertEqual(compatibility.render().plain, 'Requires Python 3.11+ [bold].')
        self.assertEqual(compatibility.styles.color, Color.parse(MUTED_COLOR))
        self.assertEqual(compatibility.region.y, title_widget.parent.region.bottom)
        body = details.query_one('#skill-body', Static).render()
        self.assertTrue(body.plain.endswith('Metadata\nauthor: river-fox\nversion: 1.0\nliteral: [bold]text'))
        self.assertLess(body.plain.index('Assets'), body.plain.index('Metadata'))
        self.assertTrue(body.get_style_at_offset(body.plain.index('Metadata')).bold)
        self.assertFalse(body.get_style_at_offset(body.plain.index('[bold]text')).bold)
        origin = details.query_one('#skill-origin', Static)
        for width in (110, 60):
            await self.pilot.resize_terminal(width, 35)
            await self.pilot.pause()
            self.assertEqual(origin.render().plain, '[workspace]')
            self.assertEqual(origin.region.right, origin.parent.content_region.right)
        self.assertEqual(option_command(self.app.query_one(CommandSuggestions).options[0]), '/check-api [directory] [note]')
        self.app.client.send.assert_not_awaited()

    async def test_optional_metadata_is_cleared_when_switching_to_plain_skill(self):
        await self.draft('/check')
        await self.pilot.press('tab')
        details = self.app.query_one(SkillDetails)
        details.show_skill({**SKILLS[0], 'license': '[bold]literal',
                            'compatibility': 'Python', 'metadata': {'author': 'Someone'}})
        self.assertEqual(details.query_one('#skill-name', Static).render().plain, 'check-api [[bold]literal]')
        for metadata in ({}, {'metadata': {}}):
            details.show_skill({**SKILLS[0], **metadata})
            self.assertEqual(details.query_one('#skill-name', Static).render().plain, 'check-api')
            self.assertFalse(details.query_one('#skill-compatibility', Static).display)
            self.assertEqual(details.query_one('#skill-compatibility', Static).render().plain, '')
            self.assertNotIn('Metadata', details.query_one('#skill-body', Static).render().plain)

    async def test_details_origin_is_right_aligned_after_resize(self):
        for source in ('workspace', 'user'):
            self.app.update_agent_info({'skills': [{**SKILLS[0], 'source': source}]})
            await self.draft('/check')
            await self.pilot.press('tab')
            details = self.app.query_one(SkillDetails)
            origin = details.query_one('#skill-origin', Static)
            self.assertEqual(origin.render().plain, f'[{source}]')
            for width in (110, 80):
                await self.pilot.resize_terminal(width, 35)
                await self.pilot.pause()
                self.assertEqual(origin.region.right, origin.parent.content_region.right)

    async def test_details_are_scrollable_and_hide_on_blur(self):
        self.app.update_agent_info({"skills": [{**SKILLS[0], "scripts": [f"scripts/{i}.py" for i in range(100)]}]})
        await self.draft('/check')
        await self.pilot.press('tab')
        details = self.app.query_one(SkillDetails)
        self.assertGreater(details.max_scroll_y, 0)
        details.scroll_end(animate=False)
        await self.pilot.pause()
        self.assertGreater(details.scroll_y, 0)
        with patch.object(self.app, 'focus_composer'):
            self.entry.blur()
            await self.pilot.pause()
            self.assertFalse(details.display)

    async def test_native_collision_reports_error_and_preserves_native_command(self):
        with patch.object(self.app, 'show_input_error', new_callable=AsyncMock) as error:
            self.app.update_agent_info({"skills": SKILLS + [{"name": "new", "description": "A skill named new", "arguments": {}}]})
            await self.pilot.pause()
            self.assertIn('/new', error.call_args.args[0])
        menu = self.app.query_one(CommandSuggestions)
        await self.draft('/')
        self.assertIn('/new', menu.matches)
        self.assertIn('/check-api', menu.matches)
        self.assertNotIn('/new', menu.catalog)
        await self.draft('/new')
        await self.pilot.press('enter')
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['type'], 'session.new')
        self.assertNotIn('skill', request)

    async def test_unknown_skill_and_old_prefix_keep_draft(self):
        for text in ('/missing', '//check-api directory=src'):
            await self.draft(text)
            await self.pilot.press('enter')
            self.assertEqual(self.entry.text, text)
            self.app.client.send.assert_not_awaited()

    async def test_submit_sends_structured_invocation_and_keeps_original_text(self):
        text = '/check-api "src/http"'
        await self.draft(text)
        await self.pilot.press('enter')
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request['type'], 'message.send')
        self.assertEqual(request['text'], text)
        self.assertEqual(request['skill'], {'name': 'check-api', 'input': '"src/http"'})

    async def test_missing_arguments_are_sent_for_skill_interpretation(self):
        await self.draft('/check-api')
        await self.pilot.press('enter')
        self.assertEqual(self.app.client.send.call_args.args[0]['skill'], {'name': 'check-api', 'input': ''})

    async def test_busy_skill_is_queued_not_sent_concurrently(self):
        self.app.busy = True
        await self.draft('/check-api src/http')
        await self.pilot.press('enter')
        self.app.client.send.assert_not_awaited()
        self.assertEqual(self.entry.text, '')

    async def test_removed_queued_skill_is_not_silently_sent_as_plain_text(self):
        self.app.update_agent_info({"skills": []})
        await self.app.send_user_message('/check-api src/http')
        self.app.client.send.assert_not_awaited()
        self.assertEqual(self.entry.text, '/check-api src/http')
        self.assertFalse(self.app.busy)

    async def test_live_skill_title_and_distinct_activity_square_are_algae(self):
        self.app.display_message({"id": "call", "role": "assistant", "content": [
            {"type": "tool_call", "toolName": "skill", "callId": "skill-1", "arguments": {"name": "check-api"}}]})
        await self.pilot.pause()
        drawer = self.app.current_activity
        self.assertTrue(drawer.toggle.has_skills)
        self.assertFalse(drawer.toggle.has_tools)
        self.assertEqual([span.style for span in drawer.toggle.render().spans], [ALGAE_MD])
        view = self.app.query_one(ToolView)
        self.assertEqual(view.query_one("CollapsibleTitle").styles.color, Color.parse(ALGAE_MD))
        self.app.display_message({"id": "bash", "role": "assistant", "content": [
            {"type": "tool_call", "toolName": "bash", "callId": "bash-1", "arguments": {"command": "true"}}]})
        await self.pilot.pause()
        self.assertEqual([span.style for span in drawer.toggle.render().spans], [BOLD_MD, ALGAE_MD])

    async def test_resumed_lazy_skill_keeps_algae_title_and_square(self):
        record = {"id": "group", "role": "activity", "kinds": ["skill"], "count": 1,
                  "activities": [{"kind": "tool", "toolName": "skill", "callId": "c", "activityIndex": 0}]}
        viewport = self.app.query_one(HistoryViewport)
        await viewport.replace({"session": {"id": "s"}, "page": {}, "messages": [record]})
        drawer = self.app.query_one(HistoryActivity)
        await drawer.populate(record)
        await self.pilot.pause()
        self.assertTrue(drawer.toggle.has_skills)
        self.assertFalse(drawer.toggle.has_tools)
        self.assertEqual([span.style for span in drawer.toggle.render().spans], [ALGAE_MD])
        self.assertEqual(self.app.query_one(ToolView).query_one("CollapsibleTitle").styles.color, Color.parse(ALGAE_MD))
