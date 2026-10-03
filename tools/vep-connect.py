"""Synthesize a Vienna Ensemble Pro VST3 connection state and apply it via the DAW API.

Usage: vep_connect.py <instrumentId> <instanceName> [hostAddress] [hostName]

Builds VE Pro's state JSON with the target instance, wraps it the way JUCE's
VST3 host expects (Size+JSON -> juce-b64 -> <VST3PluginState> -> VC2! container),
and sends it with instrument.setState. Interop format knowledge only - no VSL code.
"""
import base64
import json
import socket
import struct
import sys
import uuid

TABLE = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def juce_b64_encode(data: bytes) -> str:
    bits = len(data) * 8
    out = []
    for k in range((bits + 5) // 6):
        val = 0
        for b in range(6):
            pos = k * 6 + b
            if pos < bits and data[pos >> 3] & (1 << (pos & 7)):
                val |= 1 << b
        out.append(TABLE[val])
    return f"{len(data)}." + "".join(out)


def wrap_state(inner_json: dict) -> bytes:
    payload = json.dumps(inner_json, separators=(",", ":")).encode()
    component = b"Size" + struct.pack("<I", len(payload)) + payload
    xml = ('<?xml version="1.0" encoding="UTF-8"?> <VST3PluginState><IComponent>'
           + juce_b64_encode(component)
           + "</IComponent></VST3PluginState>").encode()
    # juce::AudioProcessor::copyXmlToBinary: magic 'VC2!' + uint32 size + text + NUL
    return b"VC2!" + struct.pack("<I", len(xml) + 1) + xml + b"\x00"


def send(cmd: str, params: dict) -> dict:
    with socket.create_connection(("127.0.0.1", 53217), timeout=30) as s:
        s.sendall((json.dumps({"id": 1, "cmd": cmd, "params": params}) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.decode())


instrument_id = int(sys.argv[1])
instance = sys.argv[2]
host_addr = sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1"
host_name = sys.argv[4] if len(sys.argv) > 4 else "localhost"

state = {
    "data": {
        "currentPreset": "",
        "currentPresetSystem": False,
        "custom": {
            "arch64": True,
            "audioInputLag": 0,
            "decoupled": True,          # connection only - no embedded instance content
            "hostAddress": host_addr,
            "hostName": host_name,
            "id": uuid.uuid4().hex,
            "instanceName": instance,
            "latencyBufferCount": 2,
        },
        "meta": {"gui": {"currentView": "main", "flow": None, "height": 280, "scale": 1, "width": 420}},
        "parameters": {},
    },
    "version": 0,
}

blob = wrap_state(state)
reply = send("instrument.setState",
             {"instrumentId": instrument_id,
              "stateBase64": base64.b64encode(blob).decode()})
print(json.dumps(reply, indent=1))
