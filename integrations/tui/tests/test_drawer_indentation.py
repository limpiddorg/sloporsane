"""Drawer contents start two cells after the first title letter."""

import unittest
from unittest.mock import patch

from textual.widgets import Collapsible, Static

from sos_tui.app import SosApp
from sos_tui.chat_collapsible import ChatCollapsible, CompactionDrawer
from sos_tui.history_viewport import HistoryViewport
from sos_tui.response_markdown import ResponseMarkdown
from sos_tui.tool_view import ToolView


class DrawerIndentationTest(unittest.IsolatedAsyncioTestCase):
    async def test_activity_children_and_open_contents(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(120, 50)) as pilot:
                activity = app.ensure_activity()
                reasoning = ChatCollapsible(ResponseMarkdown("Thinking"), title="Reasoning", classes="reasoning",
                                            collapsed=False, collapsed_symbol="▸", expanded_symbol="▾")
                tool = ToolView("call", "bash", {"command": "echo hello"})
                activity.add_activity(reasoning, "reasoning")
                activity.add_activity(tool, "tools")
                activity.body.display = True
                tool.collapsed = False
                await pilot.pause()
                for child in (reasoning, tool):
                    title = child.query_one("CollapsibleTitle")
                    self.assertEqual(title.content_region.x, activity.toggle.content_region.x + 2)
                    contents = child.query_one(Collapsible.Contents)
                    self.assertEqual(contents.content_region.x, title.content_region.x + 4)
                self.assertEqual(reasoning.query_one(ResponseMarkdown).content_region.x,
                                 reasoning.query_one("CollapsibleTitle").content_region.x + 4)
                self.assertEqual(tool.arguments_view.content_region.x,
                                 tool.query_one("CollapsibleTitle").content_region.x + 4)

    async def test_inline_and_discussion_compaction_contents(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(120, 50)) as pilot:
                viewport = app.query_one(HistoryViewport)
                drawers = [CompactionDrawer(),
                           ChatCollapsible(Static("Summary"), title="Compaction", classes="compaction sane-compaction"),
                           ChatCollapsible(Static("Summary"), title="Compaction", classes="compaction")]
                await viewport.mount_live(*drawers)
                await drawers[0].finish("Summary")
                for drawer in drawers:
                    drawer.collapsed = False
                await pilot.pause()
                for drawer in drawers:
                    self.assertEqual(drawer.query_one(Collapsible.Contents).content_region.x,
                                     drawer.query_one("CollapsibleTitle").content_region.x + 4)
