"""Workspace paths keep their beginning, parent and leaf at footer widths."""
import unittest
from unittest.mock import patch

from rich.cells import cell_len
from rich.text import Text

from sos_tui.app import SosApp
from sos_tui.workspace_path import WorkspacePath, compact_path


class CompactPathTest(unittest.TestCase):
    def test_short_paths_are_unchanged(self):
        for path in ("/", "/worktrees/feature", "/tests/me/project"):
            self.assertEqual(compact_path(path, cell_len(path)), path)
            self.assertEqual(compact_path(path, 100), path)

    def test_middle_is_removed_and_parent_and_leaf_are_preserved(self):
        path = "/example-root/fixtures/projects/source/sample-application"
        self.assertEqual(compact_path(path, 45), "/example-root/…/source/sample-application")
        shorter = compact_path(path, 35)
        self.assertTrue(shorter.startswith("/exa"))
        self.assertTrue(shorter.endswith("/source/sample-application"))
        self.assertIn("…", shorter)
        self.assertLessEqual(cell_len(shorter), 35)

    def test_longest_complete_prefix_is_kept_when_it_fits(self):
        self.assertEqual(compact_path("/tests/me/work/very-long-chain/parent/project", 35),
                         "/tests/me/work/…/parent/project")

    def test_unicode_and_very_narrow_widths_never_overflow(self):
        for path in ("/tests/élève/工作/工程/项目", "/a/very-long-parent/a-very-long-project-name", "project-name"):
            for width in range(0, cell_len(path) + 2):
                rendered = compact_path(path, width)
                self.assertLessEqual(cell_len(rendered), width)
                if 4 <= width < cell_len(path):
                    self.assertIn("…", rendered)
                    self.assertTrue(rendered.endswith(path[-1]))
        self.assertEqual(compact_path("/a/b", 0), "")


class WorkspacePathTest(unittest.IsolatedAsyncioTestCase):
    async def test_footer_resizes_and_updates_without_losing_path_end_or_changing_source(self):
        path = "/example-root/fixtures/projects/source/sample-application"
        app = SosApp()
        app.workspace = path
        with patch.object(app, "listen"):
            async with app.run_test(size=(90, 24)) as pilot:
                widget = app.query_one("#workspace", WorkspacePath)
                await pilot.pause()
                self.assertEqual(widget.render().plain, "/example-root/…/source/sample-application")
                self.assertIn("/source/sample-application", widget.render_line(0).text)
                self.assertEqual(app.workspace, path)
                self.assertEqual(widget.content, path)

                await pilot.resize_terminal(180, 24)
                await pilot.pause()
                self.assertEqual(widget.render().plain, path)

                await pilot.resize_terminal(70, 24)
                await pilot.pause()
                self.assertTrue(widget.render().plain.endswith("/source/sample-application"))
                self.assertLessEqual(cell_len(widget.render().plain), widget.content_size.width)

                replacement = "/fixtures/sample/worktrees/deep/nested/parent/feature"
                widget.update(Text(replacement))
                await pilot.pause()
                self.assertTrue(widget.render().plain.startswith("/fixtures"))
                self.assertTrue(widget.render().plain.endswith("/parent/feature"))
                self.assertIn("/parent/feature", widget.render_line(0).text)
                self.assertEqual(str(widget.content), replacement)
