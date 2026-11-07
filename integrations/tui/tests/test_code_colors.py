"""Chat fences and file views share one syntax palette and token renderer."""

import unittest

from rich.console import Console
from rich.style import Style
from rich.text import Text
from textual.content import Content

from sos_tui.code_colors import highlighted_code, highlighted_lines
from sos_tui.config import MISC_MD, MUTED_COLOR
from sos_tui.response_markdown import CodeFence


class CodeColorsTest(unittest.TestCase):
    def test_chat_and_file_views_have_identical_styles(self):
        source = '# comment\nclass Beach:\n    def wave(self):\n        return "algae" + str(42)\n'
        file = Content.from_rich_text(Text("\n").join(highlighted_lines("file.py", source)))
        chat = CodeFence.highlight(source, "python")
        self.assertEqual(str(chat), source)
        def colored_characters(content):
            return [(char, segment.style) for segment in content.render_segments()
                    for char in segment.text if char != '\n']
        self.assertEqual(colored_characters(chat), colored_characters(file))
        colors = {segment.style.color.name for segment in chat.render_segments() if segment.text}
        self.assertIn(MUTED_COLOR.lower(), colors)
        self.assertIn(MISC_MD.lower(), colors)

    def test_language_aliases_and_unknown_or_missing_language(self):
        self.assertEqual(highlighted_code('return 42', language='py').spans,
                         highlighted_code('return 42', path='file.py').spans)
        for language in ('unknown-language', '', None):
            code = CodeFence.highlight('[bold]literal[/bold]\n', language)
            self.assertEqual(str(code), '[bold]literal[/bold]\n')
        self.assertEqual(str(CodeFence.highlight('', 'python')), '')

    def test_multiline_strings_and_keyword_use_file_palette(self):
        code = highlighted_code('"""one\ntwo"""\nreturn 1', language='python')
        file = Text("\n").join(highlighted_lines('file.py', code.plain))
        console = Console()
        for index, char in enumerate(code.plain):
            if char != '\n':
                self.assertEqual(code.get_style_at_offset(console, index), file.get_style_at_offset(console, index))
        keyword = code.plain.index('return')
        self.assertEqual(code.get_style_at_offset(Console(), keyword).color, Style.parse(MISC_MD).color)
