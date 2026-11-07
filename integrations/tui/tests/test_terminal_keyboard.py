"""Real terminal encodings, rather than synthetic shortcut names."""

import os
import unittest
from unittest.mock import patch

from textual.app import App, ComposeResult
from textual._xterm_parser import XTermParser
from textual.document._document import Selection

from sos_tui.message_input import MessageInput


class Editor(App):
    def get_css_variables(self):
        return {**super().get_css_variables(), "muted": "#71808E"}

    def __init__(self):
        super().__init__()
        self.submitted = []

    def compose(self) -> ComposeResult:
        yield MessageInput()

    def on_message_input_submitted(self, event):
        self.submitted.append(event.text)


class TerminalKeyboardTest(unittest.IsolatedAsyncioTestCase):
    async def test_ctrl_w_encodings_delete_in_terminal_order(self):
        for sequence in ("\x17", "\x1b[119;5u", "\x1b[27;5;119~"):
            with self.subTest(sequence=repr(sequence)):
                app = Editor()
                async with app.run_test() as pilot:
                    entry = app.query_one(MessageInput)
                    entry.focus()
                    await pilot.pause()
                    for event in XTermParser().feed("first second" + sequence + "third\r"):
                        app.post_message(event)
                    await pilot.pause()
                    self.assertEqual(entry.text, "first third")
                    self.assertEqual(app.submitted, ["first third"])
                    entry.read_only = True
                    for event in XTermParser().feed(sequence):
                        app.post_message(event)
                    await pilot.pause()
                    self.assertEqual(entry.text, "first third")

    async def test_raw_newline_encodings_and_plain_enter(self):
        for sequence in ("\n", "\x1b[13;2u", "\x1b[27;2;13~"):
            with self.subTest(sequence=repr(sequence)):
                app = Editor()
                async with app.run_test() as pilot:
                    entry = app.query_one(MessageInput)
                    entry.focus()
                    entry.text = "ab"
                    entry.move_cursor((0, 1))
                    parser = XTermParser()
                    # Also exercise sequences fragmented by the terminal transport.
                    for char in sequence:
                        for event in parser.feed(char):
                            app.post_message(event)
                    await pilot.pause()
                    self.assertEqual(entry.text, "a\nb")
                    self.assertEqual(entry.cursor_location, (1, 0))
                    self.assertEqual(app.submitted, [])
                    for event in parser.feed("\r"):
                        app.post_message(event)
                    await pilot.pause()
                    self.assertEqual(app.submitted, ["a\nb"])

    async def test_newline_replaces_selection_and_is_undoable(self):
        app = Editor()
        async with app.run_test() as pilot:
            entry = app.query_one(MessageInput)
            entry.text = "abcd"
            entry.selection = Selection((0, 1), (0, 3))
            await pilot.press("ctrl+j")
            self.assertEqual(entry.text, "a\nd")
            entry.action_undo()
            self.assertEqual(entry.text, "abcd")
            entry.read_only = True
            await pilot.press("shift+enter", "ctrl+j")
            self.assertEqual(entry.text, "abcd")

    async def test_batched_typing_newline_and_submit_keep_terminal_order(self):
        for newline in ("\n", "\x1b[13;2u", "\x1b[27;2;13~"):
            with self.subTest(newline=repr(newline)):
                app = Editor()
                async with app.run_test() as pilot:
                    entry = app.query_one(MessageInput)
                    entry.focus()
                    await pilot.pause()
                    parser = XTermParser()
                    # A single terminal read may contain text and shortcut keys.
                    for event in parser.feed("first" + newline + "second\r"):
                        app.post_message(event)
                    await pilot.pause()
                    self.assertEqual(entry.text, "first\nsecond")
                    self.assertEqual(app.submitted, ["first\nsecond"])

    @unittest.skipUnless(os.name == "posix", "Unix terminal driver")
    async def test_driver_activation_order_and_repeated_editor_round_trips(self):
        from textual.drivers.linux_driver import LinuxDriver
        from sos_tui.terminal_driver import TerminalDriver
        from sos_tui.app import SosApp

        self.assertIs(SosApp().driver_class, TerminalDriver)
        driver = object.__new__(TerminalDriver)
        output = []
        with patch.object(LinuxDriver, "write", lambda self, data: output.append(data)):
            for _ in range(3):
                driver.write("\x1b[?1049h")
                driver.write("\x1b[>25u")
                driver.write("content")
                driver.write("\x1b[<u")
                driver.write("\x1b[?1049l")
        self.assertEqual(output, ["\x1b[?1049h", "\x1b[>4;2m", "\x1b[>25u",
                                  "content", "\x1b[<u", "\x1b[>4m", "\x1b[?1049l"] * 3)
