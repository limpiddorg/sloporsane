"""All mode labels use the same cell alignment, including hover and scan."""

import unittest
from unittest.mock import patch

from sos_tui.app import SosApp
from sos_tui.mode_button import ModeButton
from sos_tui.message_input import MessageInput


class ModeButtonTest(unittest.IsolatedAsyncioTestCase):
    async def test_all_labels_are_centred_in_rendered_terminal_cells(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(100, 30)) as pilot:
                button = app.query_one(ModeButton)
                for mode, busy, label in (("slop", False, "Slop"), ("sane", False, "Sane"),
                                           ("slop", True, "Cancel"), ("sane", True, "Cancel")):
                    button.set_mode(mode)
                    button.set_busy(busy)
                    button.animation_timer.pause()
                    with patch("sos_tui.mode_button.monotonic", return_value=button.scan_started + 2.0):
                        for hover in (False, True):
                            await pilot.hover(button if hover else MessageInput)
                            button.refresh()
                            await pilot.pause()
                            width = button.size.width
                            left = (width - len(label)) // 2
                            expected = " " * left + label + " " * (width - left - len(label))
                            self.assertEqual(button.render_line(0).text, expected)
                            row = app.screen._compositor.render_strips()[button.region.y].text
                            self.assertEqual(row[button.region.x:button.region.right], expected)
                button.set_busy(False)
