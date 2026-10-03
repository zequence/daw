#!/usr/bin/env python3
"""MCP adapter for Orchestral DAW.

A thin bridge: it asks the running app for its command surface (`describe`) and
exposes one MCP tool per command, so new commands appear here automatically.
No dependencies - speaks MCP JSON-RPC directly.

Two transports:
    stdio (default)      the MCP client spawns this script:
                             claude mcp add orchestral-daw -- python path/to/orchestral_daw_mcp.py
    --http PORT          long-lived local HTTP server (streamable HTTP transport);
                         the app itself runs this when MCP is enabled in Settings:
                             claude mcp add --transport http orchestral-daw http://127.0.0.1:PORT/mcp

Environment:
    DAW_API_PORT   TCP port of the app's control API (default 53217)
"""

import json
import os
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = int(os.environ.get("DAW_API_PORT", "53217"))

SERVER_INFO = {"name": "orchestral-daw", "version": "0.1.0"}
FALLBACK_PROTOCOL = "2024-11-05"

# Commands that legitimately take a while (sample libraries streaming in).
SLOW_COMMANDS = {"instrument.add": 300.0, "project.load": 300.0, "project.save": 60.0}
DEFAULT_TIMEOUT = 30.0

TICKS_NOTE = "Musical time is in ticks: 960000 per quarter note."


def log(message: str) -> None:
    print(f"[orchestral-daw-mcp] {message}", file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# TCP client for the app's control API (newline-delimited JSON, id-correlated)
class DawClient:
    def __init__(self) -> None:
        self.sock: socket.socket | None = None
        self.buffer = b""
        self.next_id = 1

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None
        self.buffer = b""

    def connect(self) -> None:
        if self.sock is not None:
            return
        self.sock = socket.create_connection((HOST, PORT), timeout=5.0)

    def request(self, cmd: str, params: dict | None, timeout: float) -> dict:
        """One command, one reply. Raises ConnectionError if the app is unreachable."""
        message_id = self.next_id
        self.next_id += 1
        payload = json.dumps(
            {"id": message_id, "cmd": cmd, "params": params or {}}
        ).encode("utf-8") + b"\n"

        try:
            self.connect()
            assert self.sock is not None
            self.sock.sendall(payload)
            deadline = time.monotonic() + timeout

            while True:
                newline = self.buffer.find(b"\n")
                if newline >= 0:
                    line, self.buffer = self.buffer[:newline], self.buffer[newline + 1:]
                    if not line.strip():
                        continue
                    reply = json.loads(line)
                    if reply.get("id") == message_id:
                        return reply
                    continue  # stale reply from a timed-out predecessor

                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"'{cmd}' did not reply within {timeout:.0f}s")
                self.sock.settimeout(remaining)
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise ConnectionError("connection closed by the app")
                self.buffer += chunk
        except (OSError, ConnectionError) as error:
            self.close()
            raise ConnectionError(
                f"Orchestral DAW is not reachable on {HOST}:{PORT} ({error}). "
                "Start the app (its API listens automatically) and try again."
            ) from error


daw = DawClient()
daw_lock = threading.Lock()   # the HTTP transport serves from worker threads

# ---------------------------------------------------------------------------
# Tool catalogue, generated from the app's own `describe`

RAW_TOOL = {
    "name": "daw_command",
    "description": (
        "Send any command to Orchestral DAW's control API. Use when no dedicated "
        "tool exists yet; 'describe' lists every command. " + TICKS_NOTE
    ),
    "inputSchema": {
        "type": "object",
        "properties": {
            "cmd": {"type": "string", "description": "Command name, e.g. transport.status"},
            "params": {"type": "object", "description": "Command parameters"},
        },
        "required": ["cmd"],
    },
}

tool_cache: dict = {"tools": None, "name_to_cmd": {}, "fetched": 0.0}


def build_tools() -> list:
    with daw_lock:
        return build_tools_locked()


def build_tools_locked() -> list:
    now = time.monotonic()
    if tool_cache["tools"] is not None and now - tool_cache["fetched"] < 15.0:
        return tool_cache["tools"]

    tools = [RAW_TOOL]
    name_to_cmd: dict[str, str] = {}

    try:
        reply = daw.request("describe", {}, DEFAULT_TIMEOUT)
        for command in reply.get("result", {}).get("commands", []):
            cmd = command.get("name", "")
            if not cmd:
                continue
            tool_name = "daw_" + cmd.replace(".", "_")
            name_to_cmd[tool_name] = cmd

            params = command.get("params", "") or "no parameters"
            description = f"{command.get('description', cmd)} | params: {params}"
            if "int64" in params or "tick" in params.lower():
                description += " | " + TICKS_NOTE

            tools.append({
                "name": tool_name,
                "description": description,
                "inputSchema": {
                    "type": "object",
                    "additionalProperties": True,
                    "description": params,
                },
            })

        tool_cache.update(tools=tools, name_to_cmd=name_to_cmd, fetched=now)
        log(f"catalogue: {len(tools)} tools from the running app")
    except (ConnectionError, TimeoutError) as error:
        log(f"app unreachable, exposing only daw_command ({error})")
        tool_cache.update(tools=None, name_to_cmd={}, fetched=0.0)

    return tools


def call_tool(name: str, arguments: dict) -> dict:
    with daw_lock:
        return call_tool_locked(name, arguments)


def call_tool_locked(name: str, arguments: dict) -> dict:
    if name == "daw_command":
        cmd = arguments.get("cmd", "")
        params = arguments.get("params", {}) or {}
    else:
        if name not in tool_cache["name_to_cmd"]:
            build_tools_locked()  # refresh; maybe the app just started
        cmd = tool_cache["name_to_cmd"].get(name, "")
        params = dict(arguments)

    if not cmd:
        return error_content(f"unknown tool '{name}'")

    try:
        reply = daw.request(cmd, params, SLOW_COMMANDS.get(cmd, DEFAULT_TIMEOUT))
    except (ConnectionError, TimeoutError) as error:
        return error_content(str(error))

    if not reply.get("ok", False):
        return error_content(reply.get("error", "command failed"))

    result = reply.get("result", {"ok": True})
    return {"content": [{"type": "text", "text": json.dumps(result, indent=1)}]}


def error_content(message: str) -> dict:
    return {"content": [{"type": "text", "text": message}], "isError": True}


# ---------------------------------------------------------------------------
# Transport-independent message handling (JSON-RPC 2.0)

def handle_message(message: dict):
    """Returns a response dict, or None for notifications."""
    method = message.get("method", "")
    message_id = message.get("id")
    params = message.get("params", {}) or {}

    if message_id is None:   # notification
        return None

    if method == "initialize":
        result = {
            "protocolVersion": params.get("protocolVersion", FALLBACK_PROTOCOL),
            "capabilities": {"tools": {}},
            "serverInfo": SERVER_INFO,
        }
    elif method == "tools/list":
        result = {"tools": build_tools()}
    elif method == "tools/call":
        result = call_tool(params.get("name", ""), params.get("arguments", {}) or {})
    elif method == "ping":
        result = {}
    else:
        return {"jsonrpc": "2.0", "id": message_id,
                "error": {"code": -32601, "message": f"method not found: {method}"}}

    return {"jsonrpc": "2.0", "id": message_id, "result": result}


# ---------------------------------------------------------------------------
# stdio transport (one JSON message per line)

def run_stdio() -> None:
    log(f"stdio transport ready; forwarding to {HOST}:{PORT}")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue

        response = handle_message(message)

        if response is not None:
            sys.stdout.write(json.dumps(response) + "\n")
            sys.stdout.flush()

    daw.close()


# ---------------------------------------------------------------------------
# Streamable HTTP transport (stateless: plain JSON responses, no sessions)

class McpHttpHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):   # keep stderr quiet
        pass

    def _reply(self, status: int, body: bytes = b"", content_type: str = "application/json"):
        self.send_response(status)
        if body:
            self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_POST(self):
        if self.path.rstrip("/") != "/mcp":
            return self._reply(404)

        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length) or b"null")
        except (ValueError, json.JSONDecodeError):
            return self._reply(400)

        messages = body if isinstance(body, list) else [body]
        responses = [r for m in messages if isinstance(m, dict) and (r := handle_message(m)) is not None]

        if not responses:
            return self._reply(202)   # notification(s) only

        payload = responses[0] if not isinstance(body, list) else responses
        self._reply(200, json.dumps(payload).encode("utf-8"))

    def do_GET(self):
        # No server-initiated stream; clients fall back to plain request/response.
        self._reply(405)

    def do_DELETE(self):
        self._reply(200)   # session teardown: nothing to tear down, we're stateless


def run_http(port: int) -> None:
    server = ThreadingHTTPServer((HOST, port), McpHttpHandler)
    log(f"http transport on http://{HOST}:{port}/mcp; forwarding to {HOST}:{PORT}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        daw.close()


def main() -> None:
    if "--http" in sys.argv:
        index = sys.argv.index("--http")
        port = int(sys.argv[index + 1]) if index + 1 < len(sys.argv) else 53218
        run_http(port)
    else:
        run_stdio()


if __name__ == "__main__":
    main()
