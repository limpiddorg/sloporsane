"""Loopback-only Streamable HTTP fixture, with a stdio control channel."""
import json
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

requests = []
lock = threading.Lock()
ping_answered = threading.Event()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def respond(self, status, data=b"", content_type="application/json", **headers):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(data)
        self.wfile.flush()

    def do_DELETE(self):
        with lock:
            requests.append({"path": self.path, "message": {"method": "DELETE"}, "headers": dict(self.headers)})
        if self.headers.get("Mcp-Session-Id") != "fixture-session":
            self.respond(400)
        elif self.path == "/delete-unsupported":
            self.respond(405)
        elif self.path == "/delete-failed":
            self.respond(500)
        else:
            self.respond(204)

    def do_POST(self):
        try:
            self.handle_post()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def handle_post(self):
        message = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with lock:
            requests.append({"path": self.path, "message": message, "headers": dict(self.headers)})
        if self.path == "/redirect":
            self.respond(307, Location="/json")
            return
        if self.path == "/unavailable":
            self.respond(503, b"secret diagnostic must not escape")
            return
        if self.path == "/notion-challenge":
            self.respond(401, b"secret diagnostic must not escape", **{"WWW-Authenticate":
                'Bearer realm="OAuth", resource_metadata="https://mcp.notion.com/.well-known/oauth-protected-resource/mcp", '
                'error="invalid_token", error_description="Missing or invalid access token"'})
            return
        if self.path == "/auth" or (self.path == "/token" and self.headers.get("Authorization") != "Bearer fixture-token"):
            self.respond(401, b"secret diagnostic must not escape")
            return
        method = message.get("method")
        version = "2025-11-25"
        if method != "initialize" and (self.headers.get("Mcp-Session-Id") != "fixture-session" or
                                       self.headers.get("MCP-Protocol-Version") != version):
            self.respond(400)
            return
        if method is None:
            if message.get("id") == "server-ping":
                ping_answered.set()
            self.respond(202)
            return
        if "id" not in message:
            self.respond(202)
            return
        if method == "initialize":
            result = {"protocolVersion": version, "capabilities": {"tools": {}},
                      "serverInfo": {"name": "http-fixture", "version": "1"}}
        elif method == "tools/list":
            result = {"tools": [{"name": "read", "inputSchema": {"type": "object"},
                                 "annotations": {"readOnlyHint": True}}]}
        else:
            name = message["params"]["name"]
            if name == "hang":
                time.sleep(10)
            elif name == "disconnect":
                self.close_connection = True
                return
            elif name == "missing":
                self.respond(404)
                return
            elif name == "malformed":
                self.respond(200, b"not json")
                return
            elif name == "oversized":
                self.respond(200, b"x" * (4 * 1024 * 1024 + 1))
                return
            elif name == "wrong-type":
                self.respond(200, b"{}", content_type="application/json-incorrect")
                return
            result = {"content": [{"type": "text", "text": "fixture result"}], "isError": name == "error"}
        response = {"jsonrpc": "2.0", "id": message["id"], "result": result}
        session = "changed-session" if method == "tools/call" and message["params"]["name"] == "wrong-session" else "fixture-session"
        if not self.path.startswith("/sse"):
            self.respond(200, json.dumps(response).encode(), **{"Mcp-Session-Id": session})
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Mcp-Session-Id", session)
        self.send_header("Connection", "close")
        self.end_headers()
        if method == "tools/call" and message["params"]["name"] == "reset":
            self.wfile.write(b'data: {"jsonrpc":')
            self.wfile.flush()
            self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            self.close_connection = True
            return
        if method == "tools/call" and message["params"]["name"] == "truncated":
            self.wfile.write(b'data: {"jsonrpc":')
            self.wfile.flush()
            self.close_connection = True
            return
        if method == "tools/call" and message["params"]["name"] == "ping":
            ping_answered.clear()
            self.wfile.write(b'data: {"jsonrpc":"2.0","id":"server-ping","method":"ping"}\n\n')
            self.wfile.flush()
            if not ping_answered.wait(2):
                return
        # Exercise comments, CRLF, multiline data and arbitrary chunk boundaries.
        payload = ": keepalive\r\nevent: message\r\n"
        payload += "".join("data: " + line + "\r\n" for line in json.dumps(response, indent=2).splitlines()) + "\r\n"
        if self.path == "/sse-cr":
            payload = "\ufeff" + payload.replace("\r\n", "\r")
        encoded = payload.encode()
        if self.path == "/sse-tail":
            # A completed response ends this exchange even if more data follows
            # in the same curl write callback. Never resume/reparse that buffer.
            self.wfile.write(encoded + b'data: not-json\n\n')
            self.wfile.flush()
            time.sleep(5)
            self.close_connection = True
            return
        for offset in range(0, len(encoded), 7):
            self.wfile.write(encoded[offset:offset + 7])
            self.wfile.flush()
        time.sleep(5)  # Client must return without waiting for stream closure.
        self.close_connection = True


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
threading.Thread(target=server.serve_forever, daemon=True).start()
print(json.dumps({"port": server.server_address[1]}), flush=True)
for line in sys.stdin:
    json.loads(line)
    with lock:
        print(json.dumps({"requests": requests}), flush=True)
server.shutdown()
