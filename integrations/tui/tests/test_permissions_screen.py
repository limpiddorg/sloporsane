"""AI Check keeps model, threshold and masked credentials in a draft until Save."""

import unittest
from unittest.mock import AsyncMock, patch

from textual.widgets import Button, Input, OptionList, Static

from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput
from sos_tui.permissions_screen import DESCRIPTIONS
from sos_tui.decision_model_screen import DecisionModelScreen
from dialog_fixture import row_text


LEGACY = {"protocol": "typesafe", "endpoint": "https://openrouter.ai/api/alpha/decisions",
          "model": "typesafe/jev-1.13", "has_key": True}


class PermissionsScreenTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 35)))
        self.app.connected = True
        self.app.runtime_info.update(permission_confidence=80, permission_check_version=1, permission_model=dict(LEGACY))
        self.app.client.send = AsyncMock()
        await self.open_permissions()

    async def open_permissions(self):
        entry = self.app.query_one(MessageInput)
        entry.disabled = False
        entry.text = "/permissions"
        entry.focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.screen = self.app.permissions_screen
        self.assertIsNotNone(self.screen)

    async def tune(self):
        await self.pilot.press("down", "enter")
        self.app.client.send.assert_not_awaited()
        return self.screen.query_one("#confidence-value", Input)

    async def change_model(self):
        await self.tune()
        await self.pilot.click("#permissions-change-model")
        self.assertIsInstance(self.app.screen, DecisionModelScreen)
        return self.app.screen

    async def choose_openai(self):
        child = await self.change_model()
        await self.pilot.press("up", "enter")
        return child

    async def fill_openai(self):
        child = await self.choose_openai()
        child.query_one("#decision-model", Input).value = "my-decision-model"
        child.query_one("#decision-key", Input).value = "candidate-key"
        await self.pilot.click("#decision-use")
        await self.pilot.pause()
        return child

    async def acknowledge(self):
        request = self.app.client.send.call_args.args[0]
        self.app.handle_event({"type": "permissions.selected", "requestId": request["requestId"],
                               "mode": request["mode"], "confidence": request.get("confidence", 80)})
        await self.pilot.pause()

    async def test_modes_and_danger_keep_existing_immediate_selection(self):
        listing = self.screen.query_one(OptionList)
        self.assertEqual(listing.option_count, 3)
        self.assertIn("AI Check", row_text(listing.get_option_at_index(1).prompt))
        self.assertNotIn("auto", row_text(listing.get_option_at_index(1).prompt))
        self.assertTrue(row_text(listing.get_option_at_index(0).prompt).endswith("Current"))
        await self.pilot.press("down", "down")
        self.assertEqual(str(self.screen.query_one("#permissions-description", Static).content), DESCRIPTIONS["danger"])
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["mode"], "danger")
        await self.acknowledge()
        self.assertIsNone(self.app.permissions_screen)

    async def test_ai_check_is_draft_until_save_even_with_saved_key(self):
        field = await self.tune()
        self.assertEqual(self.screen.current, "ask")
        self.assertTrue(self.screen.query_one("#permissions-confidence").display)
        description = self.screen.query_one("#ai-check-description", Static)
        self.assertEqual(str(description.content), "An AI checks whether a command can run safely.")
        self.assertGreater(description.region.height, 0)
        self.assertLessEqual(description.region.bottom, self.screen.query_one("#permissions-model").region.y)
        self.assertEqual(str(self.screen.query_one("#permissions-model", Static).content),
                         "API: TypeSafe\nModel: typesafe/jev-1.13")
        self.assertFalse(self.screen.query("#confidence-description"))
        field.value = "95"
        await self.pilot.press("escape")
        self.app.client.send.assert_not_awaited()
        self.assertIsNone(self.app.permissions_screen)

    async def test_threshold_arrows_and_versioned_save_close_only_after_ack(self):
        field = await self.tune()
        field.focus()
        field.value = "95"
        await self.pilot.press("left", "left", "right")
        self.assertEqual(field.value, "94")
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["confidence"], 94)
        self.assertEqual(request["check_version"], 1)
        self.assertEqual(request["model_config"], {k: v for k, v in LEGACY.items() if k != "has_key"})
        self.assertEqual(request["key"], "")
        self.assertEqual(self.screen.current, "ask")
        self.assertTrue(self.screen.selecting)
        await self.acknowledge()
        self.assertIsNone(self.app.permissions_screen)
        self.assertTrue(self.app.query_one(MessageInput).has_focus)

    async def test_invalid_threshold_and_zero_warning(self):
        field = await self.tune()
        for value in ("", "101", "999"):
            field.value = value
            field.focus()
            await self.pilot.press("enter")
            self.assertIn("between 0 and 100", str(self.screen.query_one("#permissions-status", Static).content))
            self.app.client.send.assert_not_awaited()
        field.value = "0"
        self.screen.has_saved_key = False
        await self.pilot.pause()
        self.assertTrue(self.screen.query_one("#confidence-warning").has_class("danger"))
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["confidence"], 0)

    async def test_no_key_opens_model_selection_only_on_save(self):
        self.screen.has_saved_key = False
        self.screen.update_model_label()
        await self.tune()
        self.assertFalse(self.screen.query_one("#permissions-model").display)
        self.assertGreater(self.screen.query_one("#ai-check-description").region.height, 0)
        self.assertEqual(str(self.screen.query_one("#permissions-model", Static).content), "")
        self.assertIs(self.app.screen, self.screen)
        await self.pilot.press("enter")
        self.assertIsInstance(self.app.screen, DecisionModelScreen)
        self.app.client.send.assert_not_awaited()

    async def test_unconfigured_model_stays_hidden_until_a_model_is_chosen(self):
        await self.pilot.press("escape")
        self.app.runtime_info.pop("permission_model", None)
        self.app.runtime_info["has_permission_key"] = False
        await self.open_permissions()
        await self.tune()
        self.assertFalse(self.screen.query_one("#permissions-model").display)
        await self.pilot.click("#permissions-change-model")
        child = self.app.screen
        self.assertIsInstance(child, DecisionModelScreen)
        await self.pilot.press("up", "enter")
        child.query_one("#decision-model", Input).value = "custom-decision-id"
        child.query_one("#decision-key", Input).value = "candidate-key"
        await self.pilot.click("#decision-use")
        label = self.screen.query_one("#permissions-model", Static)
        self.assertTrue(label.display)
        self.assertEqual(str(label.content), "API: OpenAI Decisions\nModel: custom-decision-id")
        self.app.client.send.assert_not_awaited()

    async def test_browsing_modes_preserves_threshold_draft(self):
        field = await self.tune()
        field.value = "73"
        await self.pilot.press("up")
        self.assertFalse(self.screen.query_one("#permissions-confidence").display)
        await self.pilot.press("down")
        self.assertEqual(field.value, "73")
        self.assertTrue(self.screen.query_one("#confidence-save").has_class("selected-action"))
        self.app.client.send.assert_not_awaited()

    async def test_failure_keeps_draft_editable(self):
        await self.fill_openai()
        field = self.screen.query_one("#confidence-value", Input)
        field.value = "91"
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.app.handle_event({"type": "request.failed", "requestId": request["requestId"], "error": "Cannot save"})
        await self.pilot.pause()
        self.assertFalse(self.screen.selecting)
        self.assertEqual(field.value, "91")
        self.assertEqual(self.screen.current, "ask")
        self.assertIn("Cannot save", str(self.screen.query_one("#permissions-status", Static).content))
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["key"], "candidate-key")
        await self.acknowledge()
        self.assertEqual(self.screen.pending_key, "")

    async def test_old_daemon_cannot_silently_ignore_model_or_probability(self):
        self.screen.check_supported = False
        await self.pilot.press("down", "enter")
        self.assertFalse(self.screen.query_one("#permissions-confidence").display)
        self.assertIn("restart", str(self.screen.query_one("#permissions-status", Static).content))
        self.app.client.send.assert_not_awaited()

    async def test_current_selection_comes_from_runtime_info(self):
        await self.pilot.press("escape")
        self.app.runtime_info.update(permission_mode="auto", permission_confidence=61)
        await self.open_permissions()
        self.assertEqual(self.screen.query_one("#confidence-value", Input).value, "61")
        self.assertTrue(self.screen.query_one("#confidence-save").has_focus)
        self.assertTrue(row_text(self.screen.query_one(OptionList).get_option_at_index(1).prompt).endswith("Current"))

    async def test_api_picker_precedes_fields_and_preserves_legacy_destination(self):
        child = await self.change_model()
        listing = child.query_one(OptionList)
        self.assertGreaterEqual(listing.content_region.height, 2)
        self.assertEqual(listing.option_count, 2)
        self.assertIn("OpenAI Decisions", row_text(listing.get_option_at_index(0).prompt))
        self.assertIn("TypeSafe", row_text(listing.get_option_at_index(1).prompt))
        self.assertFalse(child.query_one("#decision-fields").display)
        await self.pilot.press("enter")
        self.assertEqual(child.query_one("#decision-endpoint", Input).value, LEGACY["endpoint"])
        self.assertEqual(child.query_one("#decision-model", Input).value, LEGACY["model"])
        key = child.query_one("#decision-key", Input)
        self.assertTrue(key.password)
        self.assertEqual(key.value, "")  # The daemon never returns a saved credential.
        self.assertIn("Leave blank", str(child.query_one("#decision-key-hint", Static).content))

    async def test_model_popup_updates_only_parent_draft_then_parent_save_sends(self):
        child = await self.choose_openai()
        key_field = child.query_one("#decision-key", Input)
        child.query_one("#decision-model", Input).value = "my-decision-model"
        key_field.value = "candidate-key"
        await self.pilot.click("#decision-use")
        await self.pilot.pause()
        self.assertEqual(key_field.value, "")
        self.assertEqual(child.initial_key, "")
        self.app.client.send.assert_not_awaited()
        self.assertEqual(self.screen.model["protocol"], "openai_decisions")
        self.assertEqual(str(self.screen.query_one("#permissions-model", Static).content),
                         "API: OpenAI Decisions\nModel: my-decision-model")
        self.assertEqual(self.screen.model["endpoint"], "https://api.openai.com/v1/decisions")
        self.assertEqual(self.screen.current, "ask")
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["key"], "candidate-key")
        self.assertEqual(request["model_config"]["model"], "my-decision-model")
        self.assertNotIn("text", request)
        await self.acknowledge()

    async def test_closing_parent_discards_model_key_and_threshold_draft(self):
        await self.fill_openai()
        self.screen.query_one("#confidence-value", Input).value = "99"
        await self.pilot.press("escape")
        self.assertEqual(self.screen.pending_key, "")
        self.app.client.send.assert_not_awaited()
        self.assertEqual(self.app.runtime_info["permission_model"], LEGACY)

    async def test_cancel_child_discards_only_child_changes(self):
        child = await self.choose_openai()
        child.query_one("#decision-key", Input).value = "discarded"
        await self.pilot.press("escape")
        self.assertEqual(child.query_one("#decision-key", Input).value, "")
        self.assertTrue(child.query_one(OptionList).display)
        await self.pilot.press("escape")
        self.assertIs(self.app.screen, self.screen)
        self.assertEqual(self.screen.model["endpoint"], LEGACY["endpoint"])
        self.assertEqual(self.screen.pending_key, "")
        self.app.client.send.assert_not_awaited()

    async def test_same_destination_model_change_can_reuse_saved_key(self):
        child = await self.change_model()
        await self.pilot.press("enter")
        child.query_one("#decision-model", Input).value = "another-jev"
        await self.pilot.click("#decision-use")
        self.assertEqual(self.screen.model["model"], "another-jev")
        self.assertTrue(self.screen.can_reuse_key())
        self.app.client.send.assert_not_awaited()

    async def test_new_endpoint_cannot_reuse_saved_key(self):
        child = await self.change_model()
        await self.pilot.press("enter")
        child.query_one("#decision-endpoint", Input).value = "https://api.typesafe.ai/v1/systemone"
        await self.pilot.click("#decision-use")
        self.assertIs(self.app.screen, child)
        self.assertIn("Enter an API key", str(child.query_one("#decision-status", Static).content))
        self.assertTrue(child.query_one("#decision-key").has_focus)
        self.app.client.send.assert_not_awaited()

    async def test_endpoint_model_and_key_validation_remain_local(self):
        child = await self.choose_openai()
        for endpoint in ("http://example.com/api", "https://user:secret@example.com/api", "https://example.com/#fragment"):
            child.query_one("#decision-endpoint", Input).value = endpoint
            child.query_one("#decision-key").focus()
            await self.pilot.press("enter")
            self.assertIn("Use HTTPS", str(child.query_one("#decision-status", Static).content))
        child.query_one("#decision-endpoint", Input).value = "http://127.0.0.1:8080/v1/decisions"
        child.query_one("#decision-key").focus()
        await self.pilot.press("enter")
        self.assertIn("model ID", str(child.query_one("#decision-status", Static).content))
        child.query_one("#decision-model", Input).value = "local-model"
        child.query_one("#decision-key", Input).value = "bad key"
        child.query_one("#decision-key").focus()
        await self.pilot.press("enter")
        self.assertIn("without whitespace", str(child.query_one("#decision-status", Static).content))
        self.app.client.send.assert_not_awaited()

    async def test_actions_and_tab_target_current_popup_not_parent(self):
        await self.tune()
        await self.pilot.press("right")
        self.assertTrue(self.screen.query_one("#permissions-change-model").has_focus)
        await self.pilot.press("enter", "enter")
        child = self.app.screen
        field = child.query_one("#decision-endpoint", Input)
        self.assertTrue(field.has_focus)
        field.cursor_position = 3
        await self.pilot.press("left")
        self.assertEqual(field.cursor_position, 2)
        await self.pilot.press("tab")
        self.assertTrue(child.query_one("#decision-model").has_focus)
        await self.pilot.press("shift+tab")
        self.assertTrue(field.has_focus)

    async def test_small_terminal_save_and_popup_fields_are_reachable(self):
        await self.pilot.resize_terminal(80, 24)
        await self.tune()
        save = self.screen.query_one("#confidence-save", Button)
        save.focus()
        await self.pilot.pause()
        self.assertLessEqual(save.region.bottom, self.screen.query_one(".popup-dialog").content_region.bottom)
        await self.pilot.press("right", "enter")
        child = self.app.screen
        listing = child.query_one(OptionList)
        self.assertGreaterEqual(listing.content_region.height, 2)
        await self.pilot.click("#decision-protocols", offset=(2, 1))
        self.assertTrue(child.query_one("#decision-fields").display)
        self.assertEqual(child.protocol, "openai_decisions")
        child.query_one("#decision-key").focus()
        await self.pilot.pause()
        self.assertGreater(child.query_one("#decision-key").region.height, 0)
