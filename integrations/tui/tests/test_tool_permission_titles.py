"""Permission verdicts precede status; internal refusal instructions stay hidden."""
import copy
import unittest

from rich.console import Console
from rich.style import Style
from textual.app import App

from sos_tui.config import DEPTH_MD, MISC_MD, MUTED_COLOR, SANE_COLOR, SLOP_COLOR
from sos_tui.history_detail import HistoryToolView
from sos_tui.manual_approval import ManualApprovalView
from sos_tui.tool_view import ReadView, ToolView, format_arguments, permission_suffix, result_widgets, styled_tool_title


class ToolPermissionTitlesTest(unittest.IsolatedAsyncioTestCase):
    async def test_bash_main_command_is_muted_between_status_and_exit_live_and_restored(self):
        app = App()
        pilot = await self.enterAsyncContext(app.run_test(size=(90, 30)))
        arguments = {"command": "python -m pytest", "mainCommand": "pytest"}
        live = ToolView("call", "bash", arguments)
        await app.mount(live)
        self.assertEqual(live.title_text().plain, "bash · Pending • pytest")
        record = {"decision": "ai_accepted", "confidence": .95}
        live.record_permission({"permissionId": "p", **record})
        result = {"isError": False, "output": "Passed", "metadata": {"exit": 0, "permissions": [record]}}
        live.set_result(result)
        restored = HistoryToolView("group", {"activityIndex": 0, "callId": "call", "toolName": "bash",
                                             "arguments": {"mainCommand": "pytest"}, "result": result})
        await app.mount(restored)
        await pilot.pause()
        for view in (live, restored):
            text = view.title_text()
            self.assertEqual(text.plain, "bash • AI 95% • Completed • pytest · Exit code 0")
            command_style = text.get_style_at_offset(Console(), text.plain.index("pytest"))
            self.assertEqual(command_style.color, Style(color=MUTED_COLOR).color)
            for offset in range(len(text.plain)):
                self.assertFalse(text.get_style_at_offset(Console(), offset).bold)
            self.assertIn("Completed • pytest · Exit code 0", view._title.render_line(0).text)
        live.collapsed = False
        await pilot.pause()
        self.assertIn("python -m pytest", live.arguments_view.content.plain)
        self.assertNotIn("mainCommand", live.arguments_view.content.plain)

    async def test_bash_labels_cover_failures_replays_legacy_and_narrow_titles(self):
        await self.enterAsyncContext(App().run_test())
        view = ToolView("call", "bash", {"mainCommand": "cmake"})
        view.set_result({"isError": True, "metadata": {"exit": 1}})
        self.assertEqual(view.title_text().plain, "bash · Completed • cmake · Exit code 1")
        view.set_result({"isError": True, "metadata": {"timedOut": True}})
        self.assertEqual(view.title_text().plain, "bash · Failed • cmake")
        view.set_result({"isError": True, "metadata": {"cancelled": True, "started": True}})
        self.assertEqual(view.title_text().plain, "bash · Cancelled • cmake")
        legacy = ToolView("old", "bash", {})
        legacy.set_result({"isError": False, "metadata": {"exit": 0}})
        self.assertEqual(legacy.title_text().plain, "bash · Completed · Exit code 0")
        replay = ToolView("replay", "rerun_approved_action", {"id": "approval"})
        replay.set_result({"isError": False, "metadata": {"exit": 0, "approved_action": {
            "tool": "bash", "arguments": {"command": "git status", "mainCommand": "git"}}}})
        self.assertEqual(replay.title_text().plain, "bash · Completed • git · Exit code 0")
        view.arguments = {"mainCommand": "very_long_command_name"}
        view.set_result({"isError": False, "metadata": {"exit": 0}})
        text = view.title_text(42)
        self.assertLessEqual(text.cell_len, 42)
        self.assertIn("…", text.plain)
        self.assertTrue(text.plain.endswith("Exit code 0"))

    def test_all_tool_names_are_not_bold_without_changing_colors(self):
        for tool, arguments in (("edit", {"filePath": "a.py"}), ("bash", {"mainCommand": "git"}),
                                ("grep", {"pattern": "TODO"}), ("glob", {}), ("skill", {"name": "demo"}),
                                ("mcp_example", {})):
            text = ToolView("call", tool, arguments).title_text()
            self.assertFalse(text.get_style_at_offset(Console(), 0).bold)

    def test_search_patterns_use_misc_blue_and_paths_keep_depth_blue(self):
        console = Console()
        for tool, pattern in (("grep", "TODO|FIXME"), ("glob", "**/*.py")):
            text = format_arguments({"path": "src", "pattern": pattern}, tool)
            self.assertEqual(text.plain, "src\n" + pattern)
            self.assertEqual(text.get_style_at_offset(console, 0).color, Style(color=DEPTH_MD).color)
            for offset in range(4, len(text.plain)):
                self.assertEqual(text.get_style_at_offset(console, offset).color, Style(color=MISC_MD).color)

    async def test_replayed_read_human_score_is_green_live_and_restored(self):
        app = App()
        pilot = await self.enterAsyncContext(app.run_test())
        live, restored = (ReadView("call", {"filePath": "/example"}) for _ in range(2))
        await app.mount(live, restored)
        record = {"decision": "manually_approved", "confidence": 0.17}
        live.record_permission({"permissionId": "p", **record})
        result = {"isError": False, "metadata": {"permissions": [record]}}
        live.set_result(result)
        restored.set_result(result)
        await pilot.pause()
        for view in (live, restored):
            text = view.render()
            self.assertEqual(text.plain, "read • Human (AI 17%) · /example")
            segment = next(segment for segment in view.render_line(0) if "Human" in segment.text)
            self.assertEqual(segment.style.color.get_truecolor().hex.lower(), SANE_COLOR.lower())

    def test_live_and_saved_confidence_precedes_status(self):
        for decision, label, confidence in (("ai_accepted", "AI 83%", 0.83),
                                             ("ai_refused", "AI 17%", 0.17),
                                             ("manually_approved", "Human (AI 17%)", 0.17)):
            with self.subTest(decision=decision):
                view = ToolView("call", "bash", {})
                view.record_permission({"permissionId": "p", "decision": decision, "confidence": confidence})
                prefix = f"bash • {label} • "
                self.assertEqual(view.title, prefix + "Pending")
                failed = decision == "ai_refused"
                result = {"isError": failed, "metadata": {"permissions": [
                    {"id": "p", "decision": decision, "confidence": confidence}]}}
                view.set_result(result)
                saved = HistoryToolView("g", {"callId": "call", "toolName": "bash",
                                               "activityIndex": 0, "result": result})
                self.assertEqual(view.title, prefix + ("Failed" if failed else "Completed"))
                self.assertEqual(saved.title, view.title)

    def test_verdict_and_confidence_share_color_without_bold(self):
        console = Console()
        for decision, label, color in (("ai_accepted", "AI 17%", SANE_COLOR),
                                        ("ai_refused", "AI 17%", SLOP_COLOR),
                                        ("manually_approved", "Human (AI 17%)", SANE_COLOR),
                                        ("yes", "Human", SANE_COLOR)):
            records = [{"decision": decision, **({"confidence": 0.17} if decision != "yes" else {})}]
            title = styled_tool_title(f"bash • {label} • Completed", records)
            for offset in range(title.plain.index(label), title.plain.index(label) + len(label)):
                style = title.get_style_at_offset(console, offset)
                self.assertEqual(style.color, Style(color=color).color)
                self.assertFalse(style.bold)

    def test_human_approval_preserves_original_ai_confidence_and_revocation(self):
        view = ManualApprovalView({"toolName": "bash", "result": {"metadata": {
            "permissions": [{"decision": "ai_refused", "confidence": 0.17}]}}})
        self.assertEqual(view.title, "bash • AI 17%")
        view.set_approved(True)
        self.assertEqual(view.title, "bash • Human (AI 17%)")
        view.set_approved(False)
        self.assertEqual(view.title, "bash • AI 17%")

    def test_confidence_zero_is_not_missing_and_unknown_scores_are_not_invented(self):
        self.assertEqual(permission_suffix([{"decision": "ai_refused", "confidence": 0}]), " • AI 0%")
        for confidence in (None, True, "83", -0.2, 1.5, float("nan")):
            self.assertEqual(permission_suffix([{"decision": "ai_accepted", "confidence": confidence}]), " • AI")
        self.assertEqual(permission_suffix([{"decision": "yes"}]), " • Human")

    def test_identical_checks_are_deduplicated_without_hiding_distinct_confidences(self):
        records = [{"decision": "ai_accepted", "confidence": score} for score in (0.83, 0.83, 0.91)]
        self.assertEqual(permission_suffix(records), " • AI 83% • AI 91%")

    def test_internal_refusal_instructions_are_not_rendered_or_modified(self):
        for confidence, reason in ((None, "AI verification unavailable."), (0.17, "refused by AI verification.")):
            result = {"isError": True, "output": "Internal model instructions: Do not retry or bypass this refusal. "
                      "Use rerun_approved_action after approval.", "metadata": {"permissions": [
                          {"decision": "ai_refused", "confidence": confidence}]}}
            original = copy.deepcopy(result)
            for tool in ("bash", "write", "mcp_test"):
                widgets = result_widgets(tool, {}, result)
                self.assertEqual(str(widgets[0].content), "Action not executed: " + reason)
            self.assertEqual(result, original)

    def test_regular_tool_errors_are_not_hidden(self):
        result = {"isError": True, "output": "Command failed with a useful diagnostic", "metadata": {}}
        self.assertEqual(str(result_widgets("bash", {}, result)[0].content), result["output"])
