"""Scrollbar hover and drag colors are shared across the Textual application."""

import unittest
from unittest.mock import patch

from textual.color import Color
from textual.containers import VerticalScroll
from textual.geometry import Offset
from textual.widgets import OptionList

from sos_tui.app import SosApp
from sos_tui.command_suggestions import CommandSuggestions
from sos_tui.config import (
    BACKGROUND_COLOR, INPUT_BACKGROUND_COLOR, MUTED_COLOR, TEXT_COLOR,
    SCROLLBAR_HOVER_COLOR, SCROLLBAR_HOVER_BACKGROUND_COLOR,
)
from sos_tui.dialog_screen import DialogScreen
from sos_tui.message_input import MessageInput
from sos_tui.skill_details import SkillDetails


class ScrollbarStyleTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 40)))

    def assert_interaction_colors(self, widget):
        for suffix in ("hover", "active"):
            self.assertEqual(getattr(widget.styles, f"scrollbar_color_{suffix}"),
                             Color.parse(SCROLLBAR_HOVER_COLOR))
            self.assertEqual(getattr(widget.styles, f"scrollbar_background_{suffix}"),
                             Color.parse(SCROLLBAR_HOVER_BACKGROUND_COLOR))

    async def test_global_rule_covers_existing_widgets_and_new_dialog_contents(self):
        for widget in self.app.query("*"):
            self.assert_interaction_colors(widget)
        entry = self.app.query_one(MessageInput)
        self.assertEqual(entry.styles.scrollbar_color, Color.parse(TEXT_COLOR))
        self.assertEqual(entry.styles.scrollbar_background, Color.parse(INPUT_BACKGROUND_COLOR))
        for widget_type in (CommandSuggestions, SkillDetails):
            self.assertEqual(self.app.query_one(widget_type).styles.scrollbar_color, Color.parse(MUTED_COLOR))

        screen = DialogScreen()
        await self.app.push_screen(screen)
        panel = VerticalScroll(classes="popup-dialog")
        await screen.mount(panel)
        options = OptionList("Example", classes="popup-list")
        await panel.mount(options)
        await self.pilot.pause()
        for widget in (screen, panel, options):
            self.assert_interaction_colors(widget)
        self.assertEqual(options.styles.scrollbar_background, Color.parse(BACKGROUND_COLOR))

    async def test_skill_scrollbar_renders_darker_on_hover_and_drag(self):
        details = self.app.query_one(SkillDetails)
        details.show_skill({"name": "long-skill", "description": "Scrollable fixture",
                            "scripts": [f"scripts/{index}.py" for index in range(100)]})
        await self.pilot.pause()
        scrollbar = details.vertical_scrollbar
        normal_style = scrollbar.render().style
        await self.pilot.hover(scrollbar)
        self.assertTrue(scrollbar.mouse_over)
        hover_style = scrollbar.render().style
        self.assertEqual(hover_style.color, Color.parse(SCROLLBAR_HOVER_COLOR).rich_color)
        self.assertEqual(hover_style.bgcolor, Color.parse(SCROLLBAR_HOVER_BACKGROUND_COLOR).rich_color)
        for normal, hovered in ((normal_style.color, hover_style.color),
                                (normal_style.bgcolor, hover_style.bgcolor)):
            self.assertTrue(all(dark < light for dark, light in zip(hovered.triplet, normal.triplet)))
        scrollbar.grabbed = Offset(0, 0)
        self.assertEqual(scrollbar.render().style, hover_style)
        scrollbar.grabbed = None
        await self.pilot.hover(self.app.query_one(MessageInput))
        self.assertEqual(scrollbar.render().style, normal_style)
