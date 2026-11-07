"""Plain read rows align with drawer labels, after their arrow and space."""
import unittest
from unittest.mock import patch

from sos_tui.app import SosApp
from sos_tui.history_viewport import HistoryActivity, HistoryViewport
from sos_tui.tool_view import ReadView, ToolView
from sos_tui.turn_activity import TurnActivity


class ReadAlignmentTest(unittest.IsolatedAsyncioTestCase):
    async def test_live_and_restored_read_align_with_tool_label(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(100, 40)) as pilot:
                live = TurnActivity()
                await app.query_one(HistoryViewport).mount(live)
                live.add_activity(ReadView("read", {"filePath": "file.txt"}), "read")
                live.add_activity(ToolView("bash", "bash", {"command": "true"}), "tools")
                saved = HistoryActivity({"id": "group", "kinds": ["read", "tools"], "count": 2})
                await app.query_one(HistoryViewport).mount(saved)
                await saved.populate({"activities": [
                    {"kind": "tool", "toolName": "read", "callId": "read", "arguments": {"filePath": "file.txt"}},
                    {"kind": "tool", "toolName": "bash", "callId": "bash", "arguments": {"command": "true"}},
                ]})
                for activity in (live, saved):
                    activity.toggle.expanded = True
                    activity.body.display = True
                await pilot.pause()
                for activity in (live, saved):
                    with self.subTest(restored=activity is saved):
                        read = activity.query_one(ReadView)
                        title = activity.query_one(ToolView).query_one("CollapsibleTitle")
                        self.assertEqual(read.content_region.x, title.content_region.x + 2)
