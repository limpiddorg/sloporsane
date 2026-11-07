"""Shallow snapshot reads and a compact, bounded projection of changed paths."""
import unittest
from types import SimpleNamespace
from unittest.mock import patch
from rich.style import Style

from textual.app import App
from sos_tui.implementation_changes_tree import ImplementationChangesTree, SELECTED_BACKGROUND, HOVER_BACKGROUND
from sos_tui.config import ALGAE_MD, DEPTH_MD, SLOP_COLOR

BASELINE = ("session", "/repository", "before-tree")


def entry(name, directory=False):
    return {"name": name, "directory": directory}


class TreeApp(App):
    def __init__(self):
        super().__init__()
        self.requests = []

    def compose(self):
        yield ImplementationChangesTree()

    def on_implementation_changes_tree_directory_requested(self, event):
        self.requests.append((event.baseline, event.path))


class ImplementationChangesTreeTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = TreeApp()
        self.pilot = await self.enterAsyncContext(self.app.run_test())
        self.tree = self.app.query_one(ImplementationChangesTree)

    def show(self, changes=(), zones=(), selected=0, *, step=0, baseline=BASELINE):
        self.tree.show_files(baseline, changes, zones, str, selected, step=step)

    def reply(self, path, entries):
        self.tree.accept_directory(BASELINE, path, entries)

    def displayed(self):
        return [(self.tree.compact_labels.get(line.node.id, line.node.label.plain), len(line.path))
                for line in self.tree._tree_lines]

    async def test_deep_chain_compacts_but_preserves_file_name_and_bifurcations(self):
        prefix = "src/features/editor/review/components/panels/helpers"
        filename = "implementation_workspace_validation.py"
        changes = [{"path": f"{prefix}/{filename}", "patch": "+new\n"},
                   {"path": "src/features/editor/tests/unit/test_validation.py", "patch": "+test\n"}]
        self.show(changes)
        await self.pilot.pause()
        rows = self.displayed()
        self.assertEqual(rows, [("Changes", 1), ("src/features/editor", 2),
                               ("review/components/panels/helpers", 3), ("+ " + filename, 4),
                               ("tests/unit", 3), ("+ test_validation.py", 4)])
        self.assertEqual(set(path for _, path in self.app.requests), {"", prefix, "src/features/editor/tests/unit"})
        node = self.tree.files[f"{prefix}/{filename}"]
        self.tree.center_file(node)
        self.assertIs(self.tree.cursor_node, node)

    async def test_folder_chain_shortens_in_middle_only_when_panel_is_narrow(self):
        filename = "implementation_workspace_validation.py"
        path = "src/features/editor/review/components/panels/helpers/" + filename
        self.show([{"path": path, "patch": "+new\n"}])
        await self.pilot.resize_terminal(30, 24)
        await self.pilot.pause()
        labels = [label for label, _ in self.displayed()]
        self.assertIn("src/.../panels/helpers", labels)
        self.assertIn("+ " + filename, labels)
        await self.pilot.resize_terminal(100, 24)
        await self.pilot.pause()
        self.assertIn("src/features/editor/review/components/panels/helpers", [label for label, _ in self.displayed()])

    async def test_final_folder_shows_five_including_late_changed_files(self):
        changes = [{"path": f"src/deep/{name}.py", "patch": "+new\n"} for name in ("y", "z")]
        self.show(changes)
        self.reply("src/deep", [entry(f"{name}.py") for name in ("a", "b", "c", "d", "e", "x", "y", "z")])
        await self.pilot.pause()
        self.assertEqual([label for label, _ in self.displayed()],
                         ["Changes", "src/deep", "a.py", "b.py", "c.py", "+ y.py", "+ z.py", "...", " Show all "])
        self.assertEqual(len(self.tree.files), 8)  # Hidden neighbors stay cached, not deleted.

    async def test_all_changes_survive_limit_and_marker_only_means_hidden_neighbors(self):
        changes = [{"path": f"src/{index}.py", "patch": "+new\n"} for index in range(7)]
        self.show(changes)
        self.reply("src", [entry(f"{index}.py") for index in range(8)])
        await self.pilot.pause()
        self.assertEqual([label for label, _ in self.displayed()],
                         ["Changes", "src", *[f"+ {index}.py" for index in range(7)], "...", " Show all "])
        self.show(changes + [{"path": "src/7.py", "patch": "+new\n"}])
        await self.pilot.pause()
        self.assertNotIn("...", [label for label, _ in self.displayed()])

    async def test_each_final_folder_has_its_own_limit_and_changed_branch_stays_visible(self):
        changes = [{"path": path, "patch": "+new\n"} for path in ("src/z.py", "src/child/z.py")]
        self.show(changes)
        self.reply("src", [entry(f"{index}.py") for index in range(8)] + [entry("child", True), entry("z.py")])
        self.reply("src/child", [entry(f"{index}.py") for index in range(8)] + [entry("z.py")])
        await self.pilot.pause()
        rows = self.tree._tree_lines
        self.assertEqual(sum(line.node.label.plain == "..." for line in rows), 2)
        self.assertTrue(all(node in [line.node for line in rows] for node in self.tree.files.values()
                            if node.label.plain.startswith("+")))
        folder = self.tree.folders["src/child"]
        folder.collapse()
        await self.pilot.pause()
        self.assertNotIn(self.tree.files["src/child/z.py"], [line.node for line in self.tree._tree_lines])
        folder.expand()
        await self.pilot.pause()
        self.assertIn(self.tree.files["src/child/z.py"], [line.node for line in self.tree._tree_lines])

    async def test_compacted_folder_mouse_toggle_keeps_file_and_folder_identity(self):
        path = "src/feature/deep/file.py"
        self.show([{"path": path, "patch": "+new\n"}])
        await self.pilot.pause()
        folder, file = self.tree.folders["src/feature/deep"], self.tree.files[path]
        await self.pilot.click(self.tree, offset=(self.tree.guide_depth, 1))
        await self.pilot.pause()
        self.assertFalse(folder.is_expanded)
        self.assertNotIn(file, [line.node for line in self.tree._tree_lines])
        await self.pilot.click(self.tree, offset=(self.tree.guide_depth, 1))
        await self.pilot.pause()
        self.assertTrue(folder.is_expanded)
        self.assertIn(file, [line.node for line in self.tree._tree_lines])
        self.assertIs(self.tree.files[path], file)
        self.assertEqual([path for _, path in self.app.requests], ["", "src/feature/deep"])
        self.assertIsNot(self.app.focused, self.tree)

    async def test_initial_tree_requests_only_root_and_keeps_direct_folders_empty(self):
        self.show()
        await self.pilot.pause()
        self.assertEqual(self.app.requests, [(BASELINE, "")])
        self.reply("", [entry("README.md"), entry("src", True), entry("huge", True)])
        self.assertEqual(set(self.tree.files), {"README.md"})
        self.assertEqual(set(self.tree.folders), {"src", "huge"})
        self.assertFalse(self.tree.folders["huge"].children)
        self.assertFalse(self.tree.folders["huge"].is_expanded)
        for _ in range(10):
            self.show()
        await self.pilot.pause()
        self.assertEqual(self.app.requests, [(BASELINE, "")])

    async def click_toggle(self):
        node = self.tree.toggle_node
        self.tree.scroll_to_node(node, animate=False)
        await self.pilot.pause()
        await self.pilot.click(self.tree, offset=(2, node.line - int(self.tree.scroll_y)))
        await self.pilot.pause()

    async def test_toggle_reveals_neighbors_and_full_structure_without_recursive_reads(self):
        path = "src/features/editor/review/changed.py"
        self.show([{"path": path, "patch": "+new\n"}], [SimpleNamespace(path=path)], 1)
        self.reply("src/features/editor/review", [entry(f"neighbor-{i}.py") for i in range(8)] +
                   [entry("changed.py"), entry("archive", True)])
        await self.pilot.pause()
        original = self.tree.files[path]
        marker = next(line for line in self.tree._tree_lines if line.node.label.plain == "...")
        await self.pilot.click(self.tree, offset=(self.tree.guide_depth + 1, marker.node.line))
        await self.pilot.pause()
        self.assertFalse(self.tree.show_all)
        self.assertIs(self.tree.cursor_node, original)

        await self.click_toggle()
        self.assertTrue(self.tree.show_all)
        labels = [label for label, _ in self.displayed()]
        self.assertEqual(labels[-1], " Compact ")
        self.assertNotIn("...", labels)
        self.assertTrue(all(f"neighbor-{i}.py" in labels for i in range(8)))
        self.assertTrue(all(name in labels for name in ("src", "features", "editor", "review", "archive")))
        self.assertFalse(self.tree.folders["src/features/editor/review/archive"].is_expanded)
        self.assertEqual({directory for _, directory in self.app.requests},
                         {"", "src", "src/features", "src/features/editor", "src/features/editor/review"})
        self.assertIs(self.tree.files[path], original)
        self.assertIsNot(self.app.focused, self.tree)

        archive = self.tree.folders["src/features/editor/review/archive"]
        archive.expand()
        await self.pilot.pause()
        self.assertEqual(self.app.requests[-1][1], "src/features/editor/review/archive")
        self.reply("src/features/editor/review/archive", [entry("nested", True)])
        self.assertFalse(self.tree.folders["src/features/editor/review/archive/nested"].is_expanded)

        # New siblings arrive only for the ancestors opened by Show all.
        self.reply("src", [entry("features", True), entry("other", True)])
        await self.pilot.pause()
        self.assertFalse(self.tree.folders["src/other"].is_expanded)
        self.show([{"path": path, "patch": "+new\n"}], [SimpleNamespace(path=path)], 1)
        await self.pilot.pause()
        self.assertTrue(self.tree.show_all)  # Live refresh preserves the choice.
        await self.click_toggle()
        self.assertFalse(self.tree.show_all)
        self.assertEqual(self.displayed()[-1][0], " Show all ")
        self.assertIn("...", [label for label, _ in self.displayed()])
        self.assertIs(self.tree.files[path], original)

    async def test_path_ellipsis_alone_enables_toggle_and_context_switch_resets_it(self):
        path = "src/features/editor/review/components/panels/helpers/changed.py"
        self.show([{"path": path, "patch": "+new\n"}])
        await self.pilot.resize_terminal(30, 12)
        await self.pilot.pause()
        self.assertEqual(self.displayed()[-1][0], " Show all ")
        await self.click_toggle()
        self.assertEqual(self.displayed()[-1][0], " Compact ")
        self.assertFalse(self.tree.compact_labels)
        self.show([{"path": path, "patch": "+new\n"}], step=1)
        await self.pilot.pause()
        self.assertFalse(self.tree.show_all)
        self.show(baseline=("session", "/repository", "other-tree"))
        await self.pilot.pause()
        self.assertEqual(self.displayed(), [("Changes", 1)])

    async def test_no_toggle_when_nothing_is_hidden_and_disappears_after_resize(self):
        self.show([{"path": "src/deep/file.py", "patch": "+new\n"}])
        await self.pilot.pause()
        self.assertNotIn(" Show all ", [label for label, _ in self.displayed()])
        path = "src/features/editor/review/components/panels/helpers/file.py"
        self.show([{"path": path, "patch": "+new\n"}])
        await self.pilot.resize_terminal(30, 12)
        await self.pilot.pause()
        self.assertEqual(self.displayed()[-1][0], " Show all ")
        await self.pilot.resize_terminal(100, 24)
        await self.pilot.pause()
        self.assertNotIn(" Show all ", [label for label, _ in self.displayed()])

    async def test_changed_ancestor_chain_only_requests_final_folder_neighbors(self):
        change = {"path": "src/deep/changed.py", "patch": "+new\n"}
        self.show([change])
        await self.pilot.pause()
        self.reply("", [entry("src", True), entry("docs", True)])
        await self.pilot.pause()
        self.assertEqual(set(self.tree.files), {"src/deep/changed.py"})
        self.assertEqual(set(self.tree.folders), {"src", "src/deep", "docs"})
        self.assertTrue(self.tree.folders["src"].is_expanded)
        self.assertTrue(self.tree.folders["src/deep"].is_expanded)
        self.assertFalse(self.tree.folders["docs"].is_expanded)
        self.assertEqual(self.app.requests, [(BASELINE, ""), (BASELINE, "src/deep")])
        self.tree.folders["src"].collapse()
        await self.pilot.pause()
        self.tree.folders["src"].expand()
        await self.pilot.pause()
        self.assertEqual(self.app.requests[-1], (BASELINE, "src"))
        self.reply("src", [entry("deep", True), entry("other.py")])
        self.assertIn("src/other.py", self.tree.files)
        self.assertNotIn("src/deep", self.tree.loaded)

    async def test_opening_folder_fetches_one_level_once_even_for_empty_folder(self):
        self.show()
        self.reply("", [entry("docs", True), entry("empty", True)])
        self.tree.folders["docs"].expand()
        await self.pilot.pause()
        self.reply("docs", [entry("guide.md"), entry("archive", True)])
        self.assertIn("docs/guide.md", self.tree.files)
        self.assertFalse(self.tree.folders["docs/archive"].children)
        self.tree.folders["docs"].collapse()
        await self.pilot.pause()
        self.tree.folders["docs"].expand()
        self.tree.folders["empty"].expand()
        await self.pilot.pause()
        self.reply("empty", [])
        self.tree.folders["empty"].collapse()
        await self.pilot.pause()
        self.tree.folders["empty"].expand()
        await self.pilot.pause()
        self.assertEqual([path for _, path in self.app.requests], ["", "docs", "empty"])

    async def test_step_switch_reuses_loaded_nodes_and_only_relabels_changed_files(self):
        first, last = "src/a.py", "src/b.py"
        self.show([{"path": first, "patch": "+new\n"}], [SimpleNamespace(path=first)], 1)
        self.reply("", [entry("src", True), entry("docs", True)])
        self.tree.folders["src"].collapse()
        await self.pilot.pause()
        self.tree.folders["src"].expand()
        await self.pilot.pause()
        self.reply("src", [entry("a.py"), entry("b.py")])
        original = dict(self.tree.files)
        with patch.object(self.tree, "clear", wraps=self.tree.clear) as clear, \
                patch.object(self.tree, "add_file", wraps=self.tree.add_file) as add, \
                patch.object(self.tree, "file_label", wraps=self.tree.file_label) as label:
            self.show([{"path": last, "patch": "-old\n"}], [SimpleNamespace(path=last)], 1, step=1)
            clear.assert_not_called()
            add.assert_not_called()
            self.assertEqual({call.args[0] for call in label.call_args_list}, {first, last})
        self.assertEqual(self.tree.files, original)
        self.assertIsNone(self.tree.files[first].data)
        self.assertEqual(self.tree.files[last].data, 1)
        self.assertEqual(self.tree.files[last].label, self.tree.file_label(last, {"patch": "-old\n"}, False))
        await self.pilot.pause()
        self.assertEqual([path for _, path in self.app.requests], ["", "src"])

    async def test_selection_and_hover_only_change_background_and_preserve_change_signs(self):
        path = "src/changed.py"
        self.show([{"path": path, "patch": "+new\n-old\n"}], [SimpleNamespace(path=path)], 1)
        await self.pilot.pause()
        node = self.tree.files[path]
        console = self.app.console
        before = node.label.copy()
        selected = self.tree.render_label(node, Style(), Style(color="yellow", bold=True, bgcolor=SELECTED_BACKGROUND))
        self.assertEqual(selected.get_style_at_offset(console, 0).color, Style(color=ALGAE_MD).color)
        self.assertEqual(selected.get_style_at_offset(console, 1).color, Style(color=SLOP_COLOR).color)
        filename = selected.get_style_at_offset(console, selected.plain.index("changed.py"))
        self.assertIsNone(filename.color)
        self.assertFalse(filename.bold)
        self.assertEqual(filename.bgcolor, Style(bgcolor=SELECTED_BACKGROUND).bgcolor)
        self.tree.hover_line = node.line
        hovered = self.tree.render_label(node, Style(), Style(bgcolor=SELECTED_BACKGROUND))
        self.assertEqual(hovered.get_style_at_offset(console, -1).bgcolor, Style(bgcolor=HOVER_BACKGROUND).bgcolor)
        self.assertEqual(node.label, before)
        strip = self.tree._render_line(node.line, 0, self.tree.size.width, Style())
        self.assertEqual(list(strip)[-1].style.bgcolor, Style(bgcolor=HOVER_BACKGROUND).bgcolor)

    async def test_toggle_has_classic_background_and_blue_only_on_hover(self):
        self.show([{"path": "src/changed.py", "patch": "+new\n"}])
        self.reply("src", [entry("changed.py")] + [entry(f"neighbor-{i}.py") for i in range(8)])
        await self.pilot.pause()
        for expanded in (False, True):
            if expanded:
                self.tree.toggle_compaction()
                await self.pilot.pause()
            node = self.tree.toggle_node
            self.assertEqual(node.label.plain, " Compact " if expanded else " Show all ")
            from sos_tui.config import CONTENT_BACKGROUND_COLOR, TEXT_COLOR
            for hover_line, background in ((-1, CONTENT_BACKGROUND_COLOR), (node.line, DEPTH_MD)):
                self.tree.hover_line = hover_line
                rendered = self.tree.render_label(node, Style(), Style(color="yellow", bgcolor=HOVER_BACKGROUND))
                style = rendered.get_style_at_offset(self.app.console, 1)
                self.assertEqual(style.color, Style(color=TEXT_COLOR).color)
                self.assertEqual(style.bgcolor, Style(bgcolor=background).bgcolor)
                self.assertTrue(style.bold)

    async def test_removed_change_prunes_only_unlisted_ancestor_chains(self):
        self.show([{"path": "new/deep/file.py", "patch": "+new\n"}])
        self.reply("", [entry("docs", True)])
        self.show()
        self.assertFalse(self.tree.files)
        self.assertEqual(set(self.tree.folders), {"docs"})

    async def test_late_directory_reply_cannot_contaminate_another_snapshot(self):
        self.show()
        other = ("session", "/repository", "next-tree")
        self.show(baseline=other)
        self.tree.accept_directory(BASELINE, "", [entry("stale.py")])
        self.assertFalse(self.tree.files)
        self.tree.accept_directory(other, "", [entry("current.py")])
        self.assertEqual(set(self.tree.files), {"current.py"})

    async def test_directory_failure_keeps_changes_and_retries_only_on_explicit_reopen(self):
        self.show([{"path": "src/changed.py", "patch": "+new\n"}])
        self.tree.fail_directory(BASELINE, "")
        self.assertIn("unavailable", str(self.tree.root.label))
        self.assertIn("src/changed.py", self.tree.files)
        self.show([{"path": "src/changed.py", "patch": "+new\n"}])
        await self.pilot.pause()
        self.assertEqual([path for _, path in self.app.requests], ["", "src"])
        self.tree.root.collapse()
        await self.pilot.pause()
        self.tree.root.expand()
        await self.pilot.pause()
        self.assertEqual([path for _, path in self.app.requests], ["", "src", ""])
        self.reply("", [entry("src", True)])
        self.assertEqual(str(self.tree.root.label), "Changes")

    async def test_missing_baseline_still_shows_changed_chains_without_requests(self):
        self.show([{"path": "src/changed.py", "patch": "+new\n"}], baseline=None)
        await self.pilot.pause()
        self.assertEqual(set(self.tree.files), {"src/changed.py"})
        self.assertFalse(self.app.requests)

    async def test_many_changes_only_request_distinct_final_folders(self):
        changes = [{"path": f"src/group-{i}/deep/changed.py", "patch": "+new\n"} for i in range(100)]
        self.show(changes)
        await self.pilot.pause()
        self.assertEqual({path for _, path in self.app.requests},
                         {"", *[f"src/group-{i}/deep" for i in range(100)]})
        self.assertEqual(len(self.app.requests), 101)
        self.assertEqual(len(self.tree.files), 100)
        self.assertEqual(len(self.tree.folders), 201)
