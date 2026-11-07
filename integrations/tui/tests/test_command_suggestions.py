"""Slash completion stays in the composer and never guesses a command on Enter."""

import unittest
from unittest.mock import AsyncMock, patch
from textual._xterm_parser import XTermParser

from sos_tui.app import SosApp
from sos_tui.command_suggestions import CommandSuggestions
from sos_tui.message_input import MessageInput
from sos_tui.slash_commands import COMMANDS, COMMAND_DESCRIPTIONS, matching_commands
from sos_tui.skill_details import SkillDetails
from rich.console import Console
from textual.widgets import Static
from sos_tui.config import ALGAE_MD, MISC_MD, MUTED_COLOR


class SlashCommandsTest(unittest.TestCase):
    def test_matching_is_alphabetical_and_only_for_a_command_prefix(self):
        self.assertEqual(matching_commands("/"), ("/btw", "/compact", "/connect", "/mcps", "/model", "/new", "/permissions", "/recap", "/redo", "/resume", "/undo", "/worktree"))
        self.assertEqual(matching_commands("/re"), ("/recap", "/redo", "/resume"))
        self.assertEqual(COMMANDS, tuple(sorted(COMMANDS)))
        self.assertEqual(matching_commands("/co"), ("/compact", "/connect"))
        self.assertEqual(matching_commands("/mo"), ("/model",))
        self.assertEqual(matching_commands("  /mo"), ("/model",))
        for text in ("", "Hello /mo", "/unknown", "/model argument", "/mo\nmore", "/mo "):
            with self.subTest(text=text):
                self.assertEqual(matching_commands(text), ())


class CommandSuggestionsTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(80, 24), notifications=True))
        self.app.connected = True
        self.app.session_id = "s"
        self.app.client.send = AsyncMock()
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()
        self.menu = self.app.query_one(CommandSuggestions)
        await self.pilot.pause()

    async def draft(self, text):
        self.entry.text = text
        lines = text.split("\n")
        self.entry.move_cursor((len(lines) - 1, len(lines[-1])))
        await self.pilot.pause()

    async def test_skills_form_a_sorted_green_cluster_before_native_matches(self):
        self.app.runtime_info["skills"] = [
            {"name": "review-z", "description": "Last", "argument-hint": "[text...]"},
            {"name": "recap-a", "description": "First"},
            {"name": "other", "description": "Filtered out"},
        ]
        await self.draft("/re")
        self.assertEqual(self.menu.matches, ("/recap-a", "/review-z", "/recap", "/redo", "/resume"))
        console = Console()
        for width in (40, 65):
            for index, command in enumerate(("/recap-a", "/review-z [text...]")):
                label = self.menu.get_option_at_index(index).prompt
                lines = console.render_lines(label, console.options.update(width=width))
                self.assertEqual(len(lines), 1)
                text = "".join(segment.text for segment in lines[0])
                self.assertTrue(text.startswith(command))
                self.assertTrue(text.endswith("Skill"))
                self.assertEqual(len(text), width)
                skill = next(segment for segment in lines[0] if segment.text == "Skill")
                self.assertTrue(skill.style.bold)
                self.assertEqual(skill.style.color.get_truecolor().hex.lower(), ALGAE_MD.lower())
                left = next(segment for segment in lines[0] if segment.text.startswith(command))
                self.assertEqual(left.style.color.get_truecolor().hex.lower(), ALGAE_MD.lower())
        self.assertEqual(self.menu.get_option_at_index(2).prompt.plain, "/recap")
        await self.pilot.press("tab")
        self.assertEqual(self.entry.text, "/recap-a")
        await self.pilot.press("escape")
        self.assertFalse(self.menu.display)
        self.assertTrue(self.entry.has_focus)
        self.assertEqual(self.entry.text, "/recap-a")

    async def test_menu_is_borderless_and_preview_has_horizontal_separators(self):
        await self.draft("/")
        await self.pilot.press("down")
        details = self.app.query_one(SkillDetails)
        self.assertEqual(self.menu.styles.border.spacing.width, 0)
        self.assertEqual(self.menu.styles.border.spacing.height, 0)
        self.assertEqual(details.styles.border.spacing.top, 1)
        self.assertEqual(details.styles.border_top[1], details.styles.border_bottom[1])
        self.assertEqual(details.styles.border.spacing.left, 0)
        self.assertEqual(details.styles.border.spacing.right, 0)
        self.assertEqual(details.styles.border.spacing.bottom, 1)
        self.assertEqual(details.region.bottom, self.menu.region.y)

    async def test_typing_slash_then_prefix_filters_without_selecting(self):
        self.assertFalse(self.menu.display)
        await self.pilot.press("/")
        self.assertTrue(self.menu.display)
        self.assertEqual(self.menu.matches, COMMANDS)
        self.assertEqual([option.id for option in self.menu.options], list(COMMANDS))
        self.assertIsNone(self.menu.highlighted)
        await self.pilot.press("m", "o")
        self.assertEqual(self.menu.matches, ("/model",))
        self.assertIsNone(self.menu.highlighted)
        self.assertTrue(self.entry.has_focus)
        self.app.client.send.assert_not_awaited()

    async def test_selected_commands_have_a_compact_bold_preview_without_execution(self):
        details = self.app.query_one(SkillDetails)
        await self.draft("/")
        self.assertFalse(details.display)
        for command in COMMANDS:
            await self.pilot.press("down")
            self.assertTrue(details.display)
            self.assertTrue(details.has_class("command-preview"))
            title = details.query_one("#skill-name", Static).content
            self.assertEqual(title.plain, command.removeprefix("/"))
            self.assertEqual(title.style, f"bold {MISC_MD}")
            self.assertEqual(self.menu.get_option(command).prompt.style, MISC_MD)
            self.assertEqual(details.query_one("#skill-body", Static).content.plain,
                             COMMAND_DESCRIPTIONS[command])
            self.assertLess(details.size.height, self.app.screen.size.height // 2)
            self.assertTrue(self.entry.has_focus)
        self.app.client.send.assert_not_awaited()
        await self.draft("ordinary text")
        self.assertFalse(details.display)

    async def test_switching_preview_between_commands_and_skills_resets_layout(self):
        details = self.app.query_one(SkillDetails)
        await self.draft("/model")
        await self.pilot.press("down")
        self.assertTrue(details.has_class("command-preview"))
        details.show_skill({"name": "example", "description": "Example skill", "source": "workspace"})
        await self.pilot.pause()
        self.assertFalse(details.has_class("command-preview"))
        self.assertEqual(details.query_one("#skill-origin", Static).content.plain, "[workspace]")
        self.menu.update_details()
        await self.pilot.pause()
        self.assertTrue(details.has_class("command-preview"))
        self.assertEqual(str(details.query_one("#skill-origin", Static).content), "")
        self.menu.dismiss(self.entry.text)
        self.assertFalse(details.display)

    async def test_arrows_choose_a_suggestion_and_enter_executes_it(self):
        await self.draft("/")
        await self.pilot.press("down")
        self.assertEqual(self.menu.selected_command, "/btw")
        await self.pilot.press("down")
        self.assertEqual(self.menu.selected_command, "/compact")
        await self.pilot.press("down")
        self.assertEqual(self.menu.selected_command, "/connect")
        await self.pilot.press("up")
        self.assertEqual(self.menu.selected_command, "/compact")
        await self.pilot.press("down", "down")
        self.assertEqual(self.menu.selected_command, "/mcps")
        await self.pilot.press("down")
        self.assertEqual(self.menu.selected_command, "/model")
        self.assertEqual(self.entry.text, "/model")
        self.assertTrue(self.entry.has_focus)
        await self.pilot.press("enter")
        self.assertIsNotNone(self.app.model_screen)
        self.assertEqual(self.entry.text, "")
        self.assertFalse(self.menu.display)
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "model.list")

    async def test_up_from_no_selection_chooses_last_command(self):
        await self.draft("/")
        await self.pilot.press("up")
        self.assertEqual(self.menu.selected_command, "/worktree")

    async def test_selected_command_is_real_text_and_keeps_filtered_choices(self):
        await self.draft("/re")
        self.assertEqual(self.entry.suggestion, "")
        await self.pilot.press("tab", "tab", "tab")
        self.assertEqual(self.entry.suggestion, "")
        self.assertEqual(self.entry.text, "/resume")
        self.assertEqual(self.entry.cursor_location, (0, 7))
        self.assertEqual(self.menu.matches, ('/recap', '/redo', '/resume'))
        self.assertTrue(self.menu.display)
        await self.pilot.pause()
        line = self.entry.render_line(0)
        self.assertEqual(line.text.rstrip(), "/resume")
        for segment in line.crop(4, 7):
            self.assertNotEqual(segment.style.color.get_truecolor().hex.lower(), MUTED_COLOR.lower())
        self.assertNotEqual(next(iter(line.crop(0, 3))).style.color.get_truecolor().hex.lower(), MUTED_COLOR.lower())
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("enter")
        self.assertIsNotNone(self.app.resume_screen)
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "session.list")

    async def test_selection_updates_text_and_typing_closes_choices(self):
        await self.draft("/co")
        await self.pilot.press("down")
        self.assertEqual(self.entry.text, "/compact")
        await self.pilot.press("tab")
        self.assertEqual(self.entry.text, "/connect")
        await self.pilot.press("up")
        self.assertEqual(self.entry.text, "/compact")
        await self.pilot.press("space")
        self.assertEqual(self.entry.text, "/compact ")
        self.assertEqual(self.entry.suggestion, "")
        self.assertFalse(self.menu.display)

    async def test_deleting_selected_text_closes_menu_and_undo_restores_prefix(self):
        await self.draft('/co')
        await self.pilot.press('tab')
        self.assertEqual(self.entry.text, '/compact')
        self.entry.action_undo()
        await self.pilot.pause()
        self.assertEqual(self.entry.text, '/co')
        await self.pilot.press('tab', 'backspace')
        self.assertEqual(self.entry.text, '/compac')
        self.assertFalse(self.menu.display)

    async def test_batched_prefix_tab_and_space_preserve_event_order(self):
        for event in XTermParser().feed('/co\t argument'):
            self.app.post_message(event)
        await self.pilot.pause()
        self.assertEqual(self.entry.text, '/compact argument')
        self.assertFalse(self.menu.display)
        self.app.client.send.assert_not_awaited()

    async def test_cursor_keys_close_choices_without_replacing_text_again(self):
        await self.draft("/re")
        await self.pilot.press("tab", "tab", "tab", "right")
        self.assertEqual(self.entry.text, "/resume")
        self.assertEqual(self.entry.suggestion, "")
        self.assertFalse(self.menu.display)
        await self.pilot.press("left")
        self.assertEqual(self.entry.suggestion, "")
        await self.pilot.press("right")
        self.assertEqual(self.entry.suggestion, "")
        await self.pilot.press("shift+left")
        self.assertEqual(self.entry.suggestion, "")
        self.assertEqual(self.entry.selected_text, "e")
        await self.pilot.press("shift+right")
        self.assertEqual(self.entry.text, "/resume")

    async def test_preview_clears_on_blur_disable_and_nonmatching_draft(self):
        await self.draft("/re")
        await self.pilot.press("tab")
        self.entry.blur()
        await self.pilot.pause()
        self.assertEqual(self.entry.suggestion, "")
        self.entry.focus()
        await self.pilot.pause()
        self.assertEqual(self.entry.suggestion, "")
        await self.pilot.press("tab")
        self.entry.disabled = True
        await self.pilot.pause()
        self.assertEqual(self.entry.suggestion, "")
        self.entry.disabled = False
        self.entry.focus()
        await self.draft("/unknown")
        self.assertEqual(self.entry.suggestion, "")

    async def test_mouse_selection_inserts_command_and_keeps_leading_whitespace(self):
        await self.draft("  /co")
        await self.pilot.click(self.menu, offset=(3, 1))
        self.assertEqual(self.entry.suggestion, "")
        self.assertEqual(self.entry.text, "  /connect")
        self.assertTrue(self.entry.has_focus)
        self.assertEqual(self.entry.render_line(0).text.rstrip(), "  /connect")
        await self.draft("/connect")
        await self.pilot.press("tab")
        self.assertEqual(self.entry.suggestion, "")

    async def test_tab_cycles_real_text_without_changing_mode_or_choices(self):
        await self.draft("/")
        mode = self.app.mode
        self.assertIsNone(self.menu.selected_command)
        for command in (*COMMANDS, *COMMANDS):
            await self.pilot.press("tab")
            self.assertEqual(self.menu.selected_command, command)
            self.assertEqual(self.entry.text, command)
            self.assertEqual(self.menu.matches, COMMANDS)
            self.assertTrue(self.entry.has_focus)
            self.assertEqual(self.app.mode, mode)
        self.app.client.send.assert_not_awaited()

    async def test_shift_tab_selects_last_and_cycles_backwards(self):
        await self.draft('/')
        mode = self.app.mode
        for command in (*reversed(COMMANDS), *reversed(COMMANDS)):
            await self.pilot.press('shift+tab')
            self.assertEqual(self.menu.selected_command, command)
            self.assertEqual(self.entry.text, command)
            self.assertTrue(self.entry.has_focus)
            self.assertEqual(self.app.mode, mode)
        await self.pilot.press('tab')
        self.assertEqual(self.menu.selected_command, COMMANDS[1])
        self.app.client.send.assert_not_awaited()

    async def test_shift_tab_handles_filtered_single_and_stale_suggestions(self):
        await self.draft('/re')
        await self.pilot.press('shift+tab')
        self.assertEqual(self.entry.text, '/resume')
        await self.pilot.press('shift+tab')
        self.assertEqual(self.entry.text, '/redo')
        await self.pilot.press('shift+tab')
        self.assertEqual(self.entry.text, '/recap')
        self.entry.text = '/co'
        self.entry.move_cursor(self.entry.document.end)
        self.app.action_tab(-1)
        self.assertEqual(self.menu.selected_command, '/connect')
        await self.draft('/mo')
        await self.pilot.press('shift+tab', 'shift+tab')
        self.assertEqual(self.menu.selected_command, '/model')
        self.assertTrue(self.entry.has_focus)

    async def test_shift_tab_without_suggestions_preserves_mode_and_moves_focus(self):
        await self.draft('Plain text')
        mode = self.app.mode
        with patch.object(self.app.screen, 'focus_previous') as previous:
            await self.pilot.press('shift+tab')
            previous.assert_called_once()
        self.assertEqual(self.app.mode, mode)
        self.assertEqual(self.entry.text, 'Plain text')
        self.app.client.send.assert_not_awaited()

    async def test_tab_cycles_filtered_choices_and_enter_executes(self):
        await self.draft("/co")
        await self.pilot.press("tab")
        self.assertEqual(self.menu.selected_command, "/compact")
        await self.pilot.press("tab")
        self.assertEqual(self.menu.selected_command, "/connect")
        await self.pilot.press("tab")
        self.assertEqual(self.menu.selected_command, "/compact")
        await self.draft("/mo")
        self.assertIsNone(self.menu.selected_command)
        await self.pilot.press("tab", "tab")
        self.assertEqual(self.menu.selected_command, "/model")
        await self.pilot.press("enter")
        self.assertIsNotNone(self.app.model_screen)
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "model.list")

    async def test_tab_continues_existing_selection_and_refreshes_stale_suggestions(self):
        await self.draft("/")
        await self.pilot.press("up", "tab")
        self.assertEqual(self.menu.selected_command, COMMANDS[0])
        await self.pilot.press("down", "tab")
        self.assertEqual(self.menu.selected_command, COMMANDS[2])
        # Act before the asynchronous TextArea.Changed refresh.
        self.entry.text = "/co"
        self.entry.move_cursor(self.entry.document.end)
        self.app.action_tab()
        self.assertEqual(self.menu.selected_command, "/compact")
        self.app.client.send.assert_not_awaited()

    async def test_tab_keeps_mode_shortcut_when_no_command_list_is_visible(self):
        for draft in ("plain text", "/unknown", "/model argument", ""):
            with self.subTest(draft=draft):
                await self.draft(draft)
                self.assertFalse(self.menu.display)
                mode = self.app.mode
                await self.pilot.press("tab")
                await self.app.workers.wait_for_complete()
                self.assertNotEqual(self.app.mode, mode)
                self.assertEqual(self.entry.text, draft)
                self.assertEqual(self.app.client.send.call_args.args[0]["type"], "mode.set")

    async def test_single_match_requires_explicit_selection(self):
        await self.draft("/mo")
        with patch.object(self.app, "notify") as notify:
            await self.pilot.press("enter")
            self.assertIn("Unknown command: /mo", notify.call_args.args[0])
        self.assertIsNone(self.app.model_screen)
        self.assertEqual(self.entry.text, "/mo")
        self.app.client.send.assert_not_awaited()
        await self.draft('/mod')
        await self.pilot.press("down", "enter")
        self.assertIsNotNone(self.app.model_screen)
        self.assertFalse(self.menu.display)

    async def test_bare_slash_and_unknown_commands_report_errors_without_model_calls(self):
        for text in ("/", "/does-not-exist", "/MODEL"):
            with self.subTest(text=text):
                await self.draft(text)
                with patch.object(self.app, "notify") as notify:
                    await self.pilot.press("enter")
                    notify.assert_called_once()
                    self.assertIn("Unknown command:", notify.call_args.args[0])
                    self.assertEqual(notify.call_args.kwargs["severity"], "error")
                self.assertEqual(self.entry.text, text)
                self.assertIsNone(self.app.pending_turn)
                self.app.client.send.assert_not_awaited()

    async def test_complete_command_executes_with_no_selected_suggestion(self):
        await self.draft("/new")
        self.assertIsNone(self.menu.selected_command)
        await self.pilot.press("enter")
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "session.new")
        self.assertFalse(self.menu.display)

    async def test_unknown_command_error_is_visible_and_literal(self):
        await self.draft("/missing[bold]")
        await self.pilot.press("enter")
        await self.pilot.pause()
        toast = self.app.query_one("Toast")
        self.assertTrue(toast.visible)
        self.assertIn("Unknown command: /missing[bold]", toast.render().plain)
        self.assertTrue(self.entry.has_focus)
        self.app.client.send.assert_not_awaited()

    async def test_unsupported_arguments_are_not_sent_as_a_chat_message(self):
        await self.draft("/model extra")
        self.assertFalse(self.menu.display)
        with patch.object(self.app, "notify") as notify:
            await self.pilot.press("enter")
            self.assertIn("does not accept arguments", notify.call_args.args[0])
        self.app.client.send.assert_not_awaited()
        self.assertIsNone(self.app.model_screen)

    async def test_changing_prefix_resets_selection_even_when_matches_are_unchanged(self):
        await self.draft("/mo")
        await self.pilot.press("down")
        self.assertEqual(self.menu.selected_command, "/model")
        await self.pilot.press("o")
        self.assertFalse(self.menu.display)
        self.assertEqual(self.entry.text, '/modelo')
        self.assertIsNone(self.menu.selected_command)
        await self.draft("/")
        await self.pilot.press("down")
        # Submit before TextArea.Changed has had a chance to refresh the menu.
        self.entry.text = "/unknown"
        with patch.object(self.app, "notify") as notify:
            self.entry.action_submit()
            await self.pilot.pause()
            self.assertIn("Unknown command: /unknown", notify.call_args.args[0])
        self.app.client.send.assert_not_awaited()

    async def test_plain_messages_and_multiline_cursor_navigation_are_unchanged(self):
        await self.draft("First line\nSecond line")
        with patch.object(self.menu, "set_options", wraps=self.menu.set_options) as rebuild:
            await self.pilot.press("up")
            self.assertEqual(self.entry.cursor_location[0], 0)
            await self.pilot.press("down")
            self.assertEqual(self.entry.cursor_location[0], 1)
            rebuild.assert_not_called()
        await self.draft("Please explain /model")
        self.assertFalse(self.menu.display)
        await self.pilot.press("enter")
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "message.send")
        self.assertEqual(self.app.client.send.call_args.args[0]["text"], "Please explain /model")

    async def test_mouse_selection_does_not_take_focus_or_execute_until_enter(self):
        await self.draft("/")
        await self.pilot.click(self.menu, offset=(3, 2))
        self.assertEqual(self.menu.selected_command, "/connect")
        self.assertTrue(self.entry.has_focus)
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("enter")
        self.assertIsNotNone(self.app.connect_screen)
        self.assertFalse(self.menu.display)

    async def test_disabled_input_hides_suggestions_and_refocus_starts_unselected(self):
        await self.draft("/mo")
        await self.pilot.press("down")
        self.entry.disabled = True
        await self.pilot.pause()
        self.assertFalse(self.menu.display)
        self.entry.disabled = False
        self.entry.focus()
        await self.pilot.pause()
        self.assertTrue(self.menu.display)
        self.assertIsNone(self.menu.highlighted)

    async def test_new_command_can_be_selected_while_busy(self):
        self.app.busy = True
        await self.draft("/n")
        await self.pilot.press("down", "enter")
        self.assertEqual(self.entry.text, "/new")
        self.app.client.send.assert_awaited_once()
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "session.new")
