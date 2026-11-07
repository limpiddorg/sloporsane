"""Tracked paste spans, independent of their (possibly identical) preview labels."""

from dataclasses import dataclass, replace
from itertools import islice
import re
from typing import TYPE_CHECKING

from rich.text import Text
from textual.document._edit import Edit

if TYPE_CHECKING:
    from .message_input import MessageInput


def compact_paste(text: str) -> bool:
    return len(text) >= 200 or len(text.splitlines()) >= 3


@dataclass(frozen=True, eq=False)
class Paste:
    text: str

    @property
    def label(self) -> str:
        words = [match.group() for match in islice(re.finditer(r"\S+", self.text), 3)]
        preview = Text(" ".join(words) or "Pasted text")
        preview.truncate(23, overflow="crop")
        return f"[Pasted text • {preview.plain}…]"


@dataclass(frozen=True, eq=False)
class PathPaste(Paste):
    @property
    def label(self) -> str:
        return f"[Path • {self.text}]"


@dataclass(frozen=True)
class PasteBlock:
    start: int
    end: int
    paste: Paste


def expand_range(text: str, blocks: tuple[PasteBlock, ...], start: int, end: int) -> str:
    parts = []
    cursor = start
    for block in blocks:
        if block.start >= start and block.end <= end:
            parts.extend((text[cursor:block.start], block.paste.text))
            cursor = block.end
    parts.append(text[cursor:end])
    return "".join(parts)


class PasteEdit(Edit):
    """Keep span metadata in TextArea's existing undo/redo history."""

    def __init__(self, edit: Edit, paste: Paste | None = None, *, pasted: bool = False) -> None:
        super().__init__(edit.text, edit.from_location, edit.to_location, edit.maintain_selection_offset)
        self.paste = paste
        self.before: tuple[PasteBlock, ...] = ()
        self.pasted = pasted or paste is not None
        self.before_ranges: tuple[tuple[int, int], ...] = ()

    def do(self, text_area: "MessageInput", record_selection: bool = True):
        start = text_area.document.get_index_from_location(self.top)
        end = text_area.document.get_index_from_location(self.bottom)
        self.before = text_area.paste_blocks
        self.before_ranges = text_area.pasted_ranges
        result = super().do(text_area, record_selection)
        inserted_end = text_area.document.get_index_from_location(result.end_location)
        delta = inserted_end - end
        blocks = []
        for block in self.before:
            if block.end <= start:
                blocks.append(block)
            elif block.start >= end:
                blocks.append(replace(block, start=block.start + delta, end=block.end + delta))
        if self.paste is not None:
            blocks.append(PasteBlock(start, inserted_end, self.paste))
        text_area.paste_blocks = tuple(sorted(blocks, key=lambda block: block.start))
        ranges = []
        for left, right in self.before_ranges:
            if left < start:
                ranges.append((left, min(right, start)))
            if right > end:
                ranges.append((max(left, end) + delta, right + delta))
        if self.pasted and inserted_end > start:
            ranges.append((start, inserted_end))
        text_area.pasted_ranges = tuple(sorted(ranges))
        return result

    def undo(self, text_area: "MessageInput"):
        result = super().undo(text_area)
        text_area.paste_blocks = self.before
        text_area.pasted_ranges = self.before_ranges
        return result
