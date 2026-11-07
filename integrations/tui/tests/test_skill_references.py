"""Skill colors are a rendering layer, not edits or command interpretation."""

import unittest
from unittest.mock import patch

from textual.document._document import Selection

from sos_tui.app import SosApp
from sos_tui.command_suggestions import CommandSuggestions
from sos_tui.config import ALGAE_MD, TITLE_MD, TEXT_COLOR, INPUT_QUOTE_BACKGROUND_COLOR
from sos_tui.images import ImagePaste
from sos_tui.message_input import MessageInput
from sos_tui.paste_blocks import PathPaste
from sos_tui.skills import skill_reference_spans
from rich.style import Style


SKILLS = [{"name": "work", "arguments": ["request"], "argument-hint": "[request...]"},
          {"name": "check-api", "arguments": []}]


class SkillReferenceRenderTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(80, 30)))
        self.app.connected = True
        self.app.update_agent_info({"skills": SKILLS})
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.menu = self.app.query_one(CommandSuggestions)
        await self.pilot.pause()

    async def draft(self, text):
        self.entry.load_text(text)
        self.entry.move_cursor(self.entry.document.end)
        await self.pilot.pause()

    def color_at(self, row, column):
        return self.entry.get_line(row).get_style_at_offset(self.app.console, column).color

    def rendered_cells(self):
        return [(character, segment.style) for y in range(self.entry.size.height)
                for segment in self.entry.render_line(y) for character in segment.text]

    async def test_only_complete_references_have_algae_not_arguments(self):
        for text in ('/work request', 'Use /work request and /check-api, please',
                     'Use /unknown /work-extra /work_extra then /work.'):
            await self.draft(text)
            ranges = skill_reference_spans(text, SKILLS)
            for column in range(len(text)):
                with self.subTest(text=text, column=column):
                    self.assertEqual(self.color_at(0, column) == Style(color=ALGAE_MD).color,
                                     any(left <= column < right for left, right in ranges))
            self.assertEqual(self.entry.text, text)

    async def test_inline_and_multiline_have_skill_suggestions_without_argument_hints(self):
        await self.draft('Please use /work\nand then /check-api')
        self.assertEqual(self.color_at(0, 11), Style(color=ALGAE_MD).color)
        self.assertEqual(self.color_at(1, 9), Style(color=ALGAE_MD).color)
        self.assertTrue(self.menu.display)
        self.assertEqual(self.entry.suggestion, '')
        self.assertEqual(self.entry.command_text, '')

    async def test_initial_argument_hint_is_preserved(self):
        await self.draft('  /work')
        self.assertEqual(self.entry.suggestion, ' [request...]')
        self.assertEqual(self.color_at(0, 2), Style(color=ALGAE_MD).color)

    async def test_catalog_refresh_recolors_cached_render_without_editing(self):
        await self.draft('Use /work please')
        # Keep the composer focused so its cursor style is stable across refreshes.
        self.entry.focus()
        await self.pilot.pause()
        before = self.rendered_cells()
        self.assertTrue(any(style and style.color == Style(color=ALGAE_MD).color
                            for _, style in before))
        text, selection, scroll = self.entry.text, self.entry.selection, self.entry.scroll_offset
        self.app.update_agent_info({'skills': []})
        await self.pilot.pause()
        self.assertFalse(any(style and style.color == Style(color=ALGAE_MD).color
                             for _, style in self.rendered_cells()))
        self.app.update_agent_info({'skills': SKILLS})
        await self.pilot.pause()
        self.assertEqual(self.rendered_cells(), before)
        self.assertEqual((self.entry.text, self.entry.selection, self.entry.scroll_offset),
                         (text, selection, scroll))

    async def test_wrapping_selection_cursor_and_scroll_are_unchanged(self):
        await self.draft('Before ' + 'words ' * 9 + '/check-api after\n' + 'next line\n' * 8)
        self.entry.selection = Selection((0, 3), (0, 66))
        await self.pilot.pause()
        state = (self.entry.text, self.entry.selection, self.entry.cursor_screen_offset,
                 self.entry.scroll_offset, self.entry.wrapped_document.height, self.entry.selected_text)
        before = self.rendered_cells()
        self.app.update_agent_info({'skills': []})
        await self.pilot.pause()
        after = self.rendered_cells()
        self.assertEqual([char for char, _ in before], [char for char, _ in after])
        self.assertEqual([style.bgcolor if style else None for _, style in before],
                         [style.bgcolor if style else None for _, style in after])
        self.assertEqual((self.entry.text, self.entry.selection, self.entry.cursor_screen_offset,
                          self.entry.scroll_offset, self.entry.wrapped_document.height, self.entry.selected_text), state)
        self.app.update_agent_info({'skills': SKILLS})
        self.entry.selection = Selection.cursor((0, 62))
        await self.pilot.pause()
        cursor_state = (self.entry.cursor_location, self.entry.cursor_screen_offset, self.entry.scroll_offset)
        self.entry.render_line(0)
        self.assertEqual((self.entry.cursor_location, self.entry.cursor_screen_offset, self.entry.scroll_offset), cursor_state)

    async def test_wrapped_reference_is_colored_in_rendered_segments(self):
        await self.draft('words ' * 10 + '/check-api after')
        self.entry.blur()
        await self.pilot.pause()
        colored = ''.join(char for char, style in self.rendered_cells()
                          if style and style.color == Style(color=ALGAE_MD).color)
        self.assertEqual(colored, '/check-api')
        self.assertGreater(self.entry.wrapped_document.height, 1)

    async def test_quotes_keep_their_existing_foreground_and_background(self):
        await self.draft('> Use /work\nOutside /work')
        self.entry.blur()
        await self.pilot.pause()
        quoted = list(self.entry.render_line(0))
        self.assertTrue(self.entry.render_line(0).text.startswith('▌'))
        self.assertTrue(any('/work' in segment.text and segment.style.color == Style(color=TEXT_COLOR).color
                            and segment.style.bgcolor == Style(bgcolor=INPUT_QUOTE_BACKGROUND_COLOR).bgcolor
                            for segment in quoted))
        self.assertEqual(self.color_at(1, 8), Style(color=ALGAE_MD).color)
        self.assertEqual(self.entry.text, '> Use /work\nOutside /work')
        self.assertEqual(self.entry.command_text, '')

    async def test_visible_pasted_reference_is_colored_but_not_a_command(self):
        self.entry.paste_text('/work')
        await self.pilot.pause()
        ranges = self.entry.pasted_ranges
        self.assertEqual(self.color_at(0, 0), Style(color=ALGAE_MD).color)
        self.assertEqual(self.entry.command_text, '')
        self.assertEqual(self.entry.pasted_ranges, ranges)
        self.assertEqual(self.entry.expanded_text, '/work')

    async def test_compacted_pastes_and_attachments_keep_title_and_payloads(self):
        self.entry.paste_text('/work\nsecond\nthird')
        self.entry.add_attachments((PathPaste('/work'),
                                   ImagePaste(b'image', 'image/png', 'test', '/work', 1, 1)))
        await self.pilot.pause()
        blocks = self.entry.paste_blocks
        text, expanded = self.entry.text, self.entry.expanded_text
        for block in blocks:
            for column in range(block.start, block.end):
                self.assertEqual(self.color_at(0, column), Style(color=TITLE_MD).color)
        self.assertEqual(self.entry.command_text, '')
        self.assertFalse(any(span.style == ALGAE_MD for span in self.entry.get_line(0).spans))
        self.entry.selection = Selection((0, blocks[0].start + 1), (0, blocks[0].end - 1))
        self.assertEqual(self.entry.selected_text, blocks[0].paste.text)
        self.assertEqual((self.entry.text, self.entry.expanded_text, self.entry.paste_blocks),
                         (text, expanded, blocks))
