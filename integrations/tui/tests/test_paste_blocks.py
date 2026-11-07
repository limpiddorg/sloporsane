"""Compact pastes retain their exact payload through editing and sending."""

import unittest
from contextlib import nullcontext
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import AsyncMock, patch

from textual import events
from textual._xterm_parser import XTermParser
from textual.document._document import Selection
from textual.widgets import Button, Static

from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput
from sos_tui.paste_blocks import Paste, PathPaste, compact_paste
from sos_tui.paste_preview import PastePreview


LONG = "Traceback most recent\nline two\nline three\nline four"


class PasteLabelTest(unittest.TestCase):
    def test_thresholds_and_label(self):
        self.assertFalse(compact_paste("one\ntwo"))
        self.assertTrue(compact_paste("one\ntwo\nthree"))
        self.assertTrue(compact_paste(LONG))
        self.assertFalse(compact_paste("x" * 199))
        self.assertTrue(compact_paste("x" * 200))
        self.assertEqual(Paste(LONG).label, "[Pasted text • Traceback most recent…]")
        self.assertEqual(Paste("a" * 500).label, "[Pasted text • " + "a" * 23 + "…]")
        self.assertEqual(Paste("\n" * 500).label, "[Pasted text • Pasted text…]")


class PasteBlocksTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 30)))
        self.app.connected = True
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        await self.pilot.pause()

    async def paste(self, text=LONG):
        self.app.post_message(events.Paste(text))
        await self.pilot.pause()

    async def test_dropped_path_is_atomic_and_undo_redo_keep_its_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "notes with spaces.txt"
            file.touch()
            label = f"[Path • {file}]"
            await self.paste(str(file))
            self.assertEqual(self.entry.text, label)
            self.assertEqual(self.entry.expanded_text, str(file))
            self.assertIsInstance(self.entry.paste_blocks[0].paste, PathPaste)
            self.assertEqual(self.entry.images, ())
            await self.pilot.press("left")
            self.assertEqual(self.entry.cursor_location, (0, 0))
            await self.pilot.press("delete")
            self.assertEqual(self.entry.text, "")
            self.entry.action_undo()
            self.assertEqual(self.entry.text, label)
            self.assertEqual(self.entry.expanded_text, str(file))
            self.entry.action_redo()
            self.assertEqual(self.entry.text, "")

    async def test_dropped_directory_sends_only_its_path_not_an_image(self):
        with tempfile.TemporaryDirectory() as directory:
            await self.paste(directory)
            self.assertEqual(self.entry.text, f"[Path • {directory}]")
            await self.pilot.press("enter")
            request = self.app.client.send.call_args.args[0]
            self.assertEqual(request["type"], "message.send")
            self.assertEqual(request["text"], directory)
            self.assertFalse(request.get("images"))

    async def test_multiple_dropped_paths_remain_separate_atoms_and_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "notes.txt"
            file.touch()
            await self.paste(f'"{file}" "{directory}"')
            self.assertEqual(self.entry.text, f"[Path • {file}] [Path • {directory}]")
            self.assertEqual(self.entry.expanded_text, f"{file} {directory}")
            self.assertEqual(len(self.entry.paste_blocks), 2)

    async def test_small_paste_stays_plain_and_long_paste_stays_compact(self):
        await self.paste("small ")
        await self.paste()
        self.assertEqual(self.entry.text, "small " + Paste(LONG).label)
        self.assertEqual(self.entry.expanded_text, "small " + LONG)
        self.assertEqual(self.entry.size.height, 1)
        self.assertEqual(self.entry.cursor_location, self.entry.document.end)
        self.app.client.send.assert_not_awaited()

    async def test_paste_unknown_tilde_user_stays_plain_text(self):
        await self.paste("~typesafe/jev-latest")
        self.assertEqual(self.entry.text, "~typesafe/jev-latest")
        self.assertEqual(self.entry.expanded_text, "~typesafe/jev-latest")
        self.assertEqual(self.entry.images, ())

    async def test_short_pasted_command_is_sent_as_literal_text_without_suggestions(self):
        await self.paste("/new")
        self.assertEqual(self.entry.text, "/new")
        self.assertEqual(self.entry.command_text, "")
        self.assertFalse(self.entry.suggestions.display)
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "message.send")
        self.assertEqual(request["text"], "/new")
        self.assertEqual(self.entry.pasted_ranges, ())

    async def test_compact_paste_starting_with_slash_is_sent_unchanged(self):
        payload = "/tmp/example.cpp\n" + LONG
        await self.paste(payload)
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["text"], payload)

    async def test_pasted_skill_is_not_resolved_as_a_skill(self):
        await self.paste("//unknown-skill arguments")
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["text"], "//unknown-skill arguments")
        self.assertNotIn("skill", request)

    async def test_typed_command_still_executes_after_paste_is_removed(self):
        await self.paste("/new")
        self.entry.select_all()
        await self.pilot.press("backspace")
        await self.pilot.press("/", "n", "e", "w")
        self.assertEqual(self.entry.command_text, "/new")
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "session.new")

    async def test_paste_provenance_survives_editing_undo_and_redo(self):
        await self.paste("/new")
        self.entry.history.checkpoint()
        self.entry.insert(" ")
        self.assertEqual(self.entry.command_text, "")
        self.entry.action_undo()
        self.assertEqual(self.entry.text, "/new")
        self.assertEqual(self.entry.command_text, "")
        self.entry.action_redo()
        self.assertEqual(self.entry.command_text, "")
        self.entry.text = "/new"
        self.assertEqual(self.entry.command_text, "/new")

    async def test_queued_pasted_skill_is_sent_literally(self):
        self.app.busy = True
        await self.paste("//unknown-skill")
        await self.pilot.press("enter")
        self.assertIsNotNone(self.app.queued_message)
        self.app.busy = False
        self.app.queued_ready = True
        self.app.dispatch_queued_message()
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["text"], "//unknown-skill")
        self.assertNotIn("skill", request)

    async def test_failed_send_does_not_turn_restored_paste_into_command(self):
        await self.paste("/new")
        await self.pilot.press("enter")
        self.app.handle_event({"type": "request.failed", "requestId": self.app.pending_request, "error": "Rejected"})
        await self.pilot.pause()
        self.assertEqual(self.entry.text, "/new")
        self.assertEqual(self.entry.command_text, "")
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "message.send")

    async def test_typed_command_with_pasted_argument_remains_a_command(self):
        await self.pilot.press("/", "b", "t", "w", " ")
        await self.paste("some text")
        self.assertEqual(self.entry.command_text, "/btw some text")

    async def test_ctrl_h_deletes_one_character_and_undo_restores_it(self):
        self.entry.text = "abc"
        self.entry.move_cursor(self.entry.document.end)
        await self.pilot.press("ctrl+h")
        self.assertEqual(self.entry.text, "ab")
        self.entry.action_undo()
        self.assertEqual(self.entry.text, "abc")

    async def test_medium_and_three_line_pastes_are_compact(self):
        for payload in ("A short recap. " * 16, "First point\nSecond point\nNext step"):
            with self.subTest(payload=payload):
                self.entry.text = ""
                await self.paste(payload)
                self.assertEqual(self.entry.text, Paste(payload).label)
                self.assertEqual(self.entry.expanded_text, payload)
                self.assertEqual(len(self.entry.paste_blocks), 1)

    async def test_identical_previews_are_not_string_replacements(self):
        self.entry.text = Paste(LONG).label + " "
        self.entry.move_cursor(self.entry.document.end)
        await self.paste()
        await self.pilot.press("space")
        second = LONG + "\nsecond payload"
        await self.paste(second)
        self.assertEqual(self.entry.expanded_text, Paste(LONG).label + " " + LONG + " " + second)
        await self.pilot.press("backspace")
        self.assertEqual(self.entry.expanded_text, Paste(LONG).label + " " + LONG + " ")
        self.entry.action_undo()
        self.assertEqual(self.entry.expanded_text, Paste(LONG).label + " " + LONG + " " + second)

    async def test_arrows_selection_delete_undo_and_redo_are_atomic(self):
        await self.paste()
        await self.pilot.press("left")
        self.assertEqual(self.entry.cursor_location, (0, 0))
        await self.pilot.press("right")
        self.assertEqual(self.entry.cursor_location, self.entry.document.end)
        await self.pilot.press("shift+left")
        self.assertEqual(self.entry.selected_text, LONG)
        await self.pilot.press("backspace")
        self.assertEqual(self.entry.expanded_text, "")
        self.entry.action_undo()
        self.assertEqual(self.entry.expanded_text, LONG)
        self.entry.action_redo()
        self.assertEqual(self.entry.expanded_text, "")
        self.entry.action_undo()
        self.entry.move_cursor((0, 0))
        await self.pilot.press("delete")
        self.assertEqual(self.entry.expanded_text, "")

    async def test_inserts_and_multiline_changes_shift_blocks(self):
        await self.paste()
        self.entry.insert("Prefix\n", (0, 0))
        await self.pilot.pause()
        self.assertEqual(self.entry.expanded_text, "Prefix\n" + LONG)
        self.entry.move_cursor(self.entry.document.end)
        await self.pilot.press("space", "s", "u", "f", "f", "i", "x")
        self.assertEqual(self.entry.expanded_text, "Prefix\n" + LONG + " suffix")
        self.entry.delete((0, 0), (1, 0))
        self.assertEqual(self.entry.expanded_text, LONG + " suffix")
        self.entry.action_undo()
        self.assertEqual(self.entry.expanded_text, "Prefix\n" + LONG + " suffix")

    async def test_replacing_part_of_block_cannot_leak_a_broken_label(self):
        await self.paste()
        self.entry.replace("replacement", (0, 2), (0, 5), maintain_selection_offset=False)
        self.assertEqual(self.entry.expanded_text, "replacement")
        self.assertEqual(self.entry.paste_blocks, ())
        self.entry.action_undo()
        self.assertEqual(self.entry.expanded_text, LONG)

    async def test_copy_cut_and_paste_copy_the_payload(self):
        await self.paste()
        self.entry.selection = Selection((0, 0), self.entry.document.end)
        self.entry.action_copy()
        self.assertEqual(self.app.clipboard, LONG)
        self.entry.action_cut()
        self.assertEqual(self.app.clipboard, LONG)
        self.assertEqual(self.entry.text, "")
        self.entry.action_paste()
        self.assertEqual(self.entry.text, Paste(LONG).label)
        self.assertEqual(self.entry.expanded_text, LONG)

    async def test_paste_replaces_selection_and_undo_restores_it(self):
        self.entry.text = "before after"
        self.entry.selection = Selection((0, 0), (0, 6))
        await self.paste()
        self.assertEqual(self.entry.expanded_text, LONG + " after")
        self.entry.action_undo()
        self.assertEqual(self.entry.text, "before after")
        self.entry.action_redo()
        self.assertEqual(self.entry.expanded_text, LONG + " after")

    async def test_original_unicode_crlf_and_literal_markup_are_preserved(self):
        payload = "été [bold]\r\n第二行\r\n\r\nlast  \r\n"
        await self.paste(payload)
        self.assertEqual(self.entry.expanded_text, payload)
        self.assertNotIn("\n", self.entry.text)
        self.assertTrue(any(span.style == "#E9C46A" for span in self.entry.get_line(0).spans))

    async def test_click_previews_full_text_and_remove_is_undoable(self):
        await self.paste()
        await self.pilot.click(self.entry, offset=(4, 0))
        self.assertIsInstance(self.app.screen, PastePreview)
        self.assertAlmostEqual(self.app.screen.styles.background.a, 0.7)
        for button in self.app.screen.query(Button):
            self.assertTrue(button.compact)
            self.assertEqual(button.size.height, 1)
        self.assertEqual(self.app.screen.query_one(".paste-preview-content").styles.scrollbar_size_vertical, 1)
        self.assertEqual(str(self.app.screen.query_one("#paste-preview-text", Static).render()), LONG)
        self.assertFalse(self.app.screen.query("#paste-close"))
        await self.pilot.click("#paste-remove")
        await self.pilot.pause()
        self.assertEqual(self.entry.expanded_text, "")
        self.entry.action_undo()
        self.assertEqual(self.entry.expanded_text, LONG)

    async def test_preview_escape_keeps_paste_and_restores_composer_focus(self):
        await self.paste()
        await self.pilot.click(self.entry, offset=(4, 0))
        await self.pilot.press("escape")
        self.assertEqual(self.entry.expanded_text, LONG)
        self.assertTrue(self.entry.has_focus)

    async def test_readonly_and_disabled_ignore_paste(self):
        self.entry.read_only = True
        await self.paste()
        self.assertEqual(self.entry.text, "")
        self.entry.read_only = False
        self.entry.disabled = True
        self.entry.paste_text(LONG)
        self.assertEqual(self.entry.text, "")

    async def test_submission_sends_payload_not_label_and_reset_clears_spans(self):
        await self.paste()
        await self.pilot.press("enter")
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "message.send")
        self.assertEqual(request["text"], LONG)
        self.assertEqual(self.entry.paste_blocks, ())
        self.assertEqual(self.entry.text, "")

    async def test_queue_and_recovery_never_restore_labels_as_content(self):
        self.app.busy = True
        await self.paste()
        await self.pilot.press("enter")
        self.assertEqual(self.app.queued_message.text, LONG)
        self.assertEqual(self.entry.paste_blocks, ())
        await self.paste("second " + LONG)
        self.app.prepend_draft(LONG)
        self.assertEqual(self.entry.expanded_text, LONG + "\n\nsecond " + LONG)

    async def test_loading_new_text_and_undo_do_not_resurrect_old_payload(self):
        await self.paste()
        self.entry.text = Paste(LONG).label
        self.entry.action_undo()
        self.assertEqual(self.entry.paste_blocks, ())
        self.assertEqual(self.entry.expanded_text, Paste(LONG).label)

    async def test_real_bracketed_paste_encodes_one_block_without_sending(self):
        parser = XTermParser()
        for chunk in ("\x1b[200~", LONG[:12], LONG[12:], "\x1b[201~"):
            for event in parser.feed(chunk):
                self.app.post_message(event)
        await self.pilot.pause()
        self.assertEqual(len(self.entry.paste_blocks), 1)
        self.assertEqual(self.entry.expanded_text, LONG)
        self.app.client.send.assert_not_awaited()

    async def test_failed_send_restores_payload_before_new_compact_draft(self):
        await self.paste()
        await self.pilot.press("enter")
        request_id = self.app.pending_request
        await self.paste("Another " + LONG)
        self.app.handle_event({"type": "request.failed", "requestId": request_id, "error": "Rejected"})
        await self.pilot.pause()
        self.assertEqual(self.entry.expanded_text, LONG + "\n\nAnother " + LONG)
        self.assertEqual(len(self.entry.paste_blocks), 1)

    async def test_external_editor_gets_payload_and_keeps_blocks_on_no_change_or_failure(self):
        await self.paste()
        for returncode in (0, 1):
            with self.subTest(returncode=returncode):
                def editor(command, **kwargs):
                    self.assertEqual(Path(command[-1]).read_text(), LONG + "\n")
                    return subprocess.CompletedProcess(command, returncode)
                with patch.dict("os.environ", {"EDITOR": "test-editor"}), \
                        patch.object(self.app, "suspend", side_effect=nullcontext), \
                        patch("sos_tui.app.subprocess.run", side_effect=editor):
                    await self.app.action_edit_external()
                self.assertEqual(self.entry.text, Paste(LONG).label)
                self.assertEqual(self.entry.expanded_text, LONG)

    async def test_external_editor_can_edit_payload_as_plain_text(self):
        await self.paste()
        def editor(command, **kwargs):
            self.assertEqual(Path(command[-1]).read_text(), LONG + "\n")
            Path(command[-1]).write_text(LONG + "\nEdited\n")
            return subprocess.CompletedProcess(command, 0)
        with patch.dict("os.environ", {"EDITOR": "test-editor"}), \
                patch.object(self.app, "suspend", side_effect=nullcontext), \
                patch("sos_tui.app.subprocess.run", side_effect=editor):
            await self.app.action_edit_external()
        self.assertEqual(self.entry.expanded_text, LONG + "\nEdited")
        self.assertEqual(self.entry.paste_blocks, ())
