"""Global extents must not depend on which window is currently rendered."""
import unittest
from rich.text import Text

from textual.app import App
from textual.widgets import Static
from textual.scrollbar import ScrollTo

from sos_tui.scroll_window import ScrollWindow
from sos_tui.stable_scroll import StableScroll
from sos_tui.implementation_code import HistoricalCode, ChangeZone
from sos_tui.history_viewport import HistoryViewport
from test_history_window import ViewportApp, page


class CodeApp(App):
    def compose(self):
        with HistoricalCode():
            yield Static()


class StableScrollTest(unittest.IsolatedAsyncioTestCase):
    def test_measured_extent_and_window_padding_are_independent(self):
        window = ScrollWindow([1, 3, 1, 2, 1])
        self.assertEqual(window.total, 8)
        self.assertEqual(window.item_at(3), 1)
        self.assertEqual(window.padding(1, 3), (1, 3))
        self.assertEqual(window.around(4, 2, 0), (2, 4))

    async def test_diff_extent_is_complete_before_scrolling_and_rendering_stays_bounded(self):
        app = CodeApp()
        async with app.run_test(size=(80, 20)) as pilot:
            code = app.query_one(HistoricalCode)
            zone = ChangeZone("file.txt", [(i + 1, f"line {i}", "") for i in range(2000)], 900, "file")
            code.show_zone(zone)
            await pilot.pause()
            extent = code.virtual_size.height
            self.assertGreaterEqual(extent, 2000)
            self.assertEqual(code.scroll_y, 900)
            code.scroll_to(y=1700, animate=False, immediate=True)
            await pilot.pause()
            self.assertEqual(code.virtual_size.height, extent)
            self.assertEqual(code.scroll_y, 1700)
            self.assertLess(code.end - code.start, 150)
            self.assertLessEqual(code.start, 1700)
            self.assertGreater(code.end, 1700)
            code.scroll_home(animate=False, immediate=True)
            await pilot.pause()
            self.assertEqual(code.start, 0)
            self.assertEqual(code.virtual_size.height, extent)
            code.show_description(Text("Description"))
            await pilot.pause()
            self.assertEqual(code.query_one(Static).styles.padding.top, 0)

    async def test_history_global_bar_retains_rank_across_page_replacement_and_seeks(self):
        app = ViewportApp()
        async with app.run_test(size=(80, 8)) as pilot:
            viewport = app.query_one(HistoryViewport)
            def indexed(start):
                result = page(start, 20, older=True, newer=True)
                result["page"].update(start=start, end=start + 20, total=1000)
                return result
            await viewport.replace(indexed(100))
            viewport.seek(110)
            await pilot.pause()
            before = viewport.vertical_scrollbar.position
            extent = viewport.vertical_scrollbar.window_virtual_size
            await viewport.replace(indexed(105), "after")
            await pilot.pause()
            self.assertEqual(viewport.vertical_scrollbar.window_virtual_size, extent)
            self.assertAlmostEqual(viewport.vertical_scrollbar.position, before)
            self.assertEqual(viewport.vertical_scrollbar.window_size, viewport.container_size.height)
            viewport._on_scroll_to(ScrollTo(y=500 * viewport._scrollbar_scale, animate=False))
            await pilot.pause()
            self.assertEqual(app.directions, ["at"])
            self.assertEqual(viewport.seek_position, 500)
            await viewport.replace(indexed(490), "at")
            await pilot.pause()
            self.assertAlmostEqual(viewport.scroll_extent().position, 500)

    async def test_markdown_update_keeps_the_same_source_block_visible(self):
        from sos_tui.response_markdown import ResponseMarkdown
        class DocumentApp(App):
            def compose(self):
                with StableScroll():
                    yield ResponseMarkdown("\n\n".join(f"Paragraph {i}" for i in range(40)))
        app = DocumentApp()
        async with app.run_test(size=(50, 8)) as pilot:
            viewport = app.query_one(StableScroll)
            markdown = app.query_one(ResponseMarkdown)
            await pilot.pause()
            viewport.scroll_to(y=40, animate=False, immediate=True)
            await pilot.pause()
            anchor = viewport.reading_anchor()
            key, offset = anchor[2], anchor[1]
            await markdown.update("\n\n".join(["First paragraph grew " * 100] +
                                              [f"Paragraph {i}" for i in range(1, 40)]))
            await pilot.pause()
            block = next(widget for widget in viewport.walk_children()
                         if getattr(widget, "scroll_key", None) == key)
            self.assertEqual(block.region.y - viewport.content_region.y, offset)

    async def test_document_anchor_can_survive_widget_replacement(self):
        class DocumentApp(App):
            def compose(self):
                with StableScroll():
                    for index in range(30):
                        widget = Static(f"Paragraph {index}\n" * 3)
                        widget.scroll_key = index
                        yield widget
        app = DocumentApp()
        async with app.run_test(size=(80, 8)) as pilot:
            viewport = app.query_one(StableScroll)
            viewport.scroll_to(y=40, animate=False, immediate=True)
            await pilot.pause()
            anchor = viewport.reading_anchor()
            old, offset, key, _ = anchor
            replacement = Static("Replacement\n" * 3)
            replacement.scroll_key = key
            await viewport.mount(replacement, before=old)
            await old.remove()
            viewport.children[0].update("Earlier content grew\n" * 12)
            await pilot.pause()
            viewport.restore_reading_anchor(anchor)
            await pilot.pause()
            self.assertEqual(replacement.region.y - viewport.content_region.y, offset)
