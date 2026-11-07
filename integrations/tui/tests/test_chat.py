"""Chat tests against a local JSONL server, without model calls."""

import asyncio
from contextlib import nullcontext
import json
import os
import subprocess
import threading
import tempfile
import unittest
from unittest.mock import AsyncMock, patch
from pathlib import Path
from rich.console import Console
from rich.color import Color
from textual.containers import VerticalScroll
from textual.widgets import Button, Collapsible, Footer, Input, Markdown, OptionList, Rule, Static, Tabs
from sos_tui.app import SosApp
from sos_tui.config import BACKGROUND_COLOR, SANE_COLOR, SLOP_COLOR, TITLE_MD, BOLD_MD, MISC_MD, DEPTH_MD, ALGAE_MD, TEXT_COLOR, CONTENT_BACKGROUND_COLOR, CODE_BACKGROUND_COLOR
from sos_tui.message_input import MessageInput
from sos_tui.images import ImagePaste
from sos_tui.mode_button import ModeButton
from sos_tui.agent_info_bar import AgentInfoBar
from sos_tui.confirmation_bar import ConfirmationBar
from sos_tui.tool_view import ReadView, ToolView
from sos_tui.turn_activity import ActivityToggle, TurnActivity, format_duration
from sos_tui.config import MUTED_COLOR
from sos_tui.response_markdown import CodeFence
from sos_tui.user_message import UserMessage
from sos_tui.resume_screen import ResumeScreen
from sos_tui.permission_card import PermissionCard
from sos_tui.connect_screen import ConnectScreen
from sos_tui.model_screen import ModelScreen
from sos_tui.workspace_path import compact_path
from history_fixture import display_records
from confirmation_fixture import confirmation_input
from dialog_fixture import row_text
from async_fixture import wait_until


class DurationFormatTest(unittest.TestCase):
    def test_duration_format_uses_seconds_minutes_then_hours(self):
        for milliseconds, expected in [(0, "0s"), (999, "0s"), (59000, "59s"), (60000, "1m 00s"),
                                       (128000, "2m 08s"), (3599000, "59m 59s"), (3600000, "1h 00m"),
                                       (3900000, "1h 05m"), (90000000, "25h 00m")]:
            self.assertEqual(format_duration(milliseconds), expected)


class ChatTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="sos-ui-", dir="/tmp")
        self.path = str(Path(self.directory.name) / "daemon.sock")
        self.requests = []
        self.history_requests = []
        self.activity_requests = []
        self.writers = set()
        self.release = asyncio.Event()
        self.release.set()
        self.handshake_release = asyncio.Event()
        self.handshake_release.set()
        self.configure_release = asyncio.Event()
        self.configure_release.set()
        self.fail_configure = False
        self.compaction_release = asyncio.Event()
        self.compaction_release.set()
        self.compaction_outcome = "compaction.completed"
        self.history_release = asyncio.Event()
        self.history_release.set()
        self.disconnect_history = False
        self.compaction_summary = {"id": "compact-1", "role": "assistant",
                                   "content": [{"type": "text", "text": "## Goal\n- Keep the useful context."}],
                                   "metadata": {"model": "summary-model", "reasoning": "high", "duration_ms": 100},
                                   "compaction": {"retained_message_ids": [], "mode": "sane"}}
        self.reject = False
        self.fail_execution = False
        self.version = 7
        self.saved_sessions = []
        self.control_messages = []
        self.auth_polls = 0
        self.connected_methods = {}
        self.oauth_method = ""
        self.models = []
        self.catalog_errors = []
        self.runtime_info = {"model": "openai/test-model", "reasoning": "medium",
                             "last_turn_used_context": None, "total_context": None}
        self.server = await asyncio.start_unix_server(self.serve, path=self.path)

    async def asyncTearDown(self):
        self.handshake_release.set()
        self.configure_release.set()
        self.release.set()
        self.compaction_release.set()
        self.history_release.set()
        self.server.close()
        await self.server.wait_closed()
        for writer in list(self.writers):
            writer.close()
            await writer.wait_closed()
        self.directory.cleanup()

    async def serve(self, reader, writer):
        self.writers.add(writer)

        async def send(event):
            event.setdefault("info", dict(self.runtime_info))
            writer.write((json.dumps(event) + "\n").encode())
            await writer.drain()

        active = {"id": "session-1", "title": "Short generated title", "workspace": str(Path.cwd()), "messages": []}

        async def send_history(request, session):
            records = display_records(session["messages"])
            activities = request["type"] == "session.activities"
            if activities:
                records = [record for record in records if record["id"] == request["groupId"]]
            visible = [record for record in records if record["role"] in ("user", "assistant") and not record.get("compaction")]
            base = {"requestId": request["requestId"], "sessionId": session["id"]}
            await send({**base, "type": "session.attach_started",
                        "purpose": "activities" if activities else "page", "groupId": request.get("groupId", ""),
                        "session": {k: v for k, v in session.items() if k != "messages"},
                        "page": {"first": visible[0]["id"] if visible else "", "last": visible[-1]["id"] if visible else "",
                                 "hasOlder": False, "hasNewer": False}, "messageCount": len(records)})
            await self.history_release.wait()
            for index, message in enumerate(records):
                data = json.dumps(message, ensure_ascii=False)
                for part, offset in enumerate(range(0, len(data), 1024)):
                    await send({**base, "type": "session.history_chunk", "messageIndex": index,
                                "partIndex": part, "last": offset + 1024 >= len(data), "data": data[offset:offset + 1024]})
                    if self.disconnect_history:
                        return
            await send({**base, "type": "session.attached", "messageCount": len(records)})

        try:
            await self.handshake_release.wait()
            await send({"type": "daemon.ready", "protocolVersion": self.version})
            while line := await reader.readline():
                request = json.loads(line)
                if request["type"] == "btw.list":
                    await send({"type": "btw.listed", "requestId": request["requestId"], "sessionId": request["sessionId"]})
                    continue
                if request["type"] in ("integration.configure", "mode.set"):
                    if request["type"] == "integration.configure":
                        await self.configure_release.wait()
                        if self.fail_configure:
                            await send({"type": "request.failed", "error": "Configuration failed"})
                            continue
                    await send({"type": "integration.configured" if request["type"] == "integration.configure" else "mode.updated",
                                "requestId": request["requestId"], "mode": request["mode"]})
                    continue
                if request["type"] in ("integration.close", "integration.pong"):
                    self.control_messages.append(request)
                    if request["type"] == "integration.close":
                        break
                    continue
                if request["type"] == "session.history":
                    self.history_requests.append(request)
                    await send_history(request, active)
                    continue
                if request["type"] == "session.activities":
                    self.activity_requests.append(request)
                    await send_history(request, active)
                    continue
                self.requests.append(request)
                await self.release.wait()
                if self.reject:
                    await send({"type": "request.failed", "requestId": request["requestId"], "error": "Missing API key"})
                    continue
                if request["type"] == "provider.list":
                    await send({"type": "provider.listed", "requestId": request["requestId"], "providers": [
                        {"id": "openai", "connected": "openai" in self.connected_methods,
                         "auth_method": self.connected_methods.get("openai"), "model": "gpt-5.4"},
                        {"id": "openrouter", "connected": "openrouter" in self.connected_methods,
                         "auth_method": self.connected_methods.get("openrouter"), "model": "openai/test-model"}]})
                    continue
                if request["type"] == "model.list":
                    await send({"type": "model.listed", "requestId": request["requestId"],
                                "models": self.models, "errors": self.catalog_errors})
                    continue
                if request["type"] == "model.cancel":
                    await send({"type": "model.cancelled", "requestId": request["requestId"]})
                    continue
                if request["type"] == "auth.api":
                    self.connected_methods[request["provider"]] = "api"
                    await send({"type": "auth.saved", "requestId": request["requestId"]})
                    continue
                if request["type"] == "auth.start":
                    self.oauth_method = request["method"]
                    await send({"type": "auth.started", "requestId": request["requestId"], "authId": "flow-1",
                                "url": "https://example.com/device", "instructions": "Enter code: TEST-1234"})
                    continue
                if request["type"] == "auth.poll":
                    self.auth_polls += 1
                    if self.auth_polls > 1:
                        self.connected_methods["openai"] = self.oauth_method
                    await send({"type": "auth.pending" if self.auth_polls == 1 else "auth.saved",
                                "requestId": request["requestId"], "authId": "flow-1"})
                    continue
                if request["type"] == "auth.cancel":
                    await send({"type": "auth.cancelled", "requestId": request["requestId"]})
                    continue
                if request["type"] in ("provider.select", "model.select"):
                    selected_model = request.get("model") or ("gpt-5.4" if request["provider"] == "openai" else "openai/test-model")
                    self.runtime_info.update(provider=request["provider"], model=selected_model,
                                             last_turn_used_context=None, total_context=None)
                    await send({"type": "provider.selected" if request["type"] == "provider.select" else "model.selected",
                                "requestId": request["requestId"]})
                    continue
                if request["type"] == "session.list":
                    await send({"type": "session.listed", "requestId": request["requestId"],
                                "sessions": [{k: v for k, v in s.items() if k != "messages"} for s in self.saved_sessions]})
                    continue
                if request["type"] == "session.new":
                    self.runtime_info.update(last_turn_used_context=None, total_context=None,
                                             execution_reasoning=None, execution_model=None)
                    await send({"type": "session.reset", "requestId": request["requestId"]})
                    continue
                if request["type"] == "session.compact":
                    await send({"type": "compaction.started", "requestId": request["requestId"]})
                    await self.compaction_release.wait()
                    if self.compaction_outcome == "compaction.completed":
                        self.runtime_info.update(last_turn_used_context=None, total_context=None)
                    await send({"type": self.compaction_outcome, "requestId": request["requestId"],
                                "messages": [self.compaction_summary],
                                "error": "Summary provider failed" if self.compaction_outcome == "request.failed" else ""})
                    continue
                if request["type"] in ("session.preview", "session.attach"):
                    session = next(s for s in self.saved_sessions if s["id"] == request["sessionId"])
                    if request["type"] == "session.preview":
                        prompts = [m["content"][0]["text"] for m in session["messages"] if m["role"] == "user"]
                        await send({"type": "session.previewed", "requestId": request["requestId"],
                                    "sessionId": session["id"], "text": prompts[-1] if prompts else ""})
                    else:
                        active = session
                        await send_history(request, session)
                        if self.disconnect_history:
                            return
                    continue
                await send({"type": "session.created" if len(self.requests) == 1 else "message.accepted",
                            "requestId": request["requestId"], "session": {
                                "id": "session-1", "title": "Short generated title"}})
                for origin, role, content in [
                    ("harness", "user", [{"type": "text", "text": request["text"]}]),
                    ("provider", "assistant", [{"type": "reasoning", "text": "Reading the file"},
                        {"type": "tool_call", "toolName": "read", "callId": "call-1", "arguments": {"filePath": "test.txt"}}]),
                    ("harness", "tool", [{"type": "tool_result", "callId": "call-1", "output": "file contents", "isError": False,
                        "attachments": [{"mime": "image/png", "url": "data:DO_NOT_DISPLAY_BASE64"}]}]),
                    ("provider", "assistant", [{"type": "text", "text": "Hello from the model"}]),
                ]:
                    if origin == "provider":
                        self.runtime_info.update(last_turn_used_context=1250, total_context=128000)
                    message = {"id": f"live-{len(active['messages'])}", "role": role, "content": content}
                    if role == "user":
                        message["metadata"] = {"mode": request.get("mode", "sane")}
                    active["messages"].append(message)
                    await send({"type": origin + ".message_arrived", "messages": [message]})
                await send({"type": "execution.failed" if self.fail_execution else "execution.completed",
                            "error": "Provider failed" if self.fail_execution else ""})
        except (ConnectionError, asyncio.CancelledError):
            pass
        finally:
            writer.close()
            self.writers.discard(writer)

    async def until(self, pilot, predicate):
        await wait_until(pilot, predicate, "UI state", timeout=3.0)

    def log_text(self, app):
        return "\n".join(str(widget.render()) for widget in app.query_one("#chat").query(Static))

    async def test_latest_window_sends_without_loading_or_rebuilding_history(self):
        app = SosApp(self.path)
        self.history_release.clear()  # An accidental history request would block the send.
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "First request"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 1 and not app.busy)
            user = app.query_one(UserMessage)
            drawer = app.query_one(TurnActivity)
            drawer.toggle.action_toggle()
            await pilot.pause()
            self.assertTrue(drawer.toggle.expanded)
            with patch.object(app, "begin_history_loading", wraps=app.begin_history_loading) as loading:
                entry.text = "Second request"
                await pilot.press("enter")
                await self.until(pilot, lambda: len(self.requests) == 2 and not app.busy)
                loading.assert_not_called()
            self.assertEqual(self.history_requests, [])
            self.assertTrue(user.is_attached)
            self.assertTrue(drawer.is_attached)
            self.assertFalse(drawer.toggle.expanded)
            self.assertEqual(len(drawer.body.children), 0)
            self.assertFalse(drawer.built)
            self.assertEqual(len(app.query(UserMessage)), 2)

    async def test_next_send_purges_live_activity_and_reopening_requests_its_stable_group(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "First request"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 1 and not app.busy)
            previous = app.query_one(TurnActivity)
            self.assertEqual(previous.group_id, "live-0/activity/0")
            previous.toggle.action_toggle()
            await pilot.pause()
            self.assertEqual(len(previous.query(ReadView)), 1)
            self.release.clear()
            entry.text = "Second request"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 2)
            self.assertFalse(previous.toggle.expanded)
            self.assertFalse(previous.built)
            self.assertEqual(len(previous.body.children), 0)
            self.assertEqual(len(app.query(ReadView)), 0)
            self.assertEqual(self.history_requests, [])
            self.release.set()
            await self.until(pilot, lambda: not app.busy)
            await pilot.pause()
            current = list(app.query(TurnActivity))[-1]
            self.assertEqual(len(current.query(ReadView)), 1)
            self.assertEqual(len(previous.query(ReadView)), 0)
            previous.toggle.action_toggle()
            await self.until(pilot, lambda: previous.built)
            self.assertEqual(len(self.activity_requests), 1)
            self.assertEqual(self.activity_requests[0]["groupId"], "live-0/activity/0")
            self.assertEqual(self.activity_requests[0]["sessionId"], app.session_id)
            self.assertEqual(len(previous.query(ReadView)), 1)
            self.release.clear()
            entry.text = "Third request"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 3)
            self.assertFalse(previous.built)
            self.assertFalse(current.built)
            self.assertEqual(len(app.query(ReadView)), 0)
            self.release.set()
            await self.until(pilot, lambda: not app.busy)

    async def test_sending_from_older_window_loads_latest_once_then_sends_locally(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "First request"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 1 and not app.busy)
            app.history_window.page["hasNewer"] = True
            entry.text = "Return to latest"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 2 and not app.busy)
            self.assertEqual(len(self.history_requests), 1)
            self.assertEqual(self.history_requests[0]["direction"], "latest")
            self.assertTrue(app.history_window.latest)
            self.history_release.clear()
            entry.text = "Already at latest"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 3 and not app.busy)
            self.assertEqual(len(self.history_requests), 1)

    async def test_startup_send_waits_for_handshake_and_configuration_in_same_activity(self):
        self.handshake_release.clear()
        self.configure_release.clear()
        self.release.clear()
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            entry = app.query_one(MessageInput)
            self.assertFalse(entry.disabled)
            self.assertIs(app.focused, entry)
            entry.text = "Early message"
            await pilot.press("enter")
            activity = app.query_one(TurnActivity)
            self.assertTrue(app.busy)
            self.assertEqual(self.requests, [])
            self.assertEqual(app.query_one(UserMessage).text, "Early message")
            entry.text = "Next draft"
            self.handshake_release.set()
            await pilot.pause()
            self.assertFalse(app.connected)
            self.assertEqual(self.requests, [])
            self.configure_release.set()
            await self.until(pilot, lambda: len(self.requests) == 1)
            self.assertIs(app.running_activity, activity)
            self.assertEqual(entry.text, "Next draft")
            # MCP restoration notifications must not end or replace the turn.
            app.handle_event({"type": "mcp.restored", "errors": []})
            self.assertTrue(app.busy)
            self.assertIs(app.running_activity, activity)
            self.release.set()
            await self.until(pilot, lambda: not app.busy)
            self.assertEqual(len(self.requests), 1)
            self.assertEqual(len(app.query(UserMessage)), 1)
            self.assertIs(app.query_one(TurnActivity), activity)

    async def test_startup_failure_restores_pending_and_queued_drafts(self):
        self.configure_release.clear()
        self.fail_configure = True
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            entry = app.query_one(MessageInput)
            entry.text = "First"
            await pilot.press("enter")
            entry.text = "Second"
            await pilot.press("enter")
            entry.text = "Draft"
            self.configure_release.set()
            await self.until(pilot, lambda: not app.starting)
            await pilot.pause()
            self.assertFalse(app.busy)
            self.assertIsNone(app.startup_message)
            self.assertIsNone(app.pending_request)
            self.assertEqual(entry.text, "First\n\nSecond\nDraft")
            self.assertEqual(len(app.query(TurnActivity)), 0)
            self.assertEqual(self.requests, [])

    async def test_startup_cancel_never_sends_after_configuration(self):
        self.configure_release.clear()
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            entry = app.query_one(MessageInput)
            entry.text = "Not sent"
            image = ImagePaste(b"captured pixels", "image/png", "capture.png", "clipboard", 1, 1)
            entry.add_images((image,))
            original = entry.text
            await pilot.press("enter", "escape", "escape")
            self.assertFalse(app.busy)
            self.assertEqual(entry.text, original)
            self.assertEqual(entry.images, (image,))
            self.assertIsNone(app.startup_message)
            self.configure_release.set()
            await self.until(pilot, lambda: app.connected)
            self.assertEqual(self.requests, [])

    async def test_startup_sends_captured_images_once_and_keeps_next_message_queued(self):
        self.configure_release.clear()
        self.release.clear()
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            entry = app.query_one(MessageInput)
            entry.text = "First"
            image = ImagePaste(b"captured pixels", "image/png", "capture.png", "clipboard", 1, 1)
            entry.add_images((image,))
            await pilot.press("enter")
            entry.text = "Second"
            await pilot.press("enter")
            self.configure_release.set()
            await self.until(pilot, lambda: len(self.requests) == 1)
            self.assertEqual(self.requests[0]["images"], [image.payload()])
            self.assertEqual(app.queued_message.text, "Second")
            self.release.set()
            await self.until(pilot, lambda: len(self.requests) == 2 and not app.busy)
            self.assertEqual(self.requests[1]["text"], "Second")
            self.assertNotIn("images", self.requests[1])

    async def test_startup_command_preserves_draft_without_sending(self):
        self.configure_release.clear()
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            entry = app.query_one(MessageInput)
            entry.text = "/mcps"
            await pilot.press("enter")
            self.assertEqual(entry.text, "/mcps")
            self.assertIsNone(app.mcps_screen)
            self.assertFalse(app.busy)
            self.assertEqual(self.requests, [])

    async def test_conversation_tools_and_second_turn(self):
        app = SosApp(self.path)
        self.release.clear()
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            self.assertEqual(self.requests, [])
            self.assertEqual(str(app.query_one("#session-title", Static).render()), "-")
            entry = app.query_one(MessageInput)
            entry.text = "Question [bold] literal"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 1)
            self.assertFalse(entry.disabled)
            self.assertTrue(app.busy)
            self.assertEqual(entry.text, "")
            self.assertIsNone(app.session_id)
            pending_activity = app.query_one(TurnActivity)
            self.assertEqual(app.query_one(UserMessage).text, "Question [bold] literal")
            self.assertIs(app.running_activity, pending_activity)
            await self.until(pilot, lambda: pending_activity.toggle.spinner != "-")
            await pilot.press("D", "r", "a", "f", "t")
            self.assertEqual(entry.text, "Draft")
            self.assertEqual(len(self.requests), 1)
            self.assertEqual(str(app.query_one("#session-title", Static).render()), "-")
            self.assertEqual(self.requests[0]["workspace"], str(Path.cwd().resolve()))
            self.release.set()
            await self.until(pilot, lambda: app.session_id and not app.busy)
            await pilot.pause()
            self.assertEqual(len(app.query(ReadView)), 1)
            self.assertEqual(len(app.query(ToolView)), 0)
            text = self.log_text(app)
            self.assertEqual(text.count("Question [bold] literal"), 1)
            self.assertEqual(len(app.query(UserMessage)), 1)
            self.assertIs(app.query_one(TurnActivity), pending_activity)
            for expected in ("Reading the file", "read · test.txt", "Hello from the model"):
                self.assertIn(expected, text)
            self.assertNotIn("file contents", text)
            self.assertNotIn("DO_NOT_DISPLAY_BASE64", text)
            self.assertFalse(entry.disabled)
            self.assertEqual(entry.text, "Draft")
            self.assertEqual(str(app.query_one("#session-title", Static).render()), "Short generated title")
            entry.text = "Follow-up"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 2 and not app.busy)
            self.assertNotIn("workspace", self.requests[1])
            self.assertEqual(self.requests[1]["text"], "Follow-up")
            await pilot.press("q")
            self.assertEqual(entry.text, "q")

    async def test_queued_messages_send_once_after_execution_and_keep_the_new_draft(self):
        self.release.clear()
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "First turn"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 1)
            entry.text = "Queued one"
            await pilot.press("enter")
            entry.text = "Queued two"
            await pilot.press("enter")
            self.assertEqual(app.queued_message.text, "Queued one\nQueued two")
            self.assertEqual(len(self.requests), 1)
            entry.text = "Still editing this draft"
            app.set_mode("slop")
            self.release.set()
            await self.until(pilot, lambda: len(self.requests) == 2 and not app.busy)
            self.assertEqual(self.requests[1]["text"], "Queued one\nQueued two")
            self.assertEqual(self.requests[1]["mode"], "slop")
            self.assertNotIn("workspace", self.requests[1])
            self.assertEqual(entry.text, "Still editing this draft")
            self.assertIsNone(app.queued_message)
            self.assertEqual([message.text for message in app.query(UserMessage)], ["First turn", "Queued one\nQueued two"])

    async def test_request_error_keeps_text_for_retry(self):
        self.reject = True
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "Retry me"
            await pilot.press("enter")
            await self.until(pilot, lambda: self.requests and not app.busy)
            self.assertEqual(entry.text, "Retry me")
            self.assertIn("Missing API key", self.log_text(app))
            self.assertFalse(app.query_one(ModeButton).busy)
            self.assertFalse(entry.disabled)
            self.assertEqual(str(app.query_one("#session-title", Static).render()), "-")

    async def test_first_turn_title_failure_removes_preview_and_retry_has_no_duplicate(self):
        self.reject = True
        self.release.clear()
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "First request"
            await pilot.press("enter")
            await self.until(pilot, lambda: bool(self.requests))
            self.assertEqual(len(app.query(UserMessage)), 1)
            entry.text = "Next draft"
            self.release.set()
            await self.until(pilot, lambda: not app.busy)
            await pilot.pause()
            self.assertEqual(entry.text, "First request\n\nNext draft")
            self.assertEqual(len(app.query(UserMessage)), 0)
            self.assertEqual(len(app.query(TurnActivity)), 0)
            self.assertIsNone(app.current_activity)
            self.assertIsNone(app.pending_turn)
            self.reject = False
            entry.text = "First request"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.session_id is not None and not app.busy)
            self.assertEqual(len(app.query(UserMessage)), 1)
            self.assertEqual(len(app.query(TurnActivity)), 1)

    async def test_first_turn_cancel_before_acceptance_restores_draft_without_phantom_turn(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            with patch.object(app.client, "send", new_callable=AsyncMock):
                await app.on_message_input_submitted(MessageInput.Submitted("First request"))
            await pilot.pause()
            self.assertIsNotNone(app.pending_turn)
            app.query_one(MessageInput).text = "Next draft"
            app.handle_event({"type": "execution.cancelled", "requestId": app.pending_request, "messages": []})
            await pilot.pause()
            self.assertEqual(app.query_one(MessageInput).text, "First request\n\nNext draft")
            self.assertEqual(len(app.query(UserMessage)), 0)
            self.assertEqual(len(app.query(TurnActivity)), 0)
            self.assertIsNone(app.session_id)
            self.assertFalse(app.busy)
            self.assertIn("Execution cancelled", self.log_text(app))

    async def test_first_turn_send_failure_removes_widgets_even_before_they_finish_mounting(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            with patch.object(app.client, "send", new_callable=AsyncMock, side_effect=OSError("Send failed")):
                await app.on_message_input_submitted(MessageInput.Submitted("First request"))
            await pilot.pause()
            self.assertEqual(app.query_one(MessageInput).text, "First request")
            self.assertEqual(len(app.query(UserMessage)), 0)
            self.assertEqual(len(app.query(TurnActivity)), 0)
            self.assertFalse(app.busy)

    async def test_first_turn_disconnect_before_acceptance_removes_preview(self):
        self.release.clear()
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.query_one(MessageInput).text = "First request"
            await pilot.press("enter")
            await self.until(pilot, lambda: bool(self.requests))
            self.assertIsNotNone(app.pending_turn)
            for writer in list(self.writers):
                writer.close()
            await self.until(pilot, lambda: not app.connected)
            await pilot.pause()
            self.assertEqual(app.query_one(MessageInput).text, "First request")
            self.assertEqual(len(app.query(UserMessage)), 0)
            self.assertEqual(len(app.query(TurnActivity)), 0)

    async def test_execution_error_and_disconnect_are_visible(self):
        self.fail_execution = True
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.query_one(MessageInput).text = "Hello"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.session_id and not app.busy)
            self.assertIn("Provider failed", self.log_text(app))
            self.assertFalse(app.query_one(ModeButton).busy)
            for writer in list(self.writers):
                writer.close()
            await self.until(pilot, lambda: not app.connected)
            self.assertTrue(app.query_one(MessageInput).disabled)
            self.assertIn("Daemon disconnected", self.log_text(app))
            self.assertFalse(app.query_one(ModeButton).busy)

    async def test_missing_daemon(self):
        app = SosApp(str(Path(self.directory.name) / "missing.sock"))
        with patch("sos_tui.daemon_startup.BUNDLED_DAEMON", Path(self.directory.name) / "missing-sosd"):
            async with app.run_test() as pilot:
                await self.until(pilot, lambda: "Connection error" in self.log_text(app))
                self.assertFalse(app.connected)
                self.assertTrue(app.query_one(MessageInput).disabled)

    async def test_disconnect_automatically_reconnects_without_resending_draft(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "Keep this unsent draft"
            old_writers = set(self.writers)
            for writer in old_writers:
                writer.close()
            await self.until(pilot, lambda: not app.connected)
            await self.until(pilot, lambda: app.connected)
            self.assertTrue(set(self.writers) - old_writers)
            self.assertFalse(entry.disabled)
            self.assertEqual(entry.text, "Keep this unsent draft")
            self.assertEqual(self.requests, [])

    async def test_startup_connection_failure_is_retried(self):
        app = SosApp(self.path)
        connect = app.client.connect
        attempts = 0

        async def fail_once():
            nonlocal attempts
            attempts += 1
            if attempts == 1:
                raise ConnectionError("Daemon temporarily unavailable")
            await connect()

        with patch.object(app.client, "connect", side_effect=fail_once):
            async with app.run_test() as pilot:
                await self.until(pilot, lambda: app.connected)
                self.assertEqual(attempts, 2)
                self.assertFalse(app.query_one(MessageInput).disabled)

    async def test_reconnect_reattaches_session_and_preserves_draft(self):
        self.saved_sessions = [{"id": "saved", "title": "Existing conversation",
                                "workspace": "/project", "messages": []}]
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.session_id = "saved"
            entry = app.query_one(MessageInput)
            entry.text = "Unsent review notes"
            for writer in list(self.writers):
                writer.close()
            await self.until(pilot, lambda: not app.connected)
            await self.until(pilot, lambda: app.connected and app.workspace == "/project" and not entry.disabled)
            self.assertEqual(app.session_id, "saved")
            self.assertEqual(entry.text, "Unsent review notes")
            self.assertEqual([request["type"] for request in self.requests], ["session.attach"])

    async def test_client_replies_to_ping_and_says_goodbye_on_exit(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            writer = next(iter(self.writers))
            writer.write(b'{"type":"integration.ping","pingId":"first"}\n')
            await writer.drain()
            await self.until(pilot, lambda: bool(self.control_messages))
            self.assertEqual(self.control_messages[-1], {"type": "integration.pong", "pingId": "first"})
            self.assertEqual(self.requests, [])
        await asyncio.sleep(0.05)
        self.assertEqual(self.control_messages[-1], {"type": "integration.close"})

    async def test_external_editor_does_not_block_heartbeat(self):
        app = SosApp(self.path)
        release = threading.Event()
        entered = threading.Event()

        def edit(*args, **kwargs):
            entered.set()
            release.wait(timeout=3)
            return subprocess.CompletedProcess([], 0)

        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            with patch.dict(os.environ, {"EDITOR": "test-editor"}), \
                    patch.object(app, "suspend", side_effect=nullcontext), \
                    patch("sos_tui.app.subprocess.run", side_effect=edit):
                app.busy = True
                editing = asyncio.create_task(app.action_edit_external())
                try:
                    await self.until(pilot, entered.is_set)
                    writer = next(iter(self.writers))
                    writer.write(b'{"type":"integration.ping","pingId":"editor"}\n')
                    await writer.drain()
                    await self.until(pilot, lambda: any(m.get("pingId") == "editor" for m in self.control_messages))
                    self.assertFalse(editing.done())
                finally:
                    release.set()
                    await editing
                self.assertTrue(app.busy)

    async def test_disconnect_during_external_edit_keeps_recovered_send_and_new_draft(self):
        self.release.clear()
        app = SosApp(self.path)
        entered, release = threading.Event(), threading.Event()

        def editor(command, **kwargs):
            entered.set()
            if not release.wait(timeout=5):
                raise TimeoutError("Test editor was not released")
            Path(command[-1]).write_text("Edited next draft", encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)

        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "Unaccepted message"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 1)
            entry.text = "Queued message"
            await pilot.press("enter")
            entry.text = "Next draft"
            with patch.dict(os.environ, {"EDITOR": "test-editor"}), \
                    patch.object(app, "suspend", side_effect=nullcontext), \
                    patch("sos_tui.app.subprocess.run", side_effect=editor):
                editing = asyncio.create_task(app.action_edit_external())
                try:
                    self.assertTrue(await asyncio.to_thread(entered.wait, 2))
                    for writer in list(self.writers):
                        writer.close()
                    for _ in range(100):
                        if not app.connected:
                            break
                        await asyncio.sleep(0.01)
                    self.assertFalse(app.connected)
                    self.assertFalse(editing.done())
                finally:
                    release.set()
                    await asyncio.wait_for(editing, 3)
            await pilot.pause()
            self.assertEqual(entry.text, "Unaccepted message\n\nQueued message\nEdited next draft")
            self.assertIsNone(app.queued_message)
            self.assertTrue(entry.disabled)
            self.assertFalse(app.editing_external)
            self.assertEqual(len(self.requests), 1)

    async def test_layout_and_growing_editor(self):
        app = SosApp(self.path)
        async with app.run_test(size=(80, 24)) as pilot:
            await self.until(pilot, lambda: app.connected)
            await pilot.pause()
            entry = app.query_one(MessageInput)
            chat = app.query_one("#chat", VerticalScroll)
            self.assertEqual(len(app.query(Footer)), 0)
            self.assertEqual(app.query_one(Tabs).size.height, 1)
            self.assertEqual(entry.size.height, 1)
            self.assertEqual(app.query_one("#session-bar").region.bottom, 24)
            self.assertEqual(entry.region.bottom, 23)
            workspace = app.query_one("#workspace", Static)
            self.assertEqual(str(workspace.content), app.workspace)
            self.assertEqual(str(workspace.render()), compact_path(app.workspace, workspace.content_size.width))
            initial_chat_height = chat.size.height
            # Soft wrapping grows the editor without inserting newlines.
            entry.text = "x" * 170
            await pilot.pause()
            self.assertGreater(entry.size.height, 1)
            self.assertEqual(chat.size.height, initial_chat_height - entry.size.height + 1)
            await pilot.resize_terminal(240, 24)  # Discussion now uses the full width.
            await pilot.pause()
            self.assertEqual(entry.size.height, 1)
            entry.text = "\n".join(f"line {i}" for i in range(12))
            entry.move_cursor((11, 7))
            await pilot.pause()
            self.assertEqual(entry.size.height, 6)
            indicator = app.query_one("#mode-indicator", Static)
            self.assertEqual(indicator.size.height, entry.size.height)
            self.assertEqual(indicator.size.width, 1)
            self.assertEqual(entry.content_region.x - indicator.region.x, 2)
            self.assertGreater(entry.scroll_y, 0)
            entry.text = ""
            await pilot.pause()
            self.assertEqual(entry.size.height, 1)
            await pilot.press("a", "shift+enter", "b")
            self.assertEqual(entry.text, "a\nb")
            self.assertEqual(self.requests, [])
            await pilot.press("enter")
            await self.until(pilot, lambda: self.requests and not app.busy)
            self.assertEqual(self.requests[0]["text"], "a\nb")
            await pilot.pause()
            self.assertEqual(entry.size.height, 1)

    async def test_mode_toggle_and_user_message_colors_are_frozen_at_send(self):
        app = SosApp(self.path)
        self.release.clear()
        async with app.run_test(size=(60, 24)) as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            indicator = app.query_one("#mode-indicator", Static)
            self.assertEqual(str(indicator.render()), "")
            self.assertEqual(app.mode, "sane")
            self.assertEqual(indicator.styles.background.hex, SANE_COLOR.upper())
            await pilot.press("tab")
            self.assertEqual(app.mode, "slop")
            self.assertEqual(indicator.styles.background.hex, SLOP_COLOR.upper())
            self.assertTrue(entry.has_focus)
            entry.text = "A multiline message\n" + "wrapped words " * 12
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 1)
            self.assertEqual(self.requests[0]["mode"], "slop")
            # Switch while the daemon is still preparing the session.
            await pilot.press("tab")
            self.assertEqual(app.mode, "sane")
            self.release.set()
            await self.until(pilot, lambda: app.session_id and not app.busy)
            await pilot.pause()
            first = app.query_one(UserMessage)
            self.assertEqual(first.mode, "slop")
            band = first.query_one(".mode-band")
            body = first.query_one(".user-text")
            self.assertEqual(band.styles.background.hex, SLOP_COLOR.upper())
            self.assertEqual(band.size.width, 1)
            self.assertEqual(band.size.height, first.size.height)
            self.assertGreater(first.size.height, 2)
            self.assertEqual(body.content_region.x - first.content_region.x, 2)
            self.assertEqual(first.styles.background.a, 1)
            self.assertEqual(first.styles.background.hex, CONTENT_BACKGROUND_COLOR.upper())
            previous_height = first.size.height
            await pilot.resize_terminal(40, 24)
            await pilot.pause()
            self.assertGreater(first.size.height, previous_height)
            self.assertEqual(band.size.height, first.size.height)
            entry.text = "A second message"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 2 and not app.busy)
            await pilot.pause()
            messages = list(app.query(UserMessage))
            self.assertEqual([message.mode for message in messages], ["slop", "sane"])
            self.assertEqual(messages[1].query_one(".mode-band").styles.background.hex, SANE_COLOR.upper())
            await pilot.press("tab")
            self.assertEqual([message.mode for message in messages], ["slop", "sane"])
            self.assertNotIn("You\n", self.log_text(app))
            self.assertNotIn("Assistant\n", self.log_text(app))
            answer = next(widget for widget in app.query("#chat MarkdownParagraph")
                          if str(widget.render()) == "Hello from the model")
            self.assertEqual(answer.content_region.x, messages[0].content_region.x)

    async def test_none_reasoning_is_displayed_as_no_reasoning(self):
        self.runtime_info["reasoning"] = "none"
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            bar = app.query_one(AgentInfoBar)
            self.assertEqual(bar.labels["reasoning-info"], " | No reasoning")
            self.assertIsNone(bar.query_one("#model-details").tooltip)

    async def test_reasoning_is_collapsed_and_toggles_on_click(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.display_message({"role": "assistant", "content": [
                {"type": "reasoning", "text": "Hidden reasoning [literal]"},
                {"type": "text", "text": "Visible answer"},
            ]})
            await pilot.pause()
            reasoning = app.query_one(Collapsible)
            contents = reasoning.query_one("Contents")
            title = reasoning.query_one("CollapsibleTitle")
            self.assertTrue(reasoning.collapsed)
            self.assertFalse(contents.display)
            self.assertIn("▸", str(title.render()))
            self.assertIn("Visible answer", self.log_text(app))
            await pilot.click(app.query_one(ActivityToggle))
            await pilot.click("CollapsibleTitle")
            await pilot.pause()
            self.assertFalse(reasoning.collapsed)
            self.assertEqual(title.styles.background.hex, BACKGROUND_COLOR.upper())
            self.assertEqual(reasoning.styles.background_tint.a, 0)
            self.assertTrue(contents.display)
            self.assertIn("▾", str(title.render()))
            self.assertEqual(str(contents.query_one(Static).render()), "Hidden reasoning [literal]")
            await pilot.click("CollapsibleTitle")
            await pilot.pause()
            self.assertTrue(reasoning.collapsed)
            self.assertFalse(contents.display)
            app.display_message({"role": "assistant", "content": [
                {"type": "reasoning", "text": None}, {"type": "text", "text": "No reasoning available"},
            ]})
            await pilot.pause()
            self.assertEqual(len(app.query(Collapsible)), 1)

    async def test_agent_info_bar_context_and_mode_button(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 30)) as pilot:
            await self.until(pilot, lambda: app.connected)
            await pilot.pause()
            model = app.query_one("#model-info", Static)
            context = app.query_one("#context-info", Static)
            button = app.query_one("#mode-button")
            self.assertEqual(str(model.render()), "openai/test-model")
            self.assertEqual(str(app.query_one("#reasoning-info", Static).render()), " | medium")
            self.assertEqual(str(context.render()), "")
            self.assertFalse(context.display)
            self.assertEqual(str(app.query_one("#context-percent", Static).render()), "-")
            self.assertEqual(app.query_one("#agent-info").region.bottom, app.query_one("#composer").region.y)
            self.assertEqual(str(button.label), "Sane")
            self.assertEqual(button.styles.background.hex, SANE_COLOR.upper())
            self.assertEqual(button.region.width, AgentInfoBar.BUTTON_WIDTH)
            self.assertEqual(button.region.right, app.query_one("#discussion-column").region.right)
            self.assertEqual(button.styles.content_align_horizontal, "center")
            await pilot.click("#mode-button")
            self.assertFalse(self.requests)
            await pilot.press("tab")
            await pilot.pause()
            self.assertEqual(str(button.label), "Slop")
            self.assertEqual(button.styles.background.hex, SLOP_COLOR.upper())
            self.assertEqual(button.region.width, AgentInfoBar.BUTTON_WIDTH)
            self.assertEqual(button.region.right, 100)
            entry = app.query_one(MessageInput)
            entry.focus()
            entry.text = "Hello"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.session_id and not app.busy)
            await pilot.pause()
            self.assertEqual(str(context.render()), "1 250 / 128 000")
            context_group = app.query_one("#context-details")
            self.assertAlmostEqual(context_group.region.x + context_group.region.width / 2, 50, delta=1)
            self.assertEqual(str(app.query_one("#context-percent", Static).render()), "[1%]")
            app.handle_event({"type": "execution.completed", "info": {
                "model": "another-model", "reasoning": "high",
                "last_turn_used_context": None, "total_context": 64000}})
            self.assertEqual(str(context.render()), "— / 64 000")
            self.assertEqual(str(model.render()), "another-model")
            self.assertEqual(str(app.query_one("#reasoning-info", Static).render()), " | high")

    async def test_worktree_live_update_moves_workspace_without_resetting_chat_or_launch_root(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            launch = app.launch_workspace
            entry = app.query_one(MessageInput)
            entry.text = "Keep this draft"
            app.handle_event({"type": "session.updated", "info": {
                "workspace": "/worktrees/feature", "configuration_workspace": launch}})
            self.assertEqual(app.workspace, "/worktrees/feature")
            self.assertEqual(app.launch_workspace, launch)
            self.assertEqual(str(app.query_one("#workspace", Static).render()), "/worktrees/feature")
            self.assertEqual(entry.text, "Keep this draft")

    async def test_info_bar_hides_fields_in_priority_order_and_rounds_percent(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 30)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.set_mode("slop")  # Exercise the full-width bar independently of the Objective Plan split.
            app.update_agent_info({"model": "test-model", "reasoning": "medium",
                                   "last_turn_used_context": 1250, "total_context": 10000})
            await pilot.pause()
            percent = app.query_one("#context-percent", Static)
            self.assertEqual(str(percent.render()), "[13%]")
            fields = ("model-info", "reasoning-info", "context-info", "context-percent")
            cases = [
                (100, set(fields)),
                (AgentInfoBar.BUTTON_WIDTH + 37, {"model-info", "reasoning-info", "context-percent"}),
                (AgentInfoBar.BUTTON_WIDTH + 22, {"model-info", "context-percent"}),
                (AgentInfoBar.BUTTON_WIDTH + 15, {"model-info"}),
                (AgentInfoBar.BUTTON_WIDTH + 7, set()),
                (AgentInfoBar.BUTTON_WIDTH - 3, set()),
                (100, set(fields)),
            ]
            for width, expected in cases:
                await pilot.resize_terminal(width, 30)
                await pilot.pause()
                self.assertEqual({id for id in fields if app.query_one("#" + id).display}, expected, width)
                button = app.query_one("#mode-button")
                self.assertEqual(button.region.right, width)
                self.assertEqual(button.region.width, min(AgentInfoBar.BUTTON_WIDTH, width - 1))
            app.update_agent_info({"last_turn_used_context": 1249, "total_context": 10000})
            self.assertEqual(str(percent.render()), "[12%]")
            app.update_agent_info({"last_turn_used_context": 0, "total_context": 10000})
            self.assertEqual(str(percent.render()), "[0%]")
            app.update_agent_info({"last_turn_used_context": None, "total_context": 10000})
            self.assertEqual(str(percent.render()), "[—]")
            app.update_agent_info({"last_turn_used_context": 10, "total_context": 0})
            self.assertEqual(str(percent.render()), "[—]")

    async def test_loading_scan_encodes_decodes_and_restores_label(self):
        app = SosApp(self.path)
        self.release.clear()
        async with app.run_test(size=(100, 30)) as pilot:
            await self.until(pilot, lambda: app.connected)
            button = app.query_one(ModeButton)
            self.assertFalse(button.busy)
            app.query_one(MessageInput).text = "Hello"
            await pilot.press("enter")
            await self.until(pilot, lambda: bool(self.requests))
            self.assertTrue(button.busy)
            button.animation_timer.pause()
            with patch("sos_tui.mode_button.monotonic", return_value=button.scan_started + 0.45):
                first_frame = button.render()
                self.assertNotEqual(first_frame.plain.strip(), "Cancel")
                self.assertEqual(first_frame.cell_len, AgentInfoBar.BUTTON_WIDTH)
                self.assertGreater(len({span.style.bgcolor for span in first_frame.spans}), 1)
                self.assertEqual(button.render_line(0).text, first_frame.plain)
            with patch("sos_tui.mode_button.monotonic", return_value=button.scan_started + 2.0):
                self.assertEqual(button.render().plain.strip(), "Cancel")
            await pilot.press("tab")
            self.assertTrue(button.busy)
            self.assertEqual(button.region.width, AgentInfoBar.BUTTON_WIDTH)
            with patch("sos_tui.mode_button.monotonic", return_value=button.scan_started + 1.0):
                encoded = button.render()
                self.assertNotEqual(encoded.plain.strip(), "Cancel")
                self.assertTrue(all(span.style.bgcolor.get_truecolor().hex.lower() == SLOP_COLOR.lower()
                                    for span in encoded.spans))
            await pilot.hover(button)
            self.assertEqual(button.render().plain.strip(), "Cancel")
            await pilot.hover(MessageInput)
            self.release.set()
            await self.until(pilot, lambda: app.session_id and not app.busy)
            await pilot.pause()
            self.assertFalse(button.busy)
            self.assertEqual(button.render().plain.strip(), "Slop")
            self.assertEqual(button.render_line(0).text, "     Slop      ")
            self.assertEqual(len({span.style.bgcolor for span in button.render().spans}), 1)
            phase = button.phase
            await pilot.pause(0.1)
            self.assertEqual(button.phase, phase)  # Timer is suspended at rest.

    async def open_connect(self, app, pilot, provider, method):
        await app.on_message_input_submitted(MessageInput.Submitted("/connect"))
        await self.until(pilot, lambda: app.connect_screen is not None and bool(app.connect_screen.providers) and not app.connect_screen.pending)
        screen = app.connect_screen
        self.assertEqual(screen.query_one("#connect-title").styles.color.hex, BOLD_MD.upper())
        self.assertEqual(screen.query_one("#connect-dialog").styles.border_top, ("solid", screen.query_one("#connect-details").styles.color))
        options = screen.query_one("#connect-providers", OptionList)
        options.highlighted = 0 if provider == "openai" else 1
        options.action_select()
        await pilot.pause()
        options = screen.query_one("#connect-methods", OptionList)
        options.highlighted = (0 if method == "browser" else 1 if method == "device" else 2) if provider == "openai" else 0
        options.action_select()
        await pilot.pause()
        return screen

    async def test_model_search_navigation_and_selection_preserve_conversation(self):
        self.models = [{"provider": "openai", "model": "gpt-5.4", "auth_method": "device"},
                       {"provider": "openai", "model": "gpt-4.1", "auth_method": "device"},
                       {"provider": "openrouter", "model": "gpt-5.4", "auth_method": "api", "max_context": 128000}]
        self.runtime_info.update(provider="openai", model="gpt-4.1")
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.session_id = "existing-session"
            app.set_mode("slop")
            app.append_chat("", "Keep this conversation")
            entry = app.query_one(MessageInput)
            entry.text = "/model"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.model_screen is not None and not app.model_screen.catalog_loading)
            screen = app.model_screen
            options = screen.query_one(OptionList)
            search = screen.query_one(Input)
            self.assertTrue(search.has_focus)
            self.assertEqual(options.option_count, 3)
            self.assertEqual(options.highlighted, 0)
            self.assertEqual(screen.provider_label(self.models[0]), "Openai (Codex)")
            self.assertTrue(row_text(options.get_option_at_index(1).prompt).endswith("Current"))
            self.assertNotIn("Current", row_text(options.get_option_at_index(0).prompt))
            self.assertNotIn("Current", row_text(options.get_option_at_index(2).prompt))
            self.assertEqual(screen.provider_label(self.models[2]), "Openrouter (Api key)")
            self.assertEqual(screen.provider_label({"provider": "openai", "auth_method": "api"}), "Openai (Api key)")
            console = Console(width=80, color_system=None)
            with console.capture() as capture:
                console.print(options.get_option_at_index(0).prompt)
                console.print(options.get_option_at_index(2).prompt)
            rows = capture.get().splitlines()
            self.assertEqual(rows[0].index("Openai"), rows[1].index("Openrouter"))
            self.assertIn("Openai (Codex)", rows[0])
            self.assertIn("Openrouter (Api key)", rows[1])
            self.assertEqual(screen.query_one(".popup-heading").styles.color.hex, BOLD_MD.upper())
            self.assertEqual(options.get_component_rich_style("option-list--option-highlighted").bgcolor.triplet.hex,
                             DEPTH_MD)
            await pilot.press("G", "P", "T", "-", "5")
            self.assertEqual(options.option_count, 2)
            self.assertEqual(screen.visible_models, [self.models[0], self.models[2]])
            self.assertTrue(all("Current" not in row_text(options.get_option_at_index(i).prompt) for i in range(2)))
            await pilot.press("down", "up", "down")
            self.assertEqual(screen.highlighted_model(), self.models[2])
            # A new search returns to the first (newest) match, even if the
            # previously highlighted older/provider result still matches.
            search.value = "gpt"
            await pilot.pause()
            self.assertEqual(screen.visible_models, self.models)
            self.assertEqual(screen.highlighted_model(), self.models[0])
            search.value = "gpt-5"
            await pilot.pause()
            self.assertEqual(screen.highlighted_model(), self.models[0])
            await pilot.press("down")
            self.assertTrue(search.has_focus)
            await pilot.press("tab")
            self.assertTrue(options.has_focus)
            self.assertEqual(app.mode, "slop")
            await pilot.press("enter")
            field = screen.query_one("#model-context", Input)
            self.assertTrue(field.has_focus)
            self.assertEqual(field.value, "")
            self.assertEqual(field.placeholder, "128000")
            for invalid in ("32767", "128001", "", "-32768"):
                field.value = invalid
                await pilot.press("enter")
                self.assertFalse(screen.selecting)
                self.assertEqual(self.requests[-1]["type"], "model.list")
            field.value = "65536"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.model_screen is None)
            self.assertEqual(self.requests[-1]["context_limit"], 65536)
            self.assertEqual(self.requests[-1]["type"], "model.select")
            self.assertEqual(self.requests[-1]["provider"], "openrouter")
            self.assertEqual(self.requests[-1]["model"], "gpt-5.4")
            self.assertNotIn("auth_method", self.requests[-1])
            self.assertEqual(app.runtime_info["provider"], "openrouter")
            self.assertEqual(app.runtime_info["reasoning"], "medium")
            self.assertEqual(app.session_id, "existing-session")
            self.assertEqual(app.mode, "slop")
            self.assertIn("Keep this conversation", self.log_text(app))
            self.assertNotIn("/model", self.log_text(app))
            self.assertTrue(entry.has_focus)

    async def test_model_budget_uses_maximum_placeholder_and_escape_does_not_save(self):
        self.models = [{"provider": "openai", "model": "gpt-6-astra", "max_context": 872000},
                       {"provider": "openai", "model": "gpt-4.1", "max_context": 128000}]
        self.runtime_info.update(provider="openai", model="gpt-6-astra", context_limit=150000)
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.query_one(MessageInput).text = "/model"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.model_screen is not None and not app.model_screen.catalog_loading)
            screen = app.model_screen
            await pilot.press("enter")
            field = screen.query_one("#model-context", Input)
            self.assertEqual(field.value, "")
            self.assertEqual(field.placeholder, "872000")
            self.assertTrue(field.has_focus)
            self.assertEqual(field.region.height, 1)
            self.assertEqual(field.styles.border.top[0], "")
            self.assertEqual(field.styles.border.bottom[0], "")
            self.assertEqual(field.styles.border.left[0], "")
            self.assertEqual(field.styles.border.right[0], "")
            screen.set_focus(None)
            await pilot.pause()
            self.assertEqual(field.region.height, 1)
            self.assertEqual(field.styles.border.top[0], "")
            field.focus()
            field.value = "65536"
            await pilot.press("escape")
            self.assertIsNone(screen.pending_model)
            await pilot.press("down", "enter")
            self.assertEqual(field.value, "")
            self.assertEqual(field.placeholder, "128000")
            await pilot.press("escape", "escape")
            await self.until(pilot, lambda: app.model_screen is None)
            self.assertFalse(any(request["type"] == "model.select" for request in self.requests))
            self.assertEqual(app.runtime_info["context_limit"], 150000)

    async def test_model_literal_search_empty_results_and_failed_selection(self):
        self.models = [{"provider": "openai", "model": "gpt-5.4"},
                       {"provider": "openrouter", "model": "vendor/model[1]"}]
        self.catalog_errors = [{"provider": "openai", "error": "Catalog temporarily unavailable"}]
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.query_one(MessageInput).text = "/model"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.model_screen is not None and not app.model_screen.catalog_loading)
            screen = app.model_screen
            search = screen.query_one(Input)
            options = screen.query_one(OptionList)
            self.assertIn("Catalog temporarily unavailable", str(screen.query_one("#model-errors").render()))
            search.value = "does not exist"
            await pilot.pause()
            self.assertEqual(options.option_count, 0)
            await pilot.press("enter")
            self.assertEqual(len(self.requests), 1)
            search.value = "ROUTER"
            await pilot.pause()
            self.assertEqual(screen.visible_models, [self.models[1]])
            search.value = "[1]"
            await pilot.pause()
            self.assertEqual(screen.visible_models, [self.models[1]])
            self.reject = True
            await pilot.press("enter")
            field = screen.query_one("#model-context", Input)
            self.assertEqual(field.value, "")
            field.value = "32768"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 2 and not screen.selecting)
            self.assertIn("Missing API key", str(screen.query_one("#model-errors").render()))
            self.assertEqual(app.runtime_info["model"], "openai/test-model")
            self.assertFalse(search.disabled)
            self.reject = False
            await pilot.press("enter")
            await self.until(pilot, lambda: app.model_screen is None)
            self.assertEqual(app.runtime_info["model"], "vendor/model[1]")

    async def test_model_close_during_loading_ignores_late_reply_and_empty_list(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            self.release.clear()
            app.query_one(MessageInput).text = "/model"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 1)
            await pilot.press("escape")
            await self.until(pilot, lambda: app.model_screen is None)
            self.release.set()
            await self.until(pilot, lambda: len(self.requests) == 2 and not app.model_requests)
            self.assertEqual(self.requests[1]["type"], "model.cancel")
            self.assertEqual(app.runtime_info["model"], "openai/test-model")
            app.query_one(MessageInput).text = "/model"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.model_screen is not None and not app.model_screen.catalog_loading)
            self.assertEqual(app.model_screen.query_one(OptionList).option_count, 0)
            self.assertIn("/connect", str(app.model_screen.query_one("#model-status").render()))
            await pilot.press("enter", "escape")
            await self.until(pilot, lambda: app.model_screen is None)
            self.assertFalse(any(request["type"] == "model.select" for request in self.requests))

    async def test_model_loading_keeps_dialog_visible_and_renders_large_catalog(self):
        self.models = [{"provider": "openrouter", "model": f"vendor/model-{i}"} for i in range(400)]
        app = SosApp(self.path)
        async with app.run_test(size=(100, 30)) as pilot:
            await self.until(pilot, lambda: app.connected)
            self.release.clear()
            app.query_one(MessageInput).text = "/model"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 1)
            screen = app.model_screen
            self.assertTrue(screen.catalog_loading)
            self.assertFalse(screen.loading)
            self.assertEqual(len(screen.query("LoadingIndicator")), 0)
            self.assertIn("Select&#160;a&#160;model", app.export_screenshot())
            search = screen.query_one(Input)
            self.assertTrue(search.has_focus)
            await pilot.press("m", "o", "d", "e", "l", "-", "3", "9")
            self.release.set()
            await self.until(pilot, lambda: not screen.catalog_loading)
            await pilot.pause()
            self.assertEqual(screen.query_one(OptionList).option_count, 11)
            self.assertIn("vendor/model-39", app.export_screenshot())
            await pilot.press("escape")
            await self.until(pilot, lambda: app.model_screen is None)

    async def test_model_disconnection_allows_closing_picker(self):
        self.models = [{"provider": "openai", "model": "gpt-5.4"}]
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.query_one(MessageInput).text = "/model"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.model_screen is not None and not app.model_screen.catalog_loading)
            for writer in list(self.writers):
                writer.close()
            await self.until(pilot, lambda: not app.connected)
            self.assertIn("Daemon disconnected", str(app.model_screen.query_one("#model-errors").render()))
            await pilot.press("escape")
            await self.until(pilot, lambda: app.model_screen is None)

    async def test_connect_api_keys_are_masked_and_not_added_to_conversation(self):
        for provider in ("openrouter", "openai"):
            app = SosApp(self.path)
            async with app.run_test(size=(100, 45)) as pilot:
                await self.until(pilot, lambda: app.connected)
                app.session_id = "old-session"
                app.display_message({"role": "user", "content": [{"type": "text", "text": "Old conversation"}]})
                screen = await self.open_connect(app, pilot, provider, "api")
                key = screen.query_one("#connect-key", Input)
                self.assertTrue(key.password)
                key.value = "fake-secret-key"
                await pilot.press("enter")
                await self.until(pilot, lambda: app.connect_screen is None and app.session_id is None)
                self.assertEqual(key.value, "")
                self.assertEqual(app.runtime_info["provider"], provider)
                self.assertEqual(len(app.query_one("#chat").children), 0)
                self.assertNotIn("fake-secret-key", self.log_text(app))
                self.assertTrue(any(r["type"] == "auth.api" and r["provider"] == provider and r["key"] == "fake-secret-key"
                                    for r in self.requests))
                self.assertFalse(any(r["type"] == "message.send" for r in self.requests))

    async def test_connect_initial_method_and_keyboard_return_to_providers(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 45)) as pilot:
            await self.until(pilot, lambda: app.connected)
            await app.on_message_input_submitted(MessageInput.Submitted("/connect"))
            await self.until(pilot, lambda: app.connect_screen is not None and bool(app.connect_screen.providers))
            screen = app.connect_screen
            await pilot.press("enter")
            self.assertEqual(screen.provider, "openai")
            self.assertEqual(screen.method, "browser")
            self.assertEqual(screen.query_one("#connect-methods", OptionList).highlighted, 0)
            self.assertFalse(screen.query("#connect-submit"))
            self.assertEqual(len(screen.query("#connect-model, #connect-model-label")), 0)
            await pilot.press("shift+tab", "down", "enter")
            self.assertEqual(screen.provider, "openrouter")
            self.assertEqual(screen.method, "api")
            screen.query_one("#connect-providers", OptionList).focus()
            await pilot.press("up", "enter", "left", "down", "enter")
            self.assertEqual(screen.provider, "openrouter")

    async def test_connect_oauth_polls_and_selects_only_after_authorization(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 45)) as pilot:
            await self.until(pilot, lambda: app.connected)
            screen = await self.open_connect(app, pilot, "openai", "device")
            await self.until(pilot, lambda: bool(screen.auth_id))
            self.assertIn("TEST-1234", str(screen.query_one("#connect-details", Static).render()))
            self.assertFalse(any(r["type"] == "provider.select" for r in self.requests))
            await self.until(pilot, lambda: app.connect_screen is None)
            self.assertGreaterEqual(self.auth_polls, 2)
            self.assertEqual(app.runtime_info["provider"], "openai")

    async def test_connect_cancel_and_failure_preserve_current_conversation(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 45)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.session_id = "old-session"
            app.display_message({"role": "user", "content": [{"type": "text", "text": "Keep this"}]})
            screen = await self.open_connect(app, pilot, "openai", "browser")
            await self.until(pilot, lambda: bool(screen.auth_id))
            await pilot.press("escape")
            await self.until(pilot, lambda: not screen.returning)
            self.assertIs(app.connect_screen, screen)
            self.assertEqual(screen.provider, "")
            self.assertEqual(screen.url, "")
            for selector in ("#connect-methods", "#connect-method-picker", "#connect-key", "#connect-browser"):
                self.assertFalse(screen.query_one(selector).display)
            self.assertNotIn("TEST-1234", str(screen.query_one("#connect-details", Static).render()))
            await pilot.press("escape")
            await self.until(pilot, lambda: app.connect_screen is None)
            self.assertEqual(app.session_id, "old-session")
            self.assertTrue(any(r["type"] == "auth.cancel" and r["authId"] == "flow-1" for r in self.requests))
            self.assertFalse(any(r["type"] == "provider.select" for r in self.requests))
            screen = await self.open_connect(app, pilot, "openrouter", "api")
            self.reject = True
            screen.query_one("#connect-key", Input).value = "fake-secret-key"
            await pilot.press("enter")
            await self.until(pilot, lambda: not screen.pending)
            self.assertEqual(app.session_id, "old-session")
            self.assertNotIn("fake-secret-key", self.log_text(app))
            self.assertEqual(screen.query_one("#connect-key", Input).value, "")

    async def test_dialog_buttons_keep_flat_feedback_when_focused_or_pressed(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 45)) as pilot:
            await self.until(pilot, lambda: app.connected)
            screen = await self.open_connect(app, pilot, "openai", "api")
            screen.query_one("#connect-browser").display = True
            for selector in ("#connect-browser",):
                button = screen.query_one(selector, Button)
                button.focus()
                await pilot.pause()
                for active in (False, True):
                    button.set_class(active, "-active")
                    await pilot.pause()
                    self.assertFalse(button.styles.text_style.reverse)
                    self.assertEqual(button.styles.background.hex, "#3A4655" if active else DEPTH_MD.upper())
                    self.assertEqual(button.styles.color.hex, TEXT_COLOR.upper())
                    self.assertEqual(button.region.height, 1)
                    self.assertEqual(button.styles.tint.a, 0)
                    self.assertEqual(button.styles.background_tint.a, 0)
                button.remove_class("-active")

    async def test_connect_shows_key_check_and_does_not_select_a_provider_on_failure(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 45)) as pilot:
            await self.until(pilot, lambda: app.connected)
            screen = await self.open_connect(app, pilot, "openrouter", "api")
            self.release.clear()
            self.reject = True
            screen.query_one("#connect-key", Input).value = "rejected-key"
            await pilot.press("enter")
            self.assertIn("Checking API key", str(screen.query_one("#connect-details", Static).render()))
            self.assertTrue(screen.pending)
            self.assertTrue(screen.query_one("#connect-providers", OptionList).disabled)
            self.release.set()
            await self.until(pilot, lambda: not screen.pending)
            self.assertIs(app.connect_screen, screen)
            self.assertFalse(any(r["type"] == "provider.select" for r in self.requests))
            self.assertNotIn("rejected-key", self.log_text(app))

    async def test_connected_methods_and_horizontal_button_navigation(self):
        self.connected_methods = {"openai": "device", "openrouter": "api"}
        app = SosApp(self.path)
        async with app.run_test(size=(100, 45)) as pilot:
            await self.until(pilot, lambda: app.connected)
            await app.on_message_input_submitted(MessageInput.Submitted("/connect"))
            await self.until(pilot, lambda: app.connect_screen is not None and bool(app.connect_screen.providers))
            screen = app.connect_screen
            providers = screen.query_one("#connect-providers", OptionList)
            self.assertTrue(all(row_text(providers.get_option_at_index(i).prompt).endswith("Connected") for i in range(2)))
            await pilot.press("enter")
            methods = screen.query_one("#connect-methods", OptionList)
            self.assertEqual(screen.method, "device")
            self.assertEqual(methods.highlighted, 1)
            self.assertNotIn("Connected", row_text(methods.get_option_at_index(0).prompt))
            self.assertTrue(row_text(methods.get_option_at_index(1).prompt).endswith("Connected"))
            self.assertNotIn("Connected", str(screen.query_one("#connect-method-label", Static).render()))
            self.assertNotIn("Connected", row_text(methods.get_option_at_index(2).prompt))
            methods.highlighted = 2
            methods.action_select()
            await pilot.pause()
            key = screen.query_one("#connect-key", Input)
            self.assertEqual(key.styles.border.spacing.height, 0)
            self.assertEqual(key.styles.border.spacing.width, 0)
            key.value = "test-key"
            key.cursor_position = 4
            await pilot.press("left")
            self.assertIs(screen.focused, key)
            self.assertEqual(key.cursor_position, 3)
            await pilot.press("right")
            self.assertEqual(key.cursor_position, 4)
            self.assertFalse(screen.query("#connect-submit, #connect-cancel"))
            browser = screen.query_one("#connect-browser", Button)
            browser.display = True
            browser.focus()
            await pilot.press("right", "left")
            self.assertIs(screen.focused, browser)
            screen.back_to_providers()
            screen.choose_provider("openrouter")
            self.assertNotIn("Connected", str(screen.query_one("#connect-key-label", Static).render()))
            self.assertTrue(row_text(providers.get_option_at_index(1).prompt).endswith("Connected"))
            self.assertFalse(methods.display)

    async def test_connect_replaces_existing_auth_and_escape_clears_the_second_step(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 45)) as pilot:
            await self.until(pilot, lambda: app.connected)
            screen = await self.open_connect(app, pilot, "openai", "api")
            screen.providers["openai"]["connected"] = True
            screen.providers["openrouter"]["connected"] = True
            screen.choose_provider("openai")
            methods = screen.query_one("#connect-methods", OptionList)
            self.assertEqual([methods.get_option_at_index(i).id for i in range(methods.option_count)], ["browser", "device", "api"])
            await pilot.press("escape")
            self.assertIs(app.connect_screen, screen)
            self.assertFalse(methods.display)
            self.assertEqual(screen.provider, "")
            screen.choose_provider("openrouter")
            self.assertEqual(screen.method, "api")
            self.assertFalse(methods.display)
            self.assertFalse(screen.query_one("#connect-method-picker").display)
            key = screen.query_one("#connect-key", Input)
            self.assertTrue(key.display)
            key.value = "discard-this-key"
            await pilot.pause()
            await pilot.press("escape")
            self.assertIs(app.connect_screen, screen)
            self.assertEqual(key.value, "")
            self.assertFalse(key.display)
            self.assertFalse(any(r["type"] == "auth.api" for r in self.requests))
            await pilot.press("escape")
            self.assertIsNone(app.connect_screen)

    async def test_markdown_response_and_reasoning_palette_with_plain_user_and_tools(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.display_message({"role": "user", "content": [{"type": "text", "text": "**Literal user**"}]})
            app.display_message({"role": "assistant", "content": [
                {"type": "reasoning", "text": "**Literal reasoning**"},
                {"type": "text", "text": "# Heading\n\n**Bold** and *emphasis* with `code` and [link](https://example.com).\n\n- Item\n\n```python\nprint('hello')\n```"},
            ]})
            app.display_message({"role": "tool", "content": [{"type": "tool_result", "callId": "call",
                                "output": "**Literal tool**", "isError": False}]})
            await pilot.pause()
            app.query_one(ToolView).collapsed = False
            await pilot.pause()
            self.assertEqual(len(app.query_one("#chat").query(Markdown)), 2)
            markdown = app.query_one("#chat > Markdown", Markdown)
            heading = markdown.query_one("MarkdownH1")
            self.assertEqual(heading.styles.color.hex, TITLE_MD.upper())
            paragraph = markdown.query_one("MarkdownParagraph")
            self.assertEqual(paragraph.get_component_styles("strong").color.hex, BOLD_MD.upper())
            self.assertEqual(paragraph.get_component_styles("code_inline").color.hex, MISC_MD.upper())
            self.assertEqual(paragraph.get_component_styles("em").color.hex, MISC_MD.upper())
            self.assertEqual(markdown.styles.link_color.hex, DEPTH_MD.upper())
            self.assertEqual(len(markdown.query("MarkdownFence")), 1)
            self.assertEqual(len(markdown.query(CodeFence)), 1)
            self.assertEqual(markdown.query_one(CodeFence).styles.background.hex, CODE_BACKGROUND_COLOR.upper())
            self.assertIn("**Literal user**", self.log_text(app))
            reasoning = app.query_one(".reasoning MarkdownParagraph")
            self.assertIn("Literal reasoning", str(reasoning.render()))
            self.assertNotIn("**", str(reasoning.render()))
            self.assertEqual(reasoning.get_component_styles("strong").color.hex, BOLD_MD.upper())
            self.assertIn("**Literal tool**", self.log_text(app))
            self.assertIn("Bold and emphasis", str(paragraph.render()))

    async def test_code_highlighting_uses_only_our_palette_including_unknown_languages(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            allowed = {color.lower() for color in (TEXT_COLOR, TITLE_MD, BOLD_MD, MISC_MD, DEPTH_MD, ALGAE_MD, MUTED_COLOR)}
            for language, code in [
                ("python", '# comment\nclass Beach:\n    def wave(self):\n        return "algae" + str(42)\n'),
                ("json", '{"algae": true, "depth": 12}'),
                ("diff", "-old\n+new\n"),
                ("unknown-language", "unclassified code"),
            ]:
                for ansi in (False, True):
                    highlighted = CodeFence.highlight(code, language, ansi=ansi, dark=True)
                    segments = [segment for segment in highlighted.render_segments() if segment.text]
                    self.assertTrue(segments)
                    for segment in segments:
                        self.assertIn(segment.style.color.name, allowed)
            code = 'value = "algae"'
            highlighted = CodeFence.highlight(code, "python")
            segments = highlighted.render_segments()
            string_segments = [segment for segment in segments if "algae" in segment.text]
            self.assertTrue(string_segments)
            self.assertTrue(all(segment.style.color.name == ALGAE_MD.lower() for segment in string_segments))

    async def test_markdown_tables_and_all_blocks_use_only_our_palette(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 60)) as pilot:
            await self.until(pilot, lambda: app.connected)
            source = "\n\n".join("#" * level + " Heading" for level in range(1, 7))
            source += "\n\n---\n\n> Quote\n\n- Item\n\n| Name | Value |\n| --- | --- |\n| [Link](https://example.com) | **Bold** |\n\n```python\nvalue = 'algae'\n```"
            app.display_message({"role": "assistant", "content": [{"type": "text", "text": source}]})
            await pilot.pause()
            markdown = app.query_one(Markdown)
            palette = {color.upper() for color in (TEXT_COLOR, TITLE_MD, BOLD_MD, MISC_MD, DEPTH_MD, ALGAE_MD)}
            for theme in ("textual-dark", "textual-light"):
                app.theme = theme
                await pilot.pause()
                for widget in [markdown, *markdown.query("*")]:
                    self.assertIn(widget.styles.color.hex, palette, str(widget))
                    self.assertEqual(widget.styles.link_color.hex, DEPTH_MD.upper())
                    self.assertEqual(widget.styles.link_color_hover.hex, DEPTH_MD.upper())
                    self.assertEqual(widget.styles.link_background_hover.a, 0)
                    if widget.styles.background.a:
                        self.assertIn(widget.styles.background.hex, {BACKGROUND_COLOR.upper(), CODE_BACKGROUND_COLOR.upper()})
                headers = list(markdown.query("MarkdownTableContent > .header"))
                self.assertEqual(len(headers), 2)
                for header in headers:
                    self.assertEqual(header.styles.color.hex, TITLE_MD.upper())
                self.assertEqual(markdown.query_one("MarkdownTableContent").styles.keyline[1].hex, DEPTH_MD.upper())
                self.assertEqual(markdown.query_one("MarkdownHorizontalRule").styles.border_bottom[1].hex, MISC_MD.upper())

    async def test_tool_panels_correlate_calls_update_in_place_and_use_coral(self):
        app = SosApp(self.path)
        async with app.run_test(size=(110, 50)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.display_message({"role": "assistant", "content": [
                {"type": "tool_call", "callId": "edit-1", "toolName": "edit", "arguments": {"filePath": "a.cpp"}},
                {"type": "tool_call", "callId": "edit-2", "toolName": "edit", "arguments": {"filePath": "b.cpp"}},
            ]})
            await pilot.pause()
            first, second = list(app.query(ToolView))
            self.assertTrue(first.collapsed)
            title = first.query_one("CollapsibleTitle")
            self.assertEqual(title.styles.color.hex, BOLD_MD.upper())
            self.assertEqual(title.styles.background.hex, BACKGROUND_COLOR.upper())
            await pilot.click(app.query_one(ActivityToggle))
            await pilot.click(title)
            self.assertFalse(first.collapsed)
            self.assertEqual(title.styles.background.hex, BACKGROUND_COLOR.upper())
            self.assertEqual(first.styles.background_tint.a, 0)
            app.display_message({"role": "tool", "content": [{"type": "tool_result", "callId": "edit-2",
                "output": "second result", "isError": False, "metadata": {}}]})
            app.display_message({"role": "tool", "content": [{"type": "tool_result", "callId": "edit-1",
                "output": "Edit applied.", "isError": False,
                "metadata": {"diff": "-int value = 1;\n+int value = 2;"}}]})
            await pilot.pause()
            self.assertEqual(len(app.query(ToolView)), 2)
            self.assertFalse(first.collapsed)
            self.assertTrue(first.rendered)
            self.assertFalse(second.rendered)  # Formatting is deferred until expanded.
            self.assertEqual(first.result["output"], "Edit applied.")
            self.assertEqual(second.result["output"], "second result")
            text = self.log_text(app)
            self.assertNotIn("Arguments", text)
            self.assertNotIn("Results", text)
            self.assertIn("a.cpp", text)
            self.assertIn("-int value = 1;", text)
            self.assertIn("+int value = 2;", text)
            self.assertEqual(app.pending_tools, {})

    async def test_tool_result_formats_diffs_shell_search_web_and_errors(self):
        app = SosApp(self.path)
        cases = [
            ("edit", {"filePath": "a.cpp", "oldString": "old", "newString": "new"},
             "Edit applied successfully.", {"diff": "--- a.cpp\n+++ a.cpp\n-old\n+new\n"}, False, "+new"),
            ("write", {"filePath": "a.cpp", "content": "hello"}, "Wrote file successfully.",
              {"diff": "+hello\n", "filepath": "a.cpp"}, False, "hello"),
            ("apply_patch", {"patchText": "*** Begin Patch\n*** End Patch"}, "Success.",
             {"diff": "-before\n+after"}, False, "+after"),
            ("bash", {"command": "exit 2"}, "output\n\n<shell_metadata>\nExit code: 2\n</shell_metadata>",
             {"exit": 2, "timedOut": False}, False, "Exit code: 2"),
            ("glob", {"pattern": "*.cpp"}, "a.cpp\nb.cpp", {}, False, "b.cpp"),
            ("grep", {"pattern": "value"}, "a.cpp:\n  Line 2: value", {}, False, "Line 2: value"),
            ("webfetch", {"url": "https://example.com", "format": "markdown"}, "# Fetched page",
             {"url": "https://example.com"}, False, "Fetched page"),
            ("edit", {"filePath": "missing"}, "File not found [literal]", {}, True, "File not found [literal]"),
            ("unknown", {}, {"custom": "value"}, {}, False, '"custom": "value"'),
        ]
        async with app.run_test(size=(110, 50)) as pilot:
            await self.until(pilot, lambda: app.connected)
            for index, (name, arguments, output, metadata, error, expected) in enumerate(cases):
                call_id = f"call-{index}"
                app.display_message({"role": "assistant", "content": [{"type": "tool_call", "callId": call_id,
                                     "toolName": name, "arguments": arguments}]})
                app.display_message({"role": "tool", "content": [{"type": "tool_result", "callId": call_id,
                    "output": output, "metadata": metadata, "isError": error}]})
                await pilot.pause()
                view = list(app.query(ToolView))[-1]
                view.collapsed = False
                await pilot.pause()
                self.assertTrue(view.rendered)
                self.assertIn(expected, self.log_text(app))
                self.assertNotIn("<shell_metadata>", self.log_text(app))
                if name == "bash":
                    self.assertEqual(view.title, "bash · Completed · Exit code 2")
                elif error:
                    self.assertIn("Failed", view.title)
                view.collapsed = True
            app.display_message({"role": "assistant", "content": [{"type": "tool_call", "callId": "unfinished",
                                 "toolName": "bash", "arguments": {"command": "sleep 10"}}]})
            app.handle_event({"type": "execution.cancelled"})
            await pilot.pause()
            last = list(app.query(ToolView))[-1]
            self.assertTrue(last.result["isError"])
            self.assertEqual(app.pending_tools, {})

    async def test_read_is_a_plain_path_and_keeps_errors_visible(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.display_message({"role": "assistant", "content": [{"type": "tool_call", "callId": "read",
                "toolName": "read", "arguments": {"filePath": "src/[literal].cpp"}}]})
            await pilot.pause()
            view = app.query_one(ReadView)
            self.assertEqual(str(view.render()), "read · src/[literal].cpp")
            self.assertEqual(len(app.query(ToolView)), 0)
            app.display_message({"role": "tool", "content": [{"type": "tool_result", "callId": "read",
                "output": "File not found", "isError": True}]})
            await pilot.pause()
            self.assertEqual(str(view.render()), "read · Failed · src/[literal].cpp")
            self.assertIsNone(view.tooltip)
            self.assertEqual(app.pending_tools, {})
            app.display_message({"role": "assistant", "content": [{"type": "tool_call", "callId": "invalid-read",
                "toolName": "read", "arguments": {}}]})
            app.display_message({"role": "tool", "content": [{"type": "tool_result", "callId": "invalid-read",
                "output": "key 'filePath' not found", "isError": True}]})
            await pilot.pause()
            self.assertEqual(str(list(app.query(ReadView))[-1].render()), "read · Failed")

    async def test_activity_drawer_opens_downward_and_keeps_receiving_hidden_events(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 45)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.display_message({"role": "user", "content": [{"type": "text", "text": "First"}]})
            app.display_message({"role": "assistant", "content": [{"type": "text", "text": "Direct answer"}]})
            await pilot.pause()
            first_user = app.query_one(UserMessage)
            first_group = app.query_one(TurnActivity)
            self.assertFalse(first_group.body.display)
            self.assertEqual(first_user.region.bottom, first_group.toggle.region.y)
            self.assertEqual(first_group.toggle.styles.color.hex, MUTED_COLOR.upper())
            self.assertEqual(first_group.toggle.render().plain, "▸ Activity")
            app.display_message({"role": "user", "content": [{"type": "text", "text": "Second"}]})
            app.display_message({"role": "assistant", "content": [
                {"type": "reasoning", "text": "Checking"},
                {"type": "text", "text": "I will inspect the file."},
                {"type": "tool_call", "callId": "one", "toolName": "read", "arguments": {"filePath": "one.cpp"}},
            ]})
            app.display_message({"role": "tool", "content": [{"type": "tool_result", "callId": "one",
                "output": "contents", "isError": False}]})
            app.display_message({"role": "assistant", "content": [
                {"type": "reasoning", "text": "Checking another file"},
                {"type": "tool_call", "callId": "two", "toolName": "grep", "arguments": {"pattern": "test"}},
                {"type": "text", "text": "Final explanation"},
            ]})
            await pilot.pause()
            children = list(app.query_one("#chat").children)
            second_user = list(app.query(UserMessage))[1]
            initial_group = list(app.query(TurnActivity))[1]
            group = app.current_activity
            activities = list(group.body.children)
            self.assertEqual(len(app.query(TurnActivity)), 3)
            self.assertFalse(group.body.display)
            self.assertEqual(len(initial_group.body.query(".reasoning")), 1)
            self.assertEqual(len(initial_group.body.query(ReadView)), 0)
            for activity in activities:
                self.assertIs(activity.parent, group.body)
            self.assertGreater(children.index(list(app.query("#chat > Markdown"))[-1]), children.index(group))
            await pilot.click(group.toggle)
            await pilot.pause()
            self.assertTrue(group.body.display)
            self.assertFalse(first_group.body.display)
            self.assertTrue(group.toggle.render().plain.startswith("▾ Activity ▪"))
            self.assertEqual(second_user.region.bottom, initial_group.toggle.region.y)
            self.assertEqual(list(app.query("#chat > Markdown"))[-2].region.bottom, group.activity_separator.region.y)
            self.assertEqual(group.activity_separator.region.bottom, group.toggle.region.y)
            self.assertEqual(group.toggle.region.bottom, activities[0].region.y)
            self.assertGreater(activities[-1].region.y, group.toggle.region.y)
            self.assertEqual(activities[0].region.x, group.toggle.region.x + 2)
            tool = app.query_one(ToolView)
            tool.collapsed = False
            await pilot.pause()
            await pilot.click(group.toggle)
            self.assertFalse(group.body.display)
            app.display_message({"role": "tool", "content": [{"type": "tool_result", "callId": "two",
                "output": "Arrived while hidden", "isError": False}]})
            app.display_message({"role": "assistant", "content": [{"type": "tool_call", "callId": "three",
                "toolName": "read", "arguments": {"filePath": "three.cpp"}}]})
            await pilot.pause()
            self.assertFalse(group.body.display)
            await pilot.click(group.toggle)
            await pilot.pause()
            self.assertTrue(group.body.display)
            self.assertFalse(tool.collapsed)
            self.assertIn("Arrived while hidden", self.log_text(app))
            self.assertEqual(len(group.body.query(ReadView)), 1)
            self.assertIsNot(app.current_activity, group)
            self.assertEqual(len(app.current_activity.body.query(ReadView)), 1)
            self.assertEqual(group.toggle.styles.background.hex, BACKGROUND_COLOR.upper())

    async def test_activity_spinner_shares_pulse_and_keeps_colored_history_markers(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.busy = True
            app.display_message({"role": "user", "content": [{"type": "text", "text": "Inspect"}]})
            app.display_message({"role": "assistant", "content": [
                {"type": "reasoning", "text": "Check the file"},
                {"type": "tool_call", "callId": "read", "toolName": "read", "arguments": {"filePath": "a.cpp"}},
            ]})
            await pilot.pause()
            group = app.current_activity
            button = app.query_one(ModeButton)
            self.assertTrue(group.toggle.has_reasoning)
            self.assertFalse(group.toggle.has_tools)
            self.assertTrue(group.toggle.has_reads)
            self.assertFalse(group.body.display)
            await self.until(pilot, lambda: group.toggle.spinner != "-")
            self.assertEqual(group.toggle.spinner, ActivityToggle.SPINNER_FRAMES[button.spinner_frame])
            text = group.toggle.render()
            self.assertEqual(text.plain, f"▸ Activity ▪ ▪ {group.toggle.spinner} 2 steps | {group.toggle.duration_label}")
            self.assertEqual([span.style for span in text.spans], [MISC_MD, TITLE_MD])
            await pilot.click(group.toggle)
            self.assertEqual(group.body.query_one(".reasoning > CollapsibleTitle").styles.color.hex, MISC_MD.upper())
            app.handle_event({"type": "execution.completed"})
            self.assertEqual(group.toggle.spinner, "-")
            self.assertEqual(group.toggle.render().plain, f"▾ Activity ▪ ▪ 2 steps | {group.toggle.duration_label}")
            self.assertIsNone(app.running_activity)
            self.assertFalse(group.toggle.has_tools)
            self.assertTrue(group.toggle.has_reads)
            self.assertTrue(group.toggle.has_reasoning)
            # Starting the next request must not animate the previous turn.
            app.busy = True
            await pilot.pause(0.2)
            self.assertEqual(group.toggle.spinner, "-")
            app.display_message({"role": "user", "content": [{"type": "text", "text": "Next"}]})
            second = app.current_activity
            self.assertFalse(second.toggle.has_tools)
            self.assertFalse(second.toggle.has_reasoning)
            await self.until(pilot, lambda: second.toggle.spinner != "-")
            app.handle_event({"type": "execution.failed", "error": "Test failure"})
            self.assertEqual(second.toggle.spinner, "-")
            self.assertFalse(button.busy)

    async def test_spinner_updates_visible_current_activity_when_running_reference_lags(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.busy = True
            app.display_message({"role": "user", "content": [{"type": "text", "text": "Inspect"}]})
            first = app.current_activity
            app.display_message({"role": "assistant", "content": [
                {"type": "text", "text": "Progress"},
                {"type": "tool_call", "callId": "pending", "toolName": "bash", "arguments": {"command": "test"}},
            ]})
            current = app.current_activity
            self.assertIsNot(first, current)
            app.running_activity = first
            app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(2))
            self.assertEqual(current.toggle.spinner, "|")
            app.busy = False
            await pilot.pause()

    async def test_chat_spacing_is_flush_except_one_line_before_next_user(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 70)) as pilot:
            await self.until(pilot, lambda: app.connected)
            previous = None
            for source in ("A paragraph", "## Heading\n\n- One\n- Two", "```python\nprint('hello')\n```",
                           "| Header |\n| --- |\n| Cell |"):
                app.display_message({"role": "user", "content": [{"type": "text", "text": "Next question"}]})
                app.display_message({"role": "assistant", "content": [
                    {"type": "reasoning", "text": "Some reasoning"},
                    {"type": "tool_call", "callId": source, "toolName": "read", "arguments": {"filePath": "a.cpp"}},
                    {"type": "text", "text": source},
                ]})
                await pilot.pause()
                group = app.current_activity
                self.assertFalse(any(rule.display for rule in group.query(Rule)))
                user = list(app.query(UserMessage))[-1]
                response = list(app.query("#chat > Markdown"))[-1]
                if previous is not None:
                    self.assertEqual(user.region.y - previous.region.bottom, 1)
                self.assertEqual(group.toggle.region.bottom, response.region.y)
                self.assertEqual(response.children[0].region.y, response.region.y)
                self.assertEqual(response.children[-1].region.bottom, response.region.bottom)
                # Test layout directly: deferred scrolling can move the mouse target.
                # Separate click tests cover input routing.
                group.toggle.action_toggle()
                await self.until(pilot, lambda: group.body.display)
                await pilot.pause()
                self.assertTrue(group.toggle.expanded)
                self.assertEqual(group.body.children[-1].region.bottom, response.region.y)
                previous = response

    async def test_activity_follows_content_order_between_texts_live_and_on_resume(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 70)) as pilot:
            await self.until(pilot, lambda: app.connected)
            messages = [
                {"role": "user", "content": [{"type": "text", "text": "Inspect"}]},
                {"role": "assistant", "content": [
                    {"type": "reasoning", "text": "Before the first text"},
                    {"type": "text", "text": "First explanation"},
                    {"type": "tool_call", "callId": "one", "toolName": "read", "arguments": {"filePath": "a.cpp"}},
                    {"type": "reasoning", "text": "Between the texts"},
                    {"type": "text", "text": "Second explanation"},
                    {"type": "tool_call", "callId": "two", "toolName": "read", "arguments": {"filePath": "b.cpp"}},
                ]},
                {"role": "tool", "content": [
                    {"type": "tool_result", "callId": "one", "output": "First contents", "isError": False},
                    {"type": "tool_result", "callId": "two", "output": "Second contents", "isError": False},
                ]},
                {"role": "assistant", "content": [{"type": "text", "text": "  "}]},
                {"role": "assistant", "content": [
                    {"type": "reasoning", "text": "Before the final text"},
                    {"type": "text", "text": "Final answer"},
                ]},
            ]
            for index, message in enumerate(messages):
                message["id"] = f"source-{index}"
            for replay in (False, True):
                if replay:
                    await app.restore_session({"session": {"id": "replay", "title": "Inspect", "workspace": app.workspace},
                                               "messages": display_records(messages), "page": {}}).wait()
                    for drawer in app.query(TurnActivity):
                        record = next(record for record in app.history_window.records if record["id"] == drawer.group_id)
                        await drawer.populate(record)
                else:
                    app.busy = True
                    for message in messages:
                        app.display_message(message)
                await pilot.pause()
                groups = list(app.query(TurnActivity))
                texts = list(app.query("#chat > Markdown"))
                expected = [app.query_one(UserMessage), groups[0], texts[0], groups[1], texts[1], groups[2], texts[2]]
                if not replay:
                    expected.append(app.query_one("#chat").live_footer)
                self.assertEqual(list(app.query_one("#chat").children), expected)
                self.assertEqual([len(group.body.query(ReadView)) for group in groups], [0, 1, 1])
                self.assertEqual([len(group.body.query(".reasoning")) for group in groups], [1, 1, 1])
                self.assertFalse(app.pending_tools)
                for group, prefix in zip(groups[:2], ("▸ Activity ▪ 1 step", "▸ Activity ▪ ▪ 2 steps")):
                    duration = group.toggle.duration_label
                    self.assertEqual(group.toggle.render().plain, prefix + (f" | {duration}" if duration else ""))
                    if not replay:
                        self.assertTrue(duration)
                self.assertTrue(groups[-1].toggle.is_terminal)
                if not replay:
                    self.assertEqual([group.group_id for group in groups],
                                     [record["id"] for record in display_records(messages) if record["role"] == "activity"])
                    self.assertIs(app.running_activity, groups[-1])
                    app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(1))
                    self.assertEqual([group.toggle.spinner for group in groups], ["-", "-", "\\"])
                    self.assertTrue(all(group.toggle.started_at is None for group in groups[:-1]))
                    app.handle_event({"type": "execution.completed"})
                for index, group in enumerate(groups):
                    self.assertEqual(group.toggle.region.bottom, texts[index].region.y)
                    if index:
                        self.assertEqual(texts[index - 1].region.bottom, group.activity_separator.region.y)
                        self.assertEqual(group.activity_separator.region.bottom, group.toggle.region.y)
                    await pilot.click(group.toggle)
                    await pilot.pause()
                    self.assertEqual(group.body.children[-1].region.bottom, texts[index].region.y)

    async def test_preparing_conversation_keeps_pending_draft_until_acceptance_or_cancellation(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            app.busy = True
            app.pending_request = "prepare-1"
            app.pending_text = "Unaccepted request"
            app.handle_event({"type": "execution.preparing", "requestId": "prepare-1", "sessionId": "saved"})
            self.assertEqual(entry.placeholder, "Preparing conversation…")
            self.assertEqual(app.pending_text, "Unaccepted request")
            self.assertTrue(app.busy)
            entry.text = "New draft"
            app.handle_event({"type": "execution.cancelled", "requestId": "prepare-1"})
            self.assertFalse(app.busy)
            self.assertEqual(entry.text, "Unaccepted request\n\nNew draft")
            self.assertIsNone(app.pending_request)

    async def test_activity_live_time_then_authoritative_message_metadata(self):
        app = SosApp(self.path)
        async with app.run_test(size=(120, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.request_started_at = 100.0
            app.busy = True
            app.query_one(ModeButton).animation_timer.pause()
            with patch("sos_tui.turn_activity.monotonic", return_value=101.9):
                app.display_message({"role": "user", "content": [{"type": "text", "text": "Hello"}]})
            await pilot.pause()
            group = app.current_activity
            self.assertTrue(group.toggle.render().plain.endswith(" 1s"))
            with patch("sos_tui.turn_activity.monotonic", return_value=162.0):
                app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(1))
            self.assertTrue(group.toggle.render().plain.endswith(" 1m 02s"))
            metadata = {"model": "actual-model", "reasoning": "high", "duration_ms": 65003}
            app.display_message({"role": "assistant", "content": [{"type": "text", "text": "Done"}], "metadata": metadata})
            app.handle_event({"type": "execution.completed", "metadata": metadata})
            await pilot.pause()
            self.assertEqual(group.toggle.render().plain, "▸ Activity 1m 05s")
            self.assertIsNone(group.toggle.started_at)
            app.update_agent_info({"model": "next-model", "reasoning": "low"})
            self.assertEqual(group.toggle.model, "actual-model")

    async def test_intermediate_activity_shows_only_duration_live_and_after_restore(self):
        app = SosApp(self.path)
        messages = [
            {"id": "user", "role": "user", "content": [{"type": "text", "text": "Inspect"}]},
            {"id": "first", "role": "assistant", "content": [{"type": "text", "text": "First reply"}],
             "metadata": {"model": "first-model", "reasoning": "low", "duration_ms": 12000}},
            {"id": "thinking", "role": "assistant", "content": [{"type": "reasoning", "text": "Checking"}]},
            {"id": "last", "role": "assistant", "content": [{"type": "text", "text": "Final reply"}],
             "metadata": {"model": "last-model", "reasoning": "high", "duration_ms": 65003}},
        ]
        async with app.run_test(size=(120, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.busy = True
            for message in messages:
                app.display_message(message)
            app.handle_event({"type": "execution.completed", "metadata": messages[-1]["metadata"]})
            for replay in (False, True):
                if replay:
                    await app.restore_session({"session": {"id": "replay", "title": "Inspect", "workspace": app.workspace},
                                               "messages": display_records(messages), "page": {}}).wait()
                await pilot.pause()
                groups = list(app.query(TurnActivity))
                self.assertEqual(len(groups), 2)
                self.assertFalse(groups[0].toggle.is_terminal)
                self.assertEqual(groups[0].toggle.render().plain, "▸ Activity 12s")
                self.assertTrue(groups[1].toggle.is_terminal)
                self.assertEqual(groups[1].toggle.render().plain, "▸ Activity ▪ 1 step | 1m 05s")
                self.assertFalse(groups[0].activity_separator.display)
                self.assertTrue(groups[1].activity_separator.display)
                self.assertEqual(groups[1].activity_separator.size.width, groups[1].content_size.width)
                self.assertEqual(groups[1].activity_separator.size.height, 1)
                self.assertEqual(groups[1].activity_separator.render_line(0).text, "─" * app.query_one("#chat").size.width)
                self.assertEqual(groups[1].activity_separator.region.x, 0)
                self.assertEqual(groups[1].activity_separator.region.right, app.query_one("#chat").region.right)
                self.assertEqual(groups[1].toggle.region.x, 3)
                await pilot.resize_terminal(140 if replay else 100, 40)
                await pilot.pause()
                self.assertEqual(groups[1].activity_separator.render_line(0).text, "─" * app.query_one("#chat").size.width)

    async def test_activity_separators_restart_at_each_user_turn(self):
        app = SosApp(self.path)
        messages = []
        for turn in range(2):
            messages.extend([
                {"id": f"user-{turn}", "role": "user", "content": [{"type": "text", "text": "Inspect"}]},
                {"id": f"first-{turn}", "role": "assistant", "content": [{"type": "text", "text": "Checking"}]},
                {"id": f"second-{turn}", "role": "assistant", "content": [
                    {"type": "reasoning", "text": "Thinking"}, {"type": "text", "text": "More checks"}]},
                {"id": f"last-{turn}", "role": "assistant", "content": [
                    {"type": "reasoning", "text": "Done"}, {"type": "text", "text": "Final answer"}]},
            ])
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            for message in messages:
                app.display_message(message)
            for replay in (False, True):
                if replay:
                    await app.restore_session({"session": {"id": "replay", "title": "Inspect", "workspace": app.workspace},
                                               "messages": display_records(messages), "page": {}}).wait()
                await pilot.pause()
                groups = list(app.query(TurnActivity))
                self.assertEqual([group.activity_separator.display for group in groups],
                                 [False, True, True, False, True, True])
                groups[1].toggle.action_toggle()
                await pilot.pause()
                self.assertTrue(groups[1].activity_separator.display)

    @confirmation_input()
    async def test_double_escape_arms_coral_then_waits_for_cancel_confirmation(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            button = app.query_one(ModeButton)
            confirmation = app.query_one(ConfirmationBar)
            entry = app.query_one(MessageInput)
            with patch.object(app.client, "send", new_callable=AsyncMock) as send:
                await pilot.press("escape")
                self.assertIsNone(confirmation.kind)
                app.busy = True
                entry.text = "Next turn"
                self.assertEqual(str(button.label), "Cancel")
                await pilot.press("escape")
                self.assertEqual(confirmation.kind, "cancel")
                self.assertEqual(str(confirmation.label), "Cancel Execution ? Confirm with ESC")
                self.assertEqual(confirmation.render().spans[0].style.bgcolor, Color.parse(SLOP_COLOR))
                send.assert_not_awaited()
                await pilot.press("escape")
                send.assert_awaited_once()
                request = send.await_args.args[0]
                self.assertEqual(request["type"], "execution.cancel")
                self.assertTrue(app.busy)
                self.assertFalse(entry.disabled)
                self.assertEqual(confirmation.kind, "cancel")
                self.assertEqual(str(confirmation.label), "Cancelling…")
                self.assertIsNone(confirmation.deadline)
                await pilot.press("escape")
                self.assertEqual(send.await_count, 1)
                app.handle_event({"type": "execution.cancel_requested", "requestId": request["requestId"]})
                self.assertFalse(entry.disabled)
                app.handle_event({"type": "execution.cancelled", "error": "Execution cancelled"})
                self.assertFalse(app.busy)
                self.assertFalse(entry.disabled)
                self.assertEqual(entry.text, "Next turn")
                self.assertIsNone(confirmation.kind)
                self.assertIsNone(app.cancel_request_id)
                self.assertIsNone(app.cancel_timer)
                await pilot.pause()
                self.assertEqual(self.log_text(app).count("Execution cancelled"), 1)

    @confirmation_input()
    async def test_cancel_confirmation_expires_and_completion_clears_it(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            button = app.query_one(ModeButton)
            confirmation = app.query_one(ConfirmationBar)
            with patch.object(app.client, "send", new_callable=AsyncMock) as send:
                app.busy = True
                with patch.object(app, "set_timer", wraps=app.set_timer) as schedule:
                    await pilot.press("escape", "tab")
                self.assertEqual(confirmation.kind, "cancel")
                self.assertEqual(confirmation.render().spans[0].style.bgcolor, Color.parse(SLOP_COLOR))
                self.assertEqual(app.CANCEL_CONFIRM_SECONDS, 3.0)
                self.assertEqual(schedule.call_args.args[0], app.CANCEL_CONFIRM_SECONDS)
                app.cancel_timer.stop()
                schedule.call_args.args[1]()  # Exercise the actual callback, not wall-clock waiting.
                await pilot.pause()
                self.assertIsNone(confirmation.kind)
                self.assertEqual(button.styles.background.hex, SLOP_COLOR.upper())
                self.assertEqual([call.args[0]["type"] for call in send.await_args_list], ["mode.set"])
                send.reset_mock()
                send.assert_not_awaited()
                await pilot.press("escape")
                self.assertEqual(confirmation.kind, "cancel")
                app.handle_event({"type": "execution.completed"})
                await pilot.press("escape")
                self.assertIsNone(confirmation.kind)
                send.assert_not_awaited()

    @confirmation_input()
    async def test_failed_cancel_request_keeps_running_and_can_be_retried(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            with patch.object(app.client, "send", new_callable=AsyncMock):
                app.busy = True
                await pilot.press("escape", "escape")
                app.handle_event({"type": "request.failed", "requestId": app.cancel_request_id, "error": "Cancel unavailable"})
                self.assertTrue(app.busy)
                self.assertFalse(app.query_one(MessageInput).disabled)
                self.assertIsNone(app.cancel_request_id)
                self.assertIsNone(app.query_one(ConfirmationBar).kind)
                await pilot.press("escape")
                self.assertEqual(app.query_one(ConfirmationBar).kind, "cancel")
                app.handle_event({"type": "execution.failed", "error": "Test shutdown"})

    @confirmation_input()
    async def test_click_and_escape_share_cancel_confirmation(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            button = app.query_one(ModeButton)
            confirmation = app.query_one(ConfirmationBar)
            with patch.object(app.client, "send", new_callable=AsyncMock) as send:
                app.busy = True
                await pilot.click(button)
                self.assertEqual(str(confirmation.label), "Cancel Execution ? Confirm with ESC")
                self.assertEqual(confirmation.render().spans[0].style.bgcolor, Color.parse(SLOP_COLOR))
                send.assert_not_awaited()
                await pilot.press("escape")
                send.assert_awaited_once()
                app.handle_event({"type": "execution.cancelled", "error": "Execution cancelled"})
                app.busy = True
                await pilot.press("escape")
                self.assertEqual(str(confirmation.label), "Cancel Execution ? Confirm with ESC")
                await pilot.click(confirmation)
                self.assertEqual(send.await_count, 2)
                app.handle_event({"type": "execution.cancelled"})

    @confirmation_input()
    async def test_ctrl_c_clears_draft_then_requires_two_presses_to_quit(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "Draft\nwith another line"
            with patch.object(app, "exit") as exit_app:
                await pilot.press("ctrl+c")
                self.assertEqual(entry.text, "")
                exit_app.assert_not_called()
                await pilot.press("ctrl+c")
                exit_app.assert_not_called()
                self.assertEqual(app.query_one(ConfirmationBar).kind, "exit")
                await pilot.press("ctrl+c")
                exit_app.assert_called_once()
            self.assertEqual(self.requests, [])

    async def test_ctrl_g_roundtrips_draft_through_editor_without_sending(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "Existing draft"
            paths = []

            def edit(command, **kwargs):
                self.assertEqual(command[:-1], ["my editor", "--wait"])
                path = Path(command[-1])
                paths.append(path)
                self.assertEqual(path.read_text(encoding="utf-8"), "Existing draft\n")
                path.write_text("Edited draft\nÉcrit dans l’éditeur", encoding="utf-8")
                return subprocess.CompletedProcess(command, 0)

            with patch.dict(os.environ, {"EDITOR": '"my editor" --wait'}), \
                    patch.object(app, "suspend", side_effect=nullcontext) as suspend, \
                    patch("sos_tui.app.subprocess.run", side_effect=edit) as run:
                await pilot.press("ctrl+g")
                suspend.assert_called_once()
                run.assert_called_once()
                self.assertEqual(entry.text, "Edited draft\nÉcrit dans l’éditeur")
                self.assertEqual(entry.cursor_location, entry.document.end)
                self.assertTrue(entry.has_focus)
                self.assertFalse(paths[0].exists())
                app.busy = True
                entry.text = "Existing draft"
                await pilot.press("ctrl+g")
                self.assertEqual(run.call_count, 2)
                self.assertEqual(entry.text, "Edited draft\nÉcrit dans l’éditeur")
                self.assertTrue(app.busy)
                app.busy = False
            self.assertEqual(self.requests, [])

    async def test_editor_failure_preserves_draft_and_resumes_terminal(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "Keep this draft"
            with patch.dict(os.environ, {"EDITOR": "missing-editor"}), \
                    patch.object(app, "suspend") as suspend, \
                    patch("sos_tui.app.subprocess.run", side_effect=FileNotFoundError("No editor")):
                await pilot.press("ctrl+g")
                suspend.return_value.__exit__.assert_called_once_with(None, None, None)
                self.assertEqual(entry.text, "Keep this draft")
                self.assertIn("No editor", self.log_text(app))
            with patch.dict(os.environ, {"EDITOR": "failing-editor"}), \
                    patch.object(app, "suspend", side_effect=nullcontext), \
                    patch("sos_tui.app.subprocess.run", return_value=subprocess.CompletedProcess([], 1)):
                await pilot.press("ctrl+g")
                self.assertEqual(entry.text, "Keep this draft")
                self.assertIn("Editor exited with code 1", self.log_text(app))

    async def test_ctrl_g_requires_editor_without_visual_fallback(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "Keep this draft"
            with patch.dict(os.environ, {"VISUAL": "vi"}, clear=True), \
                    patch("sos_tui.app.subprocess.run") as run:
                await pilot.press("ctrl+g")
                run.assert_not_called()
                self.assertEqual(entry.text, "Keep this draft")
                self.assertIn("EDITOR is not set", self.log_text(app))

    async def test_ctrl_t_requests_next_effort_without_changing_current_turn(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            with patch.object(app.client, "send", new_callable=AsyncMock) as send:
                await pilot.press("ctrl+t")
                self.assertEqual(send.call_args.args[0]["type"], "reasoning.next")
                app.handle_event({"type": "reasoning.updated", "info": {"model": "openai/test-model", "reasoning": "high"}})
                self.assertEqual(app.query_one(AgentInfoBar).labels["reasoning-info"], " | high")
                app.busy = True
                app.display_message({"role": "user", "content": [{"type": "text", "text": "Work"}]})
                await pilot.pause()
                await pilot.press("ctrl+t")
                self.assertEqual(send.await_count, 2)
                app.handle_event({"type": "reasoning.updated", "info": {
                    "model": "openai/test-model", "reasoning": "low", "execution_reasoning": "high"}})
                self.assertEqual(app.query_one(AgentInfoBar).labels["reasoning-info"], " | low")
                self.assertEqual(app.running_activity.toggle.reasoning, "high")
                app.handle_event({"type": "request.failed", "requestId": "unavailable", "error": "No levels"})
                self.assertTrue(app.busy)
                app.handle_event({"type": "execution.completed", "metadata": {
                    "model": "openai/test-model", "reasoning": "high", "duration_ms": 1000},
                    "info": {"reasoning": "low", "execution_reasoning": None}})
                self.assertEqual(app.current_activity.toggle.reasoning, "high")

    async def test_resume_lists_titles_previews_with_arrows_and_restores_history(self):
        self.runtime_info.update(model="global-model", reasoning="high")
        self.saved_sessions = [{"id": f"saved-{i}", "title": f"Session title {i}", "mode": "slop", "updatedAt": 1700000000000 + i * 1000,
                                "workspace": f"/project/{i}", "messages": [
                                    {"role": "system", "content": [{"type": "text", "text": "Hidden instructions"}]},
                                    {"role": "user", "content": [{"type": "text", "text": f"Last prompt [{i}]"}]},
                                    {"role": "assistant", "content": [{"type": "text", "text": f"Answer {i}"}],
                                     "metadata": {"model": "historical-model", "reasoning": "low", "duration_ms": 1000}},
                                ]} for i in range(24)]
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "/resume"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_screen is not None and app.resume_screen.selected_id == "saved-23")
            screen = app.resume_screen
            options = screen.query_one(OptionList)
            await self.until(pilot, lambda: "Last prompt [23]" in str(screen.query_one("#resume-preview", Static).render()))
            self.assertEqual(options.option_count, 24)
            self.assertEqual(screen.query_one("#resume-heading").styles.color.hex, BOLD_MD.upper())
            self.assertEqual(screen.query_one("#resume-preview-heading").styles.color.hex, TITLE_MD.upper())
            self.assertEqual(screen.query_one("#resume-dialog").styles.border_top[0], "solid")
            self.assertEqual(screen.query_one("#resume-dialog").styles.border_top[1].hex, MUTED_COLOR.upper())
            header = screen.query_one("#resume-columns", Static)
            self.assertLess(header.region.y, options.region.y)
            header_y = header.region.y
            screen.set_preview("saved-23", "\n".join(f"Long prompt line {i}" for i in range(30)))
            await pilot.pause()
            preview = screen.query_one("#resume-preview-scroll", VerticalScroll)
            self.assertLessEqual(preview.region.height, 4)
            self.assertGreater(preview.max_scroll_y, 0)
            preview.scroll_end(animate=False)
            await pilot.pause()
            self.assertGreater(preview.scroll_y, 0)
            self.assertFalse(any(r.get("text") == "/resume" for r in self.requests))
            await pilot.press("down")
            await self.until(pilot, lambda: "Last prompt [22]" in str(screen.query_one("#resume-preview", Static).render()))
            screen.set_preview("saved-23", "Stale preview")
            self.assertNotIn("Stale preview", str(screen.query_one("#resume-preview", Static).render()))
            for _ in range(18):
                await pilot.press("down")
            self.assertGreater(options.scroll_y, 0)
            self.assertEqual(header.region.y, header_y)
            await pilot.press("up")
            selected = screen.selected_id
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_screen is None and app.session_id == selected)
            self.assertEqual(app.workspace, "/project/5")
            self.assertEqual(app.runtime_info["model"], "global-model")
            self.assertEqual(app.runtime_info["reasoning"], "high")
            self.assertEqual(app.query_one(TurnActivity).toggle.model, "historical-model")
            self.assertEqual(app.query_one(TurnActivity).toggle.reasoning, "low")
            self.assertEqual(app.mode, "slop")
            self.assertEqual(str(app.query_one(ModeButton).label), "Slop")
            self.assertIn("Session title 5", str(app.query_one("#session-title", Static).render()))
            self.assertIn("Last prompt [5]", self.log_text(app))
            self.assertIn("Answer 5", self.log_text(app))
            self.assertNotIn("Hidden instructions", self.log_text(app))
            self.assertFalse(entry.disabled)
            self.assertEqual(entry.text, "")
            entry.text = "Continue"
            await pilot.press("enter")
            await self.until(pilot, lambda: not app.busy)
            self.assertEqual(self.requests[-1]["text"], "Continue")
            self.assertNotIn("workspace", self.requests[-1])

    async def test_resume_restores_each_user_message_mode_without_recoloring_on_switch(self):
        self.saved_sessions = [{"id": "mixed", "title": "Mixed modes", "mode": "slop",
                                "updatedAt": 1700000000000, "workspace": "/project", "messages": [
            {"role": "user", "content": [{"type": "text", "text": "Investigate"}], "metadata": {"mode": "sane"}},
            {"role": "user", "content": [{"type": "text", "text": "Implement"}], "metadata": {"mode": "slop"}},
            {"role": "user", "content": [{"type": "text", "text": "Explain"}], "metadata": {"mode": "sane"}},
            {"role": "user", "content": [{"type": "text", "text": "Legacy message"}]},
            {"role": "user", "content": [{"type": "text", "text": "Legacy null"}], "metadata": None},
        ]}]
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.query_one(MessageInput).text = "/resume"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_screen is not None and app.resume_screen.selected_id == "mixed")
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_screen is None and app.session_id == "mixed")
            await pilot.pause()
            messages = list(app.query(UserMessage))
            self.assertEqual(app.mode, "slop")
            self.assertEqual([message.mode for message in messages], ["sane", "slop", "sane", "slop", "slop"])
            expected_colors = [SANE_COLOR, SLOP_COLOR, SANE_COLOR, SLOP_COLOR, SLOP_COLOR]
            for message, color in zip(messages, expected_colors):
                self.assertEqual(message.query_one(".mode-band").styles.background.hex, color.upper())
            await pilot.press("tab")
            self.assertEqual(app.mode, "sane")
            for message, color in zip(messages, expected_colors):
                self.assertEqual(message.query_one(".mode-band").styles.background.hex, color.upper())

    async def test_compact_keeps_chat_and_renders_summary_live_and_after_resume(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.set_mode("slop")
            self.compaction_summary["compaction"]["mode"] = "slop"
            entry = app.query_one(MessageInput)
            entry.text = "/compact"
            await pilot.press("enter")
            self.assertEqual(self.requests, [])
            self.assertIn("Start a conversation", self.log_text(app))
            entry.text = "Original request"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.session_id is not None and not app.busy)
            original_user = app.query_one(UserMessage)
            original_activity = app.current_activity
            self.compaction_release.clear()
            entry.text = "/compact"
            await pilot.press("enter")
            await self.until(pilot, lambda: any(r["type"] == "session.compact" for r in self.requests))
            self.assertTrue(app.busy)
            self.assertIsNotNone(app.compaction_request)
            self.assertEqual(len(app.query(UserMessage)), 1)
            drawer = app.query_one(".compaction", Collapsible)
            app.handle_event({"type": "compaction.progress", "requestId": app.compaction_request, "attempt": 1, "limit": 3})
            self.assertNotIn("1 / 3", drawer.title)
            app.handle_event({"type": "compaction.progress", "requestId": app.compaction_request, "attempt": 2, "limit": 3})
            app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(1))
            self.assertIn("2 / 3", drawer.title)
            self.assertEqual(drawer.styles.border_top[0], "heavy")
            self.assertEqual(drawer.styles.border_top[1].hex, SLOP_COLOR.upper())
            self.assertEqual(drawer.region.x, 0)
            self.assertEqual(drawer.region.right, app.query_one("#chat").scrollable_content_region.right)
            row = app.screen._compositor.render_strips()[drawer.region.y].text
            self.assertEqual(row[drawer.region.x:drawer.region.right], "━" * drawer.size.width)
            title = drawer.query_one("CollapsibleTitle")
            self.assertEqual(title.styles.color.hex, SLOP_COLOR.upper())
            self.assertTrue(title.styles.text_style.bold)
            await self.until(pilot, lambda: drawer.title ==
                             f"Compaction {ActivityToggle.SPINNER_FRAMES[app.query_one(ModeButton).spinner_frame]} 2 / 3")
            self.assertEqual(drawer.title, f"Compaction {ActivityToggle.SPINNER_FRAMES[app.query_one(ModeButton).spinner_frame]} 2 / 3")
            await pilot.click(title)
            self.assertFalse(drawer.collapsed)
            entry.text = "Draft during compaction"
            self.compaction_release.set()
            await self.until(pilot, lambda: not app.busy)
            await pilot.pause()
            self.assertIs(app.query_one(UserMessage), original_user)
            self.assertIn(original_activity, app.query(TurnActivity))
            self.assertEqual(len(app.query(".compaction")), 1)
            self.assertIs(app.query_one(".compaction", Collapsible), drawer)
            self.assertFalse(drawer.collapsed)
            self.assertEqual(drawer.title, "Compaction")
            self.assertEqual(drawer.styles.border_top[0], "heavy")
            self.assertEqual(drawer.styles.border_top[1].hex, SLOP_COLOR.upper())
            self.assertIsNone(app.running_compaction)
            self.assertEqual(len(drawer.query(Markdown)), 1)
            self.assertEqual(entry.text, "Draft during compaction")
            self.assertEqual(self.requests[-1]["type"], "session.compact")
            self.saved_sessions = [{"id": "compacted", "title": "Compacted session", "mode": "sane",
                                    "updatedAt": 1700000000000, "workspace": "/project", "messages": [
                {"role": "user", "content": [{"type": "text", "text": "Original request"}], "metadata": {"mode": "slop"}},
                {"role": "assistant", "content": [{"type": "text", "text": "Original response"}]},
                self.compaction_summary,
                {"role": "user", "content": [{"type": "text", "text": "After compaction"}], "metadata": {"mode": "sane"}},
            ]}]
            entry.text = "/resume"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_screen is not None and app.resume_screen.selected_id == "compacted")
            await pilot.press("enter")
            await self.until(pilot, lambda: app.session_id == "compacted" and app.resume_screen is None)
            await pilot.pause()
            self.assertEqual([m.text for m in app.query(UserMessage)], ["Original request", "After compaction"])
            self.assertEqual([m.mode for m in app.query(UserMessage)], ["slop", "sane"])
            self.assertEqual(len(app.query(".compaction")), 1)
            restored = app.query_one(".compaction", Collapsible)
            self.assertEqual(restored.styles.border_top[0], "heavy")
            self.assertEqual(restored.styles.border_top[1].hex, SLOP_COLOR.upper())
            self.assertEqual(restored.region.x, 0)
            self.assertEqual(restored.region.right, app.query_one("#chat").scrollable_content_region.right)
            restored.scroll_visible(animate=False, top=True)
            await pilot.pause()
            row = app.screen._compositor.render_strips()[restored.region.y].text
            self.assertEqual(row[restored.region.x:restored.region.right], "━" * restored.size.width)
            self.assertEqual(restored.title, "Compaction")
            self.assertTrue(restored.collapsed)
            self.assertEqual(restored.query_one("CollapsibleTitle").styles.color.hex, SLOP_COLOR.upper())
            self.assertTrue(restored.query_one("CollapsibleTitle").styles.text_style.bold)
            self.assertIn("Original response", self.log_text(app))

    async def test_automatic_compaction_keeps_execution_busy_and_continues_in_same_chat(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.busy = True
            app.display_message({"role": "user", "content": [{"type": "text", "text": "Keep working"}]})
            original = app.current_activity
            app.handle_event({"type": "compaction.started", "automatic": True})
            await pilot.pause()
            drawer = app.query_one(".compaction", Collapsible)
            self.assertIsNone(app.running_activity)
            self.assertTrue(app.busy)
            self.assertIsNone(app.compaction_request)
            await self.until(pilot, lambda: drawer.title != "Compaction -")
            for attempt in (1, 2, 3, 1):
                app.handle_event({"type": "compaction.progress", "automatic": True, "attempt": attempt, "limit": 3})
                app.on_mode_button_spinner_frame(ModeButton.SpinnerFrame(2))
                expected = f"Compaction {ActivityToggle.SPINNER_FRAMES[2]}"
                if attempt > 1:
                    expected += f" {attempt} / 3"
                self.assertEqual(drawer.title, expected)
                self.assertEqual(len(app.query(".compaction")), 1)
            app.query_one(MessageInput).text = "Draft"
            app.handle_event({"type": "compaction.completed", "automatic": True,
                              "messages": [self.compaction_summary]})
            await pilot.pause()
            self.assertTrue(app.busy)
            self.assertIsNotNone(app.running_activity)
            self.assertIsNotNone(app.running_activity.toggle.started_at)
            self.assertEqual(app.running_activity.toggle.step_count, 0)
            self.assertEqual(drawer.query_one("CollapsibleTitle").region.x, app.running_activity.toggle.region.x)
            self.assertEqual(drawer.title, "Compaction")
            self.assertIn(drawer.styles.border_top[0], ("", "none"))
            self.assertEqual(len(drawer.query(Markdown)), 1)
            app.handle_event({"type": "provider.message_arrived", "messages": [
                {"role": "assistant", "content": [{"type": "text", "text": "Work continued"}]}]})
            app.handle_event({"type": "execution.completed"})
            await pilot.pause()
            self.assertFalse(app.busy)
            self.assertIn(original, app.query(TurnActivity))
            self.assertIn("Work continued", self.log_text(app))
            self.assertEqual(app.query_one(MessageInput).text, "Draft")
            self.assertEqual(len(app.query(".compaction")), 1)

    async def test_automatic_compaction_failure_or_cancellation_clears_spinner(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            for outcome in ("failed", "cancelled"):
                app.busy = True
                app.handle_event({"type": "compaction.started", "automatic": True})
                await pilot.pause()
                app.handle_event({"type": f"compaction.{outcome}", "automatic": True})
                self.assertTrue(app.busy)
                app.handle_event({"type": f"execution.{outcome}", "error": "Summary failed"})
                await pilot.pause()
                self.assertFalse(app.busy)
                self.assertIsNone(app.running_compaction)
                self.assertEqual(len(app.query(".compaction")), 0)

    async def test_failed_or_cancelled_compaction_preserves_chat_and_allows_retry(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "Keep this"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.session_id is not None and not app.busy)
            original = app.query_one(UserMessage)
            for outcome in ("request.failed", "compaction.cancelled", "compaction.completed"):
                self.compaction_outcome = outcome
                entry.text = "/compact"
                await pilot.press("enter")
                await self.until(pilot, lambda: not app.busy and app.compaction_request is None)
                self.assertIs(app.query_one(UserMessage), original)
                self.assertEqual(len(app.query(".compaction")), int(outcome == "compaction.completed"))

    async def test_resume_receives_before_replacing_chat_and_restores_in_batches(self):
        self.saved_sessions = [{"id": "many", "title": "Many messages", "updatedAt": 1700000000000,
                                "workspace": "/project", "messages": [
                                    {"role": "user", "content": [{"type": "text", "text": f"Turn {i}"}]}
                                    for i in range(45)]}]
        self.history_release.clear()
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.append_chat("", "Previous conversation")
            app.query_one(MessageInput).text = "/resume"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_screen is not None and app.resume_screen.selected_id == "many")
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_transfer is not None)
            self.assertIn("Previous conversation", self.log_text(app))
            self.assertIsNone(app.session_id)
            self.history_release.set()
            await self.until(pilot, lambda: app.resume_screen is None)
            self.assertEqual(app.session_id, "many")
            self.assertEqual(len(app.query(UserMessage)), 45)
            self.assertTrue(app.connected)
            self.assertFalse(app.query_one(MessageInput).disabled)

    async def test_resume_disconnect_mid_fragment_keeps_old_chat_and_discards_partial_history(self):
        self.saved_sessions = [{"id": "partial", "title": "Partial", "updatedAt": 1700000000000,
                                "workspace": "/project", "messages": [
                                    {"role": "assistant", "content": [{"type": "text", "text": "x" * 5000}]}]}]
        self.disconnect_history = True
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.append_chat("", "Previous conversation")
            app.query_one(MessageInput).text = "/resume"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_screen is not None and app.resume_screen.selected_id == "partial")
            await pilot.press("enter")
            await self.until(pilot, lambda: not app.connected)
            self.assertIn("Previous conversation", self.log_text(app))
            self.assertIsNone(app.session_id)
            self.assertIsNone(app.resume_transfer)
            self.assertTrue(app.query_one(MessageInput).disabled)

    async def test_resume_empty_list_escape_and_failed_attach_preserve_chat(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.append_chat("", "Previous conversation")
            entry = app.query_one(MessageInput)
            entry.text = "/resume"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_screen is not None and "No saved sessions" in
                             str(app.resume_screen.query_one("#resume-status", Static).render()))
            await pilot.press("escape")
            self.assertIsNone(app.resume_screen)
            self.assertIn("Previous conversation", self.log_text(app))
            self.saved_sessions = [{"id": "saved", "title": "Existing", "updatedAt": 1700000000000,
                                    "workspace": "/project", "messages": []}]
            entry.text = "/resume"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_screen is not None and app.resume_screen.selected_id == "saved")
            self.reject = True
            await pilot.press("enter")
            await self.until(pilot, lambda: app.resume_screen is not None and not app.resume_screen.attaching)
            self.assertIn("Previous conversation", self.log_text(app))
            await pilot.press("escape")
            self.assertIsNone(app.resume_screen)

    async def test_new_keeps_current_workspace_without_sending_a_prompt(self):
        self.runtime_info.update(model="global-model", reasoning="high")
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "First conversation"
            await pilot.press("enter")
            await self.until(pilot, lambda: not app.busy)
            app.workspace = "/resumed/workspace"
            app.action_toggle_mode()
            entry.text = "/new"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.session_id is None and app.new_request is None)
            self.assertEqual(len(app.query_one("#chat").children), 0)
            self.assertEqual(app.workspace, "/resumed/workspace")
            self.assertEqual(app.mode, "sane")
            self.assertEqual(app.runtime_info["model"], "global-model")
            self.assertEqual(app.runtime_info["reasoning"], "high")
            self.assertIsNone(app.current_activity)
            self.assertEqual(app.pending_tools, {})
            self.assertEqual(app.query_one(AgentInfoBar).labels["context-percent"], "-")
            self.assertEqual(str(app.query_one("#session-title", Static).render()), "-")
            self.assertEqual(entry.text, "")
            self.assertFalse(entry.disabled)
            self.assertEqual(self.requests[-1]["type"], "session.new")
            self.assertFalse(any(r.get("text") == "/new" for r in self.requests))
            entry.text = "Fresh conversation"
            await pilot.press("enter")
            await self.until(pilot, lambda: not app.busy)
            self.assertEqual(self.requests[-1]["workspace"], "/resumed/workspace")

    async def test_new_failure_keeps_current_conversation(self):
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            app.session_id = "existing"
            app.append_chat("", "Keep this conversation")
            self.reject = True
            entry = app.query_one(MessageInput)
            entry.text = "/new"
            await pilot.press("enter")
            await self.until(pilot, lambda: app.new_request is None)
            self.assertEqual(app.session_id, "existing")
            self.assertIn("Keep this conversation", self.log_text(app))
            self.assertFalse(entry.disabled)

    async def test_chat_clicks_return_keyboard_focus_to_composer(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            app.display_message({"role": "user", "content": [{"type": "text", "text": "Question"}]})
            app.display_message({"role": "assistant", "content": [{"type": "reasoning", "text": "Notes"}]})
            await pilot.pause()
            await pilot.click(app.current_activity.toggle)
            self.assertTrue(app.current_activity.body.display)
            self.assertTrue(entry.has_focus)
            await pilot.click(app.query_one(".reasoning CollapsibleTitle"))
            self.assertFalse(app.query_one(".reasoning", Collapsible).collapsed)
            self.assertTrue(entry.has_focus)
            await pilot.click(ModeButton)
            self.assertTrue(entry.has_focus)
            await pilot.click("#chat", offset=(20, 15))
            self.assertTrue(entry.has_focus)
            await pilot.press("h", "i")
            self.assertEqual(entry.text, "hi")

    async def test_rejected_message_and_new_draft_are_both_preserved(self):
        self.reject = True
        self.release.clear()
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: app.connected)
            entry = app.query_one(MessageInput)
            entry.text = "Rejected message"
            await pilot.press("enter")
            await self.until(pilot, lambda: len(self.requests) == 1)
            entry.text = "New draft"
            self.release.set()
            await self.until(pilot, lambda: not app.busy)
            self.assertEqual(entry.text, "Rejected message\n\nNew draft")
            self.assertFalse(entry.disabled)

    async def test_permission_cards_live_in_chat_and_decisions_stay_on_tools(self):
        app = SosApp(self.path)
        async with app.run_test(size=(110, 45)) as pilot:
            await self.until(pilot, lambda: app.connected)
            for decision, label in (("yes", "Human"), ("no", "Human Refused"), ("always", "Human")):
                app.busy = True
                app.display_message({"role": "user", "content": [{"type": "text", "text": "Change a file"}]})
                app.display_message({"role": "assistant", "content": [{"type": "tool_call", "callId": decision,
                    "toolName": "write", "arguments": {"filePath": "file.txt", "content": "new"}}]})
                request = {"type": "permission.requested", "permissionId": decision, "callId": decision,
                           "permission": "edit", "tool": "write", "patterns": ["file.txt"], "always": ["*"],
                           "details": {"filepath": "file.txt", "diff": "-old\n+new"}}
                app.handle_event(request)
                await pilot.pause()
                card = app.query_one(PermissionCard)
                buttons = list(card.query(Button))
                self.assertEqual([str(button.label) for button in buttons], ["Yes", "Always yes", "No"])
                self.assertEqual([button.region.height for button in buttons], [1, 1, 1])
                self.assertEqual(card.styles.background.hex, BACKGROUND_COLOR.upper())
                self.assertEqual(card.styles.border_top[0], "")
                band = card.query_one(".permission-band")
                heading = card.query_one(".permission-heading")
                self.assertEqual(band.styles.background.hex, TITLE_MD.upper())
                self.assertEqual(band.region.height, card.region.height)
                self.assertEqual(band.region.width, 1)
                self.assertEqual(heading.region.x, band.region.right + 1)
                self.assertEqual(heading.styles.color.hex, TITLE_MD.upper())
                self.assertEqual(len(card.query(".permission-scope")), 0)
                chat = app.query_one("#chat")
                self.assertIs(card.parent, chat)
                self.assertGreater(list(chat.children).index(card), list(chat.children).index(app.current_activity))
                self.assertEqual(len(app.current_activity.query(PermissionCard)), 0)
                view = app.pending_tools[decision]
                with patch.object(app.client, "send", new_callable=AsyncMock) as send:
                    await pilot.click(card.query_one("#permission-" + decision))
                    send.assert_awaited_once()
                    self.assertEqual(send.call_args.args[0]["decision"], decision)
                    self.assertEqual(send.call_args.args[0]["permissionId"], decision)
                app.handle_event({**request, "type": "permission.resolved", "decision": decision})
                await pilot.pause()
                self.assertEqual(len(app.query(PermissionCard)), 0)
                self.assertIn(" • " + label + " • ", view.title)
                self.assertTrue(app.busy)
                result = {"type": "tool_result", "callId": decision, "output": "Result", "isError": decision == "no",
                          "metadata": {"permissions": [{"id": decision, "decision": decision, "permission": "edit", "patterns": ["file.txt"]}]}}
                app.display_message({"role": "tool", "content": [result]})
                self.assertEqual(view.title.count(" • " + label), 1)
                app.handle_event({"type": "execution.cancelled" if decision == "no" else "execution.completed"})
                # Persisted tool results reconstruct the same suffix without an approval card.
                restored = ToolView(decision, "write", {})
                restored.set_result(result)
                self.assertIn(" • " + label + " • ", restored.title)

    async def test_permission_keyboard_choices_enter_and_retry(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            app.busy = True
            entry = app.query_one(MessageInput)
            entry.text = "Keep my draft"
            for decision, keys in (("yes", ()), ("always", ("right",)), ("no", ("left",))):
                app.handle_event({"type": "permission.requested", "permissionId": decision, "callId": decision,
                                  "permission": "edit", "tool": "write", "patterns": ["file.txt"]})
                card = app.permission_card
                await self.until(pilot, card.owns_button_focus)
                self.assertTrue(card.query_one("#permission-yes", Button).has_focus)
                await pilot.click("#chat", offset=(20, 0))
                self.assertTrue(card.query_one("#permission-yes", Button).has_focus)
                await pilot.press("tab")
                self.assertTrue(card.query_one("#permission-always", Button).has_focus)
                self.assertEqual(app.mode, "sane")
                await pilot.press("shift+tab", "up", "down")
                self.assertTrue(card.query_one("#permission-yes", Button).has_focus)
                await pilot.press(*keys)
                chosen = card.query_one("#permission-" + decision, Button)
                self.assertTrue(chosen.has_focus)
                self.assertEqual(chosen.styles.background.hex, DEPTH_MD.upper())
                with patch.object(app.client, "send", new_callable=AsyncMock) as send:
                    await pilot.press("enter", "enter")
                    send.assert_awaited_once()
                    self.assertEqual(send.call_args.args[0]["decision"], decision)
                    self.assertEqual(send.call_args.args[0]["permissionId"], decision)
                    self.assertTrue(card.waiting)
                    self.assertEqual(entry.text, "Keep my draft")
                    app.handle_event({"type": "request.failed", "requestId": app.permission_reply_id, "error": "Retry"})
                    # Retry restores focus after the choice has been revealed and painted.
                    await self.until(pilot, lambda: chosen.has_focus)
                    self.assertTrue(chosen.has_focus)
                    await wait_until(pilot, lambda: not chosen.has_class("-active"), "permission button release")
                    await pilot.press("enter")
                    await wait_until(pilot, lambda: send.await_count == 2, "permission retry submission")
                    self.assertEqual(send.await_count, 2)
                app.handle_event({"type": "permission.dismissed", "permissionId": decision})
                await self.until(pilot, lambda: entry.has_focus)
                self.assertTrue(entry.has_focus)

    async def test_external_read_permission_is_visible_in_sane_and_dismisses_on_cancel(self):
        app = SosApp(self.path)
        async with app.run_test(size=(100, 40)) as pilot:
            await self.until(pilot, lambda: app.connected)
            self.assertEqual(app.mode, "sane")
            app.busy = True
            app.display_message({"role": "user", "content": [{"type": "text", "text": "Inspect another project"}]})
            app.display_message({"role": "assistant", "content": [{"type": "tool_call", "callId": "read",
                "toolName": "read", "arguments": {"filePath": "/other/file.txt"}}]})
            request = {"type": "permission.requested", "permissionId": "external", "callId": "read",
                       "permission": "external_directory", "tool": "read", "patterns": ["/other/*"],
                       "always": ["/other/*"], "details": {"filepath": "/other/file.txt", "parentDir": "/other"}}
            app.handle_event(request)
            await pilot.pause()
            self.assertIsNotNone(app.permission_card)
            app.handle_event({**request, "type": "permission.resolved", "decision": "always"})
            await pilot.pause()
            self.assertIsNone(app.permission_card)
            self.assertEqual(app.query_one(ReadView).render().plain, "read • Human · /other/file.txt")
            app.handle_event({**request, "permissionId": "next"})
            await pilot.pause()
            app.handle_event({**request, "type": "permission.dismissed", "permissionId": "next"})
            await pilot.pause()
            self.assertIsNone(app.permission_card)
            app.handle_event({"type": "execution.cancelled"})

    async def test_unsupported_protocol(self):
        self.version = 1
        app = SosApp(self.path)
        async with app.run_test() as pilot:
            await self.until(pilot, lambda: "Unsupported daemon handshake" in self.log_text(app))
            self.assertEqual(self.requests, [])
            self.assertFalse(app.connected)


if __name__ == "__main__":
    unittest.main()
