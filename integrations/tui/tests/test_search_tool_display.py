"""Glob and grep share the compact path/pattern argument presentation."""
import unittest

from textual.app import App
from textual.widgets import Collapsible, Rule, Static
from rich.console import Console
from rich.style import Style

from sos_tui.config import DEPTH_MD, MISC_MD, MUTED_COLOR
from sos_tui.history_detail import HistoryToolView
from sos_tui.tool_file_paths import ToolPathTitle
from sos_tui.tool_view import ToolView, format_arguments


class SearchToolDisplayTest(unittest.IsolatedAsyncioTestCase):
    async def test_grep_title_has_muted_pattern_and_truncates_at_the_end_on_resize(self):
        await self.check_search_title("grep", "TODO|FIXME|")

    async def test_glob_title_has_muted_pattern_and_truncates_at_the_end_on_resize(self):
        await self.check_search_title("glob", "**/sources/")

    async def check_search_title(self, tool, prefix):
        pattern = prefix + "long_pattern" * 12
        view = ToolView("search", tool, {"path": "src", "pattern": pattern})
        saved = HistoryToolView("group", {"callId": "search", "toolName": tool, "activityIndex": 0,
                                         "arguments": {"pattern": pattern}})
        class SearchApp(App):
            def compose(self):
                yield view
                yield saved

        async with SearchApp().run_test(size=(70, 20)) as pilot:
            view.record_permission({"permissionId": "p", "decision": "ai_accepted", "confidence": .95})
            view.set_result({"isError": False, "output": "No files found"})
            await pilot.pause()
            self.assertEqual(view.title_text().plain, tool + " • AI 95% • Completed • " + pattern)
            for drawer in (view, saved):
                title = drawer.query_one(ToolPathTitle)
                line = title.render_line(0)
                self.assertIn(" • " + prefix, line.text)
                self.assertTrue(line.text.rstrip().endswith("..."))
                self.assertNotIn("src", line.text)
                text = drawer.title_text(65)
                style = text.get_style_at_offset(Console(), text.plain.index(prefix))
                self.assertEqual(style.color, Style.parse(MUTED_COLOR).color)
                self.assertFalse(style.bold)
                self.assertLessEqual(text.cell_len, 65)
            await pilot.resize_terminal(240, 20)
            await pilot.pause()
            self.assertIn(pattern, view.query_one(ToolPathTitle).render_line(0).text)
            view.collapsed = False
            await pilot.pause()
            arguments = view.arguments_view.content
            self.assertEqual(arguments.get_style_at_offset(Console(), 0).color, Style.parse(DEPTH_MD).color)
            self.assertEqual(arguments.get_style_at_offset(Console(), 4).color, Style.parse(MISC_MD).color)
            title = view.title_text()
            self.assertEqual(title.get_style_at_offset(Console(), title.plain.index(prefix)).color,
                             Style.parse(MUTED_COLOR).color)

    def test_glob_arguments_match_grep_with_default_or_explicit_path(self):
        for arguments in ({"pattern": "**/*.cpp"}, {"path": "src", "pattern": "**/[ab].cpp"}):
            glob = format_arguments(arguments, "glob")
            self.assertEqual(glob, format_arguments(arguments, "grep"))
            self.assertEqual(glob.plain, f"{arguments.get('path', '.')}\n{arguments['pattern']}")
            self.assertEqual(glob.spans[0].style, DEPTH_MD)

    async def test_expanded_glob_has_no_generic_arguments_heading_or_field_labels(self):
        for lazy in (False, True):
            view = ToolView("search", "glob", {"path": "src", "pattern": "**/*.cpp"}, lazy_arguments=lazy)
            view.set_result({"output": "src/a.cpp\nsrc/b.cpp", "isError": False, "metadata": {}})

            class SearchApp(App):
                def compose(self):
                    yield view

            async with SearchApp().run_test() as pilot:
                view.collapsed = False
                await pilot.pause()
                self.assertTrue(view.rendered)
                contents = view.query_one(Collapsible.Contents)
                self.assertIs(contents.children[0], view.arguments_view)
                self.assertIsInstance(contents.children[1], Rule)
                self.assertEqual(str(view.arguments_view.render()), "src\n**/*.cpp")
                self.assertEqual(str(view.results.query_one(Static).render()), "src/a.cpp\nsrc/b.cpp")
                self.assertFalse(any(str(widget.render()) == "Arguments" for widget in view.query(Static)))
