"""Custom provider creation and action navigation, without real credentials."""

import unittest
from unittest.mock import AsyncMock, patch

from textual.widgets import Button, Input, OptionList, Static

from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput
from sos_tui.model_screen import ModelScreen
from dialog_fixture import row_text


PROVIDERS = [
    {"id": "openai", "connected": False, "auth_method": None},
    {"id": "openrouter", "connected": False, "auth_method": None},
    {"id": "custom-local", "name": "Local server", "custom": True, "connected": True,
     "auth_method": "api", "base_url": "http://localhost:1234/v1"},
    {"id": "custom-office", "name": "Office", "custom": True, "connected": True,
     "auth_method": "api", "base_url": "https://office.test/api"},
]


class CustomProviderScreenTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 40)))
        self.app.connected = True
        self.app.client.send = AsyncMock()
        await self.app.on_message_input_submitted(MessageInput.Submitted("/connect"))
        await self.pilot.pause()
        self.screen = self.app.connect_screen
        self.reply({"type": "provider.listed", "providers": PROVIDERS})
        await self.pilot.pause()

    def reply(self, payload):
        request = self.app.client.send.call_args.args[0]
        self.app.handle_event({"requestId": request["requestId"], **payload})

    async def open_form(self):
        options = self.screen.query_one("#connect-providers", OptionList)
        options.highlighted = options.option_count - 1
        await self.pilot.press("enter")
        await self.pilot.pause()

    async def test_creation_contract_masking_and_return_to_named_list(self):
        options = self.screen.query_one("#connect-providers", OptionList)
        self.assertEqual(options.option_count, 5)
        self.assertEqual(row_text(options.get_option_at_index(4).prompt), "Custom provider")
        await self.open_form()
        name = self.screen.query_one("#connect-name", Input)
        url = self.screen.query_one("#connect-url", Input)
        key = self.screen.query_one("#connect-key", Input)
        self.assertTrue(name.has_focus)
        self.assertTrue(key.password)
        contract = str(self.screen.query_one("#connect-details", Static).content)
        self.assertEqual(contract, "OpenAI-compatible Chat Completions API\nthe /models endpoint discovers models")
        details = self.screen.query_one("#connect-details")
        help_text = self.screen.query_one("#connect-help")
        self.assertEqual(help_text.region.y - details.region.bottom, 1)
        self.assertNotIn("←/→", str(self.screen.query_one("#connect-help", Static).content))
        name.value = "My server"
        url.value = "http://localhost:8000/v1"
        key.value = "fake-secret"
        key.focus()
        await self.pilot.press("enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "provider.create")
        self.assertEqual(request["name"], "My server")
        self.assertEqual(request["base_url"], url.value)
        self.assertEqual(request["key"], "fake-secret")
        self.assertNotIn("text", request)
        self.assertEqual(key.value, "")
        self.assertTrue(options.disabled)
        created = {"id": "custom-created", "name": "My server", "custom": True, "connected": True,
                   "base_url": url.value, "auth_method": "api"}
        self.reply({"type": "provider.listed", "providers": [*PROVIDERS, created], "created_provider": "custom-created"})
        await self.pilot.pause()
        self.assertEqual(self.screen.highlighted_provider(), "custom-created")
        self.assertFalse(name.display)
        self.assertFalse(url.display)
        self.assertFalse(key.display)
        await self.pilot.press("enter")
        self.assertTrue(self.screen.query_one("#connect-custom", Button).has_focus)
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_args.args[0]["type"], "provider.select")
        self.assertEqual(self.app.client.send.call_args.args[0]["provider"], "custom-created")

    async def test_connect_is_default_and_left_right_select_delete(self):
        options = self.screen.query_one("#connect-providers", OptionList)
        options.highlighted = 2
        await self.pilot.pause()
        connect = self.screen.query_one("#connect-custom", Button)
        delete = self.screen.query_one("#connect-delete", Button)
        self.assertTrue(connect.display)
        self.assertTrue(delete.display)
        self.assertEqual(str(self.screen.query_one("#connect-details", Static).content), PROVIDERS[2]["base_url"])
        self.assertIn("←/→ Action", str(self.screen.query_one("#connect-help", Static).content))
        self.assertTrue(connect.has_class("selected-action"))
        await self.pilot.press("enter")
        self.assertTrue(connect.has_focus)
        await self.pilot.press("right")
        self.assertTrue(delete.has_focus)
        self.assertFalse(connect.has_class("selected-action"))
        await self.pilot.press("left")
        self.assertTrue(connect.has_focus)
        await self.pilot.press("right", "enter")
        request = self.app.client.send.call_args.args[0]
        self.assertEqual(request["type"], "provider.delete")
        self.assertEqual(request["provider"], "custom-local")
        self.reply({"type": "provider.listed", "providers": [item for item in PROVIDERS if item['id'] != 'custom-local']})
        await self.pilot.pause()
        self.assertNotIn("custom-local", self.screen.providers)
        await self.pilot.press("enter")
        self.assertTrue(connect.has_focus)  # Next custom never inherits Delete selection.

    async def test_builtin_methods_do_not_show_new_conversation_help(self):
        await self.pilot.press("enter")
        details = self.screen.query_one("#connect-details", Static)
        self.assertEqual(str(details.content), "")
        self.screen.choose_method("api")
        await self.pilot.pause()
        self.assertEqual(str(details.content), "")

    async def test_provider_list_and_openrouter_key_have_no_extra_gap(self):
        providers = self.screen.query_one("#connect-providers", OptionList)
        picker = self.screen.query_one("#connect-provider-picker")
        columns = self.screen.query_one("#connect-provider-columns", Static)
        title = self.screen.query_one("#connect-title")
        self.assertEqual(picker.region.y - title.region.bottom, 1)
        self.assertEqual(str(columns.content), "Provider")
        self.assertEqual(columns.region.y, picker.content_region.y)
        separator = picker.query_one(".popup-separator")
        self.assertEqual(separator.region.y, columns.region.bottom)
        self.assertEqual(providers.region.y, separator.region.bottom)
        self.assertEqual(providers.region.bottom, picker.content_region.bottom)
        self.assertEqual(providers.styles.border.spacing.height, 0)
        self.assertNotEqual(columns.styles.background, providers.styles.background)
        self.assertEqual(providers.styles.margin.top, 0)
        providers.highlighted = 1
        await self.pilot.press("enter")
        key = self.screen.query_one("#connect-key", Input)
        help_text = self.screen.query_one("#connect-help")
        self.assertEqual(help_text.region.y - key.region.bottom, 1)

    async def test_creation_failure_keeps_draft_and_escape_clears_secret(self):
        await self.open_form()
        self.screen.query_one("#connect-name", Input).value = "My server"
        self.screen.query_one("#connect-url", Input).value = "http://localhost:8000/v1"
        key = self.screen.query_one("#connect-key", Input)
        key.value = "candidate"
        key.focus()
        await self.pilot.press("enter")
        self.reply({"type": "request.failed", "error": "Model discovery failed"})
        await self.pilot.pause()
        self.assertEqual(self.screen.query_one("#connect-name", Input).value, "My server")
        self.assertEqual(key.value, "")
        key.value = "another-candidate"
        await self.pilot.press("escape")
        self.assertEqual(key.value, "")
        self.assertFalse(key.display)
        self.assertFalse(self.screen.query_one("#connect-name").display)

    async def test_input_arrows_edit_and_native_provider_hides_custom_actions(self):
        await self.open_form()
        name = self.screen.query_one("#connect-name", Input)
        name.value = "Local"
        name.cursor_position = len(name.value)
        await self.pilot.press("left")
        self.assertTrue(name.has_focus)
        self.assertEqual(name.cursor_position, 4)
        await self.pilot.press("escape")
        options = self.screen.query_one("#connect-providers", OptionList)
        options.highlighted = 2
        await self.pilot.pause()
        self.assertTrue(self.screen.query_one("#connect-custom").display)
        options.highlighted = 1
        await self.pilot.pause()
        self.assertFalse(self.screen.query_one("#connect-custom").display)
        self.assertNotIn("←/→", str(self.screen.query_one("#connect-help", Static).content))
        await self.pilot.press("enter")
        self.assertEqual(self.screen.provider, "openrouter")
        self.assertTrue(self.screen.query_one("#connect-key").display)
        self.assertEqual(str(self.screen.query_one("#connect-key-label", Static).content), "API key")

    async def test_up_down_return_to_providers_after_horizontal_actions(self):
        options = self.screen.query_one("#connect-providers", OptionList)
        options.highlighted = 2
        await self.pilot.press("enter", "right")
        self.assertTrue(self.screen.query_one("#connect-delete").has_focus)
        await self.pilot.press("down")
        self.assertTrue(options.has_focus)
        self.assertEqual(options.highlighted, 3)
        self.assertEqual(str(self.screen.query_one("#connect-details", Static).content), PROVIDERS[3]["base_url"])
        await self.pilot.press("right", "up")
        self.assertTrue(options.has_focus)
        self.assertEqual(options.highlighted, 2)
        await self.pilot.press("left", "up")
        self.assertEqual(options.highlighted, 1)
        self.assertFalse(self.screen.query_one("#connect-custom").display)

    async def test_up_down_use_methods_from_key_field_and_browser_action(self):
        await self.pilot.press("enter")
        methods = self.screen.query_one("#connect-methods", OptionList)
        methods.highlighted = 2
        self.screen.choose_method("api")
        await self.pilot.pause()
        self.assertTrue(self.screen.query_one("#connect-key").has_focus)
        await self.pilot.press("up")
        self.assertTrue(methods.has_focus)
        self.assertEqual(methods.highlighted, 1)
        browser = self.screen.query_one("#connect-browser")
        browser.display = True
        browser.focus()
        await self.pilot.press("down")
        self.assertTrue(methods.has_focus)
        self.assertEqual(methods.highlighted, 2)

    async def test_first_horizontal_arrow_moves_from_visually_selected_action(self):
        options = self.screen.query_one("#connect-providers", OptionList)
        connect = self.screen.query_one("#connect-custom", Button)
        delete = self.screen.query_one("#connect-delete", Button)
        for arrow in ("right", "left"):
            options.focus()
            options.highlighted = 2
            self.screen.show_custom_actions()
            await self.pilot.pause()
            self.assertTrue(options.has_focus)
            self.assertTrue(connect.has_class("selected-action"))
            await self.pilot.press(arrow)
            self.assertTrue(delete.has_focus)
            self.assertTrue(delete.has_class("selected-action"))
            self.assertFalse(connect.has_class("selected-action"))
            await self.pilot.press(arrow)
            self.assertTrue(connect.has_focus)
            await self.pilot.press("down")
            self.assertTrue(options.has_focus)
            self.assertEqual(options.highlighted, 3)
            await self.pilot.press(arrow)
            self.assertTrue(delete.has_focus)

    async def test_connection_fields_are_single_line_without_borders(self):
        await self.open_form()
        for selector in ("#connect-name", "#connect-url", "#connect-key"):
            field = self.screen.query_one(selector, Input)
            for focused in (False, True):
                field.focus() if focused else field.blur()
                await self.pilot.pause()
                self.assertEqual(field.region.height, 1)
                self.assertEqual(field.styles.border.spacing.width, 0)
                self.assertEqual(field.styles.border.spacing.height, 0)

    def test_model_label_uses_instance_name(self):
        self.assertEqual(ModelScreen.provider_label({"provider": "custom-local", "provider_name": "Local server", "auth_method": "api"}),
                         "Local server (Custom)")
