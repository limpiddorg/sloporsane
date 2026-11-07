import unittest
from unittest.mock import AsyncMock, patch
from textual.widgets import Button, Input, OptionList, Static
from sos_tui.app import SosApp
from sos_tui.config import BOLD_MD, CONTENT_BACKGROUND_COLOR, TITLE_MD
from sos_tui.message_input import MessageInput
from sos_tui.worktree_screen import WorktreeScreen
from sos_tui.worktree_delete_screen import WorktreeDeleteScreen


class WorktreeTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(110, 40)))
        self.app.connected = True
        self.app.client.send = AsyncMock()
        entry = self.app.query_one(MessageInput)
        entry.disabled = False
        entry.text = "/worktree"
        entry.focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.screen = self.app.worktree_screen
        self.assertIsInstance(self.screen, WorktreeScreen)

    async def reply(self, event):
        event["requestId"] = self.app.client.send.call_args.args[0]["requestId"]
        self.app.handle_event(event)
        await self.pilot.pause()

    async def listed(self):
        await self.reply({"type": "worktree.listed", "workspace": "/repo", "worktrees": [
            {"path": "/repo", "branch": "refs/heads/main"},
            {"path": "/other", "detached": True}, {"path": "/bare", "bare": True}]})

    async def test_new_is_first_and_current_worktree_stays_selected(self):
        await self.listed()
        listing = self.screen.query_one(OptionList)
        self.assertEqual(str(listing.get_option_at_index(0).prompt), "New…")
        current = listing.get_option_at_index(1).prompt
        other = listing.get_option_at_index(2).prompt
        self.assertEqual([column._cells[0].plain for column in current.columns], ["/repo", "main", "Current"])
        self.assertEqual(current.columns[1]._cells[0].style, TITLE_MD)
        self.assertEqual(current.columns[2]._cells[0].style, BOLD_MD)
        self.assertEqual([column._cells[0].plain for column in other.columns], ["/other", "detached", ""])
        self.assertEqual(listing.option_count, 3)
        self.assertEqual(listing.highlighted, 1)

    async def test_select_moves_conversation_and_preserves_draft(self):
        await self.listed()
        self.app.query_one(MessageInput).text = "next draft"
        await self.pilot.press("down", "enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["path"], "/other")
        await self.reply({"type": "worktree.switched", "sessionId": "same", "info": {"workspace": "/other"}})
        self.assertEqual(self.app.workspace, "/other")
        self.assertEqual(self.app.session_id, "same")
        self.assertEqual(self.app.query_one(MessageInput).text, "next draft")
        self.assertIsNone(self.app.worktree_screen)

    async def test_column_header_uses_shared_picker_layout(self):
        await self.listed()
        columns = self.screen.query_one("#worktree-columns", Static)
        picker = self.screen.query_one("#worktree-picker")
        listing = self.screen.query_one(OptionList)
        separator = picker.query_one(".popup-separator")
        self.assertEqual([column._cells[0].plain for column in columns.content.columns], ["Path", "Branch", ""])
        self.assertEqual(columns.styles.background.hex.lower(), CONTENT_BACKGROUND_COLOR.lower())
        self.assertEqual(columns.content_region.x, listing.content_region.x)
        self.assertEqual(separator.region.y, columns.region.bottom)
        self.assertEqual(listing.region.y, separator.region.bottom)
        self.assertEqual(listing.styles.border.spacing.width, 0)

    async def test_new_failure_retry_and_escape_back(self):
        await self.listed()
        await self.pilot.press("up", "enter")
        field = self.screen.query_one(Input)
        self.assertIs(self.app.focused, field)
        self.assertFalse(self.screen.query_one("#worktree-picker").display)
        field.value = "feature"
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["name"], "feature")
        await self.reply({"type": "request.failed", "error": "Name already exists"})
        self.assertEqual(field.value, "feature")
        self.assertIn("Name already exists", str(self.screen.query_one("#worktree-status", Static).render()))
        await self.pilot.press("escape")
        self.assertTrue(self.screen.query_one(OptionList).display)
        self.assertTrue(self.screen.query_one("#worktree-picker").display)
        await self.pilot.press("escape")
        await self.pilot.pause()
        self.assertIsNone(self.app.worktree_screen)

    async def test_delete_requires_confirmation_and_refreshes_list(self):
        await self.reply({"type": "worktree.listed", "workspace": "/repo", "worktrees": [
            {"path": "/repo", "primary": True, "current": True},
            {"path": "/unused", "branch": "refs/heads/sos/feature", "deletable": True}]})
        await self.pilot.press("down", "right", "enter")
        self.assertIsInstance(self.app.screen, WorktreeDeleteScreen)
        count = self.app.client.send.call_count
        await self.pilot.press("escape")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_count, count)
        self.assertIs(self.app.screen, self.screen)
        self.assertEqual(self.screen.selected_action, "delete")
        await self.pilot.press("enter")
        self.assertIsInstance(self.app.screen, WorktreeDeleteScreen)
        await self.pilot.press("right", "enter")
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "worktree.delete")
        self.assertEqual(self.app.client.send.call_args.args[0]["path"], "/unused")
        await self.reply({"type": "worktree.deleted", "workspace": "/repo", "worktrees": [{"path": "/repo", "current": True}]})
        self.assertIs(self.app.screen, self.screen)
        self.assertEqual(self.screen.query_one(OptionList).option_count, 2)
        self.assertEqual(self.screen.selected_action, "select")

    async def test_confirmation_is_compact_and_cancel_is_default(self):
        await self.reply({"type": "worktree.listed", "workspace": "/repo", "worktrees": [
            {"path": "/repo", "current": True},
            {"path": "/unused", "branch": "refs/heads/sos/feature", "deletable": True}]})
        await self.pilot.press("down", "right", "enter")
        confirmation = self.app.screen
        self.assertIsInstance(confirmation, WorktreeDeleteScreen)
        self.assertEqual([str(widget.content) for widget in confirmation.query(Static)],
                         ["Delete worktree", "Branch: sos/feature", "/unused", "←/→ Action · Enter Confirm · Esc Cancel"])
        cancel = confirmation.query_one("#worktree-delete-cancel", Button)
        validate = confirmation.query_one("#worktree-delete-validate", Button)
        self.assertIs(self.app.focused, cancel)
        self.assertTrue(cancel.has_class("selected-action"))
        self.assertLess(cancel.region.x, validate.region.x)
        count = self.app.client.send.call_count
        await self.pilot.press("enter")
        self.assertIs(self.app.screen, self.screen)
        self.assertEqual(self.app.client.send.call_count, count)
        await self.pilot.press("enter", "right", "left", "enter")
        self.assertIs(self.app.screen, self.screen)
        self.assertEqual(self.app.client.send.call_count, count)

    async def test_confirmation_validate_button_deletes_on_click(self):
        await self.reply({"type": "worktree.listed", "workspace": "/repo", "worktrees": [
            {"path": "/repo", "current": True}, {"path": "/unused", "deletable": True}]})
        await self.pilot.press("down", "right", "enter")
        await self.pilot.click("#worktree-delete-validate")
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "worktree.delete")

    async def test_primary_current_and_other_opened_worktrees_cannot_be_deleted(self):
        await self.reply({"type": "worktree.listed", "workspace": "/current", "worktrees": [
            {"path": "/repo", "primary": True, "deletable": False},
            {"path": "/current", "current": True, "deletable": False},
            {"path": "/other", "opened": True, "deletable": False}]})
        listing = self.screen.query_one(OptionList)
        other = listing.get_option_at_index(3).prompt
        self.assertEqual(other.columns[2]._cells[0].plain, "Opened in another session")
        self.assertEqual(other.columns[2]._cells[0].style, BOLD_MD)
        count = self.app.client.send.call_count
        for index in (1, 2, 3):
            listing.highlighted = index
            await self.pilot.press("right", "enter")
            self.assertIs(self.app.screen, self.screen)
            self.assertTrue(self.screen.query_one("#worktree-delete").disabled)
        self.assertEqual(self.app.client.send.call_count, count)

    async def test_delete_failure_keeps_list_and_shows_error(self):
        await self.reply({"type": "worktree.listed", "workspace": "/repo", "worktrees": [
            {"path": "/repo", "current": True}, {"path": "/unused", "deletable": True}]})
        await self.pilot.press("down", "right", "enter", "right", "enter")
        await self.reply({"type": "request.failed", "error": "Worktree is opened in a session"})
        self.assertEqual(self.screen.query_one(OptionList).option_count, 3)
        self.assertIn("opened in a session", str(self.screen.query_one("#worktree-status", Static).render()))

    async def test_new_session_uses_workspace_returned_by_daemon(self):
        self.app.workspace = "/old"
        await self.app.reset_session({"workspace": "/current", "info": {}}).wait()
        self.assertEqual(self.app.workspace, "/current")
