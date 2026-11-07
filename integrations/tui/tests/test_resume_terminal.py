import asyncio
import unittest
from unittest.mock import AsyncMock, patch

from rich.console import Console
from textual.app import App
from textual.widgets import OptionList

from sos_tui.app import SosApp
from sos_tui.config import MISC_MD
from sos_tui.resume_screen import ResumeScreen
from sos_tui.terminal_identity import TerminalIdentity


def session(identity, updated, opened=0):
    return {"id": identity, "title": identity, "updatedAt": updated,
            "terminalLastOpenedAt": opened}


class ResumeApp(App):
    def get_css_variables(self):
        return {**super().get_css_variables(), "muted": "#888888", "title-md": "#aaaaaa",
                "bold-md": "#eeeeee", "depth-md": "#333333", "misc-md": MISC_MD,
                "content-background": "#222222", "input-background": "#111111"}

    def __init__(self):
        super().__init__()
        self.previews = []
        self.resumed = []

    def on_mount(self):
        self.push_screen(ResumeScreen())

    def on_resume_screen_preview_requested(self, event):
        self.previews.append(event.session_id)

    def on_resume_screen_resume_requested(self, event):
        self.resumed.append(event.session_id)


class ResumeTerminalTest(unittest.IsolatedAsyncioTestCase):
    async def test_groups_order_selection_and_navigation(self):
        app = ResumeApp()
        async with app.run_test() as pilot:
            screen = app.screen
            screen.set_sessions([session("global-new", 500), session("local-old", 400, 10),
                                 session("local-new", 100, 20), session("global-old", 200)])
            await pilot.pause()
            options = screen.query_one(OptionList)
            self.assertEqual(options.option_count, 5)  # Separator is not an option.
            self.assertTrue(options.get_option_at_index(0).disabled)
            heading = options.get_option_at_index(0).prompt
            for width in (50, 80):
                rendered = "".join(segment.text for segment in Console(width=width).render(heading)).rstrip("\n")
                self.assertEqual(rendered.strip(), "Invoked from this terminal session")
                left = len(rendered) - len(rendered.lstrip())
                right = len(rendered) - len(rendered.rstrip())
                self.assertLessEqual(abs(left - right), 1)
                self.assertEqual(len(rendered), width)
            for component in ("option-list--option-disabled", "option-list--separator"):
                self.assertEqual(options.get_component_rich_style(component).color.triplet.hex.lower(),
                                 MISC_MD.lower())
            self.assertEqual([options.get_option_at_index(i).id for i in range(1, 5)],
                             ["local-new", "local-old", "global-new", "global-old"])
            self.assertEqual(screen.selected_id, "local-new")
            await pilot.press("down", "down")
            self.assertEqual(screen.selected_id, "global-new")
            await pilot.press("up", "up", "up")
            self.assertNotIn(None, app.previews)
            self.assertNotEqual(options.highlighted, 0)
            await pilot.press("home", "enter")
            self.assertEqual(app.resumed, ["local-new"])

    async def test_returning_to_first_session_reveals_terminal_heading(self):
        app = ResumeApp()
        async with app.run_test(size=(80, 24)) as pilot:
            screen = app.screen
            screen.set_sessions([session(f"local-{index}", index, 100 - index) for index in range(30)]
                                + [session("global", 200)])
            await pilot.pause()
            options = screen.query_one(OptionList)
            await pilot.press("end")
            self.assertGreater(options.scroll_y, 0)
            await pilot.press(*(["up"] * 30))
            self.assertEqual(options.highlighted, 1)
            self.assertEqual(options.scroll_y, 0)
            self.assertIn("Invoked from this terminal session", options.render_line(0).text)
            await pilot.press("end", "home")
            self.assertEqual(options.scroll_y, 0)
            self.assertEqual(screen.selected_id, "local-0")
            self.assertNotIn(None, app.previews)

    async def test_no_history_has_no_heading(self):
        app = ResumeApp()
        async with app.run_test() as pilot:
            app.screen.set_sessions([session("old", 100), session("new", 200)])
            await pilot.pause()
            options = app.screen.query_one(OptionList)
            self.assertEqual(options.option_count, 2)
            self.assertEqual(options.highlighted, 0)
            self.assertEqual(app.screen.selected_id, "new")
            app.screen.set_sessions([])
            await pilot.pause()
            self.assertEqual(options.option_count, 0)
            self.assertIsNone(options.highlighted)

    def test_identity_resolved_once_per_app(self):
        identity = TerminalIdentity("terminal-v1:key", "system")
        with patch("sos_tui.app.resolve_terminal_identity", return_value=identity) as resolver:
            app = SosApp()
        resolver.assert_called_once_with()
        self.assertEqual(app.terminal_identity, identity)

    async def test_app_transmits_identity_or_omits_it(self):
        for identity in (TerminalIdentity("terminal-v1:key", "system"), None):
            with self.subTest(identity=identity), patch("sos_tui.app.resolve_terminal_identity", return_value=identity):
                app = SosApp()
            with patch.object(app.client, "connect", new=AsyncMock()), \
                 patch.object(app.client, "send", new=AsyncMock()) as send, \
                 patch.object(app.client, "receive", new=AsyncMock(side_effect=[
                     {"type": "integration.configured"}, asyncio.CancelledError
                 ])):
                async with app.run_test() as pilot:
                    await pilot.pause()
                    configure = next(call.args[0] for call in send.call_args_list
                                     if call.args[0]["type"] == "integration.configure")
                    if identity:
                        self.assertEqual(configure["terminalSessionId"], identity.key)
                    else:
                        self.assertNotIn("terminalSessionId", configure)

    async def test_parent_resume_opens_persisted_implementation_validation(self):
        from sos_tui.implementation_workspace import ImplementationWorkspace
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test() as pilot:
                app.connected = True
                app.client.send = AsyncMock()
                app.resume_screen = ResumeScreen()
                await app.push_screen(app.resume_screen)
                await app.send_resume_request("session.attach", "discussion")
                request = app.client.send.call_args.args[0]
                info = {"discussion_id": "discussion", "active_implementation_id": "branch",
                        "implementation_view": True, "implementation_execution": True}
                common = {"sessionId": "branch", "requestId": request["requestId"], "messageCount": 0, "info": info}
                app.handle_event(dict(common, type="session.attach_started", page={}, session={
                    "id": "branch", "discussion_id": "discussion", "title": "Implementation", "workspace": app.workspace, "mode": "sane"}))
                await pilot.pause()
                self.assertFalse(any(call.args[0]["type"] == "implementation.state"
                                     for call in app.client.send.call_args_list))
                app.handle_event(dict(common, type="session.attached"))
                await pilot.pause()
                self.assertEqual(app.session_id, "branch")
                self.assertIsNone(app.resume_screen)
                self.assertEqual(app.query_one("#tabs").active, "implement")
                self.assertTrue(app.query_one(ImplementationWorkspace).display)
                self.assertTrue(any(call.args[0]["type"] == "implementation.state" and call.args[0]["sessionId"] == "branch"
                                     for call in app.client.send.call_args_list))

    async def test_disconnect_during_restore_does_not_crash_worker(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test() as pilot:
                app.connected = True
                app.client.send = AsyncMock(side_effect=ConnectionError("Not connected to the daemon"))
                app.restore_session({"session": {"id": "restored", "title": "Restored", "workspace": app.workspace},
                                           "messages": [], "messageCount": 0, "page": {}})
                await app.workers.wait_for_complete()
                await pilot.pause()
                self.assertEqual(app.session_id, "restored")
                self.assertFalse(app.history_loads)
                self.assertIn("Not connected to the daemon", app.query_one("#message").placeholder)
                app.client.send.assert_awaited_once()

    async def test_implementation_metadata_never_requests_missing_or_previous_session(self):
        from sos_tui.implementation_workspace import ImplementationWorkspace
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test() as pilot:
                app.connected = True
                app.client.send = AsyncMock()
                app.runtime_info.update({"discussion_id": "discussion", "active_implementation_id": "branch",
                                         "implementation_view": True, "implementation_execution": True})
                await app.implementation.sync()
                await pilot.pause()
                app.client.send.assert_not_awaited()
                app.session_id = "previous-discussion"
                app.switch_request = "attach-branch"
                await app.implementation.sync()
                await pilot.pause()
                app.client.send.assert_not_awaited()
                app.session_id = "branch"
                app.switch_request = None
                await app.implementation.sync()
                await pilot.pause()
                requests = [call.args[0] for call in app.client.send.call_args_list]
                self.assertEqual(len(requests), 1)
                self.assertEqual(requests[0]["sessionId"], "branch")
                self.assertEqual(requests[0]["type"], "implementation.state")
                self.assertEqual(app.query_one(ImplementationWorkspace).session_id, "branch")
