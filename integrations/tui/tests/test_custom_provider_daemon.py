"""Real daemon + local Chat Completions server, with isolated XDG storage."""

import asyncio
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from textual.widgets import OptionList

from sos_tui.client import DaemonClient
from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput


DAEMON = Path(__file__).resolve().parents[3] / "build-ai/bin/sosd"


@unittest.skipUnless(DAEMON.is_file(), "Build build-ai/bin/sosd first")
class CustomProviderDaemonTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="sos-custom-", dir="/tmp")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "sample.txt").write_text("A local tool result", encoding="utf-8")
        self.requests = []
        self.http = await asyncio.start_server(self.serve_http, "127.0.0.1", 0)
        self.addAsyncCleanup(self.close_http)
        self.base_url = f"http://127.0.0.1:{self.http.sockets[0].getsockname()[1]}/v1"
        self.environment = {**os.environ, **{f"XDG_{kind}_HOME": str(self.root / kind.lower())
                                           for kind in ("STATE", "CONFIG", "DATA", "CACHE")}}
        self.processes = []
        self.addAsyncCleanup(self.close_daemons)
        self.client = None
        await self.start_daemon()

    async def close_http(self):
        self.http.close()
        await self.http.wait_closed()

    async def close_daemons(self):
        if self.client:
            await self.client.close()
        for process in self.processes:
            if process.returncode is None:
                process.terminate()
            await asyncio.wait_for(process.wait(), 5)

    async def start_daemon(self):
        path = str(self.root / "daemon.sock")
        process = await asyncio.create_subprocess_exec(str(DAEMON), path, str(self.root / "sessions"),
                                                      env=self.environment, stdout=asyncio.subprocess.DEVNULL,
                                                      stderr=asyncio.subprocess.DEVNULL)
        self.processes.append(process)
        for _ in range(100):
            if Path(path).exists():
                break
            await asyncio.sleep(0.02)
        self.client = DaemonClient(path)
        await self.client.connect_existing()
        await self.command({"type": "integration.configure", "workspace": str(self.root), "mode": "sane"})

    async def serve_http(self, reader, writer):
        try:
            header = (await reader.readuntil(b"\r\n\r\n")).decode()
            length = next((int(line.split(":", 1)[1]) for line in header.splitlines()
                           if line.lower().startswith("content-length:")), 0)
            body = json.loads(await reader.readexactly(length)) if length else None
            self.requests.append((header, body))
            if header.startswith("GET /v1/models "):
                response = {"data": [{"id": "local-model"}]}
            elif header.startswith("POST /v1/chat/completions "):
                messages = body["messages"]
                title = "Generate a short session title" in str(messages[0].get("content"))
                if title:
                    message, reason = {"role": "assistant", "content": "Local test"}, "stop"
                elif not body.get("tools"):
                    message, reason = {"role": "assistant", "content": "Summary of the local file inspection."}, "stop"
                elif any(m["role"] == "tool" for m in messages):
                    message, reason = {"role": "assistant", "content": "Read the local file successfully."}, "stop"
                else:
                    message, reason = {"role": "assistant", "content": None, "tool_calls": [
                        {"id": "call-read", "type": "function", "function": {"name": "read",
                         "arguments": json.dumps({"filePath": str(self.root / "sample.txt")})}}
                    ]}, "tool_calls"
                response = {"id": f"chat-{len(self.requests)}", "choices": [{"message": message, "finish_reason": reason}],
                            "usage": {"prompt_tokens": 100, "completion_tokens": 20}}
            else:
                response = {"error": "Unexpected endpoint"}
            data = json.dumps(response).encode()
            writer.write(f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {len(data)}\r\nConnection: close\r\n\r\n".encode() + data)
            await writer.drain()
        finally:
            writer.close()
            await writer.wait_closed()

    async def receive_until(self, predicate):
        events = []
        for _ in range(100):
            event = await asyncio.wait_for(self.client.receive(), 10)
            events.append(event)
            self.assertNotEqual(event["type"], "request.failed", event)
            self.assertNotEqual(event["type"], "execution.failed", event)
            if predicate(event):
                return event, events
        self.fail("Expected daemon event not received")

    async def command(self, payload):
        request_id = f"request-{len(self.requests)}-{payload['type']}"
        await self.client.send({**payload, "requestId": request_id})
        reply, _ = await self.receive_until(lambda event: event.get("requestId") == request_id)
        return reply

    async def test_tool_loop_compaction_restart_resume_and_delete(self):
        created = await self.command({"type": "provider.create", "name": "Local test", "base_url": self.base_url, "key": "fake-key"})
        provider = created["created_provider"]
        selected = await self.command({"type": "provider.select", "provider": provider})
        self.assertEqual(selected["info"]["provider"], provider)
        await self.command({"type": "model.list"})
        await self.command({"type": "model.select", "provider": provider, "model": "local-model", "context_limit": 65536})
        await self.client.send({"type": "message.send", "requestId": "turn", "text": "Read sample.txt", "workspace": str(self.root), "mode": "sane"})
        completed, events = await self.receive_until(lambda event: event["type"] == "execution.completed")
        serialized = json.dumps(events)
        self.assertIn("A local tool result", serialized)
        self.assertIn("Read the local file successfully", serialized)
        self.assertNotIn("fake-key", serialized)
        sessions = await self.command({"type": "session.list"})
        session = sessions["sessions"][0]["id"]
        await self.client.send({"type": "session.compact", "requestId": "compact"})
        await self.receive_until(lambda event: event["type"] == "compaction.completed")
        await self.client.close()
        await asyncio.wait_for(self.processes[-1].wait(), 5)
        await self.start_daemon()
        self.assertEqual(self.client.info["provider"], provider)
        self.assertEqual(self.client.info["context_limit"], 65536)
        await self.command({"type": "session.attach", "sessionId": session})
        await self.client.send({"type": "message.send", "requestId": "resume-turn", "text": "Continue", "workspace": str(self.root), "mode": "sane"})
        await self.receive_until(lambda event: event["type"] == "execution.completed")
        # Switch to a second configured instance before deleting the first.
        second = await self.command({"type": "provider.create", "name": "Second local", "base_url": self.base_url})
        await self.command({"type": "provider.select", "provider": second["created_provider"]})
        deleted = await self.command({"type": "provider.delete", "provider": provider})
        self.assertNotIn(provider, [item["id"] for item in deleted["providers"]])
        self.assertNotIn(provider, json.loads((self.root / "data/sos/auth.json").read_text()))
        self.assertTrue((self.root / "sessions" / session / "session.sqlite").is_file())
        generations = [body for _, body in self.requests if body]
        self.assertTrue(any(any(message["role"] == "tool" for message in body["messages"]) for body in generations))
        self.assertTrue(all(not any(field in body for field in ("input", "reasoning", "store")) for body in generations))
        self.assertTrue(all("/responses " not in header for header, _ in self.requests))

    async def test_invalid_custom_fields_keep_connection_open_for_retry(self):
        for name, url, error in (
            ("Local test", "localhost:11434/v1", "HTTP(S) base URL"),
            ("", self.base_url, "name"),
        ):
            await self.client.send({"type": "provider.create", "requestId": "invalid",
                                    "name": name, "base_url": url})
            reply = await asyncio.wait_for(self.client.receive(), 10)
            self.assertEqual(reply["type"], "request.failed")
            self.assertEqual(reply["requestId"], "invalid")
            self.assertIn(error, reply["error"])
            listed = await self.command({"type": "provider.list"})
            self.assertEqual(listed["type"], "provider.listed")
        created = await self.command({"type": "provider.create", "name": "Local test", "base_url": self.base_url})
        self.assertIn("created_provider", created)

    async def test_tui_connect_closes_dialog_and_restores_composer(self):
        created = await self.command({"type": "provider.create", "name": "Local test", "base_url": self.base_url})
        app = SosApp()
        app.client = self.client
        with patch.object(app, "listen"):
            async with app.run_test(size=(100, 40)) as pilot:
                app.connected = True
                await app.on_message_input_submitted(MessageInput.Submitted("/connect"))
                await pilot.pause()
                app.handle_event(await asyncio.wait_for(self.client.receive(), 10))
                await pilot.pause()
                screen = app.connect_screen
                options = screen.query_one("#connect-providers", OptionList)
                options.highlighted = list(screen.providers).index(created["created_provider"])
                await pilot.press("enter", "enter")
                app.handle_event(await asyncio.wait_for(self.client.receive(), 10))
                await pilot.pause()
                self.assertIsNone(app.connect_screen)
                self.assertIsNone(app.switch_request)
                self.assertFalse(app.query_one(MessageInput).disabled)
                self.assertTrue(app.query_one(MessageInput).has_focus)
