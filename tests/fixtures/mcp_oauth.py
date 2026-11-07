"""Loopback OAuth + MCP fixture. No real account, browser or external network."""
import base64
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import urlopen
from urllib.error import HTTPError

state = {"refreshes": 0, "calls": 0, "registrations": 0, "mode": "ok", "challenge": "", "redirect": "", "revocations": 0, "deleted": 0, "denied": 0, "granted_scope": ""}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, status, data):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlsplit(self.path).path
        params = parse_qs(urlsplit(self.path).query)
        if path == "/control":
            state["mode"] = params.get("mode", ["ok"])[0]
            return self.send(200, {"ok": True})
        if path == "/stats":
            return self.send(200, {k: state[k] for k in ("refreshes", "calls", "registrations", "revocations", "deleted", "denied")})
        if path == "/mcp":
            assert "text/event-stream" in self.headers.get("Accept", "")
            if state["mode"] == "well-known":
                return self.send(401, {})
            self.send_response(401)
            target = "/custom-metadata" if state["mode"] in ("challenge", "oidc", "oidc-append", "basic", "post") else "/.well-known/oauth-protected-resource"
            self.send_header("WWW-Authenticate", f'Basic realm="ignored", Bearer resource_metadata="{base}{target}", scope="read"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path in ("/.well-known/oauth-protected-resource", "/custom-metadata"):
            if state["mode"] == "challenge" and path != "/custom-metadata":
                return self.send(500, {})
            issuer = base + "/tenant" if state["mode"].startswith("oidc") else base
            return self.send(200, {"resource": base, "authorization_servers": [issuer], "scopes_supported": ["read" if state["mode"] == "well-known" else "excessive-scope"]})
        if path == "/.well-known/openid-configuration/tenant" and state["mode"] == "oidc-append":
            return self.send(404, {})
        if path in ("/.well-known/oauth-authorization-server", "/.well-known/openid-configuration/tenant", "/tenant/.well-known/openid-configuration"):
            return self.send(200, {"issuer": base + "/tenant" if state["mode"].startswith("oidc") else base if state["mode"] != "bad-issuer" else base + "/other",
                                  "authorization_endpoint": base + "/authorize", "token_endpoint": base + "/token",
                                  "registration_endpoint": base + "/register", "revocation_endpoint": base + "/revoke", "response_types_supported": ["code"],
                                  "code_challenge_methods_supported": [] if state["mode"] == "no-pkce" else ["S256"]})
        if path == "/authorize":
            assert params["code_challenge_method"] == ["S256"]
            assert params["resource"] == [base]
            if state["mode"] in ("basic", "post"):
                state["redirect"] = params["redirect_uri"][0]
            assert params["redirect_uri"] == [state["redirect"]]
            assert "read" in params["scope"][0].split()
            state["granted_scope"] = params["scope"][0]
            state["challenge"] = params["code_challenge"][0]
            query = {"state": params["state"][0], "code": "test-code"}
            if state["mode"] == "deny":
                query = {"state": params["state"][0], "error": "access_denied"}
            if state["mode"] == "wrong-state":
                query["state"] = "incorrect"
            if state["mode"] == "wrong-pkce":
                state["challenge"] = "wrong"
            try:
                with urlopen(state["redirect"] + "?" + urlencode(query), timeout=3) as response:
                    status = response.status
            except HTTPError as error:
                status = error.code
            return self.send(200, {"callback_status": status})
        return self.send(404, {})

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        path = urlsplit(self.path).path
        if path == "/register":
            data = json.loads(raw)
            assert data["token_endpoint_auth_method"] == "none"
            state["redirect"] = data["redirect_uris"][0]
            state["registrations"] += 1
            return self.send(201, {"client_id": "fixture-client", "token_endpoint_auth_method": "none"})
        if path == "/revoke":
            state["revocations"] += 1
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/token":
            data = parse_qs(raw.decode())
            if state["mode"] == "basic":
                expected = "Basic " + base64.b64encode(b"fixture-client:fixture-secret").decode()
                assert self.headers.get("Authorization") == expected
                assert "client_secret" not in data and "client_id" not in data
            else:
                assert data["client_id"] == ["fixture-client"]
                if state["mode"] == "post":
                    assert data["client_secret"] == ["fixture-secret"]
            assert data["resource"] == [base]
            if data["grant_type"] == ["authorization_code"]:
                verifier = data["code_verifier"][0]
                challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
                if challenge != state["challenge"]:
                    return self.send(400, {"error": "invalid_grant"})
                assert data["redirect_uri"] == [state["redirect"]]
                return self.send(200, {"access_token": "initial-access", "token_type": "Bearer", "refresh_token": "initial-refresh", "expires_in": 1})
            if data.get("refresh_token") != ["initial-refresh"] or state["mode"] == "refresh-denied":
                return self.send(400, {"error": "invalid_grant"})
            state["refreshes"] += 1
            return self.send(200, {"access_token": "fresh-access", "token_type": "Bearer", "refresh_token": "rotated-refresh", "expires_in": 3600})
        if path == "/mcp":
            if self.headers.get("Authorization") != "Bearer fresh-access":
                return self.send(401, {})
            data = json.loads(raw)
            if data.get("method") == "tools/call" and state["mode"] == "stepup" and "write" not in state["granted_scope"].split():
                state["denied"] += 1
                self.send_response(403)
                self.send_header("WWW-Authenticate", f'Bearer error="insufficient_scope", resource_metadata="{base}/custom-metadata", scope="write"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if "id" not in data:
                self.send_response(202)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if data["method"] == "initialize":
                result = {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}}, "serverInfo": {"name": "oauth-fixture", "version": "1"}}
            elif data["method"] == "tools/list":
                result = {"tools": [{"name": "read", "inputSchema": {"type": "object"}, "annotations": {"readOnlyHint": True}}]}
            else:
                state["calls"] += 1
                result = {"content": [{"type": "text", "text": "OAuth MCP result"}]}
            return self.send(200, {"jsonrpc": "2.0", "id": data["id"], "result": result})
        self.send(404, {})


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
base = f"http://127.0.0.1:{server.server_port}"
print(json.dumps({"url": base}), flush=True)
server.serve_forever()
