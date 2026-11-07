"""Copy on mouse release without changing drafts, clicks, or private inputs."""

import asyncio
import unittest
from unittest.mock import AsyncMock, Mock, patch

from textual.screen import ModalScreen
from textual.widgets import Input, Static, Tooltip

from sos_tui.app import SosApp
from sos_tui.clipboard import copy_command, copy_native
from sos_tui.copied_notice import CopiedNotice
from sos_tui.message_input import MessageInput


class CopySelectionTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 30), tooltips=True))
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.copy = self.enterContext(patch.object(self.app, "copy_to_clipboard", wraps=self.app.copy_to_clipboard))

    async def test_chat_copies_only_on_release_and_clears_highlight(self):
        text = Static("Alpha beta gamma", markup=False)
        await self.app.query_one("#chat").mount(text)
        await self.pilot.pause()
        await self.pilot.mouse_down(text, offset=(0, 0))
        await self.pilot.hover(text, offset=(10, 0))
        self.copy.assert_not_called()
        self.assertEqual(len(self.app.screen.query(CopiedNotice)), 0)
        selected = self.app.screen.get_selected_text()
        self.assertTrue(selected)
        await self.pilot.mouse_up(text, offset=(10, 0))
        self.copy.assert_called_once_with(selected)
        self.assertEqual(self.app.clipboard, selected)
        self.assertTrue(self.app.screen.query_one(CopiedNotice).display)
        self.assertIsNone(self.app.screen.get_selected_text())
        await self.pilot.click(text)
        self.assertEqual(self.copy.call_count, 1)

    async def test_drag_in_draft_copies_without_editing_or_submitting(self):
        self.entry.text = "Alpha beta gamma"
        await self.pilot.pause()
        await self.pilot.mouse_down(self.entry, offset=(1, 0))
        await self.pilot.hover(self.entry, offset=(9, 0))
        self.copy.assert_not_called()
        await self.pilot.mouse_up(self.entry, offset=(9, 0))
        self.assertTrue(self.entry.selected_text)
        self.copy.assert_called_once_with(self.entry.selected_text)
        self.assertEqual(self.entry.text, "Alpha beta gamma")
        self.assertIsNone(self.app.pending_text)

    async def test_reverse_multiline_selection_across_widgets(self):
        first = Static("First line", markup=False)
        last = Static("Last line", markup=False)
        await self.app.query_one("#chat").mount(first, last)
        await self.pilot.pause()
        await self.pilot.mouse_down(last, offset=(9, 0))
        await self.pilot.hover(first, offset=(0, 0))
        selected = self.app.screen.get_selected_text()
        self.assertIn("First line", selected)
        self.assertIn("Last line", selected)
        self.copy.assert_not_called()
        await self.pilot.mouse_up(first, offset=(0, 0))
        self.copy.assert_called_once_with(selected)

    async def test_popup_text_is_copied_too(self):
        class TextDialog(ModalScreen):
            def compose(self):
                yield Static("Popup text", id="popup-text")
        await self.app.push_screen(TextDialog())
        text = self.app.screen.query_one("#popup-text")
        await self.pilot.mouse_down(text, offset=(0, 0))
        await self.pilot.hover(text, offset=(5, 0))
        await self.pilot.mouse_up(text, offset=(5, 0))
        self.copy.assert_called_once()
        self.assertTrue(self.app.clipboard.startswith("Popup"))
        notice = self.app.screen.query_one(CopiedNotice)
        self.assertTrue(notice.display)
        self.assertEqual(notice.region.x, 1)
        self.assertEqual(notice.region.bottom, self.app.screen.size.height - 1)

    async def test_copied_notice_is_compact_and_does_not_move_focus_or_chat(self):
        chat = self.app.query_one("#chat")
        before = chat.region
        focused = self.app.focused
        self.app.copy_to_clipboard("text")
        await self.pilot.pause()
        notice = self.app.screen.query_one(CopiedNotice)
        label = notice.query_one(".copied-label", Static)
        band = notice.query_one(".copied-band", Static)
        self.assertEqual(str(label.render()), "Copied")
        self.assertEqual(notice.styles.background.hex, "#303A48")
        self.assertEqual(label.styles.background.hex, "#303A48")
        self.assertEqual(label.styles.color.hex, "#E9C46A")
        self.assertEqual(band.styles.border_left[0], "outer")
        self.assertEqual(band.styles.border_left[1].hex, "#E9C46A")
        self.assertEqual([line.text for line in band.render_lines(band.region.size.region)], ["▌"] * 3)
        self.assertEqual((band.region.width, band.region.height), (1, 3))
        self.assertEqual(band.region.x, notice.region.x)
        self.assertEqual(label.region.x, band.region.right)
        for edge in (notice.styles.border_top, notice.styles.border_right,
                     notice.styles.border_bottom, notice.styles.border_left):
            self.assertEqual(edge[0], "")
        self.assertEqual((notice.region.width, notice.region.height), (14, 3))
        lines = [line.text for line in label.render_lines(label.region.size.region)]
        self.assertEqual(lines, ["             ", "   Copied    ", "             "])
        self.assertEqual(notice.region.x, 1)
        self.assertEqual(notice.region.bottom, self.app.query_one("#agent-info").region.y)
        self.assertEqual(chat.region, before)
        self.assertIs(self.app.focused, focused)
        await self.pilot.resize_terminal(70, 25)
        await self.pilot.pause()
        self.assertEqual(notice.region.x, 1)
        self.assertEqual(notice.region.bottom, self.app.query_one("#agent-info").region.y)
        self.entry.text = "First line\nSecond line\nThird line"
        await self.pilot.pause()
        self.assertEqual(notice.region.bottom, self.app.query_one("#agent-info").region.y)

    async def test_copied_notice_refreshes_one_timer_and_expires(self):
        with patch.object(CopiedNotice, "DURATION", 0.4):
            self.app.copy_to_clipboard("first")
            await self.pilot.pause(0.1)
            notice = self.app.screen.query_one(CopiedNotice)
            previous = notice.hide_timer
            self.app.copy_to_clipboard("second")
            await self.pilot.pause(0.1)
            self.assertEqual(len(self.app.screen.query(CopiedNotice)), 1)
            self.assertIsNot(notice.hide_timer, previous)
            self.assertTrue(notice.display)
            await self.pilot.pause(0.5)
            self.assertFalse(notice.display)

    async def test_osc52_and_native_are_both_used_in_a_real_driver(self):
        import base64
        driver = Mock(is_headless=False, is_web=False)
        with patch.object(self.app, "_driver", driver), patch.object(self.app, "copy_native_clipboard") as native:
            self.app.copy_to_clipboard("été\ntext")
            encoded = base64.b64encode("été\ntext".encode()).decode()
            driver.write.assert_called_once_with(f"\x1b]52;c;{encoded}\a")
            native.assert_called_once_with("été\ntext")

    async def test_password_selection_is_not_automatically_copied(self):
        class PasswordDialog(ModalScreen):
            def compose(self):
                yield Input("private-key", password=True)
        await self.app.push_screen(PasswordDialog())
        field = self.app.screen.query_one(Input)
        await self.pilot.mouse_down(field, offset=(2, 1))
        await self.pilot.hover(field, offset=(8, 1))
        await self.pilot.mouse_up(field, offset=(8, 1))
        self.copy.assert_not_called()

    async def test_tooltips_disabled_even_when_widget_has_one(self):
        self.assertTrue(self.app._disable_tooltips)
        self.entry.tooltip = "Must not appear"
        self.app.TOOLTIP_DELAY = 0.01
        await self.pilot.hover(self.entry)
        await self.pilot.pause(0.1)
        self.assertFalse(any(widget.display for widget in self.app.screen.query(Tooltip)))

    async def test_keyboard_selection_does_not_copy_until_mouse_release(self):
        self.entry.text = "Draft"
        self.entry.move_cursor((0, 0))
        await self.pilot.press("shift+right", "shift+right")
        self.copy.assert_not_called()


class NativeClipboardTest(unittest.IsolatedAsyncioTestCase):
    def test_native_commands_and_ssh_fallback(self):
        with patch("sos_tui.clipboard.os.environ", {}), patch("sos_tui.clipboard.shutil.which", return_value="found"):
            for platform, env, expected in (
                ("darwin", {}, ["pbcopy"]),
                ("linux", {"WAYLAND_DISPLAY": "wayland-0"}, ["wl-copy"]),
                ("linux", {"DISPLAY": ":0"}, ["xclip", "-selection", "clipboard"]),
                ("linux", {}, None),
                ("darwin", {"SSH_CONNECTION": "remote"}, None),
            ):
                with self.subTest(platform=platform, env=env), patch("sos_tui.clipboard.sys.platform", platform), patch.dict("sos_tui.clipboard.os.environ", env, clear=True):
                    self.assertEqual(copy_command(), expected)

    async def test_selection_is_passed_as_utf8_stdin_not_shell_code(self):
        process = Mock(returncode=0, communicate=AsyncMock(return_value=(b"", b"")))
        with patch("sos_tui.clipboard.copy_command", return_value=["pbcopy"]), patch("sos_tui.clipboard.asyncio.create_subprocess_exec", new=AsyncMock(return_value=process)) as spawn:
            text = 'été\n"$(do not execute)"'
            await copy_native(text)
            self.assertEqual(spawn.call_args.args, ("pbcopy",))
            process.communicate.assert_awaited_once_with(text.encode("utf-8"))

    async def test_missing_native_command_is_optional(self):
        with patch("sos_tui.clipboard.copy_command", return_value=["pbcopy"]), patch("sos_tui.clipboard.asyncio.create_subprocess_exec", side_effect=FileNotFoundError):
            await copy_native("text")

    async def test_cancellation_terminates_native_process(self):
        process = Mock(returncode=None, communicate=AsyncMock(side_effect=asyncio.CancelledError), wait=AsyncMock())
        with patch("sos_tui.clipboard.copy_command", return_value=["pbcopy"]), patch("sos_tui.clipboard.asyncio.create_subprocess_exec", new=AsyncMock(return_value=process)):
            with self.assertRaises(asyncio.CancelledError):
                await copy_native("text")
            process.kill.assert_called_once()
            process.wait.assert_awaited_once()
