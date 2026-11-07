import asyncio
import base64
from pathlib import Path
import tempfile
import unittest
import subprocess
from contextlib import nullcontext
from unittest.mock import AsyncMock, patch

from sos_tui.app import SosApp
from sos_tui.images import pasted_paths, snapshot, clipboard_image
from sos_tui.message_input import MessageInput
from sos_tui.paste_blocks import PathPaste
from sos_tui.input_error_notice import InputErrorNotice
from sos_tui.user_message import UserMessage
from sos_tui.config import TITLE_MD
from textual.widgets import Static

PIXEL = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII=")


class ImagePathsTest(unittest.TestCase):
    def test_file_and_directory_paths_are_atoms_without_reading_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = root / "notes with spaces.txt"
            file.write_text("not attached")
            folder = root / "folder with spaces.png"
            folder.mkdir()
            for path in (file, folder):
                for text in (str(path), f'"{path}"', str(path).replace(" ", "\\ ")):
                    with self.subTest(text=text), patch.object(Path, "open", side_effect=AssertionError("No file read")):
                        atom, = pasted_paths(text)
                        self.assertIsInstance(atom, PathPaste)
                        self.assertEqual(atom.label, f"[Path • {path}]")
                        self.assertEqual(atom.text, str(path))
            self.assertEqual(pasted_paths(f"Please read {file}"), ())
            self.assertEqual(pasted_paths(str(root / "missing.txt")), ())

    def test_mixed_drop_preserves_order_and_image_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "capture.png"
            image.write_bytes(PIXEL)
            file = root / "notes.txt"
            file.touch()
            attachments = pasted_paths(f'"{file}" "{image}" "{root}"')
            self.assertEqual(len(attachments), 3)
            self.assertIsInstance(attachments[0], PathPaste)
            self.assertEqual(attachments[1].pixels, PIXEL)
            self.assertIsInstance(attachments[2], PathPaste)

    def test_sent_image_labels_use_composer_yellow_without_coloring_prose(self):
        label = "[Image • /tmp/capture [1].png]"
        text = f"Look here {label} and [Pasted Image] then [Pasted Image]."
        images = [{"origin": "/tmp/capture [1].png"}, {"origin": "clipboard"}, {"origin": "clipboard"}]
        rendered = UserMessage(text, "slop", images).display_text()
        self.assertEqual(rendered.plain, text)
        self.assertEqual([(text[span.start:span.end], span.style) for span in rendered.spans],
                         [(label, TITLE_MD), ("[Pasted Image]", TITLE_MD), ("[Pasted Image]", TITLE_MD)])
        self.assertFalse(UserMessage(text, "slop").display_text().spans)

    def test_history_message_does_not_append_image_metadata(self):
        text = "What is this? [Image • /tmp/capture.png]"
        images = [{"name": "capture.png", "width": 1234, "height": 162, "id": "img_saved"}]
        message = UserMessage(text, "slop", images)
        self.assertEqual(message.display_text().plain, text)
        self.assertEqual(message.images, images)

    def test_paths_with_spaces_are_captured_and_phrases_are_not(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "a capture.png"
            path.write_bytes(PIXEL)
            for text in (str(path), f'"{path}"', str(path).replace(" ", "\\ ")):
                images = pasted_paths(text)
                self.assertEqual(len(images), 1)
                self.assertEqual(images[0].pixels, PIXEL)
            self.assertEqual(pasted_paths(f"Please read {path}"), ())
            captured = pasted_paths(str(path))[0]
            path.unlink()
            self.assertEqual(base64.b64decode(captured.payload()["data"]), PIXEL)
            self.assertEqual(pasted_paths(str(path)), ())

    def test_non_images_and_oversized_images_are_rejected(self):
        with self.assertRaises(ValueError):
            snapshot(b"not an image", "file.png", "file.png")
        with self.assertRaises(ValueError):
            snapshot(PIXEL + b"x" * (5 * 1024 * 1024), "file.png", "file.png")

    def test_unknown_tilde_user_is_not_an_image_path(self):
        self.assertEqual(pasted_paths("~typesafe/jev-latest"), ())
        self.assertEqual(pasted_paths("~typesafe/capture.png"), ())


class ImageComposerTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 30)))
        self.app.connected = True
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.image = snapshot(PIXEL, "clipboard.png", "clipboard")
        self.send = self.enterContext(patch.object(self.app.client, "send", new_callable=AsyncMock))

    async def test_live_image_metadata_does_not_add_a_second_label(self):
        text = "What is this? [Pasted Image]"
        message = UserMessage(text, "slop")
        await self.app.query_one("#chat").mount(message)
        images = [{"name": "clipboard.png", "origin": "clipboard", "width": 1234, "height": 162, "id": "img_live"}]
        message.set_images(images)
        self.assertEqual(str(message.query_one(".user-text", Static).render()), text)
        self.assertEqual(message.images, images)

        rendered = message.query_one(".user-text", Static).render()
        self.assertEqual([(rendered.plain[span.start:span.end], span.style.foreground.hex) for span in rendered.spans],
                         [("[Pasted Image]", TITLE_MD)])

    async def test_atomic_undo_redo_and_removal(self):
        self.entry.add_images((self.image,))
        self.assertEqual(self.entry.text, "[Pasted Image]")
        self.assertEqual(self.entry.images, (self.image,))
        self.entry.action_undo()
        self.assertFalse(self.entry.images)
        self.entry.action_redo()
        self.assertEqual(self.entry.images, (self.image,))
        self.entry.remove_paste(self.image)
        self.assertEqual(self.entry.text, "")
        self.entry.action_undo()
        self.assertEqual(self.entry.images, (self.image,))

    async def test_multiple_images_replace_selection_with_one_undo_step(self):
        self.entry.add_images((self.image,) * 8)
        self.assertEqual(len(self.entry.images), 8)
        self.entry.action_select_all()
        self.entry.add_images((self.image,))
        self.assertEqual(len(self.entry.images), 1)
        self.entry.action_undo()
        self.assertEqual(len(self.entry.images), 8)

    async def test_daemon_refusal_rolls_back_optimistic_turn_and_restores_images(self):
        self.entry.add_images((self.image,))
        original = self.entry.text, self.entry.paste_blocks, self.entry.selection
        async def answer(request):
            self.assertEqual(request["type"], "message.send")
            self.assertEqual(self.entry.text, "")
            self.assertTrue(self.app.busy)
            self.assertIs(self.app.running_activity, self.app.pending_turn.activity)
            self.app.handle_event({"type": "request.failed", "requestId": request["requestId"],
                                   "code": "image_input", "error": "Image support is unknown"})
        self.send.side_effect = answer
        await self.app.on_message_input_submitted(MessageInput.Submitted(self.entry.expanded_text))
        await self.pilot.pause()
        self.assertEqual((self.entry.text, self.entry.paste_blocks, self.entry.selection), original)
        self.assertFalse(self.app.busy)
        self.assertIsNone(self.app.pending_turn)
        self.assertIsNone(self.app.running_activity)
        self.assertEqual(len(self.app.query(UserMessage)), 0)
        self.assertTrue(self.app.screen.query_one(InputErrorNotice).display)

    async def test_catalog_network_retry_before_acceptance_is_not_an_image_error(self):
        self.entry.add_images((self.image,))
        await self.app.on_message_input_submitted(MessageInput.Submitted(self.entry.expanded_text))
        request = self.send.call_args.args[0]
        self.app.handle_event({"type": "execution.retry", "retry": {
            "attempt": 2, "limit": 10, "seconds": 2, "reason": "Connection problem"}})
        await self.pilot.pause()
        self.assertIn("Connection problem", str(self.app.retry_notice.render()))
        self.assertTrue(self.app.busy)
        self.assertFalse(any(notice.display for notice in self.app.screen.query(InputErrorNotice)))
        self.app.handle_event({"type": "request.failed", "requestId": request["requestId"],
                               "code": "catalog_unavailable", "error": "Could not load the model catalog after 10 retries."})
        await self.pilot.pause()
        self.assertIsNone(self.app.retry_notice)
        self.assertFalse(self.app.busy)
        self.assertFalse(any(notice.display for notice in self.app.screen.query(InputErrorNotice)))
        self.assertEqual(self.entry.images, (self.image,))
        self.assertEqual(self.entry.text, self.image.label)
        self.assertFalse(self.app.query(UserMessage))

    async def test_supported_send_has_separate_pixels_and_failure_restores_them(self):
        self.entry.add_images((self.image,))
        await self.app.on_message_input_submitted(MessageInput.Submitted(self.entry.expanded_text))
        request = self.send.call_args.args[0]
        self.assertEqual(request["type"], "message.send")
        self.assertEqual(self.app.pending_turn.user.display_text().spans[0].style, TITLE_MD)
        self.assertEqual(base64.b64decode(request["images"][0]["data"]), PIXEL)
        self.assertNotIn(request["images"][0]["data"], request["text"])
        self.assertFalse(self.entry.images)
        self.app.handle_event({"type": "request.failed", "requestId": request["requestId"],
                               "code": "image_input", "error": "Model changed"})
        self.assertEqual(self.entry.images, (self.image,))
        self.assertEqual(self.entry.text, self.image.label)

    async def test_queue_restore_preserves_captured_objects(self):
        self.entry.add_images((self.image,))
        self.app.queue_message(self.entry.expanded_text)
        self.assertEqual(self.app.queued_message.images, (self.image,))
        self.entry.paste_text("new draft")
        self.app.restore_queued_message()
        self.assertEqual(self.entry.images, (self.image,))
        self.assertIn("new draft", self.entry.text)

    async def test_clipboard_ssh_is_explicitly_unavailable(self):
        with patch.dict("os.environ", {"SSH_CONNECTION": "remote"}):
            with self.assertRaisesRegex(ValueError, "SSH"):
                await clipboard_image()

    async def test_external_editor_keeps_images_when_labels_are_deleted(self):
        self.entry.add_images((self.image,))
        def edit(command, **kwargs):
            path = Path(command[-1])
            self.assertNotIn(base64.b64encode(PIXEL).decode(), path.read_text())
            path.write_text("Edited description\n")
            return subprocess.CompletedProcess(command, 0)
        with patch.dict("os.environ", {"EDITOR": "test-editor"}), \
                patch.object(self.app, "suspend", side_effect=nullcontext), \
                patch("sos_tui.app.subprocess.run", side_effect=edit):
            await self.app.action_edit_external()
        self.assertEqual(self.entry.images, (self.image,))
        self.assertIn("Edited description", self.entry.expanded_text)

    async def test_queued_send_rolls_back_on_daemon_refusal(self):
        async def answer(request):
            self.assertEqual(request["type"], "message.send")
            self.assertIsNotNone(self.app.pending_turn)
            self.app.handle_event({"type": "request.failed", "requestId": request["requestId"],
                                   "code": "image_input", "error": "New model has no image support"})
        self.send.side_effect = answer
        self.app.busy = True
        worker = self.app.send_queued_message(self.image.label, (self.image,))
        await worker.wait()
        self.assertFalse(self.app.busy)
        self.assertIsNone(self.app.pending_turn)
        self.assertEqual(self.entry.images, (self.image,))

    async def test_pending_turn_is_visible_and_animated_while_transport_waits(self):
        self.entry.add_images((self.image,))
        entered, release = asyncio.Event(), asyncio.Event()
        async def send(request):
            self.assertEqual(request["type"], "message.send")
            entered.set()
            await release.wait()
        self.send.side_effect = send
        sending = asyncio.create_task(self.app.on_message_input_submitted(MessageInput.Submitted(self.entry.expanded_text)))
        try:
            await asyncio.wait_for(entered.wait(), 2)
            await self.pilot.pause()
            turn = self.app.pending_turn
            self.assertTrue(turn.user.is_mounted)
            self.assertTrue(turn.activity.is_mounted)
            self.assertIs(self.app.running_activity, turn.activity)
            self.assertIsNotNone(turn.activity.toggle.started_at)
            self.assertTrue(self.app.busy)
            self.assertEqual(self.entry.text, "")
            self.assertFalse(self.entry.disabled)
            self.assertTrue(self.entry.has_focus)
            release.set()
            await sending
            self.app.display_message({"id": "accepted", "role": "user",
                "content": [{"type": "text", "text": self.image.label}],
                "images": [{"origin": "clipboard"}]})
            await self.pilot.pause()
            self.assertIsNone(self.app.pending_turn)
            self.assertEqual(list(self.app.query(UserMessage)), [turn.user])
            self.assertIs(self.app.running_activity, turn.activity)
        finally:
            release.set()
            await sending

    async def test_transport_failure_restores_image_before_new_draft_without_losing_either(self):
        self.entry.add_images((self.image,))
        async def fail(request):
            self.entry.paste_text("New draft")
            raise OSError("Socket closed")
        self.send.side_effect = fail
        await self.app.on_message_input_submitted(MessageInput.Submitted(self.entry.expanded_text))
        await self.pilot.pause()
        self.assertEqual(self.entry.images, (self.image,))
        self.assertEqual(self.entry.expanded_text, self.image.label + "\n\nNew draft")
        self.assertFalse(self.app.busy)
        self.assertIsNone(self.app.pending_turn)
        self.assertEqual(len(self.app.query(UserMessage)), 0)
