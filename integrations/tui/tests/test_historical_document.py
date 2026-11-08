"""Historical diff opening stays bounded while whole-file syntax prepares off-thread."""
from itertools import accumulate
import threading
import unittest
from unittest.mock import patch

from rich.console import Console
from rich.text import Text
from textual.app import App
from textual.widgets import Static

from sos_tui.code_colors import highlighted_lines, ADDED_BACKGROUND, REMOVED_BACKGROUND
from sos_tui.config import ALGAE_MD, BOLD_MD, MISC_MD
from sos_tui.historical_document import HistoricalDocument
from sos_tui.implementation_code import ChangeZone, HistoricalCode, change_zones


class CodeApp(App):
    def compose(self):
        with HistoricalCode():
            yield Static(markup=False)


class HistoricalDocumentTest(unittest.TestCase):
    def test_string_measurements_match_rich_wrapping_including_tabs_and_wide_characters(self):
        source = ["", "short", "word " * 30, "x" * 150, "\tvalue", "界\tvalue", "界" * 30,
                  "    " * 20, "longword" * 10 + "  tail", "é 😀 words " * 15]
        rows = [(i + 1, line, "") for i, line in enumerate(source)]
        document = HistoricalDocument("file.txt", rows)
        console = Console()
        for pane_width in (9, 24, 80):
            width, (window, _) = document.layout(pane_width)
            expected = [len(Text(line).wrap(console, width, overflow="fold")) for line in source]
            self.assertEqual(window.offsets, [0, *accumulate(expected)])

    def test_zones_expose_their_full_hunk_extent_and_own_explanation(self):
        zones = change_zones([{"path": "file.py", "after": "a\nb\nc\nd\ne\nf\ng\nh\ni\n",
                               "patch": "@@ -2 +2 @@\n-old\n+b\n@@ -8 +8 @@\n-old\n+h\n",
                               "hunk_order": [2, 1], "hunk_explanations": ["First", "Second"]}])
        self.assertEqual([zone.explanation for zone in zones], ["Second", "First"])
        self.assertEqual([zone.end - zone.center for zone in zones], [2, 2])
        self.assertIs(zones[0].rows, zones[1].rows)


class HistoricalCodeTest(unittest.IsolatedAsyncioTestCase):
    async def test_first_paint_does_not_wait_for_syntax_or_wrap_the_whole_file(self):
        release = threading.Event()
        original = highlighted_lines
        def delayed(*args):
            release.wait(10)
            return original(*args)
        app = CodeApp()
        async with app.run_test(size=(100, 35)) as pilot:
            code = app.query_one(HistoricalCode)
            zone = ChangeZone("large.py", [(i + 1, f"value = {i}", "") for i in range(30000)], 15000, "file")
            try:
                with patch("sos_tui.implementation_code.highlighted_lines", side_effect=delayed):
                    code.show_zone(zone)
                    self.assertIsNone(code.prepared.syntax)
                    self.assertLess(len(code.wrap_cache), 200)
                    self.assertEqual(code.document.total, 30000)
                    await pilot.pause()
                    self.assertIn("value = 15000", code.query_one(Static).render().plain)
                    self.assertIsNone(code.prepared.syntax)
                    position = code.scroll_y
                    release.set()
                    await app.workers.wait_for_complete()
                    self.assertIsNotNone(code.prepared.syntax)
                    self.assertEqual(code.scroll_y, position)
            finally:
                release.set()

    async def test_hunks_share_layout_and_wrapped_lines_but_not_selected_gutter(self):
        app = CodeApp()
        async with app.run_test(size=(80, 20)) as pilot:
            code = app.query_one(HistoricalCode)
            rows = [(i + 1, f"line {i}", ALGAE_MD if i == 30 else BOLD_MD if i == 40 else "") for i in range(100)]
            code.show_zone(ChangeZone("file.txt", rows, 30, "first", end=32))
            await app.workers.wait_for_complete()
            await pilot.pause()
            prepared, geometry, cache = code.prepared, code.document, code.wrap_cache
            wrapped = cache[30]
            code.show_zone(ChangeZone("file.txt", rows.copy(), 40, "second", end=42))
            self.assertIs(code.prepared, prepared)
            self.assertIs(code.document, geometry)
            self.assertIs(code.wrap_cache, cache)
            self.assertIs(cache[30], wrapped)
            text = code.query_one(Static).render()
            current = text.plain.index("line 40")
            added = text.plain.index("line 30")
            current_row = text.plain.rfind("\n", 0, current) + 1
            added_row = text.plain.rfind("\n", 0, added) + 1
            self.assertEqual(text.get_style_at_offset(current_row).background.hex.upper(), "#3A4655")
            self.assertNotEqual(text.get_style_at_offset(added_row).background.hex.upper(), "#3A4655")
            self.assertEqual(text.get_style_at_offset(added).background.hex.upper(), ADDED_BACKGROUND.upper())
            self.assertEqual(text.get_style_at_offset(current).background.hex.upper(), REMOVED_BACKGROUND.upper())

    async def test_background_syntax_preserves_multiline_state_and_never_replaces_a_new_document(self):
        app = CodeApp()
        async with app.run_test(size=(80, 20)) as pilot:
            code = app.query_one(HistoricalCode)
            source = ['text = """', *[f"inside string {i}" for i in range(100)], '"""', 'value = 42']
            rows = [(i + 1, line, "") for i, line in enumerate(source)]
            code.show_zone(ChangeZone("file.py", rows, 50, "first"))
            first = code.prepared
            code.show_zone(ChangeZone("other.py", [(1, "other = 1", "")], 0, "second"))
            second = code.prepared
            await app.workers.wait_for_complete()
            await pilot.pause()
            self.assertIs(code.prepared, second)
            self.assertIn("other = 1", code.query_one(Static).render().plain)
            self.assertNotIn("inside string", code.query_one(Static).render().plain)
            self.assertEqual(first.syntax[50].spans, highlighted_lines("file.py", "\n".join(source))[50].spans)
            code.show_zone(ChangeZone("file.py", rows, 50, "first"))
            self.assertIs(code.prepared, first)
            self.assertIsNotNone(first.syntax)
            code.show_zone(ChangeZone("file.py", [(1, "different snapshot", "")], 0, "new"))
            self.assertIsNot(code.prepared, first)

    async def test_only_modified_gutters_use_version_colors_and_code_spacing_is_aligned(self):
        app = CodeApp()
        async with app.run_test(size=(40, 20)) as pilot:
            code = app.query_one(HistoricalCode)
            rows = [(1, "before", ""), (None, "removed " + "x" * 70, BOLD_MD),
                    (2, "added " + "y" * 70, ALGAE_MD), (3, "after", ""),
                    (4, "other hunk", ALGAE_MD)]
            code.show_zone(ChangeZone("file.txt", rows, 0, "selected", end=4))
            await app.workers.wait_for_complete()
            await pilot.pause()
            text = code.query_one(Static).render()
            lines = iter(text.plain.splitlines(keepends=True))
            offset = 0
            digits = code.prepared.digits
            self.assertEqual(code.code_width, code.rendered_width - digits - 4)
            for index, (number, _, style) in enumerate(rows):
                sign = "+" if style == ALGAE_MD else "-" if style == BOLD_MD else " "
                expected_background = ADDED_BACKGROUND if sign == "+" else REMOVED_BACKGROUND if sign == "-" else None
                for continuation in range(len(code.wrap_cache[index])):
                    line = next(lines)
                    number_text = str(number) if number is not None and not continuation else ""
                    self.assertTrue(line.startswith(f'{number_text:>{digits}} ' + (sign if not continuation else " ") + "  "))
                    gutter_background = text.get_style_at_offset(offset).background
                    self.assertEqual(gutter_background.hex.upper() if gutter_background else None,
                                     "#3A4655" if index in (1, 2) else expected_background)
                    if index in (1, 2):
                        self.assertEqual(text.get_style_at_offset(offset).foreground.hex.upper(), MISC_MD.upper())
                    for column in range(digits + 1, digits + 4):
                        background = text.get_style_at_offset(offset + column).background
                        self.assertEqual(background.hex.upper() if background else None, expected_background)
                    self.assertLessEqual(len(line.rstrip("\n")), code.rendered_width)
                    offset += len(line)
            self.assertEqual(code.max_scroll_x, 0)
