"""Quote a line of the last reply without sending it or losing the draft."""

import unittest
from unittest.mock import AsyncMock, patch

from textual._xterm_parser import XTermParser
from textual.document._document import Selection
from textual.widgets import Static

from sos_tui.app import SosApp
from sos_tui.btw import BtwUserMessage
from sos_tui.command_suggestions import CommandSuggestions
from sos_tui.config import QUOTE_BACKGROUND_COLOR, INPUT_QUOTE_BACKGROUND_COLOR, MODE_COLORS, MUTED_COLOR, TEXT_COLOR, TITLE_MD
from sos_tui.history_viewport import HistoryViewport
from sos_tui.message_input import MessageInput
from sos_tui.user_message import UserMessage


def reply(text, id="answer", **fields):
    return {"id": id, "role": "assistant", "content": [{"type": "text", "text": text}], **fields}


class QuoteBlocksTest(unittest.TestCase):
    def test_quote_blocks_keep_literal_text_newlines_and_image_styles(self):
        text = "Before\n\n> - [bold] Literal\n> Second line\n\nQuestion [Pasted Image]\na > b\n>> redirect"
        message = UserMessage(text, "sane", [{"origin": "clipboard"}])
        blocks = message.text_blocks()
        self.assertEqual([(quoted, block.plain) for quoted, block in blocks], [
            (False, "Before\n"), (True, "- [bold] Literal\nSecond line"),
            (False, "\nQuestion [Pasted Image]\na > b\n>> redirect"),
        ])
        label = blocks[-1][1]
        self.assertEqual([(label.plain[span.start:span.end], span.style) for span in label.spans],
                         [("[Pasted Image]", TITLE_MD)])
        self.assertEqual(message.text, text)


class QuoteSuggestionsTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.enterContext(patch.object(self.app, "check_auto_recap"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(80, 30)))
        self.app.connected = True
        self.app.session_id = "s"
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.menu = self.app.query_one(CommandSuggestions)
        self.chat = self.app.query_one(HistoryViewport)
        await self.pilot.pause()

    async def draft(self, text, cursor=None):
        self.entry.text = text
        self.entry.move_cursor(cursor or self.entry.document.end)
        await self.pilot.pause()

    async def answer(self, text, **fields):
        self.app.display_message(reply(text, **fields))
        await self.pilot.pause()

    async def test_typing_or_newline_accepts_quote_and_applies_input_below(self):
        await self.answer("First quote\nSecond quote")
        for key, inserted in (("space", " "), ("x", "x"), ("shift+enter", "\n"), ("ctrl+j", "\n")):
            for highlighted in (False, True):
                with self.subTest(key=key, highlighted=highlighted):
                    await self.draft("Before\n>\nAfter", (1, 1))
                    if highlighted:
                        await self.pilot.press("tab")
                    quote = "Second quote" if highlighted else "First quote"
                    await self.pilot.press(key)
                    expected = "Before\n> " + quote + "\n" + inserted + "\nAfter"
                    self.assertEqual(self.entry.text, expected)
                    self.assertFalse(self.menu.display)
                    self.assertEqual(self.entry.suggestion, "")
                    self.assertTrue(self.entry.render_line(1).text.startswith("▌"))
                    self.entry.action_undo()
                    self.assertEqual(self.entry.text, "Before\n> " + quote + "\n\nAfter")
                    self.entry.action_undo()
                    if highlighted:
                        self.assertEqual(self.entry.text, "Before\n> " + quote + "\nAfter")
                        self.entry.action_undo()
                    self.assertEqual(self.entry.text, "Before\n>\nAfter")
                    self.entry.action_redo()
                    self.entry.action_redo()
                    if highlighted:
                        self.entry.action_redo()
                    self.assertEqual(self.entry.text, expected)
        self.app.client.send.assert_not_awaited()

    async def test_batched_quote_acceptance_preserves_following_text(self):
        await self.answer("A possible quote")
        await self.draft("")
        for event in XTermParser().feed(">\nQuestion"):
            self.app.post_message(event)
        await self.pilot.pause()
        self.assertEqual(self.entry.text, "> A possible quote\n\nQuestion")
        self.assertFalse(self.menu.display)
        self.assertTrue(self.entry.render_line(0).text.startswith("▌"))
        self.app.client.send.assert_not_awaited()

    async def test_typing_and_pasting_below_default_quote_while_busy(self):
        await self.answer("Default quote\nOther quote")
        self.app.busy = True
        await self.draft("")
        for event in XTermParser().feed(">Question"):
            self.app.post_message(event)
        await self.pilot.pause()
        self.assertEqual(self.entry.text, "> Default quote\nQuestion")
        await self.draft(">")
        payload = "Large pasted text " * 30
        self.entry.paste_text(payload)
        await self.pilot.pause()
        self.assertEqual(self.entry.expanded_text, "> Default quote\n" + payload)
        self.assertEqual(len(self.entry.paste_blocks), 1)
        self.entry.action_undo()
        self.assertEqual(self.entry.text, "> Default quote\n")
        self.entry.action_undo()
        self.assertEqual(self.entry.text, ">")
        self.entry.action_redo()
        self.entry.action_redo()
        self.assertEqual(self.entry.expanded_text, "> Default quote\n" + payload)
        self.assertIsNone(self.app.queued_message)
        self.app.client.send.assert_not_awaited()

    async def test_deletions_remove_entire_quote_and_undo_restores_it(self):
        text = "Before\n> First quoted line\n> Second quoted line\nQuestion"
        for key, cursor in (("backspace", (1, 5)), ("ctrl+h", (1, 5)), ("delete", (2, 3)),
                            ("ctrl+w", (2, 10)), ("ctrl+u", (1, 7)),
                            ("ctrl+k", (2, 7)), ("backspace", (3, 0))):
            with self.subTest(key=key, cursor=cursor):
                await self.draft(text, cursor)
                await self.pilot.press(key)
                self.assertEqual(self.entry.text, "Before\nQuestion")
                self.entry.action_undo()
                self.assertEqual(self.entry.text, text)
                self.entry.action_redo()
                self.assertEqual(self.entry.text, "Before\nQuestion")

    async def test_partial_selection_cut_and_multiple_quotes(self):
        text = "> First quote\nQuestion\n> Another quote"
        for key in ("backspace", "ctrl+h", "delete", "ctrl+w", "ctrl+x"):
            with self.subTest(key=key):
                await self.draft(text)
                self.entry.selection = Selection((0, 3), (0, 6))
                await self.pilot.press(key)
                self.assertEqual(self.entry.text, "Question\n> Another quote")
                if key == "ctrl+x":
                    self.assertEqual(self.app.clipboard, "> First quote\n")
                self.entry.action_undo()
                self.assertEqual(self.entry.text, text)

    async def test_final_quote_and_normal_text_deletions(self):
        await self.draft("Question\n> Last quote", (1, 4))
        await self.pilot.press("delete")
        self.assertEqual(self.entry.text, "Question\n")
        await self.draft("> Quote\nQuestion", (1, 4))
        await self.pilot.press("backspace")
        self.assertEqual(self.entry.text, "> Quote\nQuetion")
        self.entry.read_only = True
        self.entry.move_cursor((0, 4))
        await self.pilot.press("ctrl+w", "ctrl+h", "backspace", "delete")
        self.assertEqual(self.entry.text, "> Quote\nQuetion")

    async def test_wrapped_quote_deletion_keeps_following_paste_and_question(self):
        quote = "> " + "Long quotation é漢 " * 30 + "\n"
        await self.draft(quote + "Question\n")
        payload = "Pasted payload " * 30
        self.entry.paste_text(payload)
        original = self.entry.text
        self.entry.move_cursor((0, 120))
        await self.pilot.press("backspace")
        self.assertEqual(self.entry.expanded_text, "Question\n" + payload)
        self.assertEqual(len(self.entry.paste_blocks), 1)
        self.entry.action_undo()
        self.assertEqual(self.entry.text, original)
        self.assertEqual(self.entry.expanded_text, quote + "Question\n" + payload)
        self.entry.action_redo()
        self.assertEqual(self.entry.expanded_text, "Question\n" + payload)

    async def test_lines_bullets_and_duplicates_are_literal_choices_not_visual_wraps(self):
        lines = ("Introduction.", "- First [bold] bullet", "- Second bullet", "Same", "Same", "Long " * 50)
        await self.answer("\n\n".join(lines))
        await self.draft(">")
        self.assertEqual(self.menu.matches, lines)
        self.assertEqual(self.menu.highlighted, 0)
        self.assertIsNone(self.menu.selected_command)
        self.assertEqual([option.prompt.plain for option in self.menu.options], list(lines))
        mode = self.app.mode
        for line in (*lines[1:], lines[0], lines[1]):
            await self.pilot.press("tab")
            self.assertEqual(self.menu.selected_quote, line)
            self.assertEqual(self.entry.text, "> " + line)
            self.assertEqual(self.app.mode, mode)
            self.assertTrue(self.entry.has_focus)
        await self.pilot.resize_terminal(40, 30)
        self.assertEqual(self.menu.matches, lines)
        self.app.client.send.assert_not_awaited()

    async def test_shift_tab_cycles_quote_text_without_changing_mode(self):
        await self.answer('First\nSecond\nThird')
        await self.draft('>')
        mode = self.app.mode
        for expected in ('Third', 'Second', 'First', 'Third'):
            await self.pilot.press('shift+tab')
            self.assertEqual(self.menu.selected_quote, expected)
            self.assertEqual(self.entry.text, '> ' + expected)
            self.assertTrue(self.entry.has_focus)
            self.assertEqual(self.app.mode, mode)
        self.app.client.send.assert_not_awaited()

    async def test_enter_inserts_selected_bullet_then_question_is_sent_below(self):
        await self.answer("Introduction\n- Explain this bullet")
        await self.draft(">")
        await self.pilot.press("tab")
        self.assertEqual(self.entry.suggestion, "")
        self.assertEqual(self.entry.text, "> - Explain this bullet")
        await self.pilot.press("enter")
        self.assertEqual(self.entry.text, "> - Explain this bullet\n")
        self.assertEqual(self.entry.cursor_location, (1, 0))
        self.assertFalse(self.menu.display)
        self.assertEqual(self.entry.suggestion, "")
        self.assertTrue(self.entry.has_focus)
        self.app.client.send.assert_not_awaited()
        await self.pilot.press(*"Why?", "enter")
        self.app.client.send.assert_awaited_once()
        sent = self.app.client.send.call_args.args[0]
        self.assertEqual(sent["type"], "message.send")
        self.assertEqual(sent["text"], "> - Explain this bullet\nWhy?")
        self.assertEqual(self.app.pending_turn.user.text, sent["text"])
        self.assertEqual(len(self.app.pending_turn.user.query(".user-quote")), 1)

    async def test_insertion_preserves_other_lines_paste_atoms_and_undo(self):
        await self.answer("Quoted line")
        pasted = "One\nTwo\nThree"
        self.entry.paste_text(pasted)
        self.entry.insert("\n>\nLater")
        self.entry.move_cursor((1, 1))
        await self.pilot.pause()
        before = self.entry.text
        blocks = self.entry.paste_blocks
        await self.pilot.press("tab", "enter")
        self.assertEqual(self.entry.expanded_text, pasted + "\n> Quoted line\n\nLater")
        self.assertEqual(self.entry.cursor_location, (2, 0))
        self.assertEqual(self.entry.paste_blocks, blocks)
        self.entry.action_undo()
        self.assertEqual(self.entry.expanded_text, pasted + "\n> Quoted line\nLater")
        self.entry.action_undo()
        self.assertEqual(self.entry.text, before)
        self.assertEqual(self.entry.paste_blocks, blocks)
        self.entry.action_redo()
        self.entry.action_redo()
        self.assertEqual(self.entry.expanded_text, pasted + "\n> Quoted line\n\nLater")
        self.app.client.send.assert_not_awaited()

    async def test_default_enter_accepts(self):
        await self.answer("First\nSecond")
        await self.draft(">")
        await self.pilot.press("enter")
        self.assertEqual(self.entry.text, "> First\n")
        self.assertFalse(self.menu.display)
        self.app.client.send.assert_not_awaited()

    async def test_leaving_quote_navigation_moves_below_without_deleting_the_quote(self):
        await self.answer('First\nSecond')
        for key in ('enter', 'left', 'right', 'home', 'end', 'backspace', 'delete'):
            with self.subTest(key=key):
                await self.draft('>')
                await self.pilot.press('tab')
                self.assertEqual(self.entry.text, '> Second')
                self.assertTrue(self.menu.display)
                await self.pilot.press(key)
                self.assertEqual(self.entry.text, '> Second\n')
                self.assertEqual(self.entry.cursor_location, (1, 0))
                self.assertFalse(self.menu.display)
        self.app.client.send.assert_not_awaited()

    async def test_browsing_replaces_only_current_quote_line_and_paste_goes_below(self):
        await self.answer('First\nSecond')
        await self.draft('> Existing\n>\nAfter', (1, 1))
        await self.pilot.press('tab', 'shift+tab')
        self.assertEqual(self.entry.text, '> Existing\n> First\nAfter')
        self.entry.paste_text('Pasted question')
        await self.pilot.pause()
        self.assertEqual(self.entry.expanded_text, '> Existing\n> First\nPasted question\nAfter')
        self.assertFalse(self.menu.display)
        self.app.client.send.assert_not_awaited()

    async def test_arrows_and_mouse_choose_real_quote_text(self):
        await self.answer('First\nSecond')
        await self.draft(">")
        await self.pilot.press("down")
        self.assertEqual(self.menu.selected_quote, "Second")
        await self.pilot.press("up")
        self.assertEqual(self.menu.selected_quote, "First")
        await self.pilot.click(self.menu, offset=(3, 1))
        self.assertEqual(self.menu.selected_quote, "Second")
        self.assertTrue(self.entry.has_focus)
        await self.pilot.press("enter")
        self.assertEqual(self.entry.text, "> Second\n")
        self.app.client.send.assert_not_awaited()

    async def test_only_a_bare_marker_at_the_cursor_opens_the_list(self):
        await self.draft(">")
        self.assertFalse(self.menu.display)  # No previous assistant message.
        await self.answer("Quote")
        for text in ("a > b", "> manual quote", ">> file", "a\n>\nb"):
            await self.draft(text)
            self.assertFalse(self.menu.display)
        await self.draft("  >")
        self.assertTrue(self.menu.display)
        self.entry.selection = Selection((0, 0), (0, 3))
        await self.pilot.pause()
        self.assertFalse(self.menu.display)
        self.entry.move_cursor(self.entry.document.end)
        await self.pilot.pause()
        self.assertTrue(self.menu.display)
        self.entry.disabled = True
        await self.pilot.pause()
        self.assertFalse(self.menu.display)
        self.entry.disabled = False
        self.entry.focus()
        await self.pilot.pause()
        self.assertTrue(self.menu.display)
        self.assertEqual(self.menu.highlighted, 0)
        self.entry.read_only = True
        self.entry.action_submit()
        self.app.client.send.assert_not_awaited()

    async def test_quote_selection_while_busy_does_not_queue_or_send(self):
        await self.answer("Current explanation")
        self.app.busy = True
        await self.draft(">")
        await self.pilot.press("tab", "enter")
        self.assertEqual(self.entry.text, "> Current explanation\n")
        self.assertIsNone(self.app.queued_message)
        self.app.client.send.assert_not_awaited()

    async def test_source_is_last_assistant_text_not_activity_or_a_side_conversation(self):
        await self.answer("Old answer", id="old")
        await self.answer("Latest answer", id="latest")
        self.app.display_message({"id": "reasoning", "role": "assistant", "content": [
            {"type": "reasoning", "text": "Private reasoning"}]})
        self.app.display_message(reply("Recap", id="recap", recap=True))
        self.app.display_message(reply("Compaction", id="compact", compaction={}))
        self.app.display_message({"id": "tool", "role": "tool", "content": [{"type": "text", "text": "Tool output"}]})
        await self.chat.mount(Static("Side answer"))
        await self.draft(">")
        self.assertEqual(self.menu.matches, ("Latest answer",))
        await self.pilot.press("tab")
        await self.answer("New answer", id="new")
        self.entry.update_suggestions()
        self.assertEqual(self.menu.matches, ("Latest answer",))
        self.assertEqual(self.entry.text, "> Latest answer")
        await self.draft('>')
        self.assertEqual(self.menu.matches, ("New answer",))
        self.assertEqual(self.menu.highlighted, 0)

    async def test_restoration_paging_live_updates_and_new_session_keep_the_right_source(self):
        await self.chat.replace({"session": {"id": "s", "mode": "sane"}, "page": {}, "messages": [
            reply("Old answer", id="old"), reply("Part one", id="last"), reply("Part two", id="last"),
        ]})
        await self.draft(">")
        self.assertEqual(self.menu.matches, ("Part one", "Part two"))
        await self.chat.replace({"session": {"id": "s", "mode": "sane"},
                                 "page": {"hasNewer": True}, "messages": [reply("Older page", id="older")]})
        self.entry.update_suggestions()
        self.assertEqual(self.menu.matches, ("Part one", "Part two"))
        self.app.display_live_messages([reply("New live answer", id="live")])
        self.entry.update_suggestions()
        self.assertEqual(self.menu.matches, ("New live answer",))
        await self.app.reset_session({"info": {}}).wait()
        await self.draft(">")
        self.assertFalse(self.menu.display)
        self.assertEqual(self.chat.quote_source, "")

    async def test_real_batched_marker_tab_enter_and_typing_keep_order(self):
        await self.answer("Selected line")
        for event in XTermParser().feed(">\t\rWhy?\r"):
            self.app.post_message(event)
        await self.pilot.pause()
        self.assertEqual(self.app.mode, "sane")
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["text"], "> Selected line\nWhy?")

    async def test_quote_background_half_bar_wrapping_and_question_below_live_and_history(self):
        text = "> - " + "Quoted words " * 18 + "\nQuestion [Pasted Image]"
        images = [{"origin": "clipboard"}]
        self.app.display_message({"id": "user", "role": "user", "content": [{"type": "text", "text": text}],
                                  "images": images, "metadata": {"mode": "slop"}})
        for restored in (False, True):
            if restored:
                await self.chat.replace({"session": {"id": "s", "mode": "sane"}, "page": {}, "messages": [
                    {"id": "user", "role": "user", "content": [{"type": "text", "text": text}],
                     "metadata": {"mode": "slop"}, "images": images},
                ]})
            for width in (80, 40):
                await self.pilot.resize_terminal(width, 30)
                await self.pilot.pause()
                user = self.chat.query_one(UserMessage)
                quote = user.query_one(".user-quote", Static)
                question = user.query(".user-text")[-1]
                self.assertGreater(quote.region.height, 1)
                self.assertEqual(quote.styles.background.hex, QUOTE_BACKGROUND_COLOR.upper())
                self.assertEqual(quote.styles.border_left[0], "outer")
                self.assertEqual(quote.styles.border_left[1].hex, MUTED_COLOR.upper())
                self.assertEqual(question.region.y, quote.region.bottom)
                self.assertEqual(str(question.render()), "Question [Pasted Image]")
                self.assertEqual(user.query_one(".mode-band").region.height, user.region.height)
                self.assertEqual(user.query_one(".mode-band").styles.background.hex, MODE_COLORS["slop"].upper())
                for line in quote.render_lines(quote.region.size.region):
                    self.assertTrue(line.text.startswith("▌  "))
                    for segment in line.crop(1):
                        if segment.text.strip():
                            self.assertEqual(segment.style.color.get_truecolor().hex.lower(), TEXT_COLOR.lower())
                            self.assertFalse(segment.style.italic)
                            self.assertFalse(segment.style.reverse)
                user.set_images(images)
                self.assertEqual(question.render().spans[0].style.foreground.hex, TITLE_MD)
                self.assertEqual(user.text, text)

    async def test_btw_user_messages_share_quote_rendering(self):
        message = BtwUserMessage("> Quoted\nFollow-up", "sane")
        await self.chat.mount(message)
        await self.pilot.pause()
        self.assertEqual(str(message.query_one(".user-quote", Static).render()), "Quoted")
        self.assertEqual(message.query(".user-text")[-1].region.y, message.query_one(".user-quote").region.bottom)

    async def test_composer_quote_style_wraps_scrolls_and_keeps_original_text(self):
        text = "> " + "Quoted words é漢 " * 20 + "\nQuestion"
        await self.draft(text)
        for width in (80, 40):
            await self.pilot.resize_terminal(width, 30)
            await self.pilot.pause()
            self.entry.scroll_home(animate=False, immediate=True)
            await self.pilot.pause()
            for y in range(self.entry.content_size.height):
                row, _ = self.entry.wrapped_document._offset_to_line_info[y + self.entry.scroll_offset.y]
                rendered = self.entry.render_line(y)
                if row == 0:
                    self.assertTrue(rendered.text.startswith("▌  "))
                    self.assertEqual(next(iter(rendered)).style.color.get_truecolor().hex.lower(), MUTED_COLOR.lower())
                    for segment in rendered:
                        self.assertEqual(segment.style.bgcolor.get_truecolor().hex.lower(), INPUT_QUOTE_BACKGROUND_COLOR.lower())
                    for segment in rendered.crop(1):
                        if segment.text.strip():
                            self.assertEqual(segment.style.color.get_truecolor().hex.lower(), TEXT_COLOR.lower())
                            self.assertFalse(segment.style.italic)
                            self.assertFalse(segment.style.reverse)
                    if y == 1:
                        expected = self.entry.wrapped_document.offset_to_location((0, 1))
                        await self.pilot.click(self.entry, offset=(self.entry.gutter.left + 3,
                                                                  self.entry.gutter.top + 1))
                        self.assertEqual(self.entry.cursor_location, expected)
                        self.assertEqual(self.entry.cursor_screen_offset.x, self.entry.content_region.x + 3)
                else:
                    self.assertTrue(rendered.text.startswith("Question"))
            self.entry.scroll_end(animate=False, immediate=True)
            await self.pilot.pause()
            self.assertEqual(self.entry.text, text)
            self.assertEqual(self.entry.expanded_text, text)
        self.entry.selection = Selection((0, 0), (0, 8))
        await self.pilot.pause()
        self.assertEqual(self.entry.selected_text, text[:8])
        self.entry.scroll_home(animate=False, immediate=True)
        await self.pilot.pause()
        selected = self.entry.render_line(0).crop(1, 8)
        self.assertTrue(any(segment.style.bgcolor.get_truecolor().hex.lower() != INPUT_QUOTE_BACKGROUND_COLOR.lower()
                            for segment in selected))
        self.entry.move_cursor((0, 1))
        await self.pilot.press("backspace")
        self.assertFalse(self.entry.render_line(0).text.startswith("▌"))
        self.entry.action_undo()
        self.assertEqual(self.entry.text, text)

    async def test_composer_placeholder_plain_text_and_quote_use_the_same_text_origin(self):
        self.entry.placeholder = "Write a message"
        await self.draft("")
        self.assertEqual(self.entry.render_line(0).text.rstrip(), "Write a message")
        await self.draft("Plain text")
        self.assertEqual(self.entry.render_line(0).text.rstrip(), "Plain text")
        await self.answer("Selected quote")
        await self.draft(">")
        await self.pilot.press("tab", "enter")
        self.assertTrue(self.entry.render_line(0).text.startswith("▌  Selected quote"))
        self.assertEqual(self.entry.content_region.x - self.app.query_one("#mode-indicator").region.right, 1)
        self.assertTrue(self.entry.render_lines(self.entry.region.size.region)[0].text.startswith(" ▌"))
        await self.pilot.press(*"Question")
        self.assertEqual(self.entry.render_line(1).text.rstrip(), "Question")
        self.entry.move_cursor((1, 0))
        await self.pilot.pause()
        self.assertEqual(self.entry.cursor_screen_offset.x, self.entry.content_region.x)
        await self.pilot.click(self.entry, offset=(self.entry.gutter.left + 3, self.entry.gutter.top + 1))
        self.assertEqual(self.entry.cursor_location, (1, 3))
        for segment in self.entry.render_line(1):
            if "Question" in segment.text:
                self.assertFalse(segment.style.italic)
        self.assertEqual(self.entry.text, "> Selected quote\nQuestion")
        self.app.client.send.assert_not_awaited()
