# MILESTONES

Things we want to implement but aren't about to do yet. When one starts, it moves
out of here (into work, with any open questions going through ISSUES.md).

## Vienna Ensemble Pro integration (groundwork proven 2026-10-03)

Connect VE Pro VSTs to server instances fully programmatically. Proven end to
end: the plugin's VST3 state is JSON (hostAddress/hostName/instanceName/
decoupled + a zstd serverData payload), and a synthesized minimal state set
via instrument.setState makes the plugin connect - tools/vep-connect.py does
it today (server-side instance management via the user's vepro-api MCP).
App-side later: an instance browser (VSL server HTTP API), "connect to
instance..." on the instrument, auto-reconnect health checks via the
latencySamples fingerprint. Interop by observed format only - no VSL code
(licensing note applies).

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
