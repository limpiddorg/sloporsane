"""History transfer validation and real-stream regression for the former 16 MiB limit."""

import asyncio
import json
import unittest

from sos_tui.client import DaemonClient
from sos_tui.history_transfer import HistoryTransfer


def header(count=1):
    return {"type": "session.attach_started", "requestId": "r", "sessionId": "s",
            "session": {"id": "s"}, "messageCount": count}


def chunks(message, index=0, size=64 * 1024):
    data = json.dumps(message, ensure_ascii=False)
    for part, offset in enumerate(range(0, len(data), size)):
        yield {"type": "session.history_chunk", "requestId": "r", "sessionId": "s",
               "messageIndex": index, "partIndex": part, "last": offset + size >= len(data),
               "data": data[offset:offset + size]}


def completed(count=1):
    return {"type": "session.attached", "requestId": "r", "sessionId": "s", "messageCount": count}


class HistoryTransferTest(unittest.TestCase):
    def test_empty_history(self):
        self.assertEqual(HistoryTransfer(header(0)).accept(completed(0))["messages"], [])

    def test_ordered_unicode_messages_are_preserved(self):
        messages = [{"role": "assistant", "content": [{"type": "text", "text": 'é日本語\\"\n' * 30}]},
                    {"role": "tool", "content": [{"type": "tool_result", "callId": "c", "output": "ok"}]}]
        transfer = HistoryTransfer(header(2))
        for index, message in enumerate(messages):
            for frame in chunks(message, index, size=13):
                self.assertIsNone(transfer.accept(frame))
        self.assertEqual(transfer.accept(completed(2))["messages"], messages)

    def test_missing_duplicate_out_of_order_and_foreign_fragments_are_rejected(self):
        frames = list(chunks({"role": "user", "content": []}, size=10))
        for changes in ({"partIndex": 1}, {"messageIndex": 1}, {"requestId": "other"},
                        {"sessionId": "other"}, {"last": "true"}, {"data": ""}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                HistoryTransfer(header()).accept({**frames[0], **changes})
        transfer = HistoryTransfer(header())
        transfer.accept(frames[0])
        with self.assertRaises(ValueError):
            transfer.accept(frames[0])
        with self.assertRaises(ValueError):
            transfer.accept(completed())
        with self.assertRaises(ValueError):
            HistoryTransfer(header()).accept(completed())

    def test_invalid_headers_and_final_counts_are_rejected(self):
        for count in (-1, True, "1"):
            with self.assertRaises(ValueError):
                HistoryTransfer(header(count))
        with self.assertRaises(ValueError):
            HistoryTransfer(header(0)).accept(completed(1))


class HistoryStreamTest(unittest.IsolatedAsyncioTestCase):
    async def test_large_message_and_large_total_history_keep_connection_and_pings_alive(self):
        # A single message above the former limit, then a multi-message history above it.
        for sizes in ([17 * 1024 * 1024], [1024 * 1024] * 17):
            with self.subTest(sizes=sizes):
                await self.exchange(sizes)

    async def exchange(self, sizes):
        finished = asyncio.get_running_loop().create_future()

        async def serve(reader, writer):
            async def send(event):
                data = (json.dumps(event, ensure_ascii=False) + "\n").encode()
                self.assertLessEqual(len(data), 256 * 1024)
                writer.write(data)
                await writer.drain()
            try:
                await send(header(len(sizes)))
                for index, size in enumerate(sizes):
                    message = {"role": "assistant", "content": [{"type": "text", "text": "x" * size}]}
                    for frame in chunks(message, index):
                        await send(frame)
                        if frame["partIndex"] % 20 == 0:
                            await send({"type": "integration.ping", "pingId": "alive"})
                            pong = json.loads(await reader.readline())
                            self.assertEqual(pong, {"type": "integration.pong", "pingId": "alive"})
                await send(completed(len(sizes)))
                self.assertEqual(json.loads(await reader.readline())["type"], "session.list")
                await send({"type": "session.listed", "sessions": []})
                finished.set_result(None)
            except BaseException as error:
                finished.set_exception(error)
            finally:
                writer.close()
                await writer.wait_closed()

        server = await asyncio.start_server(serve, "127.0.0.1", 0)
        client = DaemonClient("unused")
        try:
            client.reader, client.writer = await asyncio.open_connection(
                "127.0.0.1", server.sockets[0].getsockname()[1], limit=16 * 1024 * 1024)
            async with asyncio.timeout(20):
                transfer = HistoryTransfer(await client.receive())
                while (result := transfer.accept(await client.receive())) is None:
                    pass
                self.assertEqual([len(m["content"][0]["text"]) for m in result["messages"]], sizes)
                await client.send({"type": "session.list"})
                self.assertEqual((await client.receive())["type"], "session.listed")
                await finished
        finally:
            await client.close()
            server.close()
            await server.wait_closed()
