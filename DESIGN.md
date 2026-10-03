# Design

This application will have world class automation capabilities. Not just automation for
things like midi CC and audio channel parameters but content generation and AI agent
interoperability with all the functions of the application.

That one sentence contains three pillars:

1. **Timeline automation** - CC lanes, plugin parameters, channel gain: curves over
   musical time, edited like any other content and applied sample-accurately.
2. **Content generation** - transforms and generators built into the app (quantize,
   humanize, arpeggiate, CC-curve shaping, legato overlap...) and external generation
   by scripts or agents writing through the API.
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

## Status

- Control API: live (API.md) - transport, tracks, routing, clips, instruments,
  channels, recording, projects.
- Timeline automation beyond MIDI CC: planned after the MIDI editor; lane editing will
  share the piano roll machinery. Parameter targeting must use stable parameter IDs,
  not indices.
- Content transforms: arrive with the MIDI editor as `clip.*` commands.
- Event subscription and richer `describe` (examples, semantics): planned.
