#!/usr/bin/env python3
"""MCP adapter for Orchestral DAW.

A thin bridge: it asks the running app for its command surface (`describe`) and
exposes one MCP tool per command, so new commands appear here automatically.
No dependencies - speaks MCP's stdio JSON-RPC directly.

Usage (Claude Code):
    claude mcp add orchestral-daw -- python path/to/orchestral_daw_mcp.py

Environment:
    DAW_API_PORT   TCP port of the app's control API (default 53217)
"""

import json
import os
import socket
import sys
import time

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
    if name == "daw_command":
        cmd = arguments.get("cmd", "")
        params = arguments.get("params", {}) or {}
    else:
        if name not in tool_cache["name_to_cmd"]:
            build_tools()  # refresh; maybe the app just started
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
# MCP stdio loop (JSON-RPC 2.0, one message per line)

def respond(message_id, result=None, error=None) -> None:
    reply: dict = {"jsonrpc": "2.0", "id": message_id}
    if error is not None:
        reply["error"] = error
    else:
        reply["result"] = result
    sys.stdout.write(json.dumps(reply) + "\n")
    sys.stdout.flush()


def main() -> None:
    log(f"ready; forwarding to {HOST}:{PORT}")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue

        method = message.get("method", "")
        message_id = message.get("id")
        params = message.get("params", {}) or {}

        if method == "initialize":
            respond(message_id, {
                "protocolVersion": params.get("protocolVersion", FALLBACK_PROTOCOL),
                "capabilities": {"tools": {}},
                "serverInfo": SERVER_INFO,
            })
        elif method == "tools/list":
            respond(message_id, {"tools": build_tools()})
        elif method == "tools/call":
            respond(message_id, call_tool(params.get("name", ""), params.get("arguments", {}) or {}))
        elif method == "ping":
            respond(message_id, {})
        elif message_id is not None:  # unknown request (notifications are ignored)
            respond(message_id, error={"code": -32601, "message": f"method not found: {method}"})

    daw.close()


if __name__ == "__main__":
    main()
