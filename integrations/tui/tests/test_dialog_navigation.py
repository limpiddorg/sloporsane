"""Vertical selection belongs to the last available picker, not the focused control."""

import unittest
from unittest.mock import patch

from textual.containers import Vertical
from textual.widgets import Button, Input, OptionList, TextArea

from sos_tui.app import SosApp
from sos_tui.dialog_screen import DialogScreen
from sos_tui.config import DEPTH_MD


class SelectionDialog(DialogScreen):
    def compose(self):
        with Vertical(classes="popup-dialog"):
            yield OptionList("First", "Second", "Third", id="first", classes="popup-list")
            with Vertical(id="second-panel"):
                yield OptionList("One", "Two", "Three", id="second", classes="popup-list")
            yield Input("Editable", id="field")
            yield Button("Action", id="action")
            yield TextArea("line one\nline two", id="editor")


class DialogNavigationTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 50)))
        self.dialog = SelectionDialog()
        await self.app.push_screen(self.dialog)
        await self.pilot.pause()

    async def test_last_list_is_remembered_from_actions_and_fields(self):
        first = self.dialog.query_one("#first", OptionList)
        second = self.dialog.query_one("#second", OptionList)
        second.focus()
        await self.pilot.press("down")
        self.dialog.query_one("#action").focus()
        await self.pilot.press("down")
        self.assertEqual(second.highlighted, 2)
        self.assertTrue(second.has_focus)
        first.focus()
        await self.pilot.pause()
        self.dialog.query_one("#field").focus()
        await self.pilot.press("down")
        self.assertEqual(first.highlighted, 1)
        self.assertEqual(second.highlighted, 2)
        self.assertTrue(first.has_focus)

    async def test_hidden_or_disabled_list_is_not_a_navigation_target(self):
        second = self.dialog.query_one("#second", OptionList)
        first = self.dialog.query_one("#first", OptionList)
        second.focus()
        await self.pilot.pause()
        self.dialog.query_one("#second-panel").display = False
        self.dialog.query_one("#action").focus()
        await self.pilot.press("down")
        self.assertEqual(first.highlighted, 1)
        self.dialog.query_one("#second-panel").display = True
        second.focus()
        await self.pilot.pause()
        self.dialog.query_one("#second-panel").disabled = True
        self.dialog.query_one("#field").focus()
        await self.pilot.press("up")
        self.assertEqual(first.highlighted, 0)

    async def test_no_available_list_preserves_editor_arrows(self):
        self.dialog.query_one("#first").display = False
        self.dialog.query_one("#second-panel").display = False
        editor = self.dialog.query_one("#editor", TextArea)
        editor.focus()
        editor.move_cursor((0, 0))
        await self.pilot.press("down")
        self.assertEqual(editor.cursor_location, (1, 0))
        self.assertTrue(editor.has_focus)

    async def test_full_border_and_selected_background_survive_focus_changes(self):
        listing = self.dialog.query_one("#first", OptionList)
        listing.focus()
        await self.pilot.pause()
        for focus in (listing, self.dialog.query_one("#action"), self.dialog.query_one("#field")):
            focus.focus()
            await self.pilot.pause()
            self.assertEqual(listing.styles.border.spacing.top, 1)
            self.assertEqual(listing.styles.border.spacing.bottom, 1)
            self.assertEqual(listing.styles.border.spacing.left, 1)
            self.assertEqual(listing.styles.border.spacing.right, 1)
            self.assertEqual(listing.content_region.height, 3)
            selected = next(segment for segment in listing.render_line(0) if "First" in segment.text)
            self.assertEqual(selected.style.bgcolor.triplet.hex.lower(), DEPTH_MD.lower())
