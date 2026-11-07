"""A multifile patch has independent drawers and width-aware file titles."""
import unittest
from unittest.mock import patch

from textual.app import App
from textual.widgets import Collapsible
from rich.console import Console
from rich.style import Style

from sos_tui.config import DEPTH_MD, MUTED_COLOR
from sos_tui.history_detail import ActivityDetailRequested, HistoryToolView
from sos_tui.tool_file_paths import ToolPathTitle, tool_files
from sos_tui.tool_view import ReadView, ToolView, result_widgets
from sos_tui.workspace_path import WorkspacePath
from sos_tui.turn_activity import TurnActivity


PATCH = """*** Begin Patch
*** Update File: src/first.py
@@
-old
+first
*** Add File: src/second.py
+second
*** End Patch"""
FILES = [{"filePath": "/repo/src/first.py", "relativePath": "src/first.py", "type": "update",
          "patch": "--- first.py\n+++ first.py\n@@ -1 +1 @@\n-old\n+first\n"},
         {"filePath": "/repo/src/second.py", "relativePath": "src/second.py", "type": "add",
          "patch": "--- second.py\n+++ second.py\n@@ -0,0 +1 @@\n+second\n"}]
RESULT = {"isError": False, "output": "Success", "metadata": {"files": FILES}}


class FilePathLogicTest(unittest.TestCase):
    def test_invalid_tool_arguments_do_not_break_failure_display(self):
        self.assertEqual(tool_files("apply_patch", {"patchText": 42}), [])
        self.assertEqual(tool_files("write", {"filePath": []}), [])

    def test_patch_headers_and_move_destination_are_displayed_without_diff_content(self):
        args = {"patchText": "*** Begin Patch\n*** Update File: old.py\n*** Move to: moved/new.py\n"
                "@@\n-a\n+b\n*** Delete File: deleted.py\n*** End Patch"}
        self.assertEqual(tool_files("apply_patch", args),
                         [{"filePath": "old.py", "movePath": "moved/new.py"}, {"filePath": "deleted.py"}])

    def test_mutation_title_path_is_muted_and_middle_compacted(self):
        path = "/fixtures/sample/very/long/deep/project/src/source.py"
        for tool in ("edit", "write"):
            view = ToolView("call", tool, {"filePath": path})
            for failed, status in ((False, "Completed"), (True, "Failed")):
                view.set_result({"isError": failed})
                text = view.title_text(52)
                self.assertTrue(text.plain.startswith(tool + " · " + status + " • "))
                self.assertIn("…", text.plain)
                self.assertTrue(text.plain.endswith("/src/source.py"))
                self.assertLessEqual(text.cell_len, 52)
                self.assertTrue(any(MUTED_COLOR.lower() in str(span.style).lower() for span in text.spans))

    def test_file_titles_never_exceed_even_tiny_widths(self):
        view = ToolView("call", "write", {"filePath": "/根/source/long_file.py"})
        for width in range(60):
            self.assertLessEqual(view.title_text(width).cell_len, width)


class FileToolViewsTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        class FileToolsApp(App):
            # Include the conversation rule which overrides default drawer padding.
            CSS = ".conversation-history Collapsible > Contents { padding: 0 0 0 4; }"

            def get_css_variables(self):
                return {**super().get_css_variables(), "muted": MUTED_COLOR}
        self.app = FileToolsApp()
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 40)))
        self.app.screen.add_class("conversation-history")

    async def test_read_path_is_muted_before_and_after_permission_and_result(self):
        path = "/repo/src/first.py"
        view = ReadView("read", {"filePath": path})
        await self.app.mount(view)

        def check_path():
            text = view.content
            self.assertTrue(text.plain.endswith(path))
            style = text.get_style_at_offset(Console(), text.plain.index(path))
            self.assertEqual(style.color, Style.parse(MUTED_COLOR).color)
            self.assertFalse(style.bold)

        check_path()
        view.record_permission({"permissionId": "p", "decision": "ai_accepted", "confidence": .95})
        check_path()
        view.set_result({"isError": True})
        check_path()

    async def test_read_stays_on_one_line_with_both_path_ends_on_resize(self):
        path = "/fixtures/sample/nested/projects/repository/src/long_source_name.py"
        view = ReadView("read", {"filePath": path})
        await self.app.mount(view)
        view.record_permission({"permissionId": "p", "decision": "ai_accepted", "confidence": .95})
        for width in (65, 45, 30):
            await self.pilot.resize_terminal(width, 40)
            await self.pilot.pause()
            text = view.render().plain
            self.assertEqual(view.region.height, 1)
            self.assertLessEqual(view.render().cell_len, view.content_size.width)
            self.assertIn("…", text)
            self.assertIn("/", text)
            self.assertTrue(text.endswith(".py"))
            self.assertIn(".py", view.render_line(0).text)
        await self.pilot.resize_terminal(150, 40)
        await self.pilot.pause()
        self.assertIn(path, view.render_line(0).text)
        self.assertEqual(view.region.height, 1)

    async def test_opened_file_path_headers_compact_without_losing_depth_color(self):
        path = "/fixtures/sample/nested/projects/a_very_long_repository_name/src/long_source_name.py"
        cases = [("write", {"filePath": path, "content": "text"}, {}),
                 ("edit", {"filePath": path}, {"filepath": path, "diff": "@@ -1 +1 @@\n-a\n+b"}),
                 ("apply_patch", {}, {"files": [{"filePath": path, "patch": "@@ -1 +1 @@\n-a\n+b"}]}),
                 ("read", {}, {"display": {"path": path, "type": "file", "text": "text"}})]
        headers = []
        for tool, arguments, metadata in cases:
            header = result_widgets(tool, arguments, {"isError": False, "output": "ok", "metadata": metadata})[0]
            self.assertIsInstance(header, WorkspacePath)
            headers.append(header)
            await self.app.mount(header)
        await self.pilot.resize_terminal(55, 40)
        await self.pilot.pause()
        for header in headers:
            self.assertEqual(header.region.height, 1)
            line = header.render_line(0)
            self.assertIn("…", line.text)
            self.assertTrue(line.text.rstrip().endswith("/src/long_source_name.py"))
            self.assertTrue(line.text.startswith("/"))
            segment = next(segment for segment in line if "long_source_name.py" in segment.text)
            self.assertEqual(segment.style.color.get_truecolor().hex.lower(), DEPTH_MD.lower())
        await self.pilot.resize_terminal(150, 40)
        await self.pilot.pause()
        for header in headers:
            self.assertIn(path, header.render_line(0).text)

    async def test_multifile_patch_is_one_call_with_one_independent_drawer_per_file(self):
        activity = TurnActivity()
        await self.app.mount(activity)
        activity.body.display = True
        view = ToolView("call", "apply_patch", {"patchText": PATCH})
        activity.add_activity(view, "tools")
        await self.pilot.pause()
        self.assertEqual(len(view.file_views), 2)
        self.assertFalse(view._title.display)
        self.assertTrue(all(child.collapsed for child in view.file_views))
        self.assertEqual(activity.toggle.step_count, 1)
        view.set_result(RESULT)
        await self.pilot.pause()
        first, second = view.file_views
        self.assertIs(view.file_rows.parent, view)
        self.assertTrue(first._title.visible)
        self.assertTrue(second._title.visible)
        self.assertGreater(first._title.region.height, 0)
        self.assertGreater(second._title.region.height, 0)
        self.assertEqual(first._title.region.x, activity.toggle.region.x + 2)
        self.assertEqual(second._title.region.x, first._title.region.x)
        self.assertEqual(first.title_text().plain, "apply_patch · Completed • src/first.py")
        self.assertEqual(second.title_text().plain, "apply_patch · Completed • src/second.py")
        first.collapsed = False
        await self.pilot.pause()
        self.assertTrue(first.rendered)
        self.assertFalse(second.rendered)
        self.assertTrue(second.collapsed)
        self.assertEqual(first._title.region.x, second._title.region.x)
        self.assertEqual(first._title.region.x, view.region.x)
        self.assertEqual(first.results.region.x, first._title.region.x + 4)
        labels = [str(child.content) for child in first.results.query("Static")]
        self.assertIn("src/first.py", labels)
        self.assertNotIn("src/second.py", labels)
        path_text = first.results.query_one("Static").content
        self.assertEqual(path_text.get_style_at_offset(Console(), 0).color, Style.parse(DEPTH_MD).color)
        self.assertEqual(view.result, RESULT)
        # Historical viewport restoration collapses all Collapsibles recursively.
        # The hidden logical call must never gate either file's visibility.
        for drawer in activity.query(Collapsible):
            drawer.collapsed = True
        await self.pilot.pause()
        self.assertGreater(first._title.region.height, 0)
        self.assertGreater(second._title.region.height, 0)
        await self.pilot.click(second._title)
        await self.pilot.pause()
        self.assertTrue(first.collapsed)
        self.assertFalse(second.collapsed)
        self.assertTrue(second.rendered)

    async def test_failed_patch_rows_keep_paths_and_share_the_call_verdict(self):
        view = ToolView("call", "apply_patch", {"patchText": PATCH})
        await self.app.mount(view)
        view.record_permission({"permissionId": "p", "decision": "ai_refused", "confidence": .17})
        view.set_result({"isError": True, "output": "Internal instructions", "metadata": {
            "permissions": [{"decision": "ai_refused", "confidence": .17}]}})
        await self.pilot.pause()
        self.assertEqual([child.title_text().plain for child in view.file_views],
                         [f"apply_patch • AI 17% • Failed • src/{name}.py" for name in ("first", "second")])

    async def test_history_file_drawers_load_only_requested_detail_and_filter_its_diff(self):
        brief = {"callId": "call", "toolName": "apply_patch", "activityIndex": 0,
                 "arguments": {"filePaths": ["src/first.py", "src/second.py"]},
                 "result": {"isError": False, "metadata": {"files": [
                     {key: value for key, value in file.items() if key != "patch"} for file in FILES]}}}
        root = HistoryToolView("group", brief)
        await self.app.mount(root)
        await self.pilot.pause()
        self.assertEqual(len(root.file_views), 2)
        self.assertFalse(root.details_loaded)
        first, second = root.file_views
        self.assertIs(root.file_rows.parent, root)
        root.collapsed = True
        await self.pilot.pause()
        self.assertGreater(first._title.region.height, 0)
        self.assertGreater(second._title.region.height, 0)
        self.assertFalse(first.details_loaded)
        self.assertFalse(second.details_loaded)
        with patch.object(first, "post_message", wraps=first.post_message) as posted:
            first.collapsed = False
            await self.pilot.pause()
            self.assertTrue(first.detail_loading)
            self.assertFalse(second.detail_loading)
            self.assertTrue(any(isinstance(call.args[0], ActivityDetailRequested) for call in posted.call_args_list))
            await first.load_detail({**brief, "kind": "tool", "arguments": {"patchText": PATCH}, "result": RESULT})
            await self.pilot.pause()
            self.assertTrue(first.rendered)
            self.assertFalse(second.rendered)
            labels = [str(child.content) for child in first.results.query("Static")]
            self.assertIn("src/first.py", labels)
            self.assertNotIn("src/second.py", labels)

    async def test_title_preserves_leaf_on_resize(self):
        path = "/fixtures/sample/nested/projects/repository/src/long_source_name.py"
        view = ToolView("call", "write", {"filePath": path, "content": "text"})
        view.set_result({"isError": False})
        await self.app.mount(view)
        await self.pilot.pause()
        title = view.query_one(ToolPathTitle)
        await self.pilot.resize_terminal(65, 40)
        await self.pilot.pause()
        line = title.render_line(0)
        self.assertIn("/src/long_source_name.py", line.text)
        self.assertIn("…", line.text)
        path_segment = next(segment for segment in line if "long_source_name.py" in segment.text)
        self.assertEqual(path_segment.style.color.get_truecolor().hex.lower(), MUTED_COLOR.lower())
        self.assertFalse(path_segment.style.bold)
        await self.pilot.resize_terminal(150, 40)
        await self.pilot.pause()
        self.assertIn(path, title.render_line(0).text)

    async def test_legacy_history_splits_after_first_detail_without_requesting_it_again(self):
        brief = {"callId": "call", "toolName": "apply_patch", "activityIndex": 0,
                 "arguments": {}, "result": {"isError": False, "metadata": {}}}
        root = HistoryToolView("group", brief)
        await self.app.mount(root)
        root.collapsed = False
        await self.pilot.pause()
        await root.load_detail({**brief, "kind": "tool", "arguments": {"patchText": PATCH}, "result": RESULT})
        await self.pilot.pause()
        self.assertEqual(len(root.file_views), 2)
        self.assertTrue(all(child.details_loaded for child in root.file_views))
        root.file_views[1].collapsed = False
        await self.pilot.pause()
        self.assertTrue(root.file_views[1].rendered)
        self.assertFalse(root.file_views[1].detail_loading)

    async def test_replayed_patch_keeps_one_permission_target_and_hides_wrapper_arguments(self):
        root = ToolView("replay", "rerun_approved_action", {"id": "approval"})
        await self.app.mount(root)
        root.record_permission({"permissionId": "p", "decision": "manually_approved", "confidence": .17,
                                "tool": "apply_patch", "details": {"arguments": {"patchText": PATCH}}})
        await self.pilot.pause()
        self.assertEqual(len(root.file_views), 2)
        self.assertTrue(all(not header.display for header in root.call_headers))
        self.assertEqual(root.file_views[0].title_text().plain,
                         "apply_patch • Human (AI 17%) • Pending • src/first.py")
        self.assertTrue(all(child.call_id == "replay" for child in root.file_views))
