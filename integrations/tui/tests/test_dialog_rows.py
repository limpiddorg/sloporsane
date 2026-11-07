"""Picker statuses share a right-aligned coral column."""

import unittest
from unittest.mock import patch

from rich.console import Console

from sos_tui.config import BOLD_MD
from sos_tui.connect_screen import ConnectScreen
from sos_tui.dialog_screen import dialog_row
from sos_tui.model_screen import ModelScreen
from dialog_fixture import row_text
from sos_tui.app import SosApp
from sos_tui.config import CONTENT_BACKGROUND_COLOR


class DialogRowsTest(unittest.TestCase):
    def test_status_is_coral_and_at_the_right_edge(self):
        rows = [dialog_row("Ask before", "Current"),
                ConnectScreen.connection_option("OpenRouter", "openrouter", True).prompt,
                ModelScreen.model_row("Openai (Codex)", "gpt-5.4", current=True)]
        for width in (50, 80):
            for row, status in zip(rows, ("Current", "Connected", "Current")):
                with self.subTest(width=width, status=status):
                    rendered = row_text(row, width)
                    self.assertEqual(len(rendered), width)
                    self.assertTrue(rendered.endswith(status))
                    segments = list(Console(width=width).render(row))
                    marker = next(segment for segment in segments if status in segment.text)
                    self.assertEqual(marker.style.color.triplet.hex.lower(), BOLD_MD.lower())

    def test_long_labels_stay_on_one_line_and_keep_the_status(self):
        rendered = row_text(dialog_row("A very long label " * 8, "Connected"), 40)
        self.assertNotIn("\n", rendered)
        self.assertEqual(len(rendered), 40)
        self.assertTrue(rendered.endswith("Connected"))

    def test_provider_column_keeps_its_alignment_with_or_without_current(self):
        first = row_text(ModelScreen.model_row("Openai (Codex)", "short", current=True))
        second = row_text(ModelScreen.model_row("Openrouter (Api key)", "another-model"))
        self.assertEqual(first.index("Openai"), second.index("Openrouter"))
        self.assertNotIn("Current", second)
        self.assertNotIn("Connected", row_text(ConnectScreen.connection_option("OpenAI", "openai", False).prompt))


class ModelLayoutTest(unittest.IsolatedAsyncioTestCase):
    async def test_context_form_has_heading_bounds_and_maximum_placeholder(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(100, 40)) as pilot:
                screen = ModelScreen({})
                await app.push_screen(screen)
                for maximum in (872000, None):
                    screen.receive({"type": "model.listed", "models": [
                        {"provider": "openai", "model": "gpt-6.1-sol", "max_context": maximum}]})
                    screen.select_model()
                    await pilot.pause()
                    heading = screen.query_one(".popup-heading")
                    name = screen.query_one("#model-name")
                    label = screen.query_one("#model-context-label")
                    field = screen.query_one("#model-context")
                    minimum = screen.query_one("#model-minimum")
                    upper = screen.query_one("#model-maximum")
                    self.assertEqual(str(heading.content), "Model")
                    self.assertTrue(heading.styles.text_style.bold)
                    self.assertEqual(heading.styles.color, screen.query_one("#model-columns").styles.color)
                    self.assertEqual(str(name.content), "gpt-6.1-sol")
                    self.assertEqual(name.styles.color, app.styles.color)
                    self.assertEqual(label.region.y - name.region.bottom, 1)
                    self.assertEqual(field.value, "")
                    self.assertEqual(field.placeholder, str(maximum or 32768))
                    self.assertEqual(field.region.height, 1)
                    self.assertEqual(minimum.region.y - field.region.bottom, 1)
                    self.assertEqual(str(minimum.content), "Minimum: 32768 tokens")
                    self.assertEqual(upper.display, maximum is not None)
                    if maximum is not None:
                        self.assertEqual(str(upper.content), "Maximum: 872000 tokens")
                        self.assertEqual(upper.styles.color, minimum.styles.color)
                        self.assertEqual(upper.region.y, minimum.region.bottom)
                    self.assertEqual(str(screen.query_one("#model-status").content), "Enter Save · Esc Back")
                    screen.action_close()
                    await pilot.pause()
                    self.assertFalse(screen.query_one("#model-context-form").display)
                    self.assertFalse(screen.has_class("context-selection"))

    async def test_search_is_next_to_heading_then_blank_line_before_columns(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(100, 40)) as pilot:
                screen = ModelScreen({})
                await app.push_screen(screen)
                await pilot.pause()
                heading = screen.query_one(".popup-heading")
                search = screen.query_one("#model-search")
                columns = screen.query_one("#model-columns")
                picker = screen.query_one("#model-picker")
                self.assertEqual(search.region.y, heading.region.bottom)
                self.assertEqual(search.content_region.x, heading.content_region.x)
                self.assertEqual(picker.region.y - search.region.bottom, 1)
                self.assertEqual(columns.region.y, picker.content_region.y)
                self.assertEqual(columns.styles.background.hex.lower(), CONTENT_BACKGROUND_COLOR.lower())
                self.assertNotEqual(picker.styles.background, columns.styles.background)
                self.assertEqual(columns.region.width, picker.content_region.width)
                self.assertEqual(columns.content_region.height, 1)
                self.assertEqual(columns.styles.border.spacing.bottom, 0)
                separator = picker.query_one(".popup-separator")
                self.assertEqual(separator.region.y, columns.region.bottom)
                self.assertEqual(separator.styles.background, picker.styles.background)
                # Check rendered cells, including the padding and the outer border.
                header_background = columns.styles.background.hex.lower()
                normal_background = picker.styles.background.hex.lower()
                for x in (0, columns.region.width - 1):
                    self.assertEqual(columns.get_style_at(x, 0).bgcolor.triplet.hex.lower(), header_background)
                for x, y in ((0, 0), (1, 0), (0, 1), (picker.region.width - 1, 1)):
                    self.assertEqual(picker.get_style_at(x, y).bgcolor.triplet.hex.lower(), normal_background)
                self.assertEqual(separator.get_style_at(0, 0).bgcolor.triplet.hex.lower(), normal_background)
                listing = screen.query_one("#model-list")
                self.assertEqual(columns.content_region.x, listing.content_region.x)
                self.assertEqual(listing.region.y, separator.region.bottom)
                self.assertEqual(listing.region.bottom, picker.content_region.bottom)
                self.assertEqual(listing.styles.border.spacing.width, 0)
