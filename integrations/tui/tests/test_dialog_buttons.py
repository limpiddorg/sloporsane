"""Popup buttons remain flat and stationary in every interaction state."""

import unittest
from unittest.mock import patch

from textual.containers import Horizontal, Vertical
from textual.widgets import Button

from sos_tui.app import SosApp
from sos_tui.config import CONTENT_BACKGROUND_COLOR, DEPTH_MD, INPUT_BACKGROUND_COLOR, MUTED_COLOR, TEXT_COLOR
from sos_tui.dialog_screen import DialogScreen


class ButtonsDialog(DialogScreen):
    def compose(self):
        with Vertical(classes="popup-dialog"):
            with Horizontal():
                yield Button("Default", id="default")
                yield Button("Compact", id="compact", compact=True)
                yield Button("Disabled", id="disabled", disabled=True)


class DialogButtonsTest(unittest.IsolatedAsyncioTestCase):
    async def test_hover_focus_press_and_disabled_share_flat_geometry(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(100, 30)) as pilot:
                dialog = ButtonsDialog()
                await app.push_screen(dialog)
                await pilot.pause()
                for selector in ("#default", "#compact"):
                    with self.subTest(button=selector):
                        button = dialog.query_one(selector, Button)
                        button.blur()
                        await pilot.hover(".popup-dialog", offset=(0, 0))
                        await pilot.pause()
                        region = button.region
                        self.assertEqual(region.height, 1)
                        self.assertTrue(button.styles.text_style.bold)
                        self.assertGreaterEqual(region.width, len(button.label.plain) + 2)
                        self.assertEqual(button.styles.background.hex, CONTENT_BACKGROUND_COLOR)
                        await pilot.hover(selector)
                        self.assertEqual(button.styles.background.hex, INPUT_BACKGROUND_COLOR)
                        button.focus()
                        await pilot.pause()
                        self.assertEqual(button.styles.background.hex.lower(), DEPTH_MD.lower())
                        self.assertEqual(button.styles.color.hex.lower(), TEXT_COLOR.lower())
                        self.assertFalse(button.styles.text_style.underline)
                        # Keep the real click's active state long enough to inspect it.
                        button.active_effect_duration = 60
                        await pilot.click(selector)
                        self.assertTrue(button.has_class("-active"))
                        self.assertEqual(button.styles.background.hex, "#3A4655")
                        self.assertEqual(button.region, region)
                        self.assertEqual(button.styles.border.spacing.top, 0)
                        self.assertEqual(button.styles.border.spacing.bottom, 0)
                        self.assertEqual(button.styles.tint.a, 0)
                        button.remove_class("-active")
                        button.blur()
                disabled = dialog.query_one("#disabled", Button)
                before = disabled.region
                await pilot.hover("#disabled")
                await pilot.click("#disabled")
                self.assertFalse(disabled.has_class("-active"))
                self.assertEqual(disabled.styles.background.hex, CONTENT_BACKGROUND_COLOR)
                self.assertEqual(disabled.styles.color.hex, MUTED_COLOR)
                self.assertEqual(disabled.region, before)
