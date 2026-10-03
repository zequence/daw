"""Connect a loaded Vienna Ensemble Pro VST to a server instance, headlessly.

Usage: vep-connect.py <instrumentId> <instanceName> [hostAddress] [hostName] [version]

Thin wrapper over the app's instrument.connectVepro command - the state-format
knowledge lives (versioned per Pro Server release) in src/integrations/VeproState.h,
and the default version comes from Settings > Integrations.
"""
import json
import socket
import sys


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


params = {
    "instrumentId": int(sys.argv[1]),
    "instance": sys.argv[2],
}
if len(sys.argv) > 3:
    params["host"] = sys.argv[3]
if len(sys.argv) > 4:
    params["hostName"] = sys.argv[4]
if len(sys.argv) > 5:
    params["version"] = sys.argv[5]

print(json.dumps(send("instrument.connectVepro", params), indent=1))
