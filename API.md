# Orchestral DAW control API

The running app listens on **127.0.0.1:53217** (TCP, local connections only).
Settings.xml keys: `apiEnabled` (default 1), `apiPort` (default 53217).

## Protocol

Newline-delimited JSON: send one object per line, receive one reply per request.

```
request:  {"id": 1, "cmd": "track.create", "params": {"name": "Violins 1"}}
reply:    {"id": 1, "ok": true, "result": {"id": 3, "name": "Violins 1"}}
failure:  {"id": 1, "ok": false, "error": "no track with id 99 (existing: 1, 2)"}
```

- `id` is echoed back, so requests can be pipelined over one connection.
- `instrument.add` and `project.load` reply when loading has actually finished
  (sample libraries can take a while).
- Musical time is in **ticks**: 960,000 per quarter note.

Quick test from a terminal while the app is running:

```powershell
.\tools\daw-api.ps1 describe
.\tools\daw-api.ps1 track.create '{"name":"Violins 1"}'
.\tools\daw-api.ps1 instrument.add '{"name":"TAL-NoiseMaker"}'
.\tools\daw-api.ps1 track.setOutput '{"trackId":1,"instrumentId":1,"channel":1}'
.\tools\daw-api.ps1 clip.addNotes '{"trackId":1,"notes":[{"start":0,"length":960000,"key":60}]}'
.\tools\daw-api.ps1 transport.play
```

## Commands

`describe` returns this list with parameter signatures, live from the app.

| Group | Commands |
|---|---|
| meta | `describe`, `app.status` |
| transport | `transport.status`, `transport.play`, `transport.stop`, `transport.locate`, `transport.setLoop`, `tempo.set` |
| tracks | `track.list`, `track.create`, `track.remove`, `track.rename`, `track.setMuted`, `track.setSoloed`, `track.arm`, `track.setOutput`, `track.addOutput`, `track.clearOutputs` |
| clips | `clip.get`, `clip.addNotes`, `clip.set`, `clip.clear` |
| instruments | `plugins.list`, `instrument.list`, `instrument.add`, `instrument.setChannelName` |
| audio | `channel.list`, `channel.setGain`, `channel.setMuted` |
| recording | `record.start`, `record.stop` |
| projects | `project.save`, `project.load`, `project.new` |

## Agents

There is no separate "agent API": agents use this same surface. The MCP adapter in
`tools/mcp/` does exactly that - it discovers the surface via `describe` and exposes
one MCP tool per command, plus a raw `daw_command` passthrough. See `tools/mcp/README.md`
for registering it with Claude Code or Claude Desktop. The design keeps replies compact
and errors self-describing for exactly that use.
