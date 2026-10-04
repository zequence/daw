# MILESTONES

Things we want to implement but aren't about to do yet. When one starts, it moves
out of here (into work, with any open questions going through ISSUES.md).

## Vienna Ensemble Pro integration - remaining parts

Delivered 2026-10-03: "Sync to VE Pro Server" (instruments view + vepro.sync)
creates one connected VE Pro instrument per server instance, synced immutable
per-player MIDI channels and one track per player; idempotent, never deletes
tracks. Track outputs carry (instrument, port, channel); server queries go
through VSL's own CLI (address in Settings > Integrations); connection states
are versioned per Pro Server release (src/integrations/VeproState.h).

Ports >= 2 delivered 2026-10-04: the plugin's own VST3 MIDI event buses are
addressed directly (like Cubase; count read from the plugin, e.g. 16) via a
small JUCE patch (patches/juce-vst3-event-bus.patch) - no Event Input
plugins needed.

Remaining:
- **Re-sync stale dialog**: today tracks whose player vanished from the
  server are kept and reported in the sync notes; the decided UX is to ASK
  per case (remove the channel vs keep it marked) from the UI.
- Reconnect health checks (periodic latencySamples fingerprint, re-apply
  connection state when a server came back).

Interop by observed format only - no VSL code (licensing note applies).

## Multi-channel editing (scoped 2026-10-03)

Double-clicking a folder opens the editor with the COMBINED midi of the
tracks inside (and selects those channels); a dropdown in the editor's top
bar picks which of the selected channels is being edited (ISSUES.md "Arrange
view" + "Midi editor"). Changes the editor's data model from one clip to a
set of (track, clip) pairs with one active edit target.

(The earlier "Unified track area" merge was retracted 2026-10-04; delivered
instead: the track list and arrangement share one Y axis and one vertical
scroll, which solved region alignment, scroll sync and folder lanes.)

## Tempo / meter editing in the timeline bar

The timeline bar (done 2026-10-03) displays the tempo and time-signature
tracks; editing changes there (insert/drag/remove tempo and meter changes,
ramps) lands in the bar later.

## Articulation / expression maps

Keyswitches, Spitfire UACC (CC32), VSL Synchron slot mapping; articulations
attached per note in the editor, named per instrument channel. The deep
orchestral feature.

Key ranges must follow articulation changes. Each Synchron sound slot and
articulation node has its own rangeFrom/rangeTo (vepro.keyRange reads them
from the player's state on the server; today it caches only the union over
all of them, fetched once). When our keyswitches or program changes switch
the player's slot, the editor's greyed-out keys should match the active
slot. The likely approach: fetch the whole tree once and cache a range per
slot and articulation path, mapped to the expression map's entries. Then
look up the range at the playhead or edit position instead of re-querying
the server on each switch, and refresh the cache when the player's setup
changes (e.g. on sync, or with `refresh`).

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
