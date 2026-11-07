"""Imported activity rows are light; individual details load only on expansion."""

import json
import unittest
from textual.widgets import Static

from sos_tui.history_detail import HistoryReasoning, HistoryToolView
from sos_tui.history_viewport import HistoryActivity, HistoryViewport
from sos_tui.response_markdown import ResponseMarkdown
from sos_tui.tool_view import ReadView, ToolView, permission_suffix
from test_history_window import ViewportApp, page, respond


ROWS = [
    {"kind": "tool", "activityIndex": 0, "toolName": "read", "callId": "r",
     "arguments": {"filePath": "/file"}, "result": {"isError": False, "metadata": {}}},
    {"kind": "tool", "activityIndex": 1, "toolName": "write", "callId": "w",
     "arguments": {}, "result": {"isError": False, "metadata": {}}},
    {"kind": "reasoning", "activityIndex": 2},
]
DETAIL = {"kind": "tool", "activityIndex": 1, "toolName": "write", "callId": "w",
          "arguments": {"content": "PRIVATE_ARGUMENT"},
          "result": {"output": "PRIVATE_RESULT", "isError": False, "metadata": {"diff": "-old\n+new"}}}


def record(items):
    return {"id": "g", "role": "activity", "content": [], "kinds": ["read", "tools", "reasoning"],
            "count": 3, "activities": items}


async def until(pilot, predicate):
    for _ in range(80):
        if predicate():
            return
        await pilot.pause(0.01)
    raise AssertionError("Detail loading did not finish")


class HistoryDetailsTest(unittest.IsolatedAsyncioTestCase):
    async def test_resumed_intermediate_times_and_ai_decisions_are_visible(self):
        app = ViewportApp()
        async with app.run_test(size=(100, 40)) as pilot:
            initial = page(0, 1)
            rows = []
            for i, decision in enumerate(("ai_accepted", "ai_refused")):
                rows.append({"kind": "tool", "activityIndex": i, "toolName": "write", "callId": str(i),
                             "arguments": {}, "result": {"isError": decision == "ai_refused",
                             "metadata": {"permissions": [{"id": str(i), "decision": decision}]}}})
            intermediate = {**record(rows), "terminal": False, "duration_ms": 65000}
            initial["messages"].append(intermediate)
            initial["messages"].append({"id": "last", "role": "activity", "content": [], "count": 0,
                "kinds": [], "terminal": True, "metadata": {"model": "model", "reasoning": "none", "duration_ms": 90000}})
            await app.query_one(HistoryViewport).replace(initial)
            await pilot.pause()
            activity = app.query_one(HistoryActivity)
            self.assertEqual(activity.toggle.duration_label, "1m 05s")
            self.assertIn("1m 05s", activity.toggle.render().plain)
            terminal = list(app.query(HistoryActivity))[-1]
            self.assertNotIn("No reasoning", terminal.toggle.render().plain)
            self.assertEqual(terminal.toggle.duration_label, "1m 30s")
            activity.toggle.action_toggle()
            await until(pilot, lambda: activity.built)
            tools = list(app.query(HistoryToolView))
            self.assertEqual(tools[0].title, "write • AI • Completed")
            self.assertEqual(tools[1].title, "write • AI • Failed")

    def test_live_ai_decision_matches_saved_result_without_duplicate_suffix(self):
        for decision, label in (("ai_accepted", "AI"), ("ai_refused", "AI")):
            view = ToolView("call", "write", {})
            view.record_permission({"permissionId": "id", "decision": decision})
            self.assertEqual(view.title, "write • " + label + " • Pending")
            view.set_result({"isError": decision == "ai_refused", "output": "result",
                             "metadata": {"permissions": [{"id": "id", "decision": decision}]}})
            self.assertEqual(str(view.title).count(label), 1)

    def test_multiple_manual_checks_display_one_suffix_without_changing_records(self):
        permissions = [{"id": "", "permission": scope, "decision": "manually_approved"}
                       for scope in ("bash", "external_directory")]
        before = json.dumps(permissions)
        result = {"isError": False, "output": "result", "metadata": {"permissions": permissions}}
        live = ToolView("call", "bash", {})
        for permission in permissions:
            live.record_permission({"permissionId": permission["id"], "decision": permission["decision"]})
        self.assertEqual(str(live.title).count(" • Human"), 1)
        live.set_result(result)
        self.assertEqual(str(live.title).count(" • Human"), 1)
        restored = HistoryToolView("group", {"callId": "call", "toolName": "bash", "activityIndex": 0, "result": result})
        self.assertEqual(str(restored.title).count(" • Human"), 1)
        self.assertEqual(len(live.permissions), 2)
        self.assertEqual(len(restored.permissions), 2)
        self.assertEqual(json.dumps(permissions), before)
        self.assertEqual(permission_suffix([permissions[0], {"decision": "ai_refused"}, permissions[1]]),
                         " • Human • AI")

    async def test_multiple_jev_checks_display_one_suffix_live_and_restored(self):
        await self.enterAsyncContext(ViewportApp().run_test())
        for decision, label in (("ai_accepted", "AI"), ("ai_refused", "AI")):
            with self.subTest(decision=decision):
                permissions = [{"id": str(i), "permission": scope, "decision": decision}
                               for i, scope in enumerate(("bash", "external_directory"))]
                before = json.dumps(permissions)
                result = {"isError": decision == "ai_refused", "output": "result",
                          "metadata": {"permissions": permissions}}
                live = ToolView("call", "bash", {})
                read = ReadView("read", {"filePath": "/file"})
                for permission in permissions:
                    event = {"permissionId": permission["id"], "decision": decision}
                    live.record_permission(event)
                    read.record_permission(event)
                self.assertEqual(str(live.title).count(" • " + label), 1)
                self.assertEqual(str(read.content).count(" • " + label), 1)
                live.set_result(result)
                read.set_result(result)
                restored = HistoryToolView("group", {"callId": "call", "toolName": "bash",
                                                   "activityIndex": 0, "result": result})
                self.assertEqual(str(live.title).count(" • " + label), 1)
                self.assertEqual(str(restored.title).count(" • " + label), 1)
                self.assertEqual(str(read.content).count(" • " + label), 1)
                self.assertEqual(len(live.permissions), 2)
                self.assertEqual(len(restored.permissions), 2)
                self.assertEqual(len(read.permissions), 2)
                self.assertEqual(json.dumps(permissions), before)

    def test_repeated_jev_verdicts_preserve_distinct_decisions_in_order(self):
        decisions = ("ai_accepted", "ai_accepted", "manually_approved", "ai_refused", "ai_accepted", "ai_refused")
        self.assertEqual(permission_suffix([{"decision": decision} for decision in decisions]),
                         " • AI • Human • AI")

    async def test_only_expanded_details_are_requested_cached_then_purged(self):
        app = ViewportApp()
        async def send(request):
            app.sent.append(request)
            if request["type"] == "session.activities":
                respond(app.window, request, {"messages": [record(ROWS)]}, "activities", "g")
            else:
                index = request["activityIndex"]
                detail = DETAIL if index == 1 else {"kind": "reasoning", "activityIndex": 2, "text": "PRIVATE_REASONING"}
                respond(app.window, request, {"messages": [record([detail])]}, "activity-detail", "g", index)
        app.send = send
        async with app.run_test(size=(100, 40)) as pilot:
            viewport = app.query_one(HistoryViewport)
            initial = page(0, 1)
            initial["messages"].append(record(ROWS))
            await viewport.replace(initial)
            activity = app.query_one(HistoryActivity)
            activity.toggle.action_toggle()
            await until(pilot, lambda: activity.built)
            self.assertEqual(app.sent, [])
            self.assertEqual(app.query_one(ReadView).path, "/file")
            tool = app.query_one(HistoryToolView)
            reasoning = app.query_one(HistoryReasoning)
            self.assertEqual(tool.arguments, {})
            self.assertNotIn("output", tool.result)
            self.assertEqual(len(reasoning.query(ResponseMarkdown)), 0)
            tool.collapsed = False
            await until(pilot, lambda: tool.rendered)
            self.assertEqual(len(app.sent), 1)
            self.assertEqual(app.sent[0]["type"], "session.activity_detail")
            self.assertEqual(app.sent[0]["activityIndex"], 1)
            self.assertEqual(tool.arguments["content"], "PRIVATE_ARGUMENT")
            self.assertEqual(tool.result["output"], "PRIVATE_RESULT")
            self.assertNotIn("PRIVATE_", json.dumps(app.window.records))
            tool.collapsed = True
            await pilot.pause()
            tool.collapsed = False
            await pilot.pause()
            self.assertEqual(len(app.sent), 1)
            reasoning.collapsed = False
            await until(pilot, lambda: reasoning.details_loaded)
            self.assertEqual(reasoning.query_one(ResponseMarkdown).source, "PRIVATE_REASONING")
            self.assertEqual(len(app.sent), 2)
            await viewport.purge_activities()
            self.assertFalse(tool.is_attached)
            self.assertFalse(reasoning.is_attached)
            self.assertEqual(len(activity.body.children), 0)
            self.assertNotIn("activities", app.window.records[-1])
            activity.toggle.action_toggle()
            await until(pilot, lambda: activity.built)
            app.query_one(HistoryToolView).collapsed = False
            await until(pilot, lambda: app.query_one(HistoryToolView).rendered)
            self.assertEqual([request["type"] for request in app.sent],
                             ["session.activity_detail", "session.activity_detail", "session.activities", "session.activity_detail"])

    async def test_failure_retries_and_purge_cancels_late_detail(self):
        app = ViewportApp()
        async def send(request):
            app.sent.append(request)
            if len(app.sent) == 1:
                app.window.accept({"type": "request.failed", "requestId": request["requestId"], "error": "Unavailable"})
        app.send = send
        async with app.run_test() as pilot:
            viewport = app.query_one(HistoryViewport)
            initial = page(0, 1)
            initial["messages"].append(record(ROWS))
            await viewport.replace(initial)
            activity = app.query_one(HistoryActivity)
            activity.toggle.action_toggle()
            await until(pilot, lambda: activity.built)
            tool = app.query_one(HistoryToolView)
            tool.collapsed = False
            await until(pilot, lambda: app.sent and not tool.detail_loading)
            self.assertFalse(tool.details_loaded)
            self.assertIn("Unavailable", str(tool.results.query_one(Static).render()))
            tool.collapsed = True
            await pilot.pause()
            tool.collapsed = False
            await until(pilot, lambda: len(app.sent) == 2)
            self.assertTrue(tool.detail_loading)
            await viewport.purge_activities()
            respond(app.window, app.sent[-1], {"messages": [record([DETAIL])]}, "activity-detail", "g", 1)
            await pilot.pause()
            self.assertFalse(tool.is_attached)
            self.assertFalse(tool.details_loaded)
            self.assertFalse(app.window.pending)
            self.assertEqual(len(activity.body.children), 0)
