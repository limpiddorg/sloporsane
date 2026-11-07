"""An external editor edits the next draft while execution events keep arriving."""

import asyncio
from contextlib import asynccontextmanager, nullcontext
import os
from pathlib import Path
import subprocess
import threading
import unittest
from unittest.mock import AsyncMock, patch

from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput


class EditorDuringExecutionTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 24)))
        self.app.connected = True
        self.app.busy = True
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.text = "Original draft"
        self.entry.focus()
        await self.pilot.pause()

    @asynccontextmanager
    async def editing(self, returncode=0):
        entered, release = threading.Event(), threading.Event()
        def editor(command, **kwargs):
            entered.set()
            if not release.wait(timeout=5):
                raise TimeoutError("Test editor was not released")
            Path(command[-1]).write_text("Edited draft", encoding="utf-8")
            return subprocess.CompletedProcess(command, returncode)
        with patch.dict(os.environ, {"EDITOR": "test-editor"}), \
                patch.object(self.app, "suspend", side_effect=nullcontext), \
                patch("sos_tui.app.subprocess.run", side_effect=editor) as run:
            task = asyncio.create_task(self.app.action_edit_external())
            try:
                for _ in range(100):
                    if entered.is_set():
                        break
                    await asyncio.sleep(0.01)
                self.assertTrue(entered.is_set())
                self.assertTrue(self.app.editing_external)
                yield task, run
            finally:
                release.set()
                await asyncio.wait_for(task, 3)
        await self.pilot.pause()
        self.assertFalse(self.app.editing_external)

    async def test_rejected_send_is_preserved_ahead_of_the_edited_draft(self):
        self.app.pending_request = "pending"
        self.app.pending_text = "Rejected message"
        async with self.editing():
            self.app.handle_event({"type": "request.failed", "requestId": "pending", "error": "Rejected"})
            self.assertEqual(self.entry.text, "Original draft")
        self.assertEqual(self.entry.text, "Rejected message\n\nEdited draft")
        self.assertEqual(self.entry.cursor_location, self.entry.document.end)
        self.assertEqual(self.app.editor_restored_text, "")
        self.app.client.send.assert_not_awaited()

    async def test_editor_failure_keeps_both_the_recovered_send_and_original_draft(self):
        self.app.pending_text = "Cancelled message"
        async with self.editing(returncode=7):
            self.app.handle_event({"type": "execution.cancelled"})
        self.assertEqual(self.entry.text, "Cancelled message\n\nOriginal draft")
        self.assertEqual(self.app.editor_restored_text, "")
        self.app.client.send.assert_not_awaited()

    async def test_permission_arriving_during_edit_waits_and_is_revealed_on_return(self):
        async with self.editing() as (task, _):
            self.app.handle_event({"type": "permission.requested", "permissionId": "p", "callId": "c",
                                   "permission": "edit", "tool": "write", "patterns": ["file.txt"]})
            self.assertFalse(task.done())
            self.app.client.send.assert_not_awaited()
        card = self.app.permission_card
        for _ in range(50):
            if card.owns_button_focus():
                break
            await self.pilot.pause(0.01)
        self.assertTrue(card.owns_button_focus())
        self.assertTrue(self.app.screen.can_view_entire(card.query_one("#permission-yes")))
        self.assertEqual(self.entry.text, "Edited draft")
        self.assertTrue(self.app.busy)
        self.app.client.send.assert_not_awaited()

    async def test_an_existing_permission_keeps_its_keyboard_choice_after_editing(self):
        self.app.handle_event({"type": "permission.requested", "permissionId": "p", "callId": "c",
                               "permission": "edit", "tool": "write", "patterns": ["file.txt"]})
        card = self.app.permission_card
        for _ in range(50):
            if card.owns_button_focus():
                break
            await self.pilot.pause(0.01)
        await self.pilot.press("left")
        chosen = card.query_one("#permission-no")
        self.assertTrue(chosen.has_focus)
        async with self.editing():
            pass
        self.assertTrue(chosen.has_focus)
        self.assertEqual(self.entry.text, "Edited draft")
        self.app.client.send.assert_not_awaited()

    async def test_a_second_external_editor_cannot_start_concurrently(self):
        async with self.editing() as (_, run):
            await self.app.action_edit_external()
            run.assert_called_once()
        self.assertEqual(self.entry.text, "Edited draft")

    async def test_editor_file_terminator_is_not_imported_as_a_blank_line(self):
        cases = [("Hello\n", "Hello"), ("Hello", "Hello"), ("", ""),
                 ("First\nSecond\n", "First\nSecond"), ("First\r\nSecond\r\n", "First\nSecond"),
                 ("Écrit  \n\n", "Écrit  \n"), ("  indented\t\n", "  indented\t")]
        for saved, expected in cases:
            with self.subTest(saved=saved):
                def editor(command, **kwargs):
                    Path(command[-1]).write_bytes(saved.encode("utf-8"))
                    return subprocess.CompletedProcess(command, 0)
                with patch.dict(os.environ, {"EDITOR": "test-editor"}), \
                        patch.object(self.app, "suspend", side_effect=nullcontext), \
                        patch("sos_tui.app.subprocess.run", side_effect=editor):
                    await self.app.action_edit_external()
                self.assertEqual(self.entry.text, expected)
                self.assertEqual(self.entry.cursor_location, self.entry.document.end)
        self.app.client.send.assert_not_awaited()

    async def test_repeated_unchanged_edits_preserve_intentional_trailing_lines(self):
        for draft in ("", "Hello", "Hello\n", "Hello\n\n", "  hello  \n"):
            with self.subTest(draft=draft):
                self.entry.text = draft
                def editor(command, **kwargs):
                    self.assertEqual(Path(command[-1]).read_text(encoding="utf-8"), draft + "\n")
                    return subprocess.CompletedProcess(command, 0)
                with patch.dict(os.environ, {"EDITOR": "test-editor"}), \
                        patch.object(self.app, "suspend", side_effect=nullcontext), \
                        patch("sos_tui.app.subprocess.run", side_effect=editor):
                    for _ in range(3):
                        await self.app.action_edit_external()
                        self.assertEqual(self.entry.text, draft)
