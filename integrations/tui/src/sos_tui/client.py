"""Minimal asynchronous JSONL client for the local daemon."""

import asyncio
import json
from typing import Any

from .daemon_startup import connect_or_start


class DaemonClient:
    def __init__(self, socket_path: str) -> None:
        self.socket_path = socket_path
        self.reader: asyncio.StreamReader | None = None
        self.writer: asyncio.StreamWriter | None = None
        self.info: dict[str, Any] = {}
        self.handshake_complete = False

    async def connect(self) -> None:
        try:
            await connect_or_start(self.connect_existing, self.socket_path)
        except BaseException:
            await self.close()
            raise

    async def connect_existing(self) -> None:
        try:
            self.reader, self.writer = await asyncio.wait_for(
                asyncio.open_unix_connection(self.socket_path, limit=16 * 1024 * 1024),
                timeout=5,
            )
            ready = await asyncio.wait_for(self.receive(), timeout=5)
            if ready.get("type") != "daemon.ready" or ready.get("protocolVersion") != 7:
                raise ValueError("Unsupported daemon handshake: expected protocol 7. "
                                 "Update the TUI and daemon together, then close old integrations and restart.")
            self.info = ready.get("info", {})
            self.handshake_complete = True
        except BaseException:
            await self.close()
            raise

    async def receive(self) -> dict[str, Any]:
        reader = self.reader
        if reader is None:
            raise ConnectionError("Not connected to the daemon")
        while True:
            line = await reader.readline()
            if not line:
                raise ConnectionError("Daemon disconnected")
            if not line.endswith(b"\n"):
                raise ConnectionError("Daemon disconnected during a message")
            message = json.loads(line)
            if not isinstance(message, dict) or not isinstance(message.get("type"), str):
                raise ValueError("Invalid daemon message")
            if message["type"] == "integration.ping":
                ping_id = message.get("pingId")
                if not isinstance(ping_id, str):
                    raise ValueError("Invalid daemon ping")
                await self.send({"type": "integration.pong", "pingId": ping_id})
                continue
            return message

    async def send(self, message: dict[str, Any]) -> None:
        if self.writer is None or self.writer.is_closing():
            raise ConnectionError("Not connected to the daemon")
        data = (json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8")
        if len(data) > 16 * 1024 * 1024:
            raise ValueError("Message exceeds the daemon's 16 MiB limit")
        self.writer.write(data)
        await self.writer.drain()

    async def close(self) -> None:
        writer, self.writer = self.writer, None
        self.reader = None
        notify, self.handshake_complete = self.handshake_complete, False
        if writer is not None:
            try:
                if notify and not writer.is_closing():
                    writer.write(b'{"type":"integration.close"}\n')
                    await asyncio.wait_for(writer.drain(), timeout=1)
            except (OSError, TimeoutError):
                pass
            finally:
                writer.close()
                try:
                    await writer.wait_closed()
                except OSError:
                    pass
