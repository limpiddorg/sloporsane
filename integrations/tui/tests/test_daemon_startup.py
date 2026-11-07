"""Real subprocess and Unix socket tests, using a tiny daemon without model calls."""

import asyncio
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from sos_tui.client import DaemonClient
# Exercise real startup without requiring a packaged/installed TUI.
DAEMON = Path(__file__).resolve().parents[3] / "build-ai/bin/sosd"


FAKE_DAEMON = '''
import asyncio, json, os, sys, time
from pathlib import Path
with open(os.environ["SOS_TEST_STARTS"], "a") as marker:
    marker.write("started\\n")
mode = os.environ.get("SOS_TEST_MODE", "")
if mode == "fail":
    print("Test startup failure", flush=True)
    sys.exit(7)
time.sleep(60 if mode == "stall" else 0.1)
async def serve(reader, writer):
    writer.write((json.dumps({"type": "daemon.ready", "protocolVersion": 7,
        "info": {"pid": os.getpid(), "environment_present": bool(os.environ.get("SOS_TEST_ENV"))}}) + "\\n").encode())
    await writer.drain()
    await reader.read()
    writer.close()
async def main():
    server = await asyncio.start_unix_server(serve, path=sys.argv[1])
    async with server:
        await server.serve_forever()
asyncio.run(main())
'''


class DaemonStartupTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="sos-start-", dir="/tmp")
        self.root = Path(self.directory.name)
        self.socket_path = str(self.root / "daemon.sock")
        script = self.root / "fake.py"
        script.write_text(FAKE_DAEMON, encoding="utf-8")
        executable = self.root / "sosd"
        executable.write_text(f"#!/bin/sh\nexec {shlex.quote(sys.executable)} {shlex.quote(str(script))} \"$@\"\n")
        executable.chmod(0o755)
        self.processes = []
        self.clients = []
        original_popen = subprocess.Popen

        def spawn(*args, **kwargs):
            process = original_popen(*args, **kwargs)
            self.processes.append(process)
            return process

        self.patches = [
            patch("sos_tui.daemon_startup.BUNDLED_DAEMON", executable),
            patch("sos_tui.daemon_startup.subprocess.Popen", side_effect=spawn),
            patch.dict(os.environ, {"XDG_STATE_HOME": str(self.root / "state"),
                                    "XDG_DATA_HOME": str(self.root / "data"),
                                    "XDG_CONFIG_HOME": str(self.root / "config"),
                                    "SOS_TEST_STARTS": str(self.root / "starts"),
                                    "SOS_TEST_MODE": "", "SOS_TEST_ENV": "inherited-value"}),
        ]
        for mocked in self.patches:
            mocked.start()

    async def asyncTearDown(self):
        for client in self.clients:
            await client.close()
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
            try:
                await asyncio.to_thread(process.wait, timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                await asyncio.to_thread(process.wait)
        for mocked in reversed(self.patches):
            mocked.stop()
        self.directory.cleanup()

    def client(self):
        client = DaemonClient(self.socket_path)
        self.clients.append(client)
        return client

    async def test_concurrent_clients_start_once_and_reuse_live_daemon(self):
        first, second = self.client(), self.client()
        await asyncio.gather(first.connect(), second.connect())
        self.assertEqual(first.info["pid"], second.info["pid"])
        self.assertTrue(first.info["environment_present"])
        self.assertEqual(len(self.processes), 1)
        self.assertEqual((self.root / "starts").read_text(), "started\n")
        await first.close()
        self.assertIsNone(self.processes[0].poll())
        with patch("sos_tui.daemon_startup.BUNDLED_DAEMON", self.root / "absent"):
            await self.client().connect()
        self.assertEqual(len(self.processes), 1)

    async def test_stale_socket_is_replaced(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stale:
            stale.bind(self.socket_path)
        await self.client().connect()
        self.assertEqual(len(self.processes), 1)

    async def test_regular_file_is_not_removed(self):
        path = Path(self.socket_path)
        path.write_text("Keep me")
        with self.assertRaises(OSError):
            await self.client().connect()
        self.assertEqual(path.read_text(), "Keep me")
        self.assertEqual(self.processes, [])

    async def test_missing_bundle_reports_build_instruction(self):
        with patch("sos_tui.daemon_startup.BUNDLED_DAEMON", self.root / "absent"):
            with self.assertRaisesRegex(FileNotFoundError, "rebuild and install"):
                await self.client().connect()
        self.assertEqual(self.processes, [])

    async def test_startup_failure_reports_log_and_exit_code(self):
        with patch.dict(os.environ, {"SOS_TEST_MODE": "fail"}):
            with self.assertRaisesRegex(ConnectionError, "exited with code 7"):
                await self.client().connect()
        self.assertIn("Test startup failure", (self.root / "state/sos/sosd.log").read_text())

    async def test_startup_timeout_terminates_unready_child(self):
        with patch.dict(os.environ, {"SOS_TEST_MODE": "stall"}), \
                patch("sos_tui.daemon_startup.STARTUP_TIMEOUT", 0.3):
            with self.assertRaisesRegex(ConnectionError, "Timed out"):
                await self.client().connect()
        await asyncio.to_thread(self.processes[0].wait, timeout=2)
        self.assertIsNotNone(self.processes[0].returncode)

    @unittest.skipUnless(DAEMON.is_file(), "Build build-ai/bin/sosd first")
    async def test_real_daemon_preserves_credentials_on_invalid_input_and_restores_selection(self):
        path = self.root / "data/sos/auth.json"
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps({provider: {"type": "api", "method": "api", "key": "fake-" + provider}
                                    for provider in ("openai", "openrouter")}))
        path.chmod(0o600)
        with patch("sos_tui.daemon_startup.BUNDLED_DAEMON", DAEMON):
            client = self.client()
            await client.connect()
            self.assertEqual(Path(self.socket_path).stat().st_mode & 0o777, 0o600)
            for provider in ("openai", "openrouter"):
                await client.send({"type": "auth.api", "requestId": "key", "provider": provider, "key": "invalid\nkey"})
                event = await client.receive()
                self.assertEqual(event["type"], "request.failed")
                self.assertNotIn("fake-", json.dumps(event))
            await client.send({"type": "provider.select", "requestId": "select", "provider": "openai", "model": "gpt-5"})
            event = await client.receive()
            self.assertEqual(event["type"], "provider.selected")
            self.assertEqual(event["info"]["provider"], "openai")
            await client.send({"type": "reasoning.next", "requestId": "reasoning"})
            self.assertEqual((await client.receive())["info"]["reasoning"], "low")
            await client.send({"type": "session.new", "requestId": "new"})
            self.assertEqual((await client.receive())["info"]["reasoning"], "low")
            await client.send({"type": "session.list", "requestId": "list"})
            self.assertEqual((await client.receive())["sessions"], [])
            path = self.root / "data/sos/auth.json"
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(json.loads(path.read_text())["openrouter"]["key"], "fake-openrouter")
            await client.close()
            await asyncio.to_thread(self.processes[-1].wait, timeout=3)
            client = self.client()
            await client.connect()
            self.assertEqual(client.info["provider"], "openai")
            self.assertEqual(client.info["model"], "gpt-5")
            self.assertEqual(client.info["reasoning"], "low")
            await client.send({"type": "provider.list", "requestId": "providers"})
            self.assertTrue(all(item["connected"] for item in (await client.receive())["providers"]))
            self.assertNotIn("fake-", (self.root / "state/sos/sosd.log").read_text())

    @unittest.skipUnless(DAEMON.is_file(), "Build build-ai/bin/sosd first")
    async def test_real_daemon_stops_only_after_last_client_and_restarts_on_next_launch(self):
        with patch("sos_tui.daemon_startup.BUNDLED_DAEMON", DAEMON):
            client = self.client()
            await client.connect()
            await client.send({"type": "session.list", "requestId": "list"})
            response = await client.receive()
            self.assertEqual(response["type"], "session.listed")
            self.assertEqual(response["sessions"], [])
            second = self.client()
            await second.connect()
            self.assertEqual(len(self.processes), 1)
            await client.close()
            self.assertIsNone(self.processes[0].poll())
            await second.send({"type": "session.list"})
            self.assertEqual((await second.receive())["sessions"], [])
            await second.close()
            self.assertEqual(await asyncio.to_thread(self.processes[0].wait, timeout=3), 0)
            self.assertFalse(Path(self.socket_path).exists())
            third = self.client()
            await third.connect()
            self.assertEqual(len(self.processes), 2)
            self.assertNotEqual(self.processes[0].pid, self.processes[1].pid)
            await third.close()
            self.assertEqual(await asyncio.to_thread(self.processes[1].wait, timeout=3), 0)
