"""Immutable historical code, grouped by unified-diff zones (three context lines)."""
import re
from dataclasses import dataclass
from rich.text import Text
from .stable_scroll import StableScroll
from .scroll_window import ScrollWindow
from textual.geometry import Size
from textual.widgets import Static
from .config import ALGAE_MD, BOLD_MD, MUTED_COLOR, SLOP_COLOR
from .code_colors import highlighted_lines, ADDED_BACKGROUND, REMOVED_BACKGROUND

HEADER = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def patch_rows(patch):
    rows = []
    for line in patch:
        style = ALGAE_MD if line.startswith("+") and not line.startswith("+++") else BOLD_MD if line.startswith("-") and not line.startswith("---") else ""
        rows.append((None, line[1:] if style else line, style))
    return rows


def change_signs(diff):
    patch = diff["patch"]
    added = any(line.startswith("+") and not line.startswith("+++") for line in patch.splitlines()) or "new file mode" in patch
    removed = any(line.startswith("-") and not line.startswith("---") for line in patch.splitlines()) or "deleted file mode" in patch
    if patch and not added and not removed:
        added = removed = True  # Binary or metadata-only modification.
    return added, removed


@dataclass
class ChangeZone:
    path: str
    rows: list
    center: int
    label: str
    hunk: int = 0
    order: int | None = None


def change_zones(diffs):
    result = []
    for diff in diffs:
        orders = diff.get("hunk_order", [])
        patch = diff["patch"].splitlines()
        headers = [(i, HEADER.match(line)) for i, line in enumerate(patch) if HEADER.match(line)]
        if not headers or "after" not in diff:
            result.append(ChangeZone(diff["path"], patch_rows(patch), 0, diff["path"], order=min(orders) if orders else None))
            continue
        source = diff["after"].splitlines()
        # Earlier saved reviews have patches but no historical file contents.
        if any(int(match[3]) - (1 if match[4] != "0" else 0) + (int(match[4]) if match[4] is not None else 1) > len(source)
               for _, match in headers):
            result.append(ChangeZone(diff["path"], patch_rows(patch), 0, diff["path"], order=min(orders) if orders else None))
            continue
        rows, cursor, centers = [], 0, []
        for position, (offset, match) in enumerate(headers):
            start = int(match[3])
            count = int(match[4]) if match[4] is not None else 1
            start = start - 1 if count else start
            while cursor < start:
                rows.append((cursor + 1, source[cursor], "")); cursor += 1
            centers.append(len(rows))
            end = headers[position + 1][0] if position + 1 < len(headers) else len(patch)
            for line in patch[offset + 1:end]:
                if line.startswith("-"):
                    rows.append((None, line[1:], BOLD_MD))
                elif line.startswith(("+", " ")):
                    rows.append((cursor + 1, line[1:], ALGAE_MD if line.startswith("+") else ""))
                    cursor += 1
        while cursor < len(source):
            rows.append((cursor + 1, source[cursor], "")); cursor += 1
        for position, center in enumerate(centers):
            match = headers[position][1]
            result.append(ChangeZone(diff["path"], rows, center, f'{diff["path"]}:{match[3]}', position,
                                     orders[position] if position < len(orders) else None))
    return sorted(result, key=lambda zone: (zone.order is None, zone.order or 0))


class HistoricalCode(StableScroll):
    """Render a bounded window inside the measured full-document extent."""
    DEFAULT_CSS = "HistoricalCode { scrollbar-gutter: stable; }"
    WINDOW = 40

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.zone = None
        self.start = self.end = 0
        self.loading = False
        self.offsets = []
        self.rendered_width = None
        self.document = None
        self.wrapped_lines = []
        self.measured_zone = None

    def show_zone(self, zone):
        with self.app.batch_update():
            self._show_zone(zone)

    def _show_zone(self, zone):
        self.remove_class("description", "centered")
        self.zone = zone
        self.loading = True
        # Fill even a tall pane before the first paint, with enough margin not
        # to trigger successive extensions just because the hunk was opened.
        margin = max(self.WINDOW, (self.content_size.height or self.parent.content_size.height) + 6)
        self.start = max(0, zone.center - margin)
        self.end = min(len(zone.rows), zone.center + margin +
                       (self.content_size.height or self.parent.content_size.height))
        self.render_window()
        self.refresh(layout=True)
        # Make the new extent available before the hunk scroll is clamped;
        # the normal layout pass will confirm this size on the next refresh.
        self.virtual_size = Size(self.rendered_width, self.document.total)
        self.scroll_to(y=self.document.offsets[zone.center], animate=False, immediate=True, force=True)
        def positioned():
            if self.zone is not zone:
                return
            self._scroll_update(self.virtual_size)
            self.scroll_to(y=self.document.offsets[zone.center], animate=False, immediate=True, force=True)
            self.loading = False
            self.call_after_refresh(self.on_resize)
        self.call_after_refresh(positioned)

    def show_description(self, text, *, centered=False):
        self.zone = None
        self.loading = False
        self.add_class("description")
        self.set_class(centered, "centered")
        text.no_wrap = False
        text.overflow = "fold"
        self.query_one(Static).styles.padding = 0
        self.query_one(Static).update(text)
        self.scroll_home(animate=False)

    def render_window(self):
        # The hidden pane has no region yet. Its column already has the final
        # width: don't first wrap the entire file at one character per line.
        width = max(1, self.content_size.width or self.parent.content_size.width)
        digits = len(str(max((number or 0 for number, _, _ in self.zone.rows), default=0)))
        gutter = digits + 3
        if self.measured_zone is not self.zone or self.rendered_width != width:
            lines = highlighted_lines(self.zone.path, "\n".join(line for _, line, _ in self.zone.rows))
            self.wrapped_lines = [line.wrap(self.app.console, max(1, width - gutter), overflow="fold") or [Text()]
                                  for line in lines]
            self.document = ScrollWindow(len(lines) for lines in self.wrapped_lines)
            self.measured_zone = self.zone
        self.rendered_width = width
        text, self.offsets = Text(no_wrap=True), []
        visual = 0
        for index in range(self.start, self.end):
            number, line, style = self.zone.rows[index]
            self.offsets.append(visual)
            sign = "+" if style == ALGAE_MD else "-" if style == BOLD_MD else " "
            background = ADDED_BACKGROUND if sign == "+" else REMOVED_BACKGROUND if sign == "-" else None
            wrapped = self.wrapped_lines[index]
            for continuation, code in enumerate(wrapped or [Text()]):
                row = Text(style=f"on {background}" if background else "")
                row.append(f'{str(number) if number is not None and not continuation else "":>{digits}} ', style=MUTED_COLOR)
                row.append((sign if not continuation else " ") + " ", style=ALGAE_MD if sign == "+" else SLOP_COLOR if sign == "-" else "")
                row.append_text(code)
                row.pad_right(max(0, width - row.cell_len))
                text.append_text(row)
                text.append("\n")
                visual += 1
        self.offsets.append(visual)
        top, bottom = self.document.padding(self.start, self.end)
        self.query_one(Static).styles.padding = (top, 0, bottom, 0)
        self.query_one(Static).update(text[:-1] if text else text)

    def on_resize(self):
        if self.zone:
            if not self.loading and self.content_size.width != self.rendered_width:
                anchor = self.document.anchor(self.scroll_y)
                self.render_window()
                self.virtual_size = Size(self.rendered_width, self.document.total)
                self.scroll_to(y=self.document.position(anchor), animate=False, immediate=True, force=True)
            self.call_after_refresh(self.extend_window)

    def watch_scroll_y(self, old_value, new_value):
        super().watch_scroll_y(old_value, new_value)
        if self.zone and not self.loading:
            self.call_after_refresh(self.extend_window)

    def extend_window(self):
        if not self.zone or self.loading:
            return
        start, end = self.document.around(self.scroll_y, self.content_size.height, self.WINDOW)
        if start >= self.start and end <= self.end:
            return
        self.loading = True
        self.start, self.end = start, end
        self.render_window()
        self.loading = False
