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
| plugin introspection | `instrument.describe` (version, buses, latency, programs), `instrument.listParameters` (paged live values), `instrument.getStateStrings` (readable strings from the state blob), `instrument.getState`/`instrument.setState` (base64 snapshots - a stored state reconnects network plugins like VE Pro, so it doubles as a connection template) |
| VE Pro | `instrument.connectVepro` (connect a loaded VE Pro plugin to a named server instance by synthesized state; the state format is versioned per Pro Server release - select in Settings > Integrations, override with `version`), `vepro.sync` (one connected instrument per server instance, synced immutable per-player MIDI channels, one track per player; idempotent, never deletes; the "Sync to VE Pro Server" button), `vepro.keyRange` (a synced Synchron track's playable key range `{available, low, high}`, read from the player's state on the server once and cached in the project; `refresh` re-reads it; the MIDI editor greys out keys outside it) |
| audio | `channel.list`, `channel.setGain`, `channel.setMuted` |
| folders | `folder.list`, `folder.create`, `folder.rename`, `folder.remove`, `folder.setParent`, `folder.setCollapsed`, `folder.setColor`, `track.setFolder`, `channel.setFolder`, `sidebar.move` (sidebar grouping and ordering; `domain` is `midi` or `audio`, folders nest; `sidebar.move` re-orders folders/members as one group - the drag operation) |
| ui | `ui.selectTrack` (select a track exactly like a sidebar click: arms it with auto-arm, the open editor follows; replies with the time it took) |
| colors | `track.setColor` / `folder.setColor` ('#rrggbb', empty = none); shown as sidebar left borders and region borders, opacity in Settings > Theming; vepro.sync seeds them from the server's instance/player colors on creation |
| recording | `record.start`, `record.stop` |
| projects | `project.save`, `project.load`, `project.new` |
| history | `history.list` (filter by category/trackId), `history.travel` (time-travel to an entry) |

## Events (subscribe)

Send `{"cmd": "subscribe"}` on a connection and the app pushes events as JSON lines on
that same connection, interleaved with your replies (replies carry `ok`, events carry
`event`). `{"cmd": "unsubscribe"}` stops them.

```
{"event":"transport","playing":true,"recording":false,"looping":false,"positionTicks":0,"bar":1,"beat":1,"bpm":120}
{"event":"position", ...}                      every 0.5 s while playing
{"event":"trackAdded","id":3,"name":"Twin 3"}  also trackRemoved, trackChanged {id, change}
{"event":"clipChanged","trackId":3,"notes":42,"controls":7}
{"event":"instrumentAdded","id":1,"name":"Twin 3","audioChannelId":1}   also instrumentRemoved
{"event":"markerAdded","tick":0,"name":"intro"}                         also markerRemoved
{"event":"tempoChanged","bpm":101}
{"event":"recordingStarted","trackId":3}  /  {"event":"recordingFinished","trackId":3,"notes":12,"controls":80}
{"event":"projectCleared"} / {"event":"projectLoaded","path":"..."} / {"event":"projectSaved","path":"..."}
```

trackChanged's `change` is one of: name, muted, soloed, armed, outputs.

## Agents

There is no separate "agent API": agents use this same surface. The MCP adapter in
`tools/mcp/` does exactly that - it discovers the surface via `describe` and exposes
one MCP tool per command, plus a raw `daw_command` passthrough. See `tools/mcp/README.md`
for registering it with Claude Code or Claude Desktop. The design keeps replies compact
and errors self-describing for exactly that use.
