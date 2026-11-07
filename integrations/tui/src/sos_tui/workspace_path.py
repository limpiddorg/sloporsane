"""Keep workspace identity and its parent visible in the session bar."""
from rich.cells import cell_len
from rich.text import Text
from textual.widgets import Static


def compact_path(path: str, width: int) -> str:
    if width <= 0:
        return ""
    if cell_len(path) <= width:
        return path
    parts = path.strip("/").split("/")
    root = "/" if path.startswith("/") else ""
    if len(parts) > 2:
        suffix = "/…/" + "/".join(parts[-2:])
        for count in range(len(parts) - 2, 0, -1):
            prefix = root + "/".join(parts[:count])
            if cell_len(prefix + suffix) <= width:
                return prefix + suffix
        # Preserve parent and leaf, allowing a partial first component before the ellipsis.
        suffix = "…/" + "/".join(parts[-2:])
        available = width - cell_len(suffix)
        if available > 0:
            prefix = Text(root + parts[0])
            prefix.truncate(available, overflow="crop")
            return prefix.plain + suffix
    # If whole components cannot fit, preserve both ends with priority to the leaf.
    left_width = (width - 1) // 3
    right_width = width - 1 - left_width
    prefix = Text(path) if left_width > 0 else Text()
    prefix.truncate(left_width, overflow="crop")
    start = len(path)
    while start > 0 and cell_len(path[start - 1:]) <= right_width:
        start -= 1
    return prefix.plain + "…" + path[start:]


class WorkspacePath(Static):
    def render(self) -> Text:
        style = self.content.style if isinstance(self.content, Text) else ""
        return Text(compact_path(str(self.content), self.content_size.width), style=style,
                    no_wrap=True, overflow="crop")

    def on_resize(self) -> None:
        self.refresh()
