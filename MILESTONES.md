# MILESTONES

Things we want to implement but aren't about to do yet. When one starts, it moves
out of here (into work, with any open questions going through ISSUES.md).

## Vienna Ensemble Pro integration (reworked 2026-10-03)

Goals:
- **"Sync to VE Pro Server"** button in the instruments UI: query the server
  for its instances and create one connected VE Pro VST instrument per
  available instance (named after the instance). Connecting is solved:
  instrument.connectVepro synthesizes the versioned state
  (src/integrations/VeproState.h; version selector in Settings >
  Integrations).
- **Named MIDI channels per player**: for each player/instrument-plugin
  inside an instance, create a MIDI channel on our instrument wired to the
  correct (port, channel) of that player, with the name inherited from the
  instance - like a regular MIDI channel, but the inherited parts
  (name/port/channel binding) are immutable; re-sync refreshes them.

What it requires:
- The app talks to the VE Pro server HTTP API itself (instance list +
  channel summaries with title/midiPort/midiChannel); server address joins
  Settings > Integrations next to the version selector.
- **MIDI channels grow a port dimension**: track outputs become (instrument,
  port, channel) instead of (instrument, channel). Port 1 is the main VE Pro
  plugin; further ports mean managing "Vienna Ensemble Pro Event Input"
  plugin instances bound to the same server instance, routed as part of the
  same rack instrument.
- Instrument channel metadata becomes structured: {port, channel, name,
  synced/immutable flag} instead of today's name-per-channel map; synced
  entries refresh from the server, manual ones stay editable.
- Sync is idempotent: re-running updates names and adds new players; health
  check via the latencySamples fingerprint.

Decisions (2026-10-03):
- Sync also creates one MIDI track per player channel (Cubase-template
  style), named after the player, so everything is playable right away.
- When a synced channel's player was deleted on the server, re-sync ASKS
  per case (there may be MIDI on tracks the user wants to keep): remove the
  channel, or keep it orphaned/marked.

Interop by observed format only - no VSL code (licensing note applies).

## Tempo / meter editing in the timeline bar

The timeline bar (done 2026-10-03) displays the tempo and time-signature
tracks; editing changes there (insert/drag/remove tempo and meter changes,
ramps) lands in the bar later.

## Articulation / expression maps

Keyswitches, Spitfire UACC (CC32), VSL Synchron slot mapping; articulations
attached per note in the editor, named per instrument channel. The deep
orchestral feature.

## Parameter automation lanes

Curves over musical time targeting (instrument, parameter), (channel, gain)...
Same immutable-snapshot pattern as TempoMap/MidiSequence; must use stable
parameter IDs, not indices. Lane editing shares the piano roll machinery.

## Stacked editor lanes

CC, note velocity, aftertouch at the bottom of the editor; velocity default;
more lanes stackable on top of each other, with controls below the piano keys.
(From ISSUES.md Midi editing - larger than a quick fix because it reworks the
lane area into a list of lanes.)

## Parallel processing graph

Render independent tracks/instruments on worker threads in dependency order;
needed once big templates make one core the bottleneck (watch the perf monitor).

## Audio domain

Real audio regions (recording, takes, fades, clip gain), device inputs on audio
channels, summing buses. Audio channels default-name from an instrument input
only; manual renames always win.

## Non-linear patching (far field)

Generator/logic nodes patched together, running live (DESIGN.md pillar 2).
Groundwork exists: the engine is a node graph and time is pulled per block.
