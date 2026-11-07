"""Write drawers contain the recorded path and source, without argument/result headings."""

import unittest
from rich.console import Console
from rich.style import Style
from rich.text import Text

from textual.app import App
from textual.widgets import Collapsible, Static

from sos_tui.config import DEPTH_MD, MISC_MD
from sos_tui.code_colors import highlighted_lines
from sos_tui.history_detail import HistoryToolView
from sos_tui.tool_view import ToolView, result_widgets


class WriteViewTest(unittest.TestCase):
    def test_write_uses_file_palette_not_chat_theme(self):
        source = 'return "hello"\n'
        widgets = result_widgets("write", {"filePath": "file.py", "content": source}, {
            "isError": False, "output": "Success"})
        code = widgets[1].content
        expected = Text("\n").join(highlighted_lines("file.py", source))
        self.assertEqual(code.plain, expected.plain)
        self.assertEqual(code.spans, expected.spans)
        self.assertEqual(code.get_style_at_offset(Console(), 0).color, Style.parse(MISC_MD).color)

    def test_path_and_complete_content_instead_of_diff(self):
        for source in ("", "first\nsecond\n", "[bold]literal[/bold]"):
            widgets = result_widgets("write", {"filePath": "relative.txt", "content": source}, {
                "isError": False, "output": "Success", "metadata": {"filepath": "/repo/saved.txt", "diff": "+not the source"}})
            self.assertEqual(len(widgets), 2)
            self.assertEqual(str(widgets[0].content), "/repo/saved.txt")
            self.assertEqual(widgets[0].content.style, DEPTH_MD)
            self.assertEqual(str(widgets[1].content), source)

    def test_path_falls_back_to_arguments_and_errors_stay_visible(self):
        widgets = result_widgets("write", {"filePath": "new.py", "content": "x = 1"}, {
            "isError": False, "output": "Success"})
        self.assertEqual(str(widgets[0].content), "new.py")
        widgets = result_widgets("write", {"filePath": "new.py", "content": "unwritten"}, {
            "isError": True, "output": "Permission denied"})
        self.assertEqual(len(widgets), 1)
        self.assertEqual(str(widgets[0].content), "Permission denied")


class WriteViewLiveTest(unittest.IsolatedAsyncioTestCase):
    async def test_live_and_lazy_restore_have_only_path_and_recorded_content(self):
        arguments = {"filePath": "file.py", "content": "value = 42\n"}
        result = {"isError": False, "output": "Wrote file successfully.", "metadata": {"filepath": "file.py"}}

        class TestApp(App):
            def compose(self):
                yield ToolView("live", "write", arguments)
                yield HistoryToolView("group", {"activityIndex": 0, "toolName": "write", "callId": "saved"})

        app = TestApp()
        async with app.run_test(size=(100, 40)) as pilot:
            live, history = list(app.query(ToolView))
            live.set_result(result)
            live.collapsed = history.collapsed = False
            await pilot.pause()
            self.assertIn("Loading details", str(history.results.query_one(Static).content))
            await history.fail_detail("Disconnected")
            self.assertIn("Disconnected", str(history.results.query_one(Static).content))
            await history.load_detail({"kind": "tool", "toolName": "write", "callId": "saved",
                                       "arguments": arguments, "result": result})
            await pilot.pause()
            for view in (live, history):
                self.assertFalse(view.query(".tool-arguments"))
                self.assertEqual(list(view.query_one(Collapsible.Contents).children), [view.results])
                path, source = list(view.results.query(Static))
                self.assertEqual(str(path.content), "file.py")
                self.assertEqual(str(source.content), arguments["content"])
