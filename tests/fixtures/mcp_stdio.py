"""Deterministic stdio MCP peer for daemon tests; never uses external services."""
import json
import os
import sys
import time
import signal

for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        continue
    method = request["method"]
    if method == "initialize":
        result = {"protocolVersion": "2025-11-25", "serverInfo": {"name": "fixture", "version": "1"},
                  "capabilities": {"tools": {"listChanged": True}, "resources": {}, "prompts": {}}}
    elif method == "resources/list":
        result = {"resources": [{"name": "Guide", "uri": "fixture://guide", "description": "Fixture resource"}]}
    elif method == "resources/templates/list":
        result = {"resourceTemplates": [{"name": "Named guide", "uriTemplate": "fixture://guide/{name}"}]}
    elif method == "prompts/list":
        result = {"prompts": [{"name": "review", "description": "Review a subject", "arguments": [{"name": "subject", "required": True}]}]}
    elif method == "resources/read":
        result = {"contents": [{"uri": request["params"]["uri"], "text": "Fixture resource content", "mimeType": "text/plain"}]}
    elif method == "prompts/get":
        result = {"messages": [{"role": "user", "content": {"type": "text", "text": "Review " + request["params"]["arguments"]["subject"]}}]}
    elif method == "tools/list":
        result = {"tools": [
            {"name": "read", "description": "Read test data", "inputSchema": {"type": "object"},
             "annotations": {"readOnlyHint": True}},
            {"name": "write", "inputSchema": {"type": "object"}},
            {"name": "false", "inputSchema": {"type": "object"}, "annotations": {"readOnlyHint": False}},
            {"name": "string", "inputSchema": {"type": "object"}, "annotations": {"readOnlyHint": "true"}},
        ]}
    elif method == "tools/call":
        name = request["params"]["name"]
        if name == "hang":
            time.sleep(10)
        if name == "exit":
            sys.exit(0)
        if name == "malformed":
            print("not json", flush=True)
            continue
        if name == "spawn_child":
            reader, writer = os.pipe()
            if os.fork() == 0:
                os.close(reader)
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
                os.write(writer, b"ready")
                os.close(writer)
                time.sleep(0.3)
                with open(os.environ["SOS_MCP_TEST_MARKER"], "w") as marker:
                    marker.write("child survived")
                os._exit(0)
            os.close(writer)
            os.read(reader, 5)
            os.close(reader)
        result = {"content": [{"type": "text", "text": json.dumps({"cwd": os.getcwd(),
                  "secret": os.getenv("SOS_MCP_TEST_SECRET"), "inherited": os.getenv("SOS_MCP_TEST_UNRELATED")})}],
                  "isError": name == "error"}
    else:
        result = {}
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
