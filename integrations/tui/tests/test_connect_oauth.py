"""OAuth browser actions remain usable while authorization is being polled."""

import unittest
from unittest.mock import patch

from textual.widgets import Button, Link, OptionList, Static

from sos_tui.app import SosApp
from sos_tui.config import BOLD_MD, TITLE_MD
from sos_tui.connect_screen import ConnectScreen


class ConnectOAuthTest(unittest.IsolatedAsyncioTestCase):
    async def test_browser_button_and_url_work_during_browser_and_device_polling(self):
        for method in ("browser", "device"):
            with self.subTest(method=method):
                app = SosApp()
                with patch.object(app, "listen"), \
                        patch.object(app, "open_url") as open_link, \
                        patch("sos_tui.connect_screen.webbrowser.open", return_value=True) as open_browser:
                    async with app.run_test(size=(100, 40)) as pilot:
                        screen = ConnectScreen()
                        await app.push_screen(screen)
                        await pilot.pause()
                        screen.receive({"type": "provider.listed", "providers": [
                            {"id": "openai", "connected": False}]})
                        screen.choose_provider("openai")
                        screen.choose_method(method)
                        screen.submit()
                        url = "https://example.test/authorize?state=example&code=123"
                        screen.receive({"type": "auth.started", "authId": "test-flow",
                                        "url": url, "instructions": "Authorize this connection. Code: TEST-1234"})
                        await pilot.pause()
                        browser = screen.query_one("#connect-browser", Button)
                        link = screen.query_one("#connect-auth-url", Link)
                        details = screen.query_one("#connect-details", Static)
                        self.assertTrue(browser.has_focus)
                        self.assertFalse(browser.disabled)
                        self.assertEqual(link.url, url)
                        self.assertEqual(link.text, url)
                        self.assertEqual(link.styles.color.hex, TITLE_MD)
                        self.assertNotEqual(link.styles.color, details.styles.color)
                        self.assertTrue(link.styles.text_style.underline)
                        self.assertNotIn(url, str(details.render()))
                        screen.poll()
                        screen.receive({"type": "auth.pending"})
                        await pilot.pause()
                        self.assertTrue(screen.pending)
                        self.assertTrue(screen.query_one("#connect-methods", OptionList).disabled)
                        self.assertFalse(browser.disabled)
                        await pilot.press("enter")
                        open_browser.assert_called_once_with(url)
                        await pilot.pause(0.3)  # Let the button's active feedback finish.
                        await pilot.click("#connect-browser")
                        self.assertEqual(open_browser.call_count, 2)
                        await pilot.click("#connect-auth-url")
                        open_link.assert_called_once_with(url)
                        link.focus()
                        await pilot.pause()
                        self.assertEqual(link.styles.color.hex, BOLD_MD)
                        await pilot.press("enter")
                        self.assertEqual(open_link.call_count, 2)
                        app.connect_screen = screen  # Route the app's priority Escape binding.
                        await pilot.press("escape")
                        self.assertFalse(browser.display)
                        self.assertFalse(link.display)
                        self.assertEqual(link.url, "")
                        self.assertEqual(screen.url, "")
                        self.assertNotIn("TEST-1234", str(details.render()))
