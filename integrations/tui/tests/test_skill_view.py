"""Skill headers preserve positional arguments and the loaded skill's names."""

import unittest

from textual.app import App
from textual.widgets import Rule, Static

from sos_tui.config import ALGAE_MD, DEPTH_MD, MISC_MD, TITLE_MD
from sos_tui.history_detail import HistoryToolView
from sos_tui.tool_view import ToolView, format_arguments, result_widgets


class SkillViewTest(unittest.TestCase):
    def test_unnamed_input_stays_one_raw_argument(self):
        for raw, expected in (("", "example"), ("ultra", "example\nargument: ultra"),
                              ('"hello world" next', 'example\nargument: "hello world" next'),
                              ("Crée un workspace pour ce ticket", "example\nargument: Crée un workspace pour ce ticket"),
                              ('unfinished"', 'example\nargument: unfinished"')):
            with self.subTest(raw=raw):
                text = format_arguments({"name": "example", "input": raw}, "skill")
                self.assertEqual(str(text), expected)
                self.assertEqual(text.spans[0].style, ALGAE_MD)

    def test_unnamed_input_ignores_legacy_split_values(self):
        text = format_arguments({"name": "initct", "input": "une phrase entière"}, "skill",
                                {"argument_names": [], "argument_values": ["une", "phrase", "entière"]})
        self.assertEqual(str(text), "initct\nargument: une phrase entière")
        self.assertEqual(text.spans[1].style, MISC_MD)

    def test_argument_labels_are_misc_without_bold(self):
        for tool, arguments, labels in (
                ("bash", {"command": "echo hello"}, ["command", "timeout"]),
                ("mcp_example", {"query": "hello"}, ["query"])):
            with self.subTest(tool=tool):
                text = format_arguments(arguments, tool)
                self.assertEqual([text.plain[span.start:span.end] for span in text.spans], labels)
                self.assertTrue(all(span.style == MISC_MD for span in text.spans))
        text = format_arguments({"name": "ponytail", "input": "lite"}, "skill",
                                {"argument_names": ["level"], "argument_values": ["lite"]})
        self.assertEqual(text.spans[1].style, MISC_MD)

    def test_named_missing_and_extra_values(self):
        text = format_arguments({"name": "example", "input": "ignored"}, "skill",
                                {"argument_names": ["name", "note"], "argument_values": ["Alice"]})
        self.assertEqual(str(text), "example\nname: Alice\nnote: ")
        text = format_arguments({"name": "example"}, "skill",
                                {"argument_names": ["name"], "argument_values": ["Alice", "extra"]})
        self.assertEqual(str(text), "example\nname: Alice\nargument 1: extra")

    def test_grep_match_ending_in_colon_is_not_a_path(self):
        output = "Found 1 matches\n\n/repo/test.log:\n  Line 55: Expected equality of these values:\n"
        text = result_widgets("grep", {}, {"output": output, "isError": False})[0].content
        lines = text.split("\n")
        self.assertEqual(str(lines[1]), "/repo/test.log:")
        self.assertEqual(lines[1].spans[0].style, DEPTH_MD)
        self.assertEqual(str(lines[2]), "  Line 55: Expected equality of these values:")
        self.assertEqual([(span.start, span.end, span.style) for span in lines[2].spans],
                         [(0, len("  Line 55:"), TITLE_MD)])

    def test_grep_count_and_files_have_no_blank_lines(self):
        output = "Found 2 matches\n\n/repo/a.py:\n  Line 1: hello\n\n/repo/b.py:\n  Line 3: hello\n"
        for metadata in ({"matches": 2}, {}):
            rendered = result_widgets("grep", {}, {"output": output, "isError": False, "metadata": metadata})
            self.assertEqual(str(rendered[0].content),
                             "2 Matches\n/repo/a.py:\n  Line 1: hello\n/repo/b.py:\n  Line 3: hello")
        rendered = result_widgets("grep", {}, {"output": "No files found", "isError": False})
        self.assertEqual(str(rendered[0].content), "0 Matches")
        error = result_widgets("grep", {}, {"output": "Search failed", "isError": True})
        self.assertEqual(str(error[0].content), "Search failed")


class SkillViewLiveTest(unittest.IsolatedAsyncioTestCase):
    async def test_live_and_history_use_saved_argument_metadata(self):
        arguments = {"name": "example", "input": '"hello world"'}
        result = {"output": "Loaded instructions", "isError": False,
                  "metadata": {"argument_names": ["message"], "argument_values": ["hello world"]}}

        class TestApp(App):
            def compose(self):
                yield ToolView("live", "skill", arguments, lazy_arguments=True)
                yield HistoryToolView("group", {"activityIndex": 0, "toolName": "skill", "callId": "saved"})

        app = TestApp()
        async with app.run_test() as pilot:
            live, history = list(app.query(ToolView))
            live.set_result(result)
            live.collapsed = history.collapsed = False
            await pilot.pause()
            await history.load_detail({"kind": "tool", "toolName": "skill", "callId": "saved",
                                       "arguments": arguments, "result": result})
            await pilot.pause()
            for view in (live, history):
                self.assertEqual(str(view.arguments_view.content), "example\nmessage: hello world")
                self.assertEqual(len(view.query(Rule)), 1)
                labels = [str(child.content) for child in view.query(Static)]
                self.assertNotIn("Arguments", labels)
                self.assertNotIn("Results", labels)
                self.assertIn("Loaded instructions", labels)
