"""Permission selection uses the standard modal and never sends keys as chat."""

import unittest
from unittest.mock import AsyncMock, patch

from textual.widgets import Button, Input, OptionList, Static

from sos_tui.app import SosApp
from sos_tui.config import BOLD_MD, DEPTH_MD, TEXT_COLOR
from sos_tui.message_input import MessageInput
from sos_tui.permissions_screen import DESCRIPTIONS
from dialog_fixture import row_text


class PermissionsScreenTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 35)))
        self.app.connected = True
        self.app.runtime_info["permission_confidence"] = 80
        self.app.client.send = AsyncMock()
        entry = self.app.query_one(MessageInput)
        entry.disabled = False
        entry.text = "/permissions"
        entry.focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.screen = self.app.permissions_screen
        self.assertIsNotNone(self.screen)
        self.app.client.send.assert_not_awaited()
        self.assertEqual(self.screen.query_one(OptionList).option_count, 3)
        self.assertFalse(self.screen.query_one(OptionList).disabled)

    async def test_mode_header_is_inside_picker_and_hidden_during_key_entry(self):
        picker = self.screen.query_one("#permissions-picker")
        columns = self.screen.query_one("#permissions-columns", Static)
        separator = picker.query_one(".popup-separator")
        listing = self.screen.query_one(OptionList)
        self.assertEqual(str(columns.content), "Mode")
        self.assertEqual(columns.region.y, picker.content_region.y)
        self.assertEqual(separator.region.y, columns.region.bottom)
        self.assertEqual(listing.region.y, separator.region.bottom)
        self.assertNotEqual(columns.styles.background, picker.styles.background)
        await self.pilot.press("down", "enter")
        self.assertFalse(picker.display)
        await self.pilot.press("escape")
        self.assertTrue(picker.display)

    async def test_help_has_one_blank_line_after_description_or_settings(self):
        status = self.screen.query_one("#permissions-status")
        description = self.screen.query_one("#permissions-description")
        for index in (0, 1, 2):
            self.screen.query_one(OptionList).highlighted = index
            await self.pilot.pause()
            self.assertEqual(status.region.y - description.region.bottom, 1)
        self.screen.current = "auto"
        self.screen.confidence_supported = True
        self.screen.show_modes()
        await self.pilot.pause()
        settings = self.screen.query_one("#permissions-confidence")
        self.assertTrue(settings.display)
        self.assertEqual(status.region.y - settings.region.bottom, 1)

    async def test_jev_settings_have_a_short_white_explanation_above_confidence(self):
        self.screen.current = "auto"
        self.screen.show_modes()
        await self.pilot.pause()
        description = self.screen.query_one("#confidence-description", Static)
        heading = self.screen.query_one("#confidence-heading", Static)
        self.assertEqual(description.styles.color.hex.lower(), TEXT_COLOR.lower())
        self.assertLessEqual(description.region.height, 2)
        self.assertEqual(heading.region.y - description.region.bottom, 1)
        self.assertEqual(str(description.content),
                         "AI checks commands before they run. Commands below your confidence threshold are refused.")

    async def test_highlight_explains_danger_and_enter_applies(self):
        options = self.screen.query_one(OptionList)
        self.assertTrue(row_text(options.get_option_at_index(0).prompt).endswith("Current"))
        marker = next(segment for segment in options.render_line(0) if "Current" in segment.text)
        self.assertEqual(marker.style.color.triplet.hex.lower(), BOLD_MD.lower())
        await self.pilot.press("down", "down")
        description = self.screen.query_one("#permissions-description", Static)
        self.assertEqual(str(description.content), DESCRIPTIONS["danger"])
        self.assertTrue(description.has_class("danger"))
        self.assertIn("no checks", DESCRIPTIONS["danger"])
        self.assertIn("choose AI Validation", DESCRIPTIONS["danger"])
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "permissions.select")
        self.assertEqual(request["mode"], "danger")
        self.app.handle_event({"type": "permissions.selected", "requestId": request["requestId"], "mode": "danger"})
        await self.pilot.pause()
        self.assertIsNone(self.app.permissions_screen)

    async def test_auto_key_is_masked_and_escape_keeps_old_mode(self):
        await self.pilot.press("down", "enter")
        self.assertEqual(DESCRIPTIONS["ask"], "You will be asked before any command with side effects")
        self.assertIn('An AI model through OpenRouter', DESCRIPTIONS["auto"])
        self.assertIn("AI Validation (auto)", row_text(self.screen.query_one(OptionList).get_option_at_index(1).prompt))
        self.assertTrue(all(len(text) < 140 for text in DESCRIPTIONS.values()))
        field = self.screen.query_one("#permissions-key", Input)
        self.assertTrue(field.password)
        self.assertTrue(field.display)
        self.assertTrue(field.has_focus)
        background = field.styles.background
        for focused in (False, True):
            self.screen.set_focus(field if focused else None)
            await self.pilot.pause()
            self.assertEqual(field.region.height, 1)
            self.assertEqual(field.styles.border.spacing.width, 0)
            self.assertEqual(field.styles.border.spacing.height, 0)
            self.assertEqual(field.styles.background, background)
        field.value = "candidate-key"
        await self.pilot.press("escape")
        self.assertFalse(field.display)
        self.assertEqual(field.value, "")
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("escape")
        self.assertIsNone(self.app.permissions_screen)

    async def test_auto_submits_dedicated_key_and_preserves_mode_on_error(self):
        await self.pilot.press("down", "enter")
        self.screen.query_one("#permissions-key", Input).value = "candidate-key"
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "permissions.select")
        self.assertEqual(request["key"], "candidate-key")
        self.assertEqual(request["mode"], "auto")
        self.assertNotIn("text", request)
        self.assertEqual(self.screen.query_one("#permissions-key", Input).value, "")
        self.app.handle_event({"type": "request.failed", "requestId": request["requestId"], "error": "Invalid key"})
        await self.pilot.pause()
        self.assertEqual(self.screen.current, "ask")
        self.assertFalse(self.screen.selecting)

    async def test_saved_key_is_used_directly_on_enter(self):
        self.screen.has_saved_key = True
        await self.pilot.press("down")
        self.app.client.send.assert_not_awaited()
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["mode"], "auto")
        self.assertEqual(request["key"], "")
        self.assertNotIn("confidence", request)
        self.app.client.send.assert_awaited_once()
        self.assertFalse(self.screen.entering_key)
        self.assertFalse(self.screen.query_one("#permissions-key").display)
        self.assertTrue(self.screen.query_one(OptionList).display)
        self.assertEqual(self.screen.current, "ask")  # Wait for daemon acknowledgement.
        await self.pilot.press("enter")
        self.app.client.send.assert_awaited_once()

    async def test_current_mode_comes_from_runtime_info_without_a_request(self):
        await self.pilot.press("escape")
        self.app.runtime_info["permission_mode"] = "danger"
        entry = self.app.query_one(MessageInput)
        entry.text = "/permissions"
        entry.focus()
        await self.pilot.press("enter")
        screen = self.app.permissions_screen
        options = screen.query_one(OptionList)
        self.assertEqual(options.highlighted, 2)
        self.assertTrue(row_text(options.get_option_at_index(2).prompt).endswith("Current"))
        self.assertTrue(screen.query_one("#permissions-description").has_class("danger"))
        self.app.client.send.assert_not_awaited()

    async def test_missing_saved_key_error_keeps_key_field_editable(self):
        self.screen.has_saved_key = True
        await self.pilot.press("down", "enter")
        request = self.app.client.send.call_args.args[0]
        self.app.handle_event({"type": "request.failed", "requestId": request["requestId"],
                               "error": "Enter an OpenRouter API key for AI verification first"})
        await self.pilot.pause()
        field = self.screen.query_one("#permissions-key", Input)
        self.assertTrue(field.display)
        self.assertFalse(field.disabled)
        self.assertTrue(field.has_focus)
        self.assertEqual(self.screen.current, "ask")
        self.assertFalse(self.screen.query("#permissions-key-cancel"))
        self.assertIn("Enter an OpenRouter API key", str(self.screen.query_one("#permissions-status", Static).content))

    async def test_blank_key_cannot_be_submitted(self):
        await self.pilot.press("down", "enter", "enter")
        self.app.client.send.assert_not_awaited()
        self.assertFalse(self.screen.query("#permissions-use-saved"))
        self.assertTrue(self.screen.entering_key)

    async def test_cancel_button_returns_to_picker_and_discards_key(self):
        await self.pilot.press("down", "enter")
        self.screen.query_one("#permissions-key", Input).value = "unsaved-key"
        await self.pilot.press("escape")
        self.assertFalse(self.screen.entering_key)
        self.assertTrue(self.screen.query_one(OptionList).display)
        self.assertEqual(self.screen.query_one("#permissions-key", Input).value, "")
        self.app.client.send.assert_not_awaited()

    async def test_new_key_can_replace_saved_key(self):
        await self.reopen_current(confidence=61)
        await self.pilot.click("#permissions-change-key")
        self.assertFalse(self.screen.query("#permissions-use-saved"))
        self.assertTrue(self.screen.query_one("#permissions-key", Input).password)
        self.assertFalse(self.screen.query("#permissions-key-cancel"))
        self.screen.query_one("#permissions-key", Input).value = "replacement-key"
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["key"], "replacement-key")
        await self.acknowledge_auto(confidence=61)
        self.assertIs(self.app.permissions_screen, self.screen)
        self.assertFalse(self.screen.entering_key)
        self.assertTrue(self.screen.query_one("#permissions-confidence").display)

    async def tune(self):
        await self.reopen_current()
        self.app.client.send.assert_not_awaited()
        field = self.screen.query_one("#confidence-value", Input)
        field.focus()
        await self.pilot.pause()
        return field

    async def reopen_current(self, confidence=80, has_key=True):
        await self.pilot.press("escape")
        self.app.runtime_info.update(permission_mode="auto", permission_confidence=confidence, has_permission_key=has_key)
        entry = self.app.query_one(MessageInput)
        entry.text = "/permissions"
        entry.focus()
        await self.pilot.press("enter")
        self.screen = self.app.permissions_screen
        await self.pilot.pause()

    async def acknowledge_auto(self, confidence=80):
        request = self.app.client.send.call_args.args[0]
        self.app.handle_event({"type": "permissions.selected", "requestId": request["requestId"], "mode": "auto",
                               "confidence": confidence, "info": {"permission_mode": "auto",
                               "permission_confidence": confidence, "has_permission_key": True}})
        await self.pilot.pause()

    async def test_confidence_is_visible_only_when_jev_is_current_and_highlighted(self):
        panel = self.screen.query_one("#permissions-confidence")
        self.assertFalse(panel.display)
        await self.pilot.press("down")
        self.assertFalse(panel.display)  # Highlight is not Current.
        await self.reopen_current(confidence=61)
        panel = self.screen.query_one("#permissions-confidence")
        options = self.screen.query_one(OptionList)
        self.assertTrue(panel.display)
        self.assertTrue(options.display)
        self.assertEqual(options.option_count, 3)
        self.assertEqual(self.screen.query_one("#confidence-value", Input).value, "61")
        picker = self.screen.query_one("#permissions-picker")
        self.assertEqual(panel.region.y, picker.region.bottom + picker.styles.margin.bottom)
        self.assertTrue(row_text(options.get_option_at_index(1).prompt).endswith("Current"))
        field = self.screen.query_one("#confidence-value", Input)
        field.value = "73"  # Browsing must not discard an unsaved draft.
        await self.pilot.press("up")
        self.assertFalse(panel.display)
        description = self.screen.query_one("#permissions-description", Static)
        self.assertTrue(description.display)
        self.assertEqual(str(description.content), DESCRIPTIONS["ask"])
        await self.pilot.press("down", "down")
        self.assertFalse(panel.display)
        self.assertTrue(description.display)
        self.assertEqual(str(description.content), DESCRIPTIONS["danger"])
        self.assertTrue(description.has_class("danger"))
        self.assertEqual(self.screen.query_one("#confidence-save").region.height, 0)
        await self.pilot.press("up")
        self.assertTrue(panel.display)
        self.assertFalse(description.display)
        self.assertEqual(field.value, "73")
        self.assertEqual(self.screen.current, "auto")
        self.app.client.send.assert_not_awaited()

    async def test_connecting_jev_returns_to_modes_instead_of_closing(self):
        await self.pilot.press("down", "enter")
        self.screen.query_one("#permissions-key", Input).value = "candidate-key"
        await self.pilot.press("enter")
        self.assertTrue(self.screen.selecting)
        self.assertEqual(self.screen.current, "ask")
        self.assertFalse(self.screen.query_one("#permissions-confidence").display)
        await self.acknowledge_auto(confidence=75)
        self.assertIs(self.app.permissions_screen, self.screen)
        self.assertFalse(self.screen.entering_key)
        self.assertFalse(self.screen.selecting)
        options = self.screen.query_one(OptionList)
        self.assertTrue(options.display)
        self.assertEqual(options.highlighted, 1)
        self.assertTrue(row_text(options.get_option_at_index(1).prompt).endswith("Current"))
        self.assertFalse(row_text(options.get_option_at_index(0).prompt).endswith("Current"))
        self.assertTrue(self.screen.query_one("#permissions-confidence").display)
        self.assertEqual(self.screen.query_one("#confidence-value", Input).value, "75")
        self.assertEqual(self.screen.query_one("#permissions-key", Input).value, "")
        self.app.client.send.assert_awaited_once()

    async def test_saved_key_connection_also_returns_to_modes(self):
        self.screen.has_saved_key = True
        await self.pilot.press("down", "enter")
        await self.acknowledge_auto()
        self.assertIs(self.app.permissions_screen, self.screen)
        self.assertTrue(self.screen.query_one("#permissions-confidence").display)

    async def test_skip_jev_is_removed(self):
        self.assertFalse(self.screen.query("#permissions-skip-jev"))
        await self.pilot.press("down", "enter")
        self.assertFalse(self.screen.query("#permissions-skip-jev"))

    async def test_confidence_arrows_and_exact_value_are_saved(self):
        field = await self.tune()
        self.assertEqual(field.value, "80")
        field.value = "95"
        await self.pilot.press("left", "left", "right")
        self.assertEqual(field.value, "94")
        field.value = "2"
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["confidence"], 2)
        self.assertEqual(request["mode"], "auto")
        self.assertEqual(request["key"], "")
        self.assertEqual(self.screen.confidence, 80)  # Confirmed value changes only on acknowledgement.
        self.assertIs(self.app.permissions_screen, self.screen)
        await self.acknowledge_auto(confidence=2)
        self.assertIsNone(self.app.permissions_screen)
        self.assertEqual(self.screen.confidence, 2)
        self.assertTrue(self.app.query_one(MessageInput).has_focus)

    async def test_save_button_closes_only_after_successful_acknowledgement(self):
        field = await self.tune()
        field.value = "73"
        await self.pilot.click("#confidence-save")
        self.assertIs(self.app.permissions_screen, self.screen)
        self.assertTrue(self.screen.selecting)
        self.assertEqual(self.app.client.send.call_args.args[0]["confidence"], 73)
        await self.acknowledge_auto(confidence=73)
        self.assertIsNone(self.app.permissions_screen)
        self.assertTrue(self.app.query_one(MessageInput).has_focus)

    async def test_invalid_confidence_cannot_be_saved(self):
        field = await self.tune()
        for value in ("", "101", "999"):
            field.value = value
            await self.pilot.press("enter")
            self.assertIn("between 0 and 100", str(self.screen.query_one("#permissions-status", Static).content))
            self.app.client.send.assert_not_awaited()

    async def test_zero_explicitly_disables_jev_without_requiring_credentials(self):
        field = await self.tune()
        field.value = "0"
        await self.pilot.pause()
        self.assertTrue(self.screen.query_one("#confidence-warning").has_class("danger"))
        self.app.client.send.assert_not_awaited()
        await self.pilot.click("#confidence-save")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["confidence"], 0)
        self.assertEqual(request["key"], "")

    async def test_enabling_jev_from_legacy_zero_without_key_requires_credentials(self):
        await self.reopen_current(confidence=0, has_key=False)
        field = self.screen.query_one("#confidence-value", Input)
        field.focus()
        field.value = "80"
        await self.pilot.pause()
        self.assertFalse(self.screen.query_one("#confidence-warning").has_class("danger"))
        await self.pilot.press("enter")
        self.assertTrue(self.screen.entering_key)
        self.assertTrue(self.screen.query_one("#permissions-key").display)
        self.app.client.send.assert_not_awaited()

    async def test_changing_key_can_return_to_current_jev_without_saving(self):
        await self.reopen_current(confidence=61)
        await self.pilot.click("#permissions-change-key")
        self.screen.query_one("#permissions-key", Input).value = "candidate"
        await self.pilot.press("escape")
        self.assertFalse(self.screen.entering_key)
        self.assertEqual(self.screen.query_one("#permissions-key", Input).value, "")
        self.assertTrue(self.screen.query_one("#permissions-confidence").display)
        self.assertEqual(self.screen.query_one("#confidence-value", Input).value, "61")
        self.app.client.send.assert_not_awaited()

    async def test_change_key_cancel_keeps_saved_key_and_current_mode(self):
        await self.reopen_current(confidence=61)
        await self.pilot.click("#permissions-change-key")
        self.screen.query_one("#permissions-key", Input).value = "discarded"
        await self.pilot.press("escape")
        self.assertIs(self.app.permissions_screen, self.screen)
        self.assertEqual(self.screen.current, "auto")
        self.assertTrue(self.screen.has_saved_key)
        self.assertEqual(self.screen.confidence, 61)
        self.assertFalse(self.screen.entering_key)
        self.assertEqual(self.screen.query_one("#permissions-key", Input).value, "")
        self.app.client.send.assert_not_awaited()

    async def test_empty_replacement_does_not_reuse_saved_key(self):
        await self.reopen_current()
        await self.pilot.click("#permissions-change-key")
        await self.pilot.press("enter")
        self.app.client.send.assert_not_awaited()
        self.assertTrue(self.screen.entering_key)
        self.assertNotIn("Use saved", str(self.screen.query_one("#permissions-status", Static).content))

    async def test_saved_confidence_is_restored_from_runtime_info(self):
        await self.reopen_current(confidence=61)
        self.assertTrue(self.screen.query_one("#permissions-confidence").display)
        self.assertTrue(self.screen.query_one("#confidence-save").has_focus)
        await self.pilot.press("enter")
        field = self.screen.query_one("#confidence-value", Input)
        self.assertEqual(field.value, "61")
        self.assertFalse(self.screen.entering_key)
        self.assertEqual(self.app.client.send.call_args.args[0]["confidence"], 61)

    async def test_actions_use_arrows_and_enter_without_consuming_input_arrows(self):
        await self.reopen_current()
        await self.pilot.press("right")
        self.assertTrue(self.screen.query_one("#permissions-change-key").has_focus)
        await self.pilot.press("left")
        self.assertTrue(self.screen.query_one("#confidence-save").has_focus)
        await self.pilot.press("right", "enter")
        field = self.screen.query_one("#permissions-key", Input)
        self.assertTrue(field.has_focus)
        field.value = "sample"
        field.cursor_position = 3
        await self.pilot.press("left")
        self.assertEqual(field.cursor_position, 2)
        await self.pilot.press("escape")
        self.assertTrue(self.screen.query_one("#confidence-save").has_focus)
        await self.pilot.press("up")
        self.assertEqual(self.screen.query_one(OptionList).highlighted, 0)
        self.app.client.send.assert_not_awaited()

    async def test_save_remains_default_when_returning_to_jev(self):
        await self.reopen_current()
        save = self.screen.query_one("#confidence-save", Button)
        change = self.screen.query_one("#permissions-change-key", Button)
        await self.pilot.press("right", "up", "down")
        self.assertTrue(self.screen.query_one(OptionList).has_focus)
        self.assertTrue(save.has_class("selected-action"))
        self.assertFalse(change.has_class("selected-action"))
        self.assertEqual(save.styles.background.hex.lower(), DEPTH_MD.lower())
        self.assertEqual(save.styles.color.hex.lower(), TEXT_COLOR.lower())
        await self.pilot.press("right")
        self.assertTrue(change.has_focus)
        self.assertTrue(change.has_class("selected-action"))
        save.focus()  # Focus via Tab or click updates the selected action too.
        await self.pilot.pause()
        self.assertTrue(save.has_class("selected-action"))
        await self.pilot.press("up", "down")
        self.assertTrue(save.has_class("selected-action"))

    async def test_confidence_can_be_saved_in_a_small_terminal(self):
        await self.pilot.resize_terminal(80, 24)
        await self.reopen_current(confidence=0)
        await self.pilot.pause()
        button = self.screen.query_one("#confidence-save", Button)
        dialog = self.screen.query_one(".popup-dialog")
        button.focus()
        await self.pilot.pause()
        self.assertLessEqual(button.region.bottom, dialog.content_region.bottom)
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["confidence"], 0)

    async def test_confidence_layout_is_compact_with_borderless_value_and_hints_on_right(self):
        for size in ((100, 35), (80, 24)):
            await self.pilot.resize_terminal(*size)
            await self.reopen_current(confidence=100)
            field = self.screen.query_one("#confidence-value", Input)
            heading = self.screen.query_one("#confidence-heading")
            hints = self.screen.query_one("#confidence-hints")
            for focused in (False, True):
                if focused:
                    field.focus()
                await self.pilot.pause()
                self.assertEqual(field.region.height, 1)
                self.assertEqual(field.region.width, 4)
                self.assertEqual(field.content_region, field.region)  # No border or padding, even focused.
                self.assertEqual(field.region.y, heading.region.bottom)
                self.assertEqual(hints.region.y, field.region.y)
                self.assertGreater(hints.region.x, field.region.right)
                self.assertEqual(hints.region.height, 2)
                description = self.screen.query_one("#confidence-description")
                self.assertEqual(self.screen.query_one("#permissions-confidence").region.height,
                                 4 + description.region.height + 1)
            self.assertFalse(self.screen.query("#confidence-presets"))
            self.assertLessEqual(self.screen.query_one("#permissions-status").region.bottom,
                                 self.screen.query_one(".popup-dialog").content_region.bottom)

    async def test_threshold_failure_keeps_modes_and_draft_without_reopening_key(self):
        field = await self.tune()
        field.value = "95"
        await self.pilot.click("#confidence-save")
        request = self.app.client.send.call_args.args[0]
        self.app.handle_event({"type": "request.failed", "requestId": request["requestId"], "error": "Cannot save"})
        await self.pilot.pause()
        self.assertIs(self.app.permissions_screen, self.screen)
        self.assertFalse(self.screen.entering_key)
        self.assertTrue(self.screen.query_one(OptionList).display)
        self.assertTrue(self.screen.query_one("#permissions-confidence").display)
        self.assertEqual(field.value, "95")
        self.assertEqual(self.screen.confidence, 80)
        self.assertTrue(field.has_focus)

    async def test_escape_from_inline_settings_closes_without_saving_draft(self):
        field = await self.tune()
        field.value = "95"
        await self.pilot.press("escape")
        self.assertIsNone(self.app.permissions_screen)
        self.app.client.send.assert_not_awaited()

    async def test_old_daemon_cannot_silently_ignore_a_confidence_setting(self):
        self.screen.confidence_supported = False
        await self.pilot.press("down", "enter")
        self.assertFalse(self.screen.entering_key)
        self.assertIn("Update and restart", str(self.screen.query_one("#permissions-status", Static).content))
        self.app.client.send.assert_not_awaited()
