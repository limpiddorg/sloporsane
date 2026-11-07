"""Shallow snapshot directories with compact branches and bounded final folders."""
from pathlib import PurePosixPath
from rich.text import Text
from rich.cells import cell_len
from rich.style import Style
from textual.color import Color
from textual.message import Message
from textual.widgets import Tree
from textual.widgets._tree import _TreeLine
from textual.geometry import Size
from .config import ALGAE_MD, BACKGROUND_COLOR, CONTENT_BACKGROUND_COLOR, DEPTH_MD, SLOP_COLOR, TEXT_COLOR
from .implementation_code import change_signs

SELECTED_BACKGROUND = Color.parse(BACKGROUND_COLOR).blend(Color.parse(TEXT_COLOR), 0.06).hex
HOVER_BACKGROUND = Color.parse(BACKGROUND_COLOR).blend(Color.parse(TEXT_COLOR), 0.10).hex


class ImplementationChangesTree(Tree, can_focus=False):
    DEFAULT_CSS = f"""
    ImplementationChangesTree > .tree--cursor {{ background: {SELECTED_BACKGROUND}; text-style: none; }}
    ImplementationChangesTree > .tree--highlight-line {{ background: {HOVER_BACKGROUND}; }}
    """
    FOCUS_ON_CLICK = False
    VISIBLE_NEIGHBORS = 5

    class DirectoryRequested(Message):
        def __init__(self, baseline, path):
            super().__init__()
            self.baseline, self.path = baseline, path

    def __init__(self, **kwargs):
        super().__init__("Changes", **kwargs)
        self.center_scroll = True
        self.show_all = False
        self.toggle_node = None
        self.compact_labels, self.more_nodes = {}, {}
        self.final_directories = set()
        self.changed_directories = set()
        self.files, self.folders, self.node_paths = {}, {}, {}
        self.selected, self.current = 0, None
        self.context = self.baseline = None
        self.listed_files, self.listed_folders, self.loaded = set(), set(), set()
        self.pending_directories, self.errors, self.extra_paths = set(), set(), set()
        self.changed_paths, self.change_patches, self.zone_indexes = set(), {}, {}
        self.changes = {}
        self.unavailable = False

    def show_files(self, baseline, changes, zones, display_path, selected, *, unavailable=False, step=None):
        baseline = tuple(baseline) if baseline else None
        current = display_path(zones[selected - 1].path) if selected else None
        changes = {display_path(diff["path"]): diff for diff in changes}
        zone_indexes = {}
        for index, zone in enumerate(zones, 1):
            zone_indexes.setdefault(display_path(zone.path), index)
        step_changed = step != self.context
        if baseline != self.baseline:
            self.reset_baseline(baseline)
        elif step_changed:
            self.show_all = False
            for folder in self.folders.values():
                folder.collapse()
        self.context, self.changes = step, changes
        self.unavailable = unavailable
        self.update_root_label()
        self.sync_extra_paths(set(changes) | set(zone_indexes))
        self.request_directory("")
        self.update_files(changes, zone_indexes, current, expand_changes=step_changed)
        if (step_changed or selected != self.selected or current != self.current) and current in self.files:
            self.call_after_refresh(self.center_file, self.files[current])
        self.selected, self.current = selected, current

    def reset_baseline(self, baseline):
        self.baseline = baseline
        self.clear()
        self.show_all = False
        self.toggle_node = None
        self.compact_labels, self.more_nodes = {}, {}
        self.final_directories = set()
        self.changed_directories = set()
        self.files, self.folders, self.node_paths = {}, {}, {}
        self.listed_files, self.listed_folders, self.loaded = set(), set(), set()
        self.pending_directories, self.errors, self.extra_paths = set(), set(), set()
        self.changed_paths, self.change_patches, self.zone_indexes = set(), {}, {}
        self.selected, self.current = 0, None
        with self.prevent(Tree.NodeExpanded):
            self.root.expand()

    def request_directory(self, path, *, retry=False):
        if not self.baseline or path in self.loaded or path in self.pending_directories or (path in self.errors and not retry):
            return
        self.errors.discard(path)
        self.pending_directories.add(path)
        self.post_message(self.DirectoryRequested(self.baseline, path))

    def on_tree_node_expanded(self, event):
        if event.control is self:
            path = "" if event.node is self.root else self.node_paths.get(event.node.id, "").rstrip("/")
            if event.node is self.root or path in self.folders:
                self.request_directory(path, retry=True)

    def accept_directory(self, baseline, path, entries):
        if baseline != self.baseline or path not in self.pending_directories:
            return
        self.pending_directories.discard(path)
        self.loaded.add(path)
        self.errors.discard(path)
        if path in self.folders:
            self.folders[path].set_label(Text(PurePosixPath(path).name))
        for entry in entries:
            name = f"{path}/{entry['name']}" if path else entry["name"]
            if entry["directory"]:
                self.listed_folders.add(name)
                self.ensure_folder(name)
            else:
                self.listed_files.add(name)
                self.extra_paths.discard(name)
                if name not in self.files:
                    self.files[name] = self.add_file(name, self.file_label(name, self.changes.get(name), name == self.current),
                                                     self.zone_indexes.get(name))
        self.update_root_label()

    def fail_directory(self, baseline, path):
        if baseline == self.baseline and path in self.pending_directories:
            self.pending_directories.discard(path)
            self.errors.add(path)
            self.update_root_label()
            if path in self.folders:
                self.folders[path].set_label(Text(PurePosixPath(path).name + " (unavailable)"))

    def update_root_label(self):
        self.root.set_label("Changes (base unavailable)" if self.unavailable or "" in self.errors else "Changes")

    def sync_extra_paths(self, paths):
        extra = paths - self.listed_files
        for path in self.extra_paths - extra:
            self.remove_file(path)
        for path in sorted(extra - self.extra_paths):
            self.files[path] = self.add_file(path, self.file_label(path, None, False), None)
        self.extra_paths = extra

    def update_files(self, changes, zone_indexes, current, *, expand_changes=False):
        patches = {path: diff["patch"] for path, diff in changes.items()}
        for path in self.changed_paths | set(changes) | {self.current, current}:
            if path not in self.files:
                continue
            if patches.get(path) != self.change_patches.get(path) or (path == current) != (path == self.current):
                node = self.files[path]
                label = self.file_label(path, changes.get(path), path == current)
                if node.label != label:
                    node.set_label(label)
            if path in changes and (expand_changes or path not in self.changed_paths):
                self.expand_parents(self.files[path])
        for path in self.zone_indexes.keys() | zone_indexes.keys():
            if path in self.files:
                self.files[path].data = zone_indexes.get(path)
        if self.changed_paths != set(changes):
            self._invalidate()
        self.changed_paths, self.change_patches = set(changes), patches
        self.zone_indexes = zone_indexes
        self.final_directories = {str(PurePosixPath(path).parent) for path in changes}
        self.changed_directories = {str(parent) for path in changes for parent in PurePosixPath(path).parents}
        for directory in sorted(self.final_directories):
            self.request_directory("" if directory == "." else directory)
        if self.show_all:
            self.request_changed_ancestors()

    def request_changed_ancestors(self):
        for directory in sorted(self.changed_directories):
            self.request_directory("" if directory == "." else directory)

    def toggle_compaction(self):
        self.show_all = not self.show_all
        with self.prevent(Tree.NodeExpanded):
            self.root.expand()
            for path, folder in self.folders.items():
                if path in self.changed_directories:
                    folder.expand()
                else:
                    folder.collapse()
        if self.show_all:
            self.request_changed_ancestors()
        self._invalidate()

    def _on_click(self, event):
        line = event.style.meta.get("line")
        node = self.get_node_at_line(line) if line is not None else None
        if node is not None and node is self.toggle_node:
            event.stop()
            event.prevent_default()
            self.toggle_compaction()
        elif node is not None and node in self.more_nodes.values():
            event.stop()
            event.prevent_default()

    def ensure_folder(self, path):
        parent = self.root
        parts = PurePosixPath(path).parts
        for depth, name in enumerate(parts, 1):
            key = str(PurePosixPath(*parts[:depth]))
            if key not in self.folders:
                self.folders[key] = parent.add(Text(name), expand=False, before=self.insertion_point(parent, key + "/"))
                self.node_paths[self.folders[key].id] = key + "/"
            parent = self.folders[key]
        return parent

    def add_file(self, path, label, data):
        folder = str(PurePosixPath(path).parent)
        parent = self.root if folder == "." else self.ensure_folder(folder)
        node = parent.add_leaf(label, data=data, before=self.insertion_point(parent, path))
        self.node_paths[node.id] = path
        return node

    def insertion_point(self, parent, path):
        return next((node for node in parent.children if self.node_paths[node.id] > path), None)

    def remove_file(self, path):
        node = self.files.pop(path)
        self.node_paths.pop(node.id)
        node.remove()
        for parent in PurePosixPath(path).parents:
            key = str(parent)
            folder = self.folders.get(key)
            if folder is None or folder.children or key in self.listed_folders:
                break
            self.node_paths.pop(folder.id)
            self.folders.pop(key)
            folder.remove()

    def visible_children(self, node):
        children = list(node.children)
        path = self.node_paths.get(node.id, "").rstrip("/") or "."
        if self.show_all or path not in self.final_directories or len(children) <= self.VISIBLE_NEIGHBORS:
            return children
        required = {child for child in children if self.contains_change(child)}
        remaining = max(0, self.VISIBLE_NEIGHBORS - len(required))
        visible = []
        for child in children:
            if child in required:
                visible.append(child)
            elif remaining:
                visible.append(child)
                remaining -= 1
        if len(visible) < len(children):
            if node.id not in self.more_nodes:
                marker = self._add_node(node, Text("...", style="dim"), None)
                marker.allow_expand = False
                self.more_nodes[node.id] = marker
            visible.append(self.more_nodes[node.id])
        return visible

    def contains_change(self, node):
        path = self.node_paths.get(node.id, "")
        return path.rstrip("/") in self.changed_directories if path.endswith("/") else path in self.changed_paths

    def _build(self):
        # Keep the logical nodes/cache intact. Only Textual's line projection is
        # compacted, so late directory replies and live updates retain identities.
        cursor = self.cursor_node
        self.compact_labels = {}
        lines = []
        has_fold = False

        def append(node, ancestors, last):
            nonlocal has_fold
            chain = [node]
            while not self.show_all and node is not self.root and node.allow_expand and node.is_expanded:
                children = list(node.children)
                if len(children) != 1 or not children[0].allow_expand:
                    break
                node = children[0]
                chain.append(node)
            if len(chain) > 1:
                label = "/".join(part.label.plain for part in chain)
                width = max(3, self.content_size.width - len(ancestors) * self.guide_depth - 2)
                self.compact_labels[node.id] = self.compact_folder_label(label, width)
                has_fold |= self.compact_labels[node.id] != label
            for part in chain:
                part._line = len(lines)
            path = [*ancestors, node]
            lines.append(_TreeLine(path, last))
            if node.is_expanded:
                children = self.visible_children(node)
                has_fold |= any(child in self.more_nodes.values() for child in children)
                for index, child in enumerate(children):
                    append(child, path, index == len(children) - 1)

        append(self.root, [], True)
        if self.show_all or has_fold:
            if self.toggle_node is None:
                self.toggle_node = self._add_node(None, Text(""), None)
                self.toggle_node.allow_expand = False
            label = Text(" Compact " if self.show_all else " Show all ")
            if self.toggle_node.label != label:
                self.toggle_node.set_label(label)
            self.toggle_node._line = len(lines)
            lines.append(_TreeLine([self.toggle_node], True))
        self._tree_lines_cached = lines
        width = max((self.get_label_width(line.node) + line._get_guide_width(self.guide_depth, self.show_root)
                     for line in lines), default=self.size.width)
        self.virtual_size = Size(width, len(lines))
        self.cursor_line = next((index for index, line in enumerate(lines)
                                 if line.node is cursor), -1)

    @staticmethod
    def compact_folder_label(label, width):
        if cell_len(label) <= width:
            return label
        parts = label.split("/")
        for removed in range(1, len(parts) - 1):
            compact = "/".join([parts[0], "...", *parts[removed + 1:]])
            if cell_len(compact) <= width:
                return compact
        return "/".join([parts[0], "...", parts[-1]]) if len(parts) > 2 else label

    def on_mouse_move(self, event):
        line = event.style.meta.get("line")
        node = self.get_node_at_line(line) if line is not None else None
        self.tooltip = self.node_paths.get(node.id) if node is not None else None

    def render_label(self, node, base_style, style):
        if node is self.toggle_node:
            background = DEPTH_MD if node.line == self.hover_line else CONTENT_BACKGROUND_COLOR
            return Text(node.label.plain, style=Style(color=TEXT_COLOR, bgcolor=background, bold=True))
        # Tree's cursor style normally overrides the label's font and colors,
        # including the change signs. Selection and hover only tint the surface.
        style = Style(bgcolor=HOVER_BACKGROUND if node.line == self.hover_line else style.bgcolor)
        label = super().render_label(node, base_style, style)
        if node.id in self.compact_labels:
            prefix = len(label) - len(node.label)
            return label[:prefix] + Text(self.compact_labels[node.id], style=style)
        return label

    def _render_line(self, y, x1, x2, base_style):
        if y == self.cursor_line:
            base_style += Style(bgcolor=SELECTED_BACKGROUND)
        return super()._render_line(y, x1, x2, base_style)

    @staticmethod
    def file_label(path, change, active):
        label = Text()
        if change:
            added, removed = change_signs(change)
            if added:
                label.append("+", style=f"bold {ALGAE_MD}")
            if removed:
                label.append("-", style=f"bold {SLOP_COLOR}")
            if added or removed:
                label.append(" ")
        label.append(PurePosixPath(path).name)
        return label

    def expand_parents(self, node):
        # Automatic changed-path expansion must never fetch siblings/descendants.
        with self.prevent(Tree.NodeExpanded):
            parent = node.parent
            while parent is not None:
                parent.expand()
                parent = parent.parent

    def center_file(self, node):
        if node.id not in self.node_paths:
            return
        self.expand_parents(node)
        self.move_cursor(node)
        self.scroll_to_node(node, animate=False)
