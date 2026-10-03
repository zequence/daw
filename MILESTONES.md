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

Remaining:
- **Ports >= 2 routing**: players beyond MIDI port 1 need "Vienna Ensemble
  Pro Event Input" plugin instances managed as part of the rack instrument
  (the model and sync already record the port; such outputs are silent and
  reported in the sync notes until then).
- **Re-sync stale dialog**: today tracks whose player vanished from the
  server are kept and reported in the sync notes; the decided UX is to ASK
  per case (remove the channel vs keep it marked) from the UI.
- Reconnect health checks (periodic latencySamples fingerprint, re-apply
  connection state when a server came back).

Interop by observed format only - no VSL code (licensing note applies).

## Unified track area (scoped 2026-10-03)

The track sidebar and the arrange view become ONE unit (user decision; from
ISSUES.md "Arrange view"): header rows on the left (today's track list -
name, R/E/S/M/I, record mode), each row's lane continuing directly to the
right at the same height and y; one shared vertical scroll; folder rows draw
a spanning region across their content's lanes. The timeline bar stays above
the whole area. Region height and position therefore follow the channels by
construction.

Implications:
- The shared TimeAxis gutter becomes dynamic: the header column's width
  (resizable) instead of the fixed 56 px. The piano roll keeps alignment by
  drawing its keys in that same column (keys at the column's right edge).
- TrackList rows and ArrangementView lanes merge into one component (or two
  children of one scroll container); drag/multi-select/folders carry over.
- The audio domain gets the same treatment later (channel list + audio
  regions).

Order: colors land first (ISSUES.md), so folder spans and region borders
draw colored from day one.

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
