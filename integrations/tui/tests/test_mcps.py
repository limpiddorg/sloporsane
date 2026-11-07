import unittest
from unittest.mock import AsyncMock, patch

from textual.widgets import Button, Input, OptionList, Static
from sos_tui.app import SosApp
from sos_tui.config import DEPTH_MD, TEXT_COLOR, TITLE_MD
from sos_tui.message_input import MessageInput
from sos_tui.mcps_screen import McpsScreen
from sos_tui.mcp_oauth_screen import McpOAuthScreen
from sos_tui.mcp_connect_screen import McpConnectScreen
from sos_tui.input_error_notice import InputErrorNotice


class McpsTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(110, 55)))
        self.app.connected = True
        self.app.client.send = AsyncMock()
        entry = self.app.query_one(MessageInput)
        entry.disabled = False
        entry.text = "/mcps"
        entry.focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.screen = self.app.mcps_screen
        self.assertIsInstance(self.screen, McpsScreen)
        self.screen.shared_state_timer.pause()

    async def test_action_labels_are_fully_rendered(self):
        for state, enabled, connect_label, enable_label in (
            ("connected", True, "Disconnect", "Disable"),
            ("disconnected", False, "Connect", "Enable"),
        ):
            server = self.server(trusted=True, state=state)
            server["config"]["enabled"] = enabled
            self.screen.receive({"type": "mcp.listed", "servers": [server], "issues": [], "workspace": "/workspace"})
            await self.pilot.pause()
            for selector, label in (("#mcp-connect", connect_label), ("#mcp-enable", enable_label)):
                button = self.screen.query_one(selector, Button)
                self.assertIn(label, button.render_line(0).text)

    async def test_columns_use_standard_section_colors(self):
        await self.reply([self.server()])
        columns = self.screen.query_one("#mcp-columns")
        self.assertEqual(columns.styles.color.hex.lower(), TITLE_MD.lower())
        picker = self.screen.query_one("#mcp-picker")
        listing = self.screen.query_one("#mcp-list")
        self.assertEqual(columns.region.y, picker.content_region.y)
        separator = picker.query_one(".popup-separator")
        self.assertEqual(separator.region.y, columns.region.bottom)
        self.assertEqual(listing.region.y, separator.region.bottom)
        self.assertEqual(listing.region.bottom, picker.content_region.bottom)
        self.assertEqual(columns.content_region.x, listing.content_region.x)
        self.assertNotEqual(columns.styles.background, listing.styles.background)
        self.assertNotEqual(picker.styles.background, columns.styles.background)
        self.assertEqual(separator.styles.background, picker.styles.background)
        self.assertEqual(columns.region.width, picker.content_region.width)
        self.assertEqual(columns.styles.border.spacing.bottom, 0)

    async def test_arrows_work_after_initial_load_and_during_background_refresh(self):
        servers = [dict(self.server(), name=f"server-{index}") for index in range(12)]
        await self.reply(servers)
        listing = self.screen.query_one(OptionList)
        self.assertIs(self.app.focused, listing)
        await self.pilot.press("down", "down")
        self.assertEqual(listing.highlighted, 2)
        self.screen.refresh_shared_state()
        await self.pilot.press("down")
        self.assertEqual(listing.highlighted, 3)
        with patch.object(listing, "set_options", wraps=listing.set_options) as rebuild:
            await self.reply(servers)
            rebuild.assert_not_called()
        self.assertIs(self.app.focused, listing)
        self.assertEqual(listing.highlighted, 3)
        await self.pilot.press("up")
        self.assertEqual(listing.highlighted, 2)

    async def test_refresh_keeps_buttons_stable_and_does_not_drop_user_action(self):
        await self.reply([self.server(trusted=True, state="disconnected")])
        button = self.screen.query_one("#mcp-enable", Button)
        button.focus()
        self.screen.refresh_shared_state()
        await self.pilot.pause()
        self.assertFalse(button.disabled)
        self.assertIs(self.app.focused, button)
        await self.press("enable")
        self.assertEqual(self.last_type(), "mcp.list")
        await self.reply([self.server(trusted=True, state="disconnected")])
        self.assertEqual(self.last_type(), "mcp.enable")
        await self.reply([self.server(trusted=True, state="disconnected")])
        self.assertFalse(self.screen.pending)

    async def test_close_discards_action_queued_behind_background_refresh(self):
        await self.reply([self.server(trusted=True, state="disconnected")])
        self.screen.refresh_shared_state()
        await self.pilot.pause()
        request = self.app.client.send.call_args.args[0]
        await self.press("enable")
        self.assertIsNotNone(self.screen.deferred_request)
        await self.press("close")
        self.assertIsNone(self.screen.deferred_request)
        self.app.handle_event({"type": "mcp.listed", "requestId": request["requestId"],
                               "servers": [self.server()], "issues": []})
        await self.pilot.pause()
        self.assertNotIn("mcp.enable", [call.args[0]["type"] for call in self.app.client.send.call_args_list])

    async def test_refresh_only_replaces_changed_rows_without_resetting_scroll(self):
        servers = [dict(self.server(), name=f"server-{index}") for index in range(15)]
        await self.reply(servers)
        listing = self.screen.query_one(OptionList)
        await self.pilot.press("end")
        await self.pilot.pause()
        scroll = listing.scroll_offset
        self.screen.refresh_shared_state()
        servers[-1]["state"] = "connected"
        with patch.object(listing, "set_options", wraps=listing.set_options) as rebuild:
            await self.reply(servers)
            rebuild.assert_not_called()
        self.assertEqual(listing.highlighted, 14)
        self.assertEqual(listing.scroll_offset, scroll)
        self.assertIs(self.app.focused, listing)

    async def test_shared_state_refresh_updates_idle_screen_but_never_interrupts_connect(self):
        await self.reply([self.server(trusted=True, state="connected")])
        self.screen.refresh_shared_state()
        await self.pilot.pause()
        self.assertEqual(self.last_type(), "mcp.list")
        await self.reply([self.server(trusted=True, state="disconnected")])
        self.assertEqual(str(self.screen.query_one("#mcp-connect", Button).label), "Connect")
        await self.press("connect")
        count = self.app.client.send.call_count
        self.screen.refresh_shared_state()
        await self.pilot.pause()
        self.assertEqual(self.app.client.send.call_count, count)

    async def test_background_restore_is_visible_and_failure_uses_error_notice(self):
        request = self.app.client.send.call_args.args[0]
        self.app.handle_event({"type": "mcp.listed", "requestId": request["requestId"],
                               "servers": [self.server(trusted=True, state="connecting")],
                               "issues": [], "restoring": True})
        await self.pilot.pause()
        self.assertIn("Connecting MCP servers", str(self.screen.query_one("#mcp-status", Static).content))
        self.assertEqual(str(self.screen.query_one("#mcp-detail", Static).content), "Connecting…")
        self.app.handle_event({"type": "mcp.restored", "errors": ["docs: startup failed"]})
        await self.pilot.pause()
        self.assertEqual(self.last_type(), "mcp.list")
        notice = self.app.screen.query_one(InputErrorNotice)
        self.assertIn("docs: startup failed", str(notice.content))

    async def test_restoration_error_above_composer_when_management_is_closed(self):
        await self.reply([self.server()])
        await self.press("close")
        await self.pilot.pause()
        self.assertIsNot(self.app.screen, self.screen)
        self.app.handle_event({"type": "mcp.restored", "errors": ["docs: startup failed"]})
        await self.pilot.pause()
        await self.pilot.pause()
        notice = self.app.screen.query_one(InputErrorNotice)
        self.assertIn("docs: startup failed", str(notice.content))
        self.assertEqual(notice.region.bottom, self.app.query_one("#agent-info").region.y)

    async def reply(self, servers):
        request = self.app.client.send.call_args.args[0]
        self.app.handle_event({"type": "mcp.listed", "requestId": request["requestId"],
                               "servers": servers, "issues": [], "workspace": "/workspace"})
        await self.pilot.pause()

    def server(self, auth="token", trusted=False, state="needs_trust"):
        return {"name": "docs", "scope": "workspace", "identity": "a" * 64, "state": state,
                "trusted": trusted, "tool_count": 0, "error": "", "config": {
                    "type": "http", "url": "https://example.test/mcp", "authentication": auth, "enabled": False}}

    async def press(self, action):
        if action in ("close", "oauth-cancel", "review-cancel", "secret-cancel"):
            await self.pilot.press("escape")
        elif action in ("confirm", "token-save"):
            await self.pilot.press("enter")
        else:
            self.app.screen.query_one(f"#mcp-{action}", Button).press()
        await self.pilot.pause()

    def last_type(self):
        return self.app.client.send.call_args.args[0]["type"]

    async def test_file_only_screen_and_browser_button_label(self):
        await self.reply([])
        visible = {button.id for button in self.screen.query(Button) if button.region.width > 0}
        self.assertEqual(visible, {"mcp-connect", "mcp-enable"})
        self.assertTrue(self.screen.query_one("#mcp-connect", Button).disabled)
        self.assertFalse(self.screen.query("#mcp-add, #mcp-edit, #mcp-remove, #mcp-editor"))
        self.assertIn(".sos/mcps.json", str(self.screen.query_one("#mcp-status", Static).render()))
        self.assertFalse(self.screen.query("#mcp-oauth, #mcp-browser"))
        self.assertLess(self.screen.query_one(".popup-dialog").region.height, 30)
        self.screen.request({"type": "mcp.list"})
        await self.pilot.pause()
        await self.reply([self.server(trusted=True)])
        button = self.screen.query_one("#mcp-enable", Button)
        button.blur()
        await self.pilot.pause()
        resting = button.styles.background
        await self.pilot.hover("#mcp-enable")
        self.assertNotEqual(resting, button.styles.background)
        await self.pilot.resize_terminal(80, 28)
        self.assertLessEqual(self.screen.query_one("#mcp-enable").region.right, 80)

    async def test_action_labels_fit_and_selected_action_uses_tab_colors(self):
        server = self.server(trusted=True, state="connected")
        server["config"]["enabled"] = True
        await self.reply([server])
        for size in ((110, 55), (60, 24)):
            await self.pilot.resize_terminal(*size)
            await self.pilot.pause()
            connect = self.screen.query_one("#mcp-connect", Button)
            enable = self.screen.query_one("#mcp-enable", Button)
            self.assertEqual(str(connect.label), "Disconnect")
            self.assertEqual(str(enable.label), "Disable")
            self.assertGreaterEqual(connect.content_region.width, len("Disconnect"))
            self.assertGreaterEqual(enable.content_region.width, len("Disable"))
            self.assertEqual(connect.styles.background.hex.lower(), DEPTH_MD.lower())
            self.assertEqual(connect.styles.color.hex.lower(), TEXT_COLOR.lower())
            await self.pilot.press("right")
            self.assertEqual(enable.styles.background.hex.lower(), DEPTH_MD.lower())
            self.assertEqual(enable.styles.color.hex.lower(), TEXT_COLOR.lower())
            await self.pilot.press("left")

    async def test_keyboard_help_replaces_close_and_escape_cancels_pending_connection(self):
        await self.reply([self.server(trusted=True, state="disconnected")])
        footer = self.screen.query_one("#mcp-footer")
        self.assertFalse(self.screen.query("#mcp-close"))
        self.assertIn("Esc Close", str(self.screen.query_one("#mcp-help", Static).content))
        for size in ((110, 55), (60, 24)):
            await self.pilot.resize_terminal(*size)
            buttons = [self.screen.query_one(f"#mcp-{name}", Button)
                       for name in ("connect", "enable")]
            self.assertEqual({button.region.y for button in buttons}, {footer.region.y})
            self.assertEqual({button.region.height for button in buttons}, {1})
            self.assertLessEqual(buttons[1].region.right, footer.content_region.right)
            self.assertLessEqual(self.screen.query_one(".popup-dialog").region.height, 16)
        await self.press("connect")
        await self.press("close")
        self.assertIsNone(self.app.mcps_screen)
        self.assertEqual(self.last_type(), "mcp.cancel")

    async def test_keyboard_selects_server_and_action_and_preserves_choice_during_refresh(self):
        servers = [self.server(trusted=True, state="connected"),
                   dict(self.server(trusted=True, state="disconnected"), name="second")]
        await self.reply(servers)
        self.assertEqual(self.screen.selected()["name"], "docs")
        self.assertTrue(self.screen.query_one("#mcp-connect").has_class("selected-action"))
        await self.pilot.press("enter")
        self.assertEqual(self.last_type(), "mcp.disconnect")
        await self.reply(servers)
        await self.pilot.press("right", "down")
        self.assertEqual(self.screen.selected()["name"], "second")
        self.assertEqual(self.screen.selected_action, "enable")
        self.screen.refresh_shared_state()
        await self.pilot.pause()
        await self.reply(servers)
        self.assertEqual(self.screen.selected_action, "enable")
        await self.pilot.press("enter")
        self.assertEqual(self.last_type(), "mcp.enable")
        self.assertEqual(self.app.client.send.call_args.args[0]["name"], "second")
        await self.reply(servers)
        await self.pilot.press("left", "up", "enter")
        self.assertEqual(self.last_type(), "mcp.disconnect")
        self.assertEqual(self.app.client.send.call_args.args[0]["name"], "docs")

    async def test_empty_list_keyboard_does_not_send_actions(self):
        await self.reply([])
        count = self.app.client.send.call_count
        await self.pilot.press("right", "enter", "left", "up", "down", "enter")
        self.assertEqual(self.app.client.send.call_count, count)

    async def test_keyboard_disable_is_not_repeated_and_reopening_resets_action(self):
        server = self.server(trusted=True, state="connected")
        server["config"]["enabled"] = True
        await self.reply([server])
        await self.pilot.press("right", "enter")
        self.assertEqual(self.last_type(), "mcp.enable")
        self.assertFalse(self.app.client.send.call_args.args[0]["enabled"])
        count = self.app.client.send.call_count
        await self.pilot.press("enter")
        self.assertEqual(self.app.client.send.call_count, count)
        await self.reply([server])
        await self.pilot.press("escape")
        entry = self.app.query_one(MessageInput)
        entry.text = "/mcps"
        await self.pilot.press("enter")
        self.screen = self.app.mcps_screen
        self.screen.shared_state_timer.pause()
        await self.reply([server])
        self.assertEqual(self.screen.selected_action, "connect")
        self.assertEqual(self.screen.query_one(OptionList).highlighted, 0)

    async def test_escape_cancels_approval_and_token_dialogs_only(self):
        await self.reply([self.server()])
        height = self.screen.query_one(".popup-dialog").region.height
        await self.press("connect")
        self.assertEqual(self.screen.query_one(".popup-dialog").region.height, height)
        await self.pilot.press("escape")
        await self.pilot.pause()
        self.assertIs(self.app.screen, self.screen)
        self.assertIsNone(self.screen.connect_target)
        self.screen.authorize(self.server(trusted=True, state="needs_auth"))
        await self.pilot.pause()
        secret = self.app.screen.query_one(Input)
        secret.value = "discard-me"
        await self.pilot.press("escape")
        await self.pilot.pause()
        self.assertEqual(secret.value, "")
        self.assertIs(self.app.screen, self.screen)

    async def test_escape_cancels_pending_connection(self):
        await self.reply([self.server(trusted=True, state="disconnected")])
        await self.press("connect")
        self.assertEqual(self.last_type(), "mcp.connect")
        await self.pilot.press("escape")
        await self.pilot.pause()
        self.assertIsNone(self.app.mcps_screen)
        self.assertEqual(self.last_type(), "mcp.cancel")

    async def test_connect_approves_prompts_for_token_and_finishes_without_enabling(self):
        await self.reply([self.server()])
        count = self.app.client.send.await_count
        await self.press("connect")
        self.assertEqual(self.app.client.send.await_count, count)
        self.assertIsInstance(self.app.screen, McpConnectScreen)
        self.assertFalse(self.screen.query("#mcp-review, #mcp-secret"))
        self.assertFalse(self.app.screen.query(Button))
        await self.press("confirm")
        self.assertEqual(self.last_type(), "mcp.trust")
        await self.reply([self.server(trusted=True, state="disconnected")])
        self.assertEqual(self.last_type(), "mcp.connect")
        await self.reply([self.server(trusted=True, state="needs_auth")])
        secret = self.app.screen.query_one("#mcp-token-value", Input)
        self.assertTrue(secret.password)
        self.assertTrue(secret.has_focus)
        background = secret.styles.background
        for focused in (False, True):
            self.app.screen.set_focus(secret if focused else None)
            await self.pilot.pause()
            self.assertEqual(secret.region.height, 1)
            self.assertEqual(secret.styles.border.spacing.width, 0)
            self.assertEqual(secret.styles.border.spacing.height, 0)
            self.assertEqual(secret.styles.background, background)
        self.assertFalse(self.app.screen.query(Button))
        self.assertIsInstance(self.app.screen, McpConnectScreen)
        secret.value = "test-token"
        await self.press("token-save")
        self.assertEqual(secret.value, "")
        self.assertEqual(self.last_type(), "mcp.token")
        await self.reply([self.server(trusted=True, state="disconnected")])
        self.assertEqual(self.last_type(), "mcp.connect")
        await self.reply([self.server(trusted=True, state="connected")])
        self.assertIsNone(self.screen.connect_target)
        self.assertEqual(str(self.screen.query_one("#mcp-connect", Button).label), "Disconnect")
        self.assertEqual(str(self.screen.query_one("#mcp-enable", Button).label), "Enable")
        self.assertNotIn("mcp.enable", [call.args[0]["type"] for call in self.app.client.send.call_args_list])

    async def test_connection_error_survives_refresh_without_zero_tools_prefix(self):
        item = self.server(trusted=True, state="disconnected")
        await self.reply([item])
        await self.press("connect")
        request = self.app.client.send.call_args.args[0]
        error = "MCP HTTP transport failed (curl 60): SSL peer certificate was not OK"
        self.app.handle_event({"type": "request.failed", "requestId": request["requestId"], "error": error})
        await self.pilot.pause()
        self.assertEqual(self.last_type(), "mcp.list")
        item.update(state="failed", error=error)
        await self.reply([item])
        detail = str(self.screen.query_one("#mcp-detail", Static).render())
        self.assertIn(error, detail)
        self.assertNotIn("0 tools", detail)
        self.assertIsNone(self.screen.connect_target)

    async def start_oauth(self, browser_result=True):
        item = self.server(auth="auto", trusted=True, state="disconnected")
        await self.reply([item])
        await self.press("connect")
        self.assertIs(self.app.screen, self.screen)
        item["state"] = "needs_auth"
        await self.reply([item])
        self.assertEqual(self.last_type(), "mcp.oauth_start")
        self.assertIs(self.app.screen, self.screen)
        request = self.app.client.send.call_args.args[0]
        with patch("sos_tui.mcps_screen.webbrowser.open", return_value=browser_result) as browser:
            self.app.handle_event({"type": "mcp.oauth_started", "requestId": request["requestId"],
                                   "url": "https://example.test/authorize", "instructions": "Authorize"})
            await self.pilot.pause()
            browser.assert_called_once()
        self.assertIsInstance(self.app.screen, McpOAuthScreen)
        return self.app.screen

    async def test_oauth_link_is_in_overlay_and_escape_returns_to_management(self):
        dialog = await self.start_oauth()
        self.assertTrue(self.screen.oauth_active)
        self.assertTrue(self.screen.query_one("#mcp-connect", Button).disabled)
        self.assertIn("https://example.test/authorize", str(dialog.query_one("#mcp-oauth-url", Static).render()))
        self.assertEqual(str(dialog.query_one("#mcp-browser", Button).label), "Open in Browser")
        with patch("sos_tui.mcps_screen.webbrowser.open", return_value=True) as browser:
            await self.press("browser")
            browser.assert_called_once()
        await self.pilot.resize_terminal(80, 28)
        self.assertFalse(dialog.query("#mcp-oauth-cancel"))
        await self.pilot.press("escape")
        await self.pilot.pause()
        self.assertEqual(self.last_type(), "mcp.cancel")
        self.assertIs(self.app.screen, self.screen)
        self.assertIs(self.app.mcps_screen, self.screen)
        self.assertIsNone(self.screen.oauth_screen)
        self.screen.receive({"type": "mcp.oauth_pending"})  # Late poll must not resume the journey.
        self.assertTrue(self.screen.pending)
        self.screen.receive({"type": "mcp.cancelled"})
        await self.pilot.pause()
        await self.reply([self.server(auth="auto", trusted=True, state="needs_auth")])
        self.assertFalse(self.screen.query_one("#mcp-connect", Button).disabled)

    async def test_browser_failure_remains_visible_during_polling_and_cancel_button_works(self):
        dialog = await self.start_oauth(browser_result=False)
        self.screen.receive({"type": "mcp.oauth_pending"})
        self.assertIn("Open the displayed URL manually", str(dialog.query_one("#mcp-oauth-status", Static).render()))
        await self.press("oauth-cancel")
        self.assertEqual(self.last_type(), "mcp.cancel")
        self.assertIs(self.app.screen, self.screen)

    async def test_oauth_success_dismisses_overlay_and_finishes_connection(self):
        await self.start_oauth()
        self.screen.last_action = "mcp.oauth_poll"
        self.screen.receive({"type": "mcp.listed", "servers": [self.server(auth="auto", trusted=True, state="disconnected")]})
        await self.pilot.pause()
        self.assertIs(self.app.screen, self.screen)
        self.assertEqual(self.last_type(), "mcp.connect")
        await self.reply([self.server(auth="auto", trusted=True, state="connected")])
        self.assertIsNone(self.screen.connect_target)
        self.assertIsNone(self.screen.oauth_screen)

    async def test_oauth_error_closes_overlay_and_shows_error_in_management(self):
        await self.start_oauth()
        self.screen.last_action = "mcp.oauth_poll"
        self.screen.receive({"type": "request.failed", "error": "Authorization failed"})
        await self.pilot.pause()
        self.assertIs(self.app.screen, self.screen)
        self.assertIn("Authorization failed", str(self.screen.query_one("#mcp-status", Static).render()))

    async def test_cancel_approval_and_changed_identity_do_not_connect(self):
        await self.reply([self.server()])
        count = self.app.client.send.await_count
        await self.press("connect")
        await self.press("review-cancel")
        self.assertIsNone(self.screen.connect_target)
        self.assertEqual(self.app.client.send.await_count, count)
        await self.press("connect")
        await self.press("confirm")
        item = self.server(trusted=True)
        item["identity"] = "b" * 64
        await self.reply([item])
        self.assertIsNone(self.screen.connect_target)
        self.assertEqual(self.last_type(), "mcp.trust")

    async def test_rejected_credentials_do_not_loop_authorization(self):
        item = self.server(trusted=True, state="needs_auth")
        await self.reply([item])
        await self.press("connect")
        await self.reply([item])
        self.app.screen.query_one("#mcp-token-value", Input).value = "wrong"
        await self.press("token-save")
        await self.reply([item])
        await self.reply([item])
        self.assertIsNone(self.screen.connect_target)
        self.assertIsNone(self.screen.connect_screen)
        self.assertEqual(self.last_type(), "mcp.connect")

    async def test_disabled_connected_server_can_be_enabled_without_reconnecting(self):
        item = self.server(trusted=True, state="connected")
        await self.reply([item])
        await self.press("enable")
        self.assertEqual(self.last_type(), "mcp.enable")
        item["config"]["enabled"] = True
        await self.reply([item])
        self.assertEqual(str(self.screen.query_one("#mcp-connect", Button).label), "Disconnect")
        self.assertEqual(str(self.screen.query_one("#mcp-enable", Button).label), "Disable")
