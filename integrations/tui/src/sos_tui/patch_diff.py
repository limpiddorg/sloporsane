"""Side-by-side views of the actual unified diffs returned by apply_patch."""

import re
from itertools import zip_longest

from rich import box
from rich.padding import Padding
from rich.table import Table
from rich.text import Text
from textual.widgets import Static

from .config import ALGAE_MD, DEPTH_MD, MUTED_COLOR, SLOP_COLOR
from .code_colors import highlighted_lines, ADDED_BACKGROUND, REMOVED_BACKGROUND
from .workspace_path import WorkspacePath

HUNK = re.compile(r"^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@")


def diff_rows(patch):
    """Pair consecutive deletions/additions; retain context and original line numbers."""
    rows, removed, added = [], [], []
    before = after = None

    def flush():
        rows.extend(zip_longest(removed, added))
        removed.clear()
        added.clear()

    for line in patch.splitlines():
        match = HUNK.match(line)
        if match:
            flush()
            before, after = int(match[1]), int(match[2])
            rows.append(((None, line, "hunk"), (None, line, "hunk")))
        elif before is None:
            continue  # File headers, not source code.
        elif line.startswith("-"):
            removed.append((before, line[1:], "removed"))
            before += 1
        elif line.startswith("+"):
            added.append((after, line[1:], "added"))
            after += 1
        elif line.startswith(" "):
            flush()
            rows.append(((before, line[1:], "context"), (after, line[1:], "context")))
            before += 1
            after += 1
        elif line.startswith("\\"):
            # Preserve the no-final-newline marker on the side it describes.
            marker = (None, line, "hunk")
            if added:
                added.append(marker)
            elif removed:
                removed.append(marker)
            else:
                rows.append((marker, marker))
    flush()
    return rows


def highlighted_cells(rows, path):
    """Lex each side of each hunk independently, preserving multiline token state."""
    result = {}
    indices = [[], []]

    def flush():
        for side, positions in enumerate(indices):
            source = "\n".join(rows[index][side][1] for index in positions)
            for index, line in zip(positions, highlighted_lines(path, source)):
                result[index, side] = line
            positions.clear()

    for index, row in enumerate(rows):
        if row[0] is not None and HUNK.match(row[0][1]) and row[0][0] is None:
            flush()
        for side, cell in enumerate(row):
            if cell is not None and cell[0] is not None:
                indices[side].append(index)
    flush()
    return result


def diff_table(patch, path="file.txt"):
    table = Table(box=box.SQUARE, expand=True, show_edge=False, show_header=False, padding=(0, 1))
    table.add_column(ratio=1, min_width=1, overflow="fold")
    table.add_column(ratio=1, min_width=1, overflow="fold")
    rows = diff_rows(patch)
    syntax = highlighted_cells(rows, path)
    width = max((len(str(cell[0])) for row in rows for cell in row if cell and cell[0] is not None), default=1)
    for index, row in enumerate(rows):
        cells = []
        for side, cell in enumerate(row):
            text = Text()
            background = None
            if cell is not None:
                number, source, kind = cell
                text.append(f"{number:>{width}}  " if number is not None else " " * (width + 2), style=MUTED_COLOR)
                style = {"removed": SLOP_COLOR, "added": ALGAE_MD, "hunk": "dim"}.get(kind, "")
                text.append({"removed": "- ", "added": "+ "}.get(kind, "  "), style=style)
                if number is None:
                    text.append(source, style=style)
                else:
                    text.append_text(syntax[index, side])
                background = {"removed": REMOVED_BACKGROUND, "added": ADDED_BACKGROUND}.get(kind)
            cells.append(Padding(text, 0, style=f"on {background}") if background else text)
        table.add_row(*cells)
    return table


def patch_widgets(metadata):
    widgets = []
    for file in metadata.get("files", []):
        path = file.get("relativePath") or file.get("filePath", "Unknown file")
        title = Text(path, style=f"bold {DEPTH_MD}")
        widgets.append(WorkspacePath(title))
        patch = file.get("patch", "")
        if diff_rows(patch):
            widgets.append(Static(diff_table(patch, path), classes="tool-patch-diff"))
        else:
            widgets.append(Static(Text(patch or "No textual changes", style="dim")))
    return widgets
