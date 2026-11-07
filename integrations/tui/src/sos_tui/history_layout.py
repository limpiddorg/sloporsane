"""Keep enough trailing space to place the latest user message at the top."""

from textual.geometry import Size, Spacing
from textual.layout import ArrangeResult
from textual.layouts.vertical import VerticalLayout
from textual.widget import Widget
from collections.abc import Callable

from .history_window import HistoryWindow
from .user_message import UserMessage


class HistoryLayout(VerticalLayout):
    def __init__(self, window: HistoryWindow) -> None:
        super().__init__()
        self.window = window
        self.scroll_anchor: Callable[[], Widget | None] | None = None
        self.anchor_y: int | None = None

    def arrange(self, parent: Widget, children: list[Widget], size: Size, greedy: bool = True) -> ArrangeResult:
        placements = super().arrange(parent, children, size, greedy)
        user = next((item for item in reversed(placements) if isinstance(item.widget, UserMessage)), None)
        anchor_y = None
        anchor = self.scroll_anchor() if self.scroll_anchor is not None else None
        if anchor is not None and anchor.is_attached:
            child = anchor
            while child.parent is not None and child.parent is not parent:
                child = child.parent
            placement = next((item for item in placements if item.widget is child), None)
            if placement is not None:
                # A branch update replaces its message widgets atomically. Keep
                # the extent until the new anchor has received its first layout.
                anchor_y = (placement.region.y + anchor.region.y - child.region.y
                            if anchor.region.height else self.anchor_y)
        self.anchor_y = anchor_y
        if anchor_y is None and user is not None and (self.window.latest or user.widget.has_class("pending-turn")):
            anchor_y = user.region.y
        if anchor_y is not None and placements:
            last = placements[-1]
            margin = last.margin
            bottom = max(margin.bottom, anchor_y + size.height - last.region.bottom)
            # Extend only the scrollable extent, not the last widget's painted area.
            placements[-1] = last._replace(margin=Spacing(margin.top, margin.right, bottom, margin.left))
        return placements
