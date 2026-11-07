"""File-only projections of mutation calls, without executing or validating patches."""
import re

from rich.text import Text
from textual.widgets._collapsible import CollapsibleTitle

from .config import MUTED_COLOR
from .workspace_path import compact_path


FILE_TOOLS = {"apply_patch", "edit", "write"}
PATCH_HEADER = re.compile(r"^\*\*\* (?:Add|Update|Delete) File: (.+)$")


def append_title_path(text, path, width=None, separator=" • "):
    """Fit a styled title and both ends of its path on a single line."""
    if width is not None:
        if width <= 0:
            return text[:0]
        if width <= len(separator):
            text.truncate(width, overflow="ellipsis")
            return text
        reserved = min(8, width - len(separator))
        prefix_width = width - len(separator) - reserved
        if prefix_width:
            text.truncate(prefix_width, overflow="ellipsis")
        else:
            text = text[:0]
        path = compact_path(path, width - text.cell_len - len(separator))
    text.append(separator)
    text.append(path, style=f"not bold {MUTED_COLOR}")
    return text


def tool_files(tool, arguments, result=None):
    metadata = (result or {}).get("metadata") or {}
    if tool == "apply_patch":
        if metadata.get("files"):
            return metadata["files"]
        if "patchText" not in arguments and isinstance(arguments.get("filePaths"), list):
            return [{"filePath": path} for path in arguments["filePaths"] if isinstance(path, str)]
        files = []
        patch = arguments.get("patchText", "")
        if not isinstance(patch, str):
            return files
        for line in patch.splitlines():
            if match := PATCH_HEADER.match(line):
                files.append({"filePath": match[1]})
            elif line.startswith("*** Move to: ") and files:
                files[-1]["movePath"] = line[13:]
        return files
    if tool in ("edit", "write"):
        path = metadata.get("filepath") or arguments.get("filePath")
        return [{"filePath": path}] if isinstance(path, str) and path else []
    return []


def file_path(file):
    return file.get("relativePath") or file.get("movePath") or file.get("filePath", "")


def file_result(result, index):
    if index is None or not (result.get("metadata") or {}).get("files"):
        return result
    metadata = result["metadata"]
    return {**result, "metadata": {**metadata, "files": metadata["files"][index:index + 1]}}


class ToolPathTitle(CollapsibleTitle):
    DEFAULT_CSS = "ToolPathTitle { width: 1fr; height: 1; }"

    def __init__(self, owner):
        self.owner = owner
        super().__init__(label=owner.title, collapsed_symbol="▸", expanded_symbol="▾", collapsed=owner.collapsed)

    def render(self):
        text = Text("▸ " if self.collapsed else "▾ ", style="not bold", no_wrap=True, overflow="crop")
        text.append_text(self.owner.title_text(max(0, self.content_size.width - 2)))
        return text

    def on_resize(self):
        self.refresh()
