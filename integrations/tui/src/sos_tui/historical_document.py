"""Shared historical source preparation, independent of the selected hunk."""
from collections import OrderedDict

from rich.cells import cell_len
from rich._wrap import divide_line
from rich.text import Text

from .scroll_window import ScrollWindow


class HistoricalDocument:
    def __init__(self, path, rows):
        self.path, self.rows = path, rows
        self.digits = len(str(max((number or 0 for number, _, _ in rows), default=0)))
        self.source = [line for _, line, _ in rows]
        # Match Rich's tab expansion, including wide characters before a tab.
        for index, line in enumerate(self.source):
            if "\t" in line:
                text = Text(line)
                text.expand_tabs(8)
                self.source[index] = text.plain
        self.lengths = [cell_len(line) for line in self.source]
        self.syntax = None
        self.preparing = False
        self.widths = OrderedDict()

    def layout(self, width):
        width = max(1, width - self.digits - 4)
        if width not in self.widths:
            # Measure strings, not styled Text objects. Only the visible window
            # needs wrapping; the entire document still has an exact extent.
            heights = [1 if length <= width else len(divide_line(line, width, fold=True)) + 1
                       for line, length in zip(self.source, self.lengths)]
            self.widths[width] = (ScrollWindow(heights), {})
            if len(self.widths) > 2:
                self.widths.popitem(last=False)
        self.widths.move_to_end(width)
        return width, self.widths[width]

    def wrapped(self, index, width, cache, console):
        if index not in cache:
            text = self.syntax[index] if self.syntax is not None else Text(self.source[index])
            cache[index] = text.wrap(console, width, overflow="fold") or [Text()]
        return cache[index]

    def set_syntax(self, lines):
        self.syntax = lines
        # Plain and styled wrapping have identical geometry.
        for _, cache in self.widths.values():
            cache.clear()
