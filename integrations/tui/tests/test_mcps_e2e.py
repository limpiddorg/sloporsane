"""Real Textual app, daemon and loopback OAuth/stdio servers, no real accounts.

Requires build-ai/bin/sosd. Browser opening is replaced by a local HTTP visit;
all management, callback, token refresh and tools/list traffic is real.
"""
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.request import urlopen

from textual.widgets import Button, Input
from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput

ROOT = Path(__file__).resolve().parents[3]
DAEMON = ROOT / "build-ai/bin/sosd"


@unittest.skipUnless(DAEMON.exists(), "Build build-ai/bin/sosd for MCP end-to-end management test")
class McpsE2ETest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        # Unix socket paths must also fit when the checkout is a deep SOS worktree.
        self.directory = tempfile.TemporaryDirectory(prefix="mcp-e2e-", dir="/tmp")
        self.root = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)
        self.processes = []
        self.enterContext(patch.dict(os.environ, {
            "XDG_DATA_HOME": str(self.root / "data"), "XDG_CONFIG_HOME": str(self.root / "config"),
            "XDG_STATE_HOME": str(self.root / "state"), "XDG_CACHE_HOME": str(self.root / "cache")}))
        self.addAsyncCleanup(self.stop_processes)
        server = subprocess.Popen([sys.executable, str(ROOT / "tests/fixtures/mcp_oauth.py")],
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.processes.append(server)
        line = await asyncio.wait_for(asyncio.to_thread(server.stdout.readline), 5)
        self.base = json.loads(line)["url"]
        self.socket = str(self.root / "daemon.sock")
        daemon = subprocess.Popen([str(DAEMON), self.socket, str(self.root / "sessions")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.processes.append(daemon)
        for _ in range(100):
            if Path(self.socket).exists():
                break
            await asyncio.sleep(0.05)
        self.assertTrue(Path(self.socket).exists())
        self.app = SosApp(self.socket)
        self.app.launch_workspace = str(self.root)
        self.enterContext(patch("sos_tui.mcps_screen.webbrowser.open", side_effect=self.browser))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(120, 65)))
        await self.wait(lambda: self.app.connected)
        entry = self.app.query_one(MessageInput)
        entry.text = "/mcps"
        entry.focus()
        await self.pilot.press("enter")
        await self.pilot.pause()
        self.screen = self.app.mcps_screen
        await self.wait(lambda: not self.screen.pending)

    def browser(self, url):
        with urlopen(url, timeout=5) as response:
            return response.status == 200

    async def stop_processes(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
            try:
                await asyncio.wait_for(asyncio.to_thread(process.wait), 5)
            except TimeoutError:
                process.kill()
                await asyncio.to_thread(process.wait)
            if process.stdout:
                process.stdout.close()

    async def wait(self, condition):
        for _ in range(200):
            if condition():
                return
            await asyncio.sleep(0.025)
        self.fail("Timed out waiting for MCP state")

    async def press(self, name):
        if name.endswith("cancel"):
            await self.pilot.press("escape")
        elif name in ("confirm", "token-save"):
            await self.pilot.press("enter")
        else:
            self.app.screen.query_one(f"#mcp-{name}", Button).press()
        await self.pilot.pause()
        await self.wait(lambda: not self.screen.pending)

    async def load_config(self, servers):
        # MCP configuration changes use the JSON file, not UI actions.
        path = self.root / ".sos/mcps.json"
        path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps({"mcpServers": servers}))
        self.screen.request({"type": "mcp.list"})
        await self.pilot.pause()
        await self.wait(lambda: not self.screen.pending)

    async def add_remote(self):
        await self.load_config({"remote": {"url": self.base + "/mcp"}})

    async def connect_and_approve(self):
        await self.press("connect")
        await self.press("confirm")

    async def test_connect_handles_oauth_and_disable_keeps_transport_connected(self):
        await self.add_remote()
        await self.connect_and_approve()
        await self.wait(lambda: self.screen.selected()["state"] == "connected" and not self.screen.pending)
        self.assertEqual(self.screen.selected()["tool_count"], 1)
        auth = self.root / "data/sos/mcps/auth.json"
        self.assertEqual(auth.stat().st_mode & 0o777, 0o600)
        credentials = next(iter(json.loads(auth.read_text())["servers"].values()))["credentials"]
        self.assertEqual(credentials["refresh_token"], "rotated-refresh")
        self.assertNotIn("refresh_token", json.dumps(self.screen.servers))
        await self.press("enable")
        self.assertFalse(self.screen.selected()["config"]["enabled"])
        self.assertEqual(self.screen.selected()["state"], "connected")
        await self.press("enable")
        self.assertTrue(self.screen.selected()["config"]["enabled"])
        self.assertEqual(self.screen.selected()["state"], "connected")
        await self.press("connect")
        self.assertEqual(self.screen.selected()["state"], "disconnected")
        await self.load_config({})
        self.assertEqual(self.screen.servers, [])

    async def test_local_connect_does_not_require_enabled_or_secret_management(self):
        await self.load_config({"local": {"command": sys.executable,
            "args": [str(ROOT / "tests/fixtures/mcp_stdio.py")]}})
        await self.press("enable")
        await self.connect_and_approve()
        self.assertEqual(self.screen.selected()["state"], "connected")
        self.assertFalse(self.screen.selected()["config"]["enabled"])
        self.assertGreater(self.screen.selected()["tool_count"], 0)
        self.assertFalse(self.screen.query("#mcp-token, #mcp-content, #mcp-oauth-revoke"))
        await self.load_config({})
        self.assertEqual(self.screen.servers, [])

    async def test_global_choices_restore_on_configure_and_propagate_to_an_open_screen(self):
        config = {"local": {"command": sys.executable,
                            "args": [str(ROOT / "tests/fixtures/mcp_stdio.py")]}}
        await self.load_config(config)
        await self.connect_and_approve()
        await self.press("enable")
        preferences = self.root / "config/sos/mcp-preferences.json"
        self.assertEqual(json.loads(preferences.read_text())["local"], {"connected": True, "enabled": False})
        self.assertEqual(preferences.stat().st_mode & 0o777, 0o600)
        self.assertEqual(json.loads((self.root / ".sos/mcps.json").read_text())["mcpServers"], config)

        async def request(reader, writer, payload):
            writer.write((json.dumps({"requestId": "probe", **payload}) + "\n").encode())
            await writer.drain()
            while True:
                reply = json.loads(await asyncio.wait_for(reader.readline(), 10))
                if reply.get("requestId") == "probe":
                    return reply

        # Configuration responds before restoration; the background job will
        # eventually connect this server without a model turn or a list request.
        socket = str(self.root / "restored.sock")
        daemon = subprocess.Popen([str(DAEMON), socket, str(self.root / "restored-sessions")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.processes.append(daemon)
        await self.wait(lambda: Path(socket).exists())
        reader, writer = await asyncio.open_unix_connection(socket)
        try:
            await reader.readline()
            result = await request(reader, writer, {"type": "integration.configure", "workspace": str(self.root), "mode": "slop"})
            self.assertEqual(result["type"], "integration.configured")
            for _ in range(100):
                result = await request(reader, writer, {"type": "mcp.list"})
                if result["servers"][0]["state"] == "connected":
                    break
                await asyncio.sleep(0.05)
            self.assertEqual(result["servers"][0]["state"], "connected")
            self.assertFalse(result["servers"][0]["config"]["enabled"])
        finally:
            writer.close()
            await writer.wait_closed()

        # Another integration in the original daemon changes the shared choice.
        reader, writer = await asyncio.open_unix_connection(self.socket)
        try:
            await reader.readline()
            await request(reader, writer, {"type": "integration.configure", "workspace": str(self.root), "mode": "slop"})
            item = self.screen.selected()
            result = await request(reader, writer, {"type": "mcp.disconnect", "name": "local", "identity": item["identity"]})
            self.assertEqual(result["servers"][0]["state"], "disconnected")
            await self.wait(lambda: self.screen.selected()["state"] == "disconnected")
            self.assertFalse(json.loads(preferences.read_text())["local"]["connected"])
        finally:
            writer.close()
            await writer.wait_closed()

    async def test_slow_restore_does_not_block_configure_or_listing_and_reports_failure(self):
        script = self.root / "slow_mcp.py"
        marker, waiting, release = (self.root / name for name in ("second-start", "waiting", "release"))
        script.write_text("import pathlib, runpy, sys, time\n"
                          f"marker, waiting, release = map(pathlib.Path, {tuple(map(str, (marker, waiting, release)))!r})\n"
                          "if marker.exists():\n"
                          "    waiting.touch()\n"
                          "    while not release.exists(): time.sleep(0.01)\n"
                          "    sys.exit(1)\n"
                          f"runpy.run_path({str(ROOT / 'tests/fixtures/mcp_stdio.py')!r}, run_name='__main__')\n")
        await self.load_config({"slow": {"command": sys.executable, "args": [str(script)]}})
        await self.connect_and_approve()
        await self.wait(lambda: self.screen.selected()["state"] == "connected")
        marker.touch()

        socket = str(self.root / "slow.sock")
        daemon = subprocess.Popen([str(DAEMON), socket, str(self.root / "slow-sessions")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.processes.append(daemon)
        await self.wait(lambda: Path(socket).exists())
        reader, writer = await asyncio.open_unix_connection(socket)
        try:
            await reader.readline()  # daemon.ready
            writer.write((json.dumps({"type": "integration.configure", "requestId": "configure",
                                      "workspace": str(self.root), "mode": "slop"}) + "\n").encode())
            await writer.drain()
            self.assertEqual(json.loads(await asyncio.wait_for(reader.readline(), 1))["type"], "integration.configured")
            await self.wait(waiting.exists)
            writer.write((json.dumps({"type": "mcp.list", "requestId": "list"}) + "\n").encode())
            await writer.drain()
            listing = json.loads(await asyncio.wait_for(reader.readline(), 1))
            self.assertTrue(listing["restoring"])
            self.assertEqual(listing["servers"][0]["state"], "connecting")
            release.touch()
            failed = json.loads(await asyncio.wait_for(reader.readline(), 5))
            self.assertEqual(failed["type"], "mcp.restored")
            self.assertIn("slow:", failed["errors"][0])
            writer.write((json.dumps({"type": "mcp.list", "requestId": "after"}) + "\n").encode())
            await writer.drain()
            listing = json.loads(await asyncio.wait_for(reader.readline(), 1))
            self.assertFalse(listing["restoring"])
            self.assertEqual(listing["servers"][0]["state"], "failed")
        finally:
            release.touch()
            writer.close()
            await writer.wait_closed()

    async def test_cancel_oauth_does_not_connect_and_connect_can_retry(self):
        await self.add_remote()
        with patch("sos_tui.mcps_screen.webbrowser.open", return_value=True):
            await self.connect_and_approve()
            await self.wait(lambda: self.screen.oauth_active)
            await self.press("oauth-cancel")
            await self.wait(lambda: not self.screen.oauth_active and not self.screen.pending)
        self.assertNotEqual(self.screen.selected()["state"], "connected")
        auth = self.root / "data/sos/mcps/auth.json"
        entry = next(iter(json.loads(auth.read_text())["servers"].values()))
        self.assertNotIn("credentials", entry)
        await self.press("connect")
        await self.wait(lambda: self.screen.selected()["state"] == "connected" and not self.screen.pending)
        self.assertTrue(self.screen.selected()["trusted"])
