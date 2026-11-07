"""MCP titles use full protocol names rather than hashed registry identifiers."""

import unittest

from textual.app import App

from sos_tui.history_detail import HistoryToolView
from sos_tui.manual_approval import ManualApprovalView
from sos_tui.tool_view import ToolView


class McpToolTitlesTest(unittest.TestCase):
    def test_live_permission_and_result_keep_names_and_suffix(self):
        view = ToolView("call", "mcp_notion_truncated_0123456789", {})
        self.assertEqual(view.title, "mcp • Pending")
        view.record_permission({"permissionId": "p", "decision": "ai_accepted",
                                "details": {"server": "notion", "mcp_tool": "notion-get-tool-access"}})
        self.assertEqual(view.title, "mcp • notion • notion-get-tool-access • AI • Pending")
        view.set_result({"isError": False, "output": "ok", "metadata": {
            "mcp_server": "notion", "mcp_tool": "notion-get-tool-access"}})
        self.assertEqual(view.title, "mcp • notion • notion-get-tool-access • AI • Completed")

    def test_restored_success_and_failure(self):
        for failed, status in ((False, "Completed"), (True, "Failed")):
            view = HistoryToolView("group", {"callId": "call", "activityIndex": 0,
                "toolName": "mcp_fetch_fetch_hash", "result": {
                    "isError": failed, "metadata": {"mcp_server": "fetch", "mcp_tool": "fetch",
                    "permissions": [{"id": "p", "decision": "yes"}]}}})
            self.assertEqual(view.title, f"mcp • fetch • fetch • Human • {status}")

    def test_builtin_title_is_unchanged(self):
        view = ToolView("call", "bash", {})
        view.set_result({"isError": True, "output": "error"})
        self.assertEqual(view.title, "bash · Failed")


class McpRefusalTitlesTest(unittest.IsolatedAsyncioTestCase):
    def item(self, with_names=True):
        metadata = {"refused_message_id": "result", "permissions": [
            {"decision": "ai_refused", "confidence": 0.02}]}
        if with_names:
            metadata.update(mcp_server="notion", mcp_tool="notion-get-tool-access")
        return {"toolName": "mcp_notion_notion_get_t_hash", "callId": "call", "arguments": {},
                "result": {"isError": True, "metadata": metadata}}

    def test_live_and_lazy_titles_preserve_approval_identity(self):
        for lazy in (False, True):
            item = self.item()
            view = ManualApprovalView(item, lazy=lazy)
            label = "mcp • notion • notion-get-tool-access • AI 2%"
            self.assertEqual(view.title, label)
            view.set_approved(True)
            self.assertEqual(view.title, "mcp • notion • notion-get-tool-access • Human (AI 2%)")
            view.set_approved(False)
            self.assertEqual(view.title, label)
            self.assertEqual(view.tool, item["toolName"])
            self.assertEqual(view.refused_id, "result")

    async def test_lazy_details_restore_missing_names_without_changing_identity(self):
        await self.enterAsyncContext(App().run_test())
        view = ManualApprovalView(self.item(False), lazy=True)
        self.assertEqual(view.title, "mcp • AI 2%")
        view.set_approved(True)
        await view.load_detail(self.item())
        self.assertEqual(view.title,
                          "mcp • notion • notion-get-tool-access • Human (AI 2%)")
        self.assertEqual(view.tool, self.item()["toolName"])
        with self.assertRaises(ValueError):
            await view.load_detail({**self.item(), "toolName": "other"})
