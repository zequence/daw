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

## Articulation / expression maps (drafted 2026-10-05)

A custom system inspired by standard expression maps. Standard maps have four
fixed articulation groups; ours has any number. The point: pick how a note
should be played once, in the editor, and have the right keyswitch / CC /
program change reach the instrument at the right time.

### Concepts

- **Expression map**: a named set of groups. Its name is its identifier.
- **Group**: has a **name** (identifier, unique within the map) and a
  **description**, and holds articulations. Groups are ordered.
  - **Group 0 is the root group.** Every map must have it. Its articulations are
    the "main" ones (Staccato, Legato, Tremolo, Pizzicato...).
  - Every other group is a **modifier group** (Release, Attack, Mute...).
- **Articulation**: has a **name** (identifier, unique within its group), a
  **symbol** (a short glyph or text shown on notes and in menus), a
  **description**, and its **output** (below).
- **Applicability lives on the modifier articulations, not on the groups.** Each
  modifier articulation declares which root articulations it works with
  (default: all). A whole group can therefore apply to a root articulation, or
  only some of its articulations can, or none.
- **Names are case-insensitive**: "Staccato" and "staccato" can't coexist in one
  scope, lookups (API, files, agents) ignore case, and the typed case is kept
  for display.
- **A note's articulation** = one root articulation, plus **at most one**
  articulation from each modifier group (or none from that group).
- **Choices follow the root.** When a root articulation is selected, the
  modifiers that don't apply to it are removed from the choices (a group with
  nothing left disappears from the menu), and a modifier already chosen that no
  longer applies is dropped; the others stay.

### Output (how an articulation reaches the instrument)

An articulation's output is one of:
- a **keyswitch** note (key, velocity, held or tapped),
- a **CC** (number, value) - e.g. Spitfire UACC on CC32,
- a **program change** (with optional bank).

The active combination's output is the root's, then its modifiers' in group
order; if two target the same keyswitch/CC, the later group wins. An articulation
may also have a **lead time** (the keyswitch must arrive slightly before the note).

### Key ranges and named keys

- An articulation may carry its **playable key range**. The editor greys out the
  keys outside the range of the articulation in effect - this is the home of the
  "key ranges must follow articulation changes" item below, and replaces the
  "first loaded slot" range once maps exist.
- A map may also give **keys names (instructions)** - "C0: Legato", "C#0: Repeat
  once", the keyswitch keys that libraries label on their keyboards. The piano
  roll shows them on the key column and as tooltips, like a drum map's note
  names. A keyswitch articulation's key is named automatically.

### Creating maps: Synchron detection

Building a big map by hand is the pain point. For VE Pro Synchron players the
state we already read (`vepro.keyRange`: the sampler tree of Attack / Type /
Release groups with sound slots as leaves, each with its patch name and key range)
can **seed a map**: a "detect from player" step proposes the groups,
articulations, their key ranges and the program-change / keyswitch output that
selects each slot, and the user edits the result. It is a creation aid, not a
separate kind of output.

### Where things live

- **Maps belong to MIDI tracks, for now.** The map is saved with the track
  (`<TRACK>`), so a project is self-contained, tracks are already in the history
  snapshot (assignment and edits are undoable), and VE Pro re-syncs keep it
  (sync never deletes tracks). `MidiSourceProcessor` is per track, so playback
  gets the map directly. A **user library** of maps (user data folder, like
  themes) lets maps be copied to tracks, exported and imported. Whether maps
  later move up to the instrument channel is an open question.
- **Notes carry the choice.** `MidiSequence::Note` gets a trailing
  `articulation` member (default none), stored in `<NOTE>` as names, so old
  projects load unchanged. In memory it should be a small interned id into the
  map's combination table, not a vector per note. **Renaming** a group or
  articulation (including a case-only change) is a command that rewrites every
  note using it, in one undo step.

### The editor

- An **articulation dropdown in the MIDI editor's top bar** (before the track
  label, wired like the other boxes; disabled when the track has no map). It
  opens a menu: the root articulations first (symbol + name, description as
  tooltip); once a root is chosen, the modifier groups with applicable
  modifiers appear as further sections. It edits the **selected notes**; with
  nothing selected it sets the articulation new notes are drawn with. A
  selection that mixes articulations shows "Mixed".
- Notes show their **symbol** in the piano roll (colour per articulation later).
- Articulation changes are undoable edits like any other (clip commands).

### Playback

`MidiSourceProcessor` already emits controls before note-ons and chases state
on locate. With the track's map it emits the output whenever the active
articulation changes from one note to the next, just before the note-on (minus
the lead time). Locating mid-song chases the last articulation before the
playhead. The map reaches the audio thread as an immutable snapshot, like
`setSequence`. Live playing uses the editor's current articulation (later).

### Dynamics, velocity layers and CC sequences

Dynamics (CC1/CC11 curves, velocity layers) stay separate from articulations
for now, but one idea belongs here: an articulation whose output is a
**programmed CC sequence** - a small shape (swell, crescendo, a tremolo-like
CC pattern) stored in the map and written over the note's length, so a single
articulation choice can imply a controller gesture, not just a switch. To
explore after the basic outputs work (and to decide how it interacts with
CC curves the user draws).

### Command-first (DESIGN.md)

Everything lands as API commands first: `expressionmap.list/get/create/delete/
rename`, group and articulation add/update/remove/reorder (including a modifier's
applicable roots), `track.setExpressionMap`, and `clip.addNotes` /
`clip.updateNotes` accepting an `articulation`, plus `clip.setArticulation` for a
selection. The UI is a client of the same commands.

### Phases

1. Model + persistence + commands + tests (case-insensitive names, applicability
   and drop-on-root-change rules, one modifier per group, project round trip,
   rename rewriting, sync keeps maps). No UI.
2. Editor: the dropdown, note assignment, symbols on notes, named keys and
   per-articulation key ranges.
3. Playback: output, lead time, chase on locate.
4. Map editor UI and the channel configuration view (own milestone below).
5. Library, presets (Spitfire UACC, a generic keyswitch map), Cubase
   `.expressionmap` import (observed format only), Synchron detection, then
   programmed CC sequences.

### Open questions

- Two selected articulations setting the same CC/keyswitch: "later group wins" as
  drafted - enough?
- Copying a map to another track: copy by value (as drafted) or share a reference?
- Should maps move up to the instrument channel later, so every track on a
  channel shares one?
- Default lead time and whether it is per articulation or per map.

### Key ranges must follow articulation changes

Key ranges must follow articulation changes. Each Synchron sound slot and
articulation node has its own rangeFrom/rangeTo (vepro.keyRange reads them
from the player's state on the server; today it caches only the range of the
first loaded slot, fetched once; empty "Custom" slots report 0-127 and are
skipped). When our keyswitches or program changes switch
the player's slot, the editor's greyed-out keys should match the active
slot. The likely approach: fetch the whole tree once and cache a range per
slot and articulation path, mapped to the expression map's entries. Then
look up the range at the playhead or edit position instead of re-querying
the server on each switch, and refresh the cache when the player's setup
changes (e.g. on sync, or with `refresh`).

## MIDI channel configuration view (drafted 2026-10-05)

A separate piece of work from the map system, but the place where maps get
assigned. A configuration view for a track's MIDI channel. Today
`InstrumentEditorView` shows 16 channels of port 1 only, with name editing and
nothing else; the track output choice is a popup in `MainComponent`.

Per MIDI channel it sets:
- **Port** and **channel**, beyond port 1 (the plugin's event buses, e.g. 16 for a
  VE Pro plugin - `getInstrumentMidiPortCount`),
- **Name**,
- **Expression map** (the track's map, for now: pick one from the library,
  create or edit it, or none).

Notes:
- Synced (VE Pro) channels stay immutable for port, channel and name; only the map
  can be set. Manual channels need to exist without a name (a channel is only kept
  today if it has a name).
- Needs `track.setExpressionMap` (and friends) as commands first.
- The view also hosts the key range display for Synchron channels.

## Parameter automation lanes

Curves over musical time targeting (instrument, parameter), (channel, gain)...
Same immutable-snapshot pattern as TempoMap/MidiSequence; must use stable
parameter IDs, not indices. Lane editing shares the piano roll machinery.

## Stacked editor lanes

CC, note velocity, aftertouch at the bottom of the editor; velocity default;
more lanes stackable on top of each other, with controls below the piano keys.
(From ISSUES.md Midi editing - larger than a quick fix because it reworks the
lane area into a list of lanes.)

## Theming (planned 2026-10-04; full plan in THEMING.md)

Settings > Theming becomes a theme editor. Every kind of UI item has its own
color slot (a "token"); some are unique (arrange/audio background), some are
shared and inherited (panel border). Specific items follow a common parent
until overridden, so one change can retheme everything or just one thing.

Settings > Theming must have:
- **Theme picker.** Built-in Dark is the default and cannot be deleted or
  edited in place (editing it forks a copy).
- **Save / delete custom themes** (XML of overrides in the user data dir).
- **One setting per item**, each with a color picker: a color area plus a hex
  field (hex can be copied and pasted), and a reset-to-inherited button.
- **Live preview**: changes show immediately in the real app.

Items (first pass; THEMING.md has the token table):
- Arrange/audio background (one item), transport line (one item).
- Channel background and channel border; folder background (and border).
- MIDI region background and border, each with **opacity** and **brightness**
  (these are not plain colors: they modify the track color the region is
  drawn with; today's global track color opacity moves here).
- Also proposed: playhead, loop range, markers (loop/tempo/meter/marker),
  grid and bar lines, lane stripes (even/odd), selection (row and outline),
  piano roll background, keys (white/black), note fill / selected note /
  note border, unplayable-key grey-out, timeline bar, track list rows, top
  bar and its panel/separators, transport buttons (play/record/loop/arm/
  solo/mute, off and on), text (primary/secondary/dim), settings window,
  status bar, popup menus and standard widgets (via a LookAndFeel).

Out of scope for v1: fonts, sizes, corner radii, per-project themes.

Phases: (1) audit + token registry, default theme pixel-identical; (2) theme
core; (3) convert views one by one; (4) LookAndFeel; (5) `theme.*` API
commands (command-first); (6) settings UI; (7) extra built-in themes + lint
against new hard-coded colors. Open questions are at the end of THEMING.md.

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
