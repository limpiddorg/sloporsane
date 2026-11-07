"""Bash shows execution parameters, a separator, and the existing terminal output."""

import unittest
from unittest.mock import patch

from textual.app import App
from textual.widgets import Collapsible, Rule, Static

from sos_tui.history_detail import HistoryToolView
from sos_tui.app import SosApp
from sos_tui.tool_view import ToolView, format_arguments


class BashViewTest(unittest.TestCase):
    def test_command_exit_is_distinct_from_tool_failure_live_and_restored(self):
        cases = [({"exit": 0}, False, "Completed · Exit code 0"),
                  ({"exit": 1}, False, "Completed · Exit code 1"),
                  ({"exit": 2}, True, "Completed · Exit code 2"),
                  ({"exit": -9}, True, "Completed · Exit code -9"),
                 ({"exit": None, "timedOut": True}, False, "Failed"),
                 ({}, True, "Failed"),
                 ({"cancelled": True}, True, "Not run"),
                 ({"cancelled": True, "started": True}, True, "Cancelled")]
        for metadata, is_error, status in cases:
            with self.subTest(metadata=metadata):
                result = {"isError": is_error, "metadata": {**metadata, "permissions": [{"decision": "ai_accepted"}]}}
                view = ToolView("call", "bash", {})
                view.set_result(result)
                restored = HistoryToolView("group", {"callId": "call", "toolName": "bash",
                                                      "activityIndex": 0, "result": result})
                expected = f"bash • AI • {status}"
                self.assertEqual(view.title, expected)
                self.assertEqual(restored.title, expected)

    def test_command_and_default_timeout_are_explicit(self):
        self.assertEqual(str(format_arguments({"command": "echo hello"}, "bash")),
                         "command: echo hello\ntimeout: 120000 ms")

    def test_explicit_timeout_workdir_and_multiline_command(self):
        self.assertEqual(str(format_arguments({"workdir": "/repo", "timeout": 2500,
                                              "command": "echo one\necho two"}, "bash")),
                         "command:\necho one\necho two\ntimeout: 2500 ms\nworkdir: /repo")


class BashViewLiveTest(unittest.IsolatedAsyncioTestCase):
    async def test_outcome_and_validation_styles_override_inherited_bold(self):
        from sos_tui.config import SANE_COLOR, SLOP_COLOR
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(120, 40)) as pilot:
                activity = app.ensure_activity()
                activity.body.display = True
                for index, (metadata, is_error, labels) in enumerate([
                    ({"exit": 0}, False, [("Completed", SANE_COLOR), ("Exit code 0", SANE_COLOR)]),
                    ({"exit": 1}, False, [("Completed", SANE_COLOR), ("Exit code 1", SLOP_COLOR)]),
                    ({"exit": 2}, True, [("Completed", SANE_COLOR), ("Exit code 2", SLOP_COLOR)]),
                    ({"timedOut": True}, False, [("Failed", SLOP_COLOR)]),
                ]):
                    view = ToolView(str(index), "bash", {})
                    view.set_result({"isError": is_error, "metadata": {
                        **metadata, "permissions": [{"decision": "yes" if index else "ai_accepted"}]}})
                    activity.add_activity(view, "tools")
                    await pilot.pause()
                    title = view.query_one("CollapsibleTitle")
                    for label, color in labels:
                        segment = next(segment for segment in title.render_line(0) if label in segment.text)
                        self.assertEqual(segment.style.color.get_truecolor().hex.lower(), color.lower())
                        self.assertFalse(segment.style.bold)
                    validation = next(segment for segment in title.render_line(0) if ("Human" if index else "AI") in segment.text)
                    self.assertFalse(validation.style.bold)
                    self.assertEqual(validation.style.color.get_truecolor().hex.lower(), SANE_COLOR.lower())

    async def test_grep_and_generic_separator_layout(self):
        for tool, arguments in (("grep", {"path": "/repo", "pattern": "hello"}),
                                ("mcp_example", {"query": "hello"})):
            with self.subTest(tool=tool):
                app = SosApp()
                with patch.object(app, "listen"):
                    async with app.run_test(size=(100, 40)) as pilot:
                        activity = app.ensure_activity()
                        view = ToolView("call", tool, arguments)
                        activity.add_activity(view, "tools")
                        activity.body.display = True
                        view.set_result({"isError": False, "output": "Found 1 match", "metadata": {}})
                        view.collapsed = False
                        await pilot.pause()
                        separator = view.query_one(Rule)
                        self.assertEqual(separator.region.height, 1)
                        self.assertEqual(separator.region.y, view.arguments_view.region.bottom)
                        self.assertEqual(view.results.region.y, separator.region.bottom)
                        labels = [str(child.content) for child in view.query(Static)]
                        self.assertNotIn("Results", labels)
                        if tool == "grep":
                            self.assertNotIn("Arguments", labels)
                            self.assertEqual(str(view.arguments_view.content), "/repo\nhello")

    async def test_separator_has_no_blank_lines_in_discussion(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(100, 40)) as pilot:
                activity = app.ensure_activity()
                view = ToolView("call", "bash", {"command": "echo hello"})
                activity.add_activity(view, "tools")
                activity.body.display = True
                view.set_result({"isError": False, "output": "hello", "metadata": {"exit": 0}})
                view.collapsed = False
                await pilot.pause()
                separator = view.query_one(Rule)
                status = view.results.query(Static).first()
                self.assertEqual(separator.region.height, 1)
                self.assertEqual(separator.styles.padding.top + separator.styles.padding.bottom, 0)
                self.assertEqual(separator.region.y, view.arguments_view.region.bottom)
                self.assertEqual(status.region.y, separator.region.bottom)

    async def test_live_lazy_restore_and_generic_tools(self):
        arguments = {"command": "echo hello", "timeout": 4000}
        result = {"isError": False, "output": "hello\n\n<shell_metadata>\nExit code: 0\n</shell_metadata>",
                  "metadata": {"exit": 0}}

        class TestApp(App):
            def compose(self):
                yield ToolView("live", "bash", arguments)
                yield HistoryToolView("group", {"activityIndex": 0, "toolName": "bash", "callId": "saved"})
                yield ToolView("mcp", "mcp_example", {"query": "hello"})
                yield ToolView("skill", "skill", {"name": "example"})

        app = TestApp()
        async with app.run_test(size=(100, 50)) as pilot:
            live, history, mcp, skill = list(app.query(ToolView))
            live.set_result(result)
            live.collapsed = history.collapsed = False
            await pilot.pause()
            await history.load_detail({"kind": "tool", "toolName": "bash", "callId": "saved",
                                       "arguments": arguments, "result": result})
            await pilot.pause()
            for view in (live, history):
                children = list(view.query_one(Collapsible.Contents).children)
                self.assertEqual(children[0], view.arguments_view)
                self.assertIsInstance(children[1], Rule)
                self.assertEqual(children[2], view.results)
                self.assertEqual(children[1].region.height, 1)
                self.assertEqual(children[1].region.y, view.arguments_view.region.bottom)
                self.assertEqual(view.results.region.y, children[1].region.bottom)
                self.assertEqual(str(view.arguments_view.content), "command: echo hello\ntimeout: 4000 ms")
                output = list(view.results.query(Static))
                self.assertEqual(str(output[0].content), "Exit code: 0")
                self.assertEqual(str(output[1].content), "hello")
                self.assertEqual(view.title, "bash · Completed · Exit code 0")
            for view in (mcp, skill):
                labels = [str(child.content) for child in view.query_one(Collapsible.Contents).children
                          if isinstance(child, Static)]
                if view is mcp:
                    self.assertIn("Arguments", labels)
                else:
                    self.assertNotIn("Arguments", labels)
                self.assertNotIn("Results", labels)
                self.assertEqual(len(view.query(Rule)), 1)
