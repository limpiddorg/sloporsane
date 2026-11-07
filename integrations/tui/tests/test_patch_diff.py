"""Patch reviews display historical result diffs, never the current checkout."""

import unittest

from rich.console import Console
from rich.style import Style
from textual.app import App
from textual.widgets import Collapsible, Static

from sos_tui.patch_diff import diff_rows, diff_table, patch_widgets, highlighted_cells
from sos_tui.tool_view import ToolView, result_widgets
from sos_tui.history_detail import HistoryToolView
from sos_tui.config import MUTED_COLOR, MISC_MD, ALGAE_MD
from sos_tui.code_colors import ADDED_BACKGROUND, REMOVED_BACKGROUND

PATCH = "--- a/file.py\n+++ b/file.py\n@@ -10,3 +10,4 @@\n same\n-old\n+new\n+extra\n tail\n"
RESULT = {"isError": False, "output": "Success", "metadata": {"files": [
    {"relativePath": "src/file.py", "type": "update", "patch": PATCH}]}}


class PatchDiffTest(unittest.TestCase):
    def test_syntax_colors_remain_on_sane_diff_backgrounds(self):
        patch = '@@ -1 +1 @@\n-return "old"\n+return "new"\n'
        console = Console(width=100)
        segments = list(console.render(diff_table(patch, "file.py")))
        for background, literal in ((REMOVED_BACKGROUND, "old"), (ADDED_BACKGROUND, "new")):
            expected_background = Style.parse(f"on {background}").bgcolor
            keywords = [s for s in segments if "return" in s.text]
            self.assertTrue(any(s.style.color == Style.parse(MISC_MD).color and
                                s.style.bgcolor == expected_background for s in keywords))
            strings = [s for s in segments if literal in s.text]
            self.assertTrue(any(s.style.color == Style.parse(ALGAE_MD).color and
                                s.style.bgcolor == expected_background for s in strings))
            self.assertTrue(any(s.text.isspace() and s.style and s.style.bgcolor == expected_background
                                for s in segments))  # Background extends beyond the code.

    def test_multiline_syntax_is_independent_per_side_and_resets_at_hunks(self):
        patch = '@@ -1,2 +1,3 @@\n-value = 1\n+value = """new\n+body\n return value\n@@ -30 +31 @@\n return value\n'
        rows = diff_rows(patch)
        syntax = highlighted_cells(rows, "file.py")
        console = Console()
        context = next(i for i, row in enumerate(rows) if row[0] and row[0][1] == "return value")
        self.assertEqual(syntax[context, 0].get_style_at_offset(console, 0).color, Style.parse(MISC_MD).color)
        self.assertEqual(syntax[context, 1].get_style_at_offset(console, 0).color, Style.parse(ALGAE_MD).color)
        self.assertEqual(syntax[len(rows) - 1, 1].get_style_at_offset(console, 0).color, Style.parse(MISC_MD).color)

    def test_line_numbers_use_muted_color(self):
        console = Console(width=100)
        numbers = [segment for segment in console.render(diff_table(PATCH)) if segment.text.strip().isdigit()]
        self.assertTrue(numbers)
        for segment in numbers:
            self.assertEqual(segment.style.color, Style.parse(MUTED_COLOR).color)

    def test_edit_uses_result_diff_and_filepath_not_replacement_arguments(self):
        result = {"isError": False, "output": "Success", "metadata": {
            "filepath": "/repo/file.py", "diff": PATCH}}
        widgets = result_widgets("edit", {"filePath": "other.py", "oldString": "not the full file"}, result)
        self.assertEqual(str(widgets[0].content), "/repo/file.py")
        self.assertTrue(widgets[1].has_class("tool-patch-diff"))
        self.assertEqual(widgets[1].content.row_count, len(diff_rows(PATCH)))
        result["metadata"].pop("filepath")
        widgets = result_widgets("edit", {"filePath": "fallback.py"}, result)
        self.assertEqual(str(widgets[0].content), "fallback.py")

    def test_edit_error_or_missing_diff_keeps_output(self):
        for result in ({"isError": True, "output": "Could not find oldString", "metadata": {"diff": PATCH}},
                       {"isError": False, "output": "Edit applied successfully.", "metadata": {}}):
            widgets = result_widgets("edit", {}, result)
            self.assertEqual(str(widgets[0].content), result["output"])

    def test_replacements_align_with_context_and_numbering(self):
        rows = diff_rows(PATCH)
        self.assertEqual(rows[1], ((10, "same", "context"), (10, "same", "context")))
        self.assertEqual(rows[2], ((11, "old", "removed"), (11, "new", "added")))
        self.assertEqual(rows[3], (None, (12, "extra", "added")))
        self.assertEqual(rows[4], ((12, "tail", "context"), (13, "tail", "context")))

    def test_creation_deletion_multiple_hunks_and_literal_header_like_source(self):
        self.assertEqual(diff_rows("@@ -0,0 +1,1 @@\n+created\n")[-1],
                         (None, (1, "created", "added")))
        self.assertEqual(diff_rows("@@ -1 +0,0 @@\n-deleted\n")[-1],
                         ((1, "deleted", "removed"), None))
        rows = diff_rows("@@ -1 +1 @@\n---literal\n+++literal\n@@ -50 +51 @@\n end\n")
        self.assertEqual(rows[1], ((1, "--literal", "removed"), (1, "++literal", "added")))
        self.assertEqual(rows[-1], ((50, "end", "context"), (51, "end", "context")))

    def test_multiple_files_and_move_header(self):
        widgets = patch_widgets({"files": [
            *RESULT["metadata"]["files"],
            {"relativePath": "new.py", "filePath": "/repo/old.py", "type": "move", "patch": ""}]})
        self.assertEqual(str(widgets[0].content), "src/file.py")
        self.assertEqual(str(widgets[2].content), "new.py")
        self.assertEqual(str(widgets[3].content), "No textual changes")

    def test_failure_and_legacy_results_keep_their_output(self):
        for result in ({"isError": True, "output": "Invalid patch", "metadata": RESULT["metadata"]},
                       {"isError": False, "output": "Legacy output", "metadata": {}}):
            widgets = result_widgets("apply_patch", {}, result)
            self.assertEqual(str(widgets[0].content), result["output"])

    def test_narrow_columns_wrap_without_losing_long_lines(self):
        table = diff_table("@@ -1 +1 @@\n-" + "x" * 100 + "\n+after\n")
        console = Console(width=44, force_terminal=False)
        with console.capture() as captured:
            console.print(table)
        rendered = captured.get()
        self.assertNotIn("Before", rendered)
        self.assertNotIn("After", rendered)
        self.assertIn("│", rendered)
        self.assertIn("- ", rendered)
        self.assertIn("+ after", rendered)
        self.assertIn("@@", rendered)
        self.assertEqual(rendered.count("x"), 100)
        self.assertTrue(all(len(line) <= 44 for line in rendered.splitlines()))


class PatchDiffLiveTest(unittest.IsolatedAsyncioTestCase):
    async def test_edit_live_and_lazy_history(self):
        await self.check_live_and_history("edit", {"filePath": "file.py", "oldString": "old", "newString": "new"},
            {"isError": False, "output": "Edit applied successfully.",
             "metadata": {"filepath": "file.py", "diff": PATCH}})

    async def test_expanding_live_and_lazy_history_shows_same_diff(self):
        await self.check_live_and_history("apply_patch", {"patchText": "raw patch"}, RESULT)

    async def check_live_and_history(self, tool, arguments, result):
        class TestApp(App):
            def compose(self):
                yield ToolView("call", tool, arguments)
                yield HistoryToolView("group", {"activityIndex": 0, "toolName": tool, "callId": "saved"})

        app = TestApp()
        async with app.run_test(size=(100, 40)) as pilot:
            live, history = list(app.query(ToolView))
            live.set_result(result)
            live.collapsed = False
            await pilot.pause()
            self.assertEqual(len(live.query(".tool-patch-diff")), 1)
            self.assertFalse(live.query(".tool-arguments"))
            self.assertEqual(len(live.query(Collapsible)), 0)
            self.assertEqual([child for child in live.query_one(Collapsible.Contents).children if child.display], [live.results])
            self.assertEqual(live.arguments, arguments)
            history.collapsed = False
            await pilot.pause()
            self.assertIn("Loading details", str(history.results.query_one(Static).content))
            await history.fail_detail("Disconnected")
            self.assertIn("Disconnected", str(history.results.query_one(Static).content))
            await history.load_detail({"kind": "tool", "toolName": tool, "callId": "saved",
                                       "arguments": arguments, "result": result})
            history.collapsed = False
            await pilot.pause()
            self.assertEqual(len(history.query(".tool-patch-diff")), 1)
            self.assertFalse(history.query(".tool-arguments"))
            self.assertEqual(str(live.results.query(Static).first().content),
                             str(history.results.query(Static).first().content))
