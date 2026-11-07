"""Reusable global scrollbar for windowed content, and document reading anchors.

Content adapters expose logical coordinates independently of mounted widgets.
Native wheel / keyboard scrolling remains in screen lines; only the scrollbar
uses the logical document. Unknown heights therefore cannot resize its range.
"""
from dataclasses import dataclass

from textual.containers import VerticalScroll
from textual.scrollbar import ScrollTo


@dataclass(frozen=True)
class ScrollExtent:
    total: float
    position: float
    visible: float = 1


class StableScroll(VerticalScroll):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.pending_reading_anchor = None
        self._scrollbar_scale = 1
        self.reading_revision = 0

    def retain_reading_position(self):
        self.pending_reading_anchor = self.reading_anchor()

    def scroll_extent(self) -> ScrollExtent | None:
        """Return None for ordinary, fully loaded documents."""
        return None

    def seek(self, position: float) -> None:
        """Adapters translate a logical scrollbar destination into a load/scroll."""
        self.scroll_to(y=position, animate=False, immediate=True)

    def _sync_global_scrollbar(self):
        extent = self.scroll_extent()
        if extent is not None and self.show_vertical_scrollbar:
            bar = self.vertical_scrollbar
            # Textual's drag math uses terminal-row deltas, not logical units.
            # Scale the entire range so window_size remains a physical height.
            bar.window_size = max(1, self.container_size.height - self.scrollbar_size_horizontal)
            self._scrollbar_scale = bar.window_size / max(1, extent.visible)
            bar.window_virtual_size = (max(1, extent.total) + extent.visible) * self._scrollbar_scale
            bar.position = max(0, min(extent.total, extent.position)) * self._scrollbar_scale

    def _arrange_scrollbars(self, region):
        for widget, placement in super()._arrange_scrollbars(region):
            self._sync_global_scrollbar()
            yield widget, placement

    def _scroll_update(self, virtual_size):
        super()._scroll_update(virtual_size)
        self._sync_global_scrollbar()

    def _refresh_scrollbars(self):
        super()._refresh_scrollbars()
        extent = self.scroll_extent()
        if extent is not None and extent.total > extent.visible and self.styles.overflow_y != "hidden":
            self.show_vertical_scrollbar = True
            self.vertical_scrollbar.display = True

    def watch_scroll_y(self, old_value, new_value):
        super().watch_scroll_y(old_value, new_value)
        self._sync_global_scrollbar()

    def _on_scroll_to(self, message: ScrollTo):
        self.reading_revision += 1
        if message.y is not None and self.scroll_extent() is not None:
            self.seek(message.y / self._scrollbar_scale)
            message.stop()
        else:
            super()._on_scroll_to(message)

    def _on_mouse_scroll_up(self, event):
        self.reading_revision += 1
        super()._on_mouse_scroll_up(event)

    def _on_mouse_scroll_down(self, event):
        self.reading_revision += 1
        super()._on_mouse_scroll_down(event)

    def on_key(self, event):
        if event.key in ("up", "down", "home", "end", "pageup", "pagedown", "ctrl+home", "ctrl+end"):
            self.reading_revision += 1

    def reading_anchor(self):
        """Retain a visible leaf, so earlier Markdown blocks may grow safely."""
        top = self.content_region.y
        candidates = [child for child in self.walk_children()
                      if child.display and child.region.height and not child.children
                      and child.region.y <= top < child.region.bottom]
        if not candidates:
            return None
        widget = candidates[-1]
        keyed = next((parent for parent in reversed((widget, *widget.ancestors))
                      if getattr(parent, "scroll_key", None) is not None), None)
        widget = keyed or widget
        return widget, widget.region.y - top, getattr(widget, "scroll_key", None), self.reading_revision

    def restore_reading_anchor(self, anchor):
        if anchor is not None:
            widget, offset, key, revision = anchor
            if revision != self.reading_revision:
                return
            if not widget.is_attached and key is not None:
                widget = next((child for child in self.walk_children()
                               if getattr(child, "scroll_key", None) == key), None)
            if widget is None or not widget.is_attached:
                return
            self.scroll_to(y=self.scroll_y + widget.region.y - self.content_region.y - offset,
                           animate=False, immediate=True, force=True)

    def on_markdown_table_of_contents_updated(self, event):
        anchor = self.pending_reading_anchor
        self.pending_reading_anchor = None
        self.call_after_refresh(self.restore_reading_anchor, anchor)
