# Design

This application will have world class automation capabilities. Not just automation for
things like midi CC and audio channel parameters but content generation and AI agent
interoperability with all the functions of the application.

That one sentence contains three pillars:

1. **Timeline automation** - CC lanes, plugin parameters, channel gain: curves over
   musical time, edited like any other content and applied sample-accurately.
2. **Content generation** - transforms and generators built into the app (quantize,
   humanize, arpeggiate, CC-curve shaping, legato overlap...) and external generation
   by scripts or agents writing through the API. Eventually this goes non-linear:
   using the application like something similar to Pure Data - generator and logic
   nodes patched together, running live - not only typical DAW functions. That will
   need its own UI, which comes later; until then the engine and command layer must
   simply not assume the linear timeline is the only driver.
3. **Agent interoperability** - everything the application can do is reachable,
   discoverable and observable through its control API (see API.md). An AI agent is a
   first-class operator of the application, not an add-on.

## Engineering principles

These keep the pillars true as the app grows:

- **Command-first development.** Every feature lands as an API command; the UI is a
  client of the same commands. If something can only be done with the mouse, that's a
  bug in the feature's design.
- **Discoverable surface.** `describe` always reflects the full command set, with
  parameters. Clients - scripts, tests, agents - learn the app at runtime, not from
  stale docs.
- **Helpful failure.** Errors name what exists ("no track with id 99, existing: 1, 2")
  and disambiguate ("'Kontakt' matches: Kontakt 7, Kontakt 8"). An error message is
  part of the API.
- **Observable state.** Clients should be able to subscribe to changes (transport,
  tracks, recording...) rather than poll. (Planned: a `subscribe` command over the
  existing socket.)
- **Transforms are commands.** Generators and edit operations are clip/parameter
  transforms in the command layer, so the piano roll menu, scripts and agents all use
  the identical operation - and undo/redo wraps commands in one place.
- **Immutable musical data.** TempoMap, MidiSequence and future automation lanes are
  immutable snapshots shared with the audio thread; edits swap pointers. No locks on
  the audio path.

## MIDI data model: one stream per track, meta-regions on top

Each MIDI track holds a single continuous stream of notes and controller data, with
bounds derived from the content (first event to last). There are no stored MIDI clip
objects, and that is a deliberate choice:

- one coordinate system (absolute ticks), one editing surface per track, no
  clip-boundary bugs, trivial recording merge and serialization;
- CC curves (dynamics, expression) stay continuous across musical sections - no
  part boundaries chopping automation, no chase-across-boundary edge cases;
- agents and scripts address music as "bars 17-24 of the horns", which maps onto
  range commands, not region handles.

The region *experience* is provided by **meta-regions**: computed time ranges, not
containers. Two sources:

1. **Markers.** Named positions dividing the project into parts (theme-1, verse,
   chorus...). A part is the span from one marker to the next; selecting a part
   selects that range on whichever tracks you choose.
2. **Content gaps.** Enough silence inside a track's stream (threshold adjustable,
   about a bar by default) visually divides it into phrase blocks, so the
   arrangement view shows structure and pauses. Notes define phrases; CC data rides
   along when a range is selected or copied.

Selection by part or by phrase block feeds the same range operations: loop this,
copy/move/repeat this, erase this, (later) mute or scale this. Because meta-regions
are queries over the stream, they can never desynchronize from the content.

What this gives up: clip aliasing (edit one looped part, all instances follow).
Repetition is explicit duplication via repeat-range. If aliasing ever becomes a felt
need, clips-as-windowed-references can be layered on top of streams without
replacing the model. The reverse migration would be a rewrite, which is why streams
win as the foundation.

**MIDI recording** gets two modes (final wording open): *add to existing* (merge,
today's behavior) and *replace on first input* - playback of existing material is
untouched until the first played event; from that moment sounding notes are
truncated and existing events are erased until recording stops.

**Audio is different**: audio takes cannot merge into a stream, so the Audio domain
will have real regions, with the mute/volume/fade handling that implies. Audio
channels get their default name from their input - but only when that input is an
instrument; device inputs and unrouted channels are named by the user. As with MIDI
tracks, a manual rename is never overwritten. The
Midi/Audio domain split in GUI_DESIGN.md keeps the two paradigms from leaking into
each other.

## Status

- Control API: live (API.md) - transport, tracks, routing, clips, instruments,
  channels, recording, projects.
- Timeline automation beyond MIDI CC: planned after the MIDI editor; lane editing will
  share the piano roll machinery. Parameter targeting must use stable parameter IDs,
  not indices.
- Content transforms: arrive with the MIDI editor as `clip.*` commands.
- Non-linear patching: far field, but the ground is prepared - the engine is already a
  processor graph (sources, routes, instruments are nodes), nodes pull time from the
  transport per block rather than being driven by it, and patching maps onto future
  `node.*` commands.
- Event subscription and richer `describe` (examples, semantics): planned.
