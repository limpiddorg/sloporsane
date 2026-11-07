"""Real Textual + daemon: historical drawers, pagination and branch selection.

A current SQLite session is seeded through SessionJournal. No provider or account is used;
implementation plan generation cannot proceed without credentials.
"""
import asyncio
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from textual.widgets import Button

from sos_tui.app import SosApp
from sos_tui.history_viewport import HistoryViewport
from sos_tui.message_input import MessageInput
from sos_tui.objective_drawer import ObjectivePlanDrawer
from sos_tui.response_markdown import ResponseMarkdown

ROOT = Path(__file__).resolve().parents[3]
DAEMON = ROOT / "build-ai/bin/sosd"
FIXTURE = ROOT / "build-ai/bin/session-fixture"
SESSION = "a" * 32


@unittest.skipUnless(DAEMON.exists() and FIXTURE.exists(), "Build sosd and session-fixture in build-ai for end-to-end tests")
class ObjectiveDrawersE2ETest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        # Unix socket paths must also fit when the checkout is a deep SOS worktree.
        directory = tempfile.TemporaryDirectory(prefix="objective-e2e-", dir="/tmp")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.enterContext(patch.dict(os.environ, {
            "XDG_DATA_HOME": str(self.root / "data"), "XDG_CONFIG_HOME": str(self.root / "config"),
            "XDG_STATE_HOME": str(self.root / "state"), "XDG_CACHE_HOME": str(self.root / "cache")}))
        self.seed_session()
        socket = str(self.root / "daemon.sock")
        self.daemon = subprocess.Popen([str(DAEMON), socket, str(self.root / "sessions")],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addAsyncCleanup(self.stop_daemon)
        await self.until(lambda: Path(socket).exists())
        self.app = SosApp(socket)
        self.events = self.enterContext(patch.object(self.app, "handle_event", wraps=self.app.handle_event))
        self.requests = self.enterContext(patch.object(self.app.client, "send", wraps=self.app.client.send))
        self.app.launch_workspace = str(self.root)
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(120, 45)))
        await self.until(lambda: self.app.connected and not self.app.connect_requests)
        self.app.query_one(MessageInput).text = "/resume"
        await self.pilot.press("enter")
        await self.until(lambda: self.app.resume_screen is not None and self.app.resume_screen.selected_id == SESSION)
        await self.pilot.press("enter")
        await self.until(lambda: self.app.session_id == SESSION and not self.app.history_loads)
        await self.pilot.pause()
        self.chat = self.app.query_one(HistoryViewport)

    def seed_session(self):
        directory = self.root / "sessions" / SESSION
        directory.mkdir(parents=True)
        messages = []
        for turn in range(30):
            messages.append(self.message(f"user-{turn}", "user", f"Discuss {turn}"))
            messages.append(self.message(f"intermediate-{turn}", "assistant", "Intermediate response"))
            if turn in (1, 25):
                plan = self.message(f"plan-{turn}", "assistant", f"Historical objective {turn}")
                plan.update(objective_plan=True, objective_stack="- Existing stack")
                messages.append(plan)
            messages.append(self.message(f"answer-{turn}", "assistant", f"Final response {turn}"))
        seed = self.root / "seed.json"
        seed.write_text(json.dumps({"id": SESSION, "title": "Objective drawer E2E",
                                    "workspace": str(self.root), "messages": messages}))
        subprocess.run([str(FIXTURE), str(directory), str(seed)], check=True, capture_output=True, text=True)

    @staticmethod
    def message(id, role, text):
        return {"id": id, "role": role, "content": [{"type": "text", "text": text}]}

    async def until(self, condition):
        for _ in range(300):
            if condition():
                return
            await asyncio.sleep(.025)
        failures = [call.args[0] for call in self.events.call_args_list
                    if call.args[0].get("type") == "request.failed"] if hasattr(self, "events") else []
        recent = [call.args[0].get("type") for call in self.events.call_args_list[-8:]] if hasattr(self, "events") else []
        self.fail(f"Timed out waiting for real daemon / TUI state: {failures}; recent={recent}; "
                  f"session={getattr(getattr(self, 'app', None), 'session_id', None)}")

    async def stop_daemon(self):
        if self.daemon.poll() is None:
            self.daemon.terminate()
        try:
            await asyncio.wait_for(asyncio.to_thread(self.daemon.wait), 5)
        except TimeoutError:
            self.daemon.kill()
            await asyncio.to_thread(self.daemon.wait)

    async def test_resume_paginate_and_launch_from_historical_turn(self):
        self.assertGreater(self.chat.size.width, 110)
        drawers = list(self.chat.query(ObjectivePlanDrawer))
        self.assertTrue(drawers)
        self.assertTrue(all(drawer.collapsed for drawer in drawers))
        self.assertTrue(any(drawer.selected_id == "plan-25" and drawer.updated for drawer in drawers))
        self.assertTrue(any(drawer.selected_id == "plan-25" and not drawer.updated for drawer in drawers))
        for _ in range(12):
            if self.chat.query(ObjectivePlanDrawer) and any(
                    drawer.selected_id == "plan-1" and drawer.updated for drawer in self.chat.query(ObjectivePlanDrawer)):
                break
            self.assertTrue(await self.app.load_history_page("before"))
            await self.pilot.pause()
        early = next(drawer for drawer in self.chat.query(ObjectivePlanDrawer)
                     if drawer.selected_id == "plan-1" and drawer.updated)
        self.assertEqual(str(early.title), "Objective plan · updated")
        anchor = next(widget for record, widget in self.chat.rendered if record["id"] == "answer-1")
        children = list(self.chat.children)
        self.assertIs(children[children.index(anchor) + 1], early)
        early.collapsed = False
        await self.pilot.pause()
        self.assertIn("Historical objective 1", early.query_one(ResponseMarkdown)._markdown)
        button = early.query_one(".objective-implement", Button)
        button.scroll_visible(animate=False)
        await self.pilot.pause()
        await self.pilot.click(button)
        await self.until(lambda: self.app.runtime_info.get("implementation_view") and self.app.session_id != SESSION)
        sent = [call.args[0] for call in self.requests.await_args_list if call.args[0].get("type") == "implementation.open"]
        self.assertEqual([request.get("objectiveId") for request in sent], ["plan-1"])
        branch = next(branch for branch in self.app.runtime_info["implementation_branches"]
                      if branch["id"] == self.app.session_id)
        self.assertEqual(branch["objective_id"], "plan-1")
