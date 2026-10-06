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

## Articulation / expression maps (drafted 2026-10-05, revised: maps on instruments)

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
- **Selecting is a toggle, and modifier groups are exclusive.** Choosing an
  item and choosing it again is the same menu item: select / unselect. In a
  modifier group, once an item is chosen the other items of that group become
  unavailable (greyed out) until it is unselected.
- **The root group is different: all roots are always visible and available.**
  Still only one root per note, but clicking another root switches directly to
  it (no unselecting first); clicking the selected root again unselects it
  (the note then has no root).
- **Choices follow the root.** The modifiers that don't apply to the selected
  root are removed from the choices (a group with nothing left disappears from
  the menu). When a different root is chosen, the modifiers already chosen that
  still apply are **kept**. Those that no longer apply are handled by a setting,
  **Settings > Editor > Midi** ("when changing the root articulation would drop
  incompatible modifiers"): **Ask first** (the default; a prompt lists what would
  be dropped and the user accepts or cancels the change) or **Drop
  automatically**. (There is no "Editor" settings tab yet; it is added with
  this.)
- **Modifiers are not available without a root.** With no root selected, the
  menu offers the roots only; modifiers appear once a root is chosen (and
  unselecting the root drops them).
- **Default root (a setting).** Also under **Settings > Editor > Midi**: "Use the
  first root articulation as the default" (off by default). When on, a note with
  no root articulation behaves as if the map's first root articulation were
  selected: it is sent for playback and shown as such, and new notes start with
  it. This is practical, and it also avoids the instrument staying on whatever
  the previous note switched to. It applies implicitly (nothing is written to the
  notes), so turning it off again restores them to "none".

### Output (how an articulation reaches the instrument)

An articulation has a list of outputs, sent **in series** (in the listed order,
one after the other) when it becomes active. Each output is one of:
- a **keyswitch** note (key, velocity, held or tapped),
- a **CC** (number, value) - e.g. Spitfire UACC on CC32,
- a **program change** (with optional bank).

So one articulation can send, say, a keyswitch, then a CC, then a program change.
The list may be empty (an articulation that needs nothing sent). Repeating a CC
inside one articulation is the author's explicit sequence, not a conflict.

The active combination's outputs are the root's, then its modifiers'. (Replaced
by "Sound slots" below: outputs belong to slots, not to articulations.)

**No restrictions on combining outputs.** Any articulation may use any key, CC or
program change, including the same one as another articulation, even one that
can be active at the same time (a root and its modifier on the same CC, two
modifiers on the same keyswitch...). This is left to the user for now - it may
be what a library needs, and refusing it could get in the way. (An advisory
check can come later if it turns out to be useful.) Validation only checks
structure and value ranges: names present and unique, "applies to" naming real
roots, numbers in range.

The output is always sent exactly before its note - no setting, no default
offset. (Timing is a separate thing, next.)

### Sound slots (decided 2026-10-05; phase 6)

Adding up outputs from the parts (root, then modifiers) can't describe a library
like Synchron, where each combination needs its own programs, and the same name
(Legato on Long notes, Legato on Tremolo) sends different things. So a map gets
**sound slots**, as in a Cubase expression map. Not VSL's sound slots: those are
where a Synchron map's combinations can be derived from (below). When a map
says "slot" it means an **expression-map sound slot**.

- **A sound slot is one combination of articulations** (one root plus at most one
  modifier per group, exactly what a note stores as its selection) **and what that
  combination does**: its outputs, sent in series (as above), its playable key
  range, and its timing offset. Nothing is added up: "Long notes", "Long notes +
  Legato" and "Long notes + Legato + Soft" are three slots with three output lists.
- **Groups and articulations are for the UI only**: name, symbol, description. They
  no longer carry outputs, key ranges or timing offsets in a map that has slots
  (those live on the slots).
- **The UI follows the slots; it never offers or stores a combination that has no
  slot.** There is no separately stored list of what works with what: it is
  derived. A modifier is available when some slot contains it together with
  everything that is already selected; the others are not shown (a group with
  nothing left disappears), so choosing a colour hides the modifiers that no slot
  has with it. Choosing a root keeps the modifiers that still lead to a slot and
  drops the rest (ask or drop, as today). A note's stored selection is always the
  combination of one slot.
- **Same depth.** Two modifier groups are at the same depth for a choice when no
  slot that fits it has an articulation from both (repetitions: a transition -
  Legato / Slur - or a release - Cut - never both; under Long they combine). It
  follows from the slots, nothing is stored. Alternatives don't hide each other,
  choosing one replaces the other silently, and the articulation panel stacks
  them in one column.
- **Defaults.** An articulation can name a default articulation per group
  (Rep. -> Tempo 120; colour Con sordino -> Main Long notes). Choosing it fills its
  defaults into the groups that have nothing chosen, and the defaults' own defaults
  after them, so what is stored is always a complete combination with a slot; the
  filled-in choices show as chosen and can be changed. Validation: every default
  must lead to an existing slot.
- **Playback sends the outputs of the note's slot** exactly before the note,
  shifted by the slot's timing offset. In normal use every note has a slot (the UI
  offers nothing else). Only a later change can leave a note without one: a slot
  removed from the map, the track moved to an instrument with another map, a
  project loaded without its map. Such a note plays with no articulation and is
  marked as an error so it can be repaired.
- **Key range follows the slot**: the editor greys out what the slot in effect
  can't play (replaces the per-articulation range and the one-range-per-channel
  stopgap).
- **Deriving the slots from a Synchron player.** VSL's sound slots (the leaves of
  the preset tree, each with its program path and key range) enumerate every valid
  combination, so a map's slots can be generated from them. `vsl-manager`
  (`tools/daw_maps.py`) already enumerates them for the Cubase maps (185 for the
  merged Duality 1st Violins). "Synchron detection" (below) produces slots, too.
- **Every map uses slots**; the outputs-on-articulations model and applies-to lists
  go (no maps of that kind are in use). The built-in keyswitch template becomes a
  map of slots.
- **Validation** (slots): each slot names an existing root and existing modifiers,
  one per group at most, a root is required, no two slots have the same
  combination, every output and range is in range. Renaming a group or an
  articulation rewrites the slots that use it; deleting an articulation is refused
  while a slot uses it (or removes those slots, on request).
- **Editor**: the map editor gets a slot list (combination, outputs, range, offset)
  with a filter, add / remove / duplicate, and defaults per articulation. Commands: `expressionmap.setSlot`, `removeSlot`, `slots`
  (list, filterable by a selection), and `expressionmap.available` (what the menu
  would offer for a selection).

Example (Duality 1st Violins, groups Color (root) / Main / Legato / Release /
Tempo): choosing the colour Ponticello leaves only the Main items some Ponticello
slot has; choosing Main "Rep." offers the five tempos (the only Main with tempo
slots); choosing "Long notes" offers Legato, Lyrical, Slur, Portamento and the
releases. Legato on "Tremolo" and Legato on "Long notes" are two slots with
different programs under the same label.

Phase 6. **Delivered 2026-10-05:** the model (slots, defaults, `offeredModifiers`,
choosing with defaults - filled-in defaults that no longer fit drop silently, your
own choices are reported - validation, XML/JSON, renames into slots and defaults);
the menu follows the slots; playback sends the slot's outputs with its timing
offset and the editor's key range is the slot's; commands `expressionmap.slots /
setSlot / removeSlot / offered`; vsl-manager `tools/daw_maps.py` writes slot maps
(the merged Duality 1st Violins: 177 slots, groups Color / Main / Legato / Release /
Tempo), checked against the app's model by a test with the generated file.
The map editor (delivered the same day): a "Sound slots" column with a filter
(every word must match: "Ponticello Rep."), Add / Duplicate / Remove; a slot's
details set its combination (one choice per group), timing offset, key range and
outputs; in a slot map an articulation shows its defaults instead of outputs; a map
built the old way turns into slots from its details ("Make sound slots from the
articulations", `ExpressionMap::convertToSlots`: one slot per combination its
applies-to lists allow). The keyswitch template is a map of slots.
**Still to do:** try the editor and the menu in the app with the Duality map
against VE Pro; then the old model (outputs and applies-to on articulations) goes.

### Timing offset (working name)

Some articulations sound late: a Spitfire legato has a delay between the note
being triggered and being heard. To line such notes up with the others, every
articulation can have a **timing offset**: time added to or removed from the
moment its notes are triggered. A legato at -70 ms is triggered 70 ms early so
it is heard on the beat.

- Per articulation, in **milliseconds** (the delay is real time, not musical
  time; converted to ticks with the tempo map at the note's position). Default
  0: no offset unless the user sets one. Negative = earlier, positive = later.
- The offset moves the **note**, and the switch goes with it: the keyswitch / CC /
  program change is still sent exactly (just) before the shifted note-on. The
  gap between switch and note does not change.
- It only changes what is **sent**. The editor keeps drawing notes where they
  are written (on the grid, on the beat); the offset is playback compensation.
- The note's length is kept (note-off moves with note-on).
- **No look-ahead in the audio thread: the shift is baked in.** There are two
  sequences per track: the **written** one (what the editor shows, what is saved,
  what the user edits) and a derived **playback** one that is generated from
  (written sequence, the instrument's map, the tempo map): notes shifted by
  their articulation's offset, and the switch events inserted just before each
  shifted note-on. `MidiSourceProcessor` plays the playback sequence exactly
  like it plays sequences today (it arrives through `setSequence`, an immutable
  swap), so a negative offset is just a note sitting earlier in the sequence.
  - It is regenerated whenever something it depends on changes: notes or their
    articulations, the map (an offset edited, an articulation added), the
    track's instrument assignment, the tempo map. It is cheap (one pass over the
    track), done off the audio thread.
  - Loop wraps and locates need nothing special - the events are where they are.
    Switch events are tagged so the existing chase-on-locate can replay the
    last one when the playhead lands mid-phrase.
- **A negative offset at the very start grows the playback sequence backwards,
  and playback starts before the transport moves.** The playback sequence may
  hold events at negative times (the written sequence never does; today
  `MidiSequence::create` clamps every tick to >= 0, so the playback sequence
  needs to be allowed to go negative). Pressing Play then starts a **pre-roll**:
  the engine runs from -N ms (N = the largest negative offset the track
  maps use) up to the start position while the transport bar and position
  readout stay put, and only then begins to move. This is potentially a hard
  problem (see below) but it is the right behaviour.
  - **It is not only the start of the song.** Playing from bar 5 has the same
    problem: a legato written at bar 5 is triggered 70 ms before the playhead
    gets there, and when playback starts AT bar 5 those events lie before the
    start position. So every Play start gets the pre-roll (nothing is added when
    no map has a negative offset).
  - The **transport** needs a pre-roll phase: a position that counts from -N up to
    the start position at the normal speed (tempo map extrapolated from the first
    tempo before the start), a displayed position held at the start meanwhile,
    and the recorder, metronome and anything else that reads the position told
    which of the two it gets.
  - **Loops**: a note written at the loop start whose trigger lies before it
    must play near the END of the previous lap. The loop is effectively circular
    for the playback sequence: at the wrap the early events of the next lap
    are due before the wrap itself.
  - **Locating while playing** is a restart with the pre-roll, or a short gap -
    to decide.
  - Rendering/bouncing includes the pre-roll.
- **Live edits only affect the future.** When notes, articulations or an offset
  are edited while playing, the regenerated playback sequence replaces the old
  one, but playback can only play what is still ahead of it: an event whose
  (shifted) time has already passed is not played retroactively, it is simply
  missed this lap and right on the next one (adding a -70 ms legato to a note 30 ms
  ahead of the playhead cannot be heard in time). The swap must still be safe for
  notes already sounding: their note-offs are sent as before, never lost, so
  nothing hangs.
  - **The spike is done (2026-10-05)** and the idea works - see "What the pre-roll
    spike found" below.
- Notes shifted by different amounts can overlap or reorder (a shifted legato
  overlapping the previous note). That is usually the point, but the playback
  sequence must keep each note's on/off pair together.
- Not sure what to call it: "timing offset", "latency compensation" and "delay
  compensation" are candidates; Cubase recently added this to its expression
  maps, so its naming is worth a look when we get there.

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

- **Maps belong to instruments.** An instrument here is the sound a track plays
  through: one MIDI channel of an instrument plugin (one VE Pro player, one
  Kontakt channel). One plugin hosts many different sounds, so the assignment is
  per instrument channel (`MidiChannelInfo`, next to its name and key range), not
  per plugin and not per track. So the channels of one multi-instrument plugin
  (Kontakt, VE Pro) each get their own map, and every track playing the same
  channel shares it.
  - The **maps themselves are project data**: a named collection saved in the
    project (`<EXPRESSIONMAPS>`), referenced by name from the instrument
    channels, so several instruments (1st and 2nd violins) can share one map and
    editing it changes them all. A **user library** of maps (user data folder, like
    themes) lets maps be copied into a project, exported and imported.
  - The assignment is saved on the channel's `<MIDICHANNEL>`; synced (VE Pro)
    channels keep their immutable name/port/channel but their map stays editable,
    and `setSyncedInstrumentChannels` carries it over on every re-sync (as it
    does the key range).
  - **Undo**: history snapshots do not cover instrument channels today (channel
    names are not undoable either). Maps and assignments need to be added to the
    snapshot.
- **Notes carry the choice.** `MidiSequence::Note` gets a trailing
  `articulation` member (default none), stored in `<NOTE>` as names, so old
  projects load unchanged. In memory it should be a small interned id into the
  map's combination table, not a vector per note. **Renaming** a group or
  articulation (including a case-only change) is a command that rewrites every
  note using it, across all tracks on the instruments using the map, in one undo
  step.
- **Articulations that don't exist are visible errors.** If a note's
  articulation isn't in the instrument's map any more (a track moved to another
  instrument, the map changed, an articulation deleted, a project loaded without
  its map), the note keeps the name it has (nothing is cleared, so the data can
  be repaired) but is treated as having **no articulation** for playback: no
  switch is sent for it. In the editor it is marked as an error (distinct from
  "no articulation"), which helps finding where edits are required. The
  articulation menu offers to select all notes with a missing articulation.
  Fixing is done by adding the articulation to the map, renaming, or assigning
  another one.

### The editor

- An **articulation dropdown in the MIDI editor's top bar** (before the track
  label, wired like the other boxes; disabled when the track's instrument has no
  map). It opens a menu: the root articulations first, all always available
  (symbol + name, description as tooltip, the selected one checked); the
  modifier groups with applicable modifiers follow as further sections, an item
  greyed out when another item of its group is chosen. Every item toggles. It
  edits the **selected
  notes**; with nothing selected it sets the articulation new notes are drawn
  with. A selection that mixes articulations shows "Mixed".
- Notes show their **symbol** in the piano roll (colour per articulation later).
  Notes whose articulation is missing from the map show an error mark instead.
- Articulation changes are undoable edits like any other (clip commands).

### Playback

The processor changes very little. The track's instrument channel (output
instrument + port + channel) tells which map applies. From the written
sequence, the map and the tempo map a **playback sequence** is generated (see
"Timing offset"): notes shifted by their articulation's offset, and the switch
events (keyswitch, CC, program change) inserted just before each shifted
note-on whenever the active articulation changes. `MidiSourceProcessor` is one
per track and already emits controls before note-ons and chases state on
locate; it gets the playback sequence through `setSequence` and plays it. The
switch events are tagged so locating mid-song re-sends the last one (a
keyswitch is not a CC, the existing chase would not know it). Live playing
uses the editor's current articulation (later).

### What the pre-roll spike found (2026-10-05)

Built and tested in `Transport`, `MidiSourceProcessor`, `TempoMap` and
`MidiSequence` (tests/PreRollTests.cpp, sample-exact at 48 kHz). Nothing
changes while the pre-roll is 0 (the default); no engine or UI wiring yet.

**It works.** `Transport::setPreRollMs()` makes playback begin that long before
the position it shows; the readout stays at the start until it is reached, and an
event scheduled 70 ms early sounds exactly 3360 samples before the transport's
tick 0. Stop during a pre-roll leaves no stuck notes and the position at the start.

**What the design needs, learnt on the way:**

- **Every event of the playback sequence must remember where it was WRITTEN, not
  only when it is scheduled** (`sourceTick` on `Note` and `Control`;
  `written()`). The pre-roll window holds two kinds of events that timing alone
  cannot tell apart: early-shifted events of the part being played (must sound) and
  ordinary events written before the start (must stay silent). The rule: an event
  scheduled before the *gate* (the start of playback, or a lap's start) plays only
  if it was written at or after the gate; and an event written at or after the
  loop end never plays. Switch events inherit their note's written time.
- **Time before tick 0 exists.** `TempoMap` now continues backwards at the first
  tempo (it clamped before); `MidiSequence::create(..., allowNegativeTimes)` lets
  a *playback* sequence hold negative times. Bars, beats and the written
  sequence still never go below 0.
- **A locate while playing is a restart with a pre-roll** (so events due before
  the new position still sound), with the readout moving to the new position at
  once. The cost is a silent gap of the pre-roll length at every locate.
- **Loops need a look-ahead.** In the last pre-roll's worth of a lap, the block
  carries an extra "ahead" segment: the same samples, one loop length earlier,
  whose events (scheduled just before the loop start, written inside the loop)
  sound at the end of the previous lap. Those notes are *carried* over the wrap
  instead of being cut; their end is tracked in the next lap's time. If the loop
  is switched off (or the position moves) before the wrap, the carried notes
  belong to a lap that will not happen and end at once. No duplicates at the wrap.
- **The controller chase** must count only events both scheduled and written
  before the chase tick, otherwise an early-shifted controller is both chased and
  played.
- **Verified:** pre-roll from tick 0 and from the middle; gating (a note 10 ms
  before the start stays silent, one written 10 ms after sounds in the pre-roll,
  a late-shifted one plays normally); controller chase; locate while playing;
  stop during the pre-roll; loop look-ahead over three laps (one early note-on
  per lap, exact samples, none stuck); events written past the loop end; stop and
  loop-off while a look-ahead note sounds.

### Playback wiring (delivered 2026-10-05)

- **The generator** (`model/PlaybackSequence.h`): from the written sequence, the map
  of the channel the track plays and the tempo map it builds the playback sequence:
  notes shifted by the sum of their root's and modifiers' timing offsets (ms,
  converted at the note's position, length kept), and the articulation's outputs
  inserted at the note's own tick, before its note-on, whenever the articulation
  changes (keyswitch notes - tapped 30 ms, or held until the next change - then CCs,
  program changes with bank select CC0/CC32). An unresolved articulation plays as none;
  a note with none sends nothing. Every generated event carries its written tick.
- **The engine plays it:** rebuilt on every sequence change (edit, undo, redo, time
  travel, load), when a map is set/removed/renamed, when an assignment or a track's
  output changes, when the tempo changes, after a re-sync or an instrument removal, and
  when the "first root as default" setting is toggled. The written sequence is never
  touched. The transport's pre-roll is the most negative offset any track uses
  (0 when none does: nothing changes for projects without maps).
- **The chase covers keyswitches:** starting mid-piece, or wrapping a loop, re-sends
  the keyswitch of the articulation in effect (a tapped one for its own length, a held
  one that is still down until its end), together with the CC/program state.
- **The recorder** stamps what is played during a pre-roll at the start of the take,
  never before it.

**Still open:**
- **Positive offsets across the loop wrap** (a late note written near the loop end,
  scheduled after it) are not carried into the next lap.
- **Tempo changes inside the pre-roll window** convert correctly (it is wall-clock
  time), but a tempo change right before the start moves the tick where the
  pre-roll begins.
- **The silent gap:** every Play and every locate while playing now waits one
  pre-roll (70 ms for a Spitfire legato) before the position moves.
- **No bounce/export exists yet**; when it does it must include the pre-roll. The same
  goes for a metronome or count-in (neither exists).
- **A loop shorter than the pre-roll:** the look-ahead window is clamped to the loop.
- **Editing the loop region or a map while rolling:** the pre-roll changes at once, the
  position does not jump; the first lap after the change may miss an early note.
- **Notes sounding in the old state when a sequence is swapped live** are not
  restarted, only their note-offs stay correct (the existing tracking).
- **Several notes at the same written tick with different articulations** each send
  their own switches in note order; the last one wins on the instrument.
- **Multi-output tracks:** the map of the track's FIRST output decides.

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
rename/validate`, group and articulation add/update/remove/reorder (including a
modifier's applicable roots), `instrument.setChannelMap`, and `clip.addNotes` /
`clip.updateNotes` accepting an `articulation`, plus `clip.setArticulation` for a
selection. The UI is a client of the same commands.

### Phases

1. Model + persistence + commands + tests (case-insensitive names, applicability
   and drop-on-root-change rules, one modifier per group, structure/range validation,
   project round trip, rename rewriting, sync keeps assignments, snapshots cover
   maps and assignments). No UI.
   **Delivered 2026-10-05** (ExpressionMap.h, AudioEngine, CommandDispatcher):
   the model and validation, XML/JSON, the project's maps and the channel
   assignment (kept across re-syncs), articulations on notes, the choosing
   rules (`expressionmap.choose`, `clip.setArticulation`), the commands in
   API.md, and maps + assignments in the history snapshots.
**Renaming inside a map** (delivered the same day): `expressionmap.renameGroup` /   `expressionmap.renameArticulation` keep names unique ignoring case, update the   modifiers' applies-to lists when a root is renamed, and rewrite the notes of   every track playing a channel that uses the map, as one undo step. (`expressionmap.set`   still replaces a whole map without touching notes; those that no longer   match show as visible errors.) **Phase 1 is complete.**
2. Editor: the dropdown, note assignment, symbols on notes, named keys and
   per-articulation key ranges.
   **Delivered 2026-10-05:** the dropdown in the top bar (the shared menu rules,
   toggle, exclusive groups, ask-or-drop per Settings > Editor > Midi, one undo
   step), the Editor settings tab (drop behaviour, first root as default),
   symbols and error marks on notes, key names and keyswitch marks on the piano
   keys with instructions as tooltips, and the greyed-out range following the
   articulation in effect. **Still to do:** a menu entry that selects the notes
   whose articulation is missing; the editor's new colors (keyswitch mark, error
   red) are hard-coded and belong in the theme; maps can only be created through
   the API until the map editor (phase 4).
3. Playback: the pre-roll spike and the wiring are done (see "What the pre-roll spike found" and
   "Playback wiring"); what is left is listed under "Still open" there.
4. The two configuration views (own milestone below) and the map editor UI.
   **Delivered 2026-10-05:** the map editor (instrument view > "Expression maps..."):
   maps, groups, articulations, outputs in series, timing offsets, key ranges,
   applies-to, key names - all through the commands, with names renamed so notes
   follow, and an invalid edit refused with the reason; the instrument view with a
   port selector and, per channel, a map dropdown (None / the project's maps / a
   missing one) and an Edit button (synced channels keep their name locked, their
   map is editable); the MIDI track panel (track context menu > "Port and
   channel...": port and channel of the output, greyed out for a synced track).
   **Still to do:** the editor's colors and the new views' colors are not in the
   theme yet; the map editor has no undo button of its own (the global history
   has every change); a map can't be copied between projects until the library
   (phase 5); no drag-and-drop ordering (Up/Down buttons).
5. Library, presets (Spitfire UACC, a generic keyswitch map), Cubase
   `.expressionmap` import (observed format only), Synchron detection, then
   **Status of phase 5:** the library is done (5a): maps are XML files in the
   user data folder's Maps; commands `expressionmap.libraryList / saveToLibrary /
   addFromLibrary / deleteFromLibrary / export / import`; the map editor's
   "Library..." menu. One built-in template ("Keyswitch starter", placeholder
   keys). **Blocked on source data:** the Spitfire UACC preset (needs the
   official table) and the Cubase `.expressionmap` import (needs sample files).
   **Not done yet:** Synchron detection, programmed CC sequences.
   programmed CC sequences.

6. Sound slots (see "Sound slots"): the UI follows the combinations that exist,
   each combination has its own outputs, key range and timing offset.

### Open questions

- **Timing offset**: what it is called. (The pre-roll for negative offsets is
  decided; its transport, loop and locate details are in "Timing offset".)

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

## MIDI track and instrument configuration views (drafted 2026-10-05)

Two separate, small configuration views, the places where routing and maps get
set. Today `InstrumentEditorView` shows 16 channels of port 1 only, with name
editing and nothing else; the track output choice is a popup in `MainComponent`.

**MIDI track configuration** - for now only:
- **Port** and **channel** of the track's output, beyond port 1 (the plugin's
  event buses, e.g. 16 for a VE Pro plugin - `getInstrumentMidiPortCount`).
- Greyed out when they are immutable (tracks created by "Sync to VE Pro
  Server": their instrument, port and channel come from the server).

**Instrument configuration** - for now only:
- **Expression map** selection: pick one of the project's maps (or none), with
  a way into the map editor to create or edit one.
- Applies to the instrument channel, so it is shown per channel (synced channels
  included: only the map can be set on them).

Later additions can go in both views (name, colour, key range display for
Synchron channels...); manual channels also need to exist without a name (a
channel is only kept today if it has a name).

Needs `instrument.setChannelMap` (and a way to set a track's port/channel)
as commands first.

## Parameter automation lanes

Curves over musical time targeting (instrument, parameter), (channel, gain)...
Same immutable-snapshot pattern as TempoMap/MidiSequence; must use stable
parameter IDs, not indices. Lane editing shares the piano roll machinery.

## Stacked editor lanes

CC, note velocity, aftertouch at the bottom of the editor; velocity default;
more lanes stackable on top of each other, with controls below the piano keys.
(From ISSUES.md Midi editing - larger than a quick fix because it reworks the
lane area into a list of lanes.)

## CC editing with points (drafted 2026-10-06)

Today a CC (or pitch bend) lane is edited by drawing: a stroke writes a value
every 1/32 note. Orchestral parts want shapes - a swell over two bars, a quick
dip, a held level - that are easier to place, read and change as a few points
than as hundreds of values. So a controller lane becomes a list of **points**
with a **curve to the next point**.

- **Points.** A point has a position (tick) and a value (0-127, or the bend
  range). Click in the lane to add one, drag to move it (position and value),
  right-click or Delete to remove; a selection of points moves together, and
  dragging a selection's top or bottom edge scales its values. Notes are not
  involved: a lane edits only its controller.
- **Where points can go: Q and the grid.** A new or moved point snaps to the
  editor's grid when Snap is on (the Snap toggle and grid dropdown the notes use);
  Alt (temporarily) frees it. Quantize (Q) on selected points moves them to the
  grid, like notes.
- **Curves.** Each point says how the value travels to the next one:
  - **Step** (staircase): it holds until the next point, then jumps.
  - **Ramp**: a straight line from this point's value to the next.
  - **Bent ramp**: a curve - its bend set by dragging the segment's middle up or
    down (one handle per segment; straight when centred). An ease-in or
    ease-out swell is one bent segment.

  The lane shows the curve, the points as handles, and the segment handle on hover.
  A point's curve is changed from its context menu, or a default for new points
  (Step / Ramp / Bent) in the lane's header.
- **What is sent.** Playback turns the points into CC messages: a step sends one
  message at its point; a ramp or bent ramp sends values along the segment, only
  when the 7-bit value changes (and at least every 1/64 note at most, so a slow
  ramp stays light). The pre-roll and the chase send the value in effect at the
  play position, as they do for CCs today.
- **Recorded and imported CCs.** A recorded or imported controller stream is a
  dense list of values; it is kept as points with Step curves (exact playback),
  with a "Thin" command that reduces it to fewer points with ramps within a
  tolerance (the Cubase "reduce" idea), as one undo step.
- **Model and file.** A lane stores its points (tick, value, curve, bend); the
  project keeps them per track and controller next to the notes. Old projects'
  CC events load as Step points. Commands: `clip.addControlPoints`,
  `clip.updateControlPoints`, `clip.removeControlPoints`, `clip.thinControls`
  (same undo and history rules as the note commands).
- **Lanes.** Goes with "Stacked editor lanes": every CC lane in the stack edits
  points. Velocity stays a per-note bar lane.

Phases: model, file, playback rendering and commands with tests; the lane editor
(add / move / delete / select / snap); curves and the bend handle; thinning of
recorded data.

## Articulation remote control: key commands and MIDI controllers (drafted 2026-10-06)

Choosing an articulation without the mouse: a computer key command, a MIDI
message from a controller, or both, per articulation. (ISSUES.md "Settings
Window": controller or key commands for specific articulations.)

- **Bindings live on the articulations of a map** (stored in the map, like the
  Cubase maps' "remote" keys), so a map brings its controls along: per
  articulation an optional **key command** (e.g. Ctrl+1) and an optional **MIDI
  trigger** (a note, a CC with a value, or a program change, on a channel or any).
  Set in the map editor's articulation details: a key field that records the
  next key pressed, and "Learn" for the MIDI trigger (the next control moved or
  pressed on a MIDI controller). A binding used twice in a map is refused with
  the reason.
- **MIDI controllers (Settings > Audio & MIDI).** A MIDI controller is a control
  surface: any MIDI input device can be added as one ("use as a controller").
  Its notes and controllers then never reach tracks or recording; its controls
  are assigned to functions - articulations first (an articulation's MIDI
  trigger), other functions (transport, ...) can follow. Saved in the
  settings. A device that is not a controller plays notes as today.
- **What a trigger does:** exactly what choosing that articulation in the
  articulation panel does, with the same rules (defaults filled in, alternatives
  replaced, only what a slot has): on the selected notes in the MIDI editor; with
  no notes selected, on the articulation new notes are drawn and recorded with,
  for the editor's track or the armed track.
- **Hearing it while playing.** A trigger also sends the new combination's slot
  outputs to the instrument at once (through the track's route), so the next note
  played live sounds in that articulation.
- **Recording.** Notes recorded while articulations are switched carry the
  articulation in effect when each note started (a switch timeline per take),
  so a played performance keeps its articulations.

Phases: (1) settings: MIDI controllers (their input kept away from
tracks), the bindings in the map and the map editor (key field, MIDI Learn),
triggers applying to the selection / the new-note choice, live switching;
(2) recording stamps the articulation on recorded notes.

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

## Audio mixer (drafted 2026-10-06)

A mixer for the audio channels: names, level faders, pan, solo, mute, meters,
aux sends, insert effects and output selection. F4 opens it (F1 MIDI arrangement,
F2 MIDI editor, F3 audio arrangement / automation); M stays mute.

Today an audio channel is one stereo strip per instrument (gain, mute, a block
peak) wired straight to the device's stereo output, and only the instrument's
first output pair is used (VE Pro's other outputs are dropped). Effects are
scanned but filtered out.

- **Channels.** A channel gets a name (defaulting to its source; a rename wins),
  a fader (dB), pan (stereo balance, -3 dB centre), solo, mute, a colour and an
  output. Every instrument OUTPUT PAIR can have its own channel, so a VE Pro
  instance with 16 stereo outputs gives 16 strips (named from VE Pro's channel
  names when the sync knows them).
- **Buses.** Aux buses (stereo channels with no source of their own, fed by sends)
  and a master bus that every channel goes to by default. Output selection per
  channel: the master, a bus, or a pair of device outputs directly.
- **Sends.** Per channel, sends to aux buses: a level each, pre- or post-fader.
- **Inserts.** Per channel and bus, a list of effect plugins (the effects the
  scanner already finds), in order, each with bypass and its own window. Added
  from a menu of the scanned effects.
- **Solo.** Solo works across the mixer (in-place: everything not soloed goes
  silent, except buses fed by a soloed channel). Solo-safe for buses.
- **Meters.** Peak and RMS per strip, with a peak-hold line and a clip
  indicator (click to reset); the master shows them too.
- **The view.** Vertical strips side by side, scrolling sideways: name, inserts,
  sends, pan, fader with meter, solo/mute, output. Strips follow the sidebar's
  folder order; buses and the master at the right. The selected track's channel
  is highlighted, and selecting a strip selects its channel.
- **Model, file, undo.** Channels, buses, sends and inserts (with plugin state)
  are saved in the project; every change is an undoable history entry; commands
  for all of it in the API (`mixer.*`).

- **Console styles** (added 2026-10-07). The strip is drawn in a console style;
  SSL first (others later, chosen in the mixer's header). The SSL strip, top to
  bottom: the inserts (16 on a channel, 8 on an Aux bus and the master) behind the
  EQ and dynamics, flipped with a button at the top; EQ after the SSL 4000 E channel (HPF, LPF; HF gain / freq +
  BELL; HMF gain / freq / Q; LMF gain / freq / Q; LF gain / freq + BELL; EQ IN);
  dynamics (threshold, ratio, attack, release, make-up; DYN IN); six aux send
  knobs, one per built-in Aux bus (no bus choosing); drive and pan; a shorter fader; solo / mute;
  output. Small on/off buttons, lit when on. Six Aux bus strips sit between the
  channels and the master. The EQ, dynamics, aux knobs and Aux buses are
  placeholders until their phases (they show and turn, nothing more).

Phases, each usable on its own: (1) the strip basics - names, fader, pan, solo,
mute, meters, master bus, the view and F4 (DONE 2026-10-06; inserts, sends and
output show as placeholders); (2) output selection and multi-output instruments
(one strip per USED output pair); (3) the six Aux buses and the sends; (4) the EQ and dynamics (the SSL-style
processing behind the knobs); (5) insert effects.
Later: the mixer docked under the arrangement like the MIDI editor.

## Non-linear patching (far field)

Generator/logic nodes patched together, running live (DESIGN.md pillar 2).
Groundwork exists: the engine is a node graph and time is pulled per block.

## Linux port (planned 2026-10-06)

Status: branch `linux-build` builds and runs on Linux (build.sh / run.sh).
Plugin scanning, VE Pro Server sync, multiport MIDI (DAW -> JUCE patch ->
yabridge -> VE Pro plugin -> server) and audio through PipeWire work. VE Pro and
the VSL players run under Wine in the shared prefix of the parent ilok-linux
setup; Windows plugins load through yabridge.

- **Performance: Synchron players under Wine.** The main unknown. Benchmark:
  load N Synchron players at a fixed buffer size, raise N until dropouts; record
  CPU, dropouts and buffer size. Compare Wine defaults vs `WINEESYNC=1` (needs a
  high open-files limit), before/after real-time tuning, and against Windows on
  the same machine if possible. Then decide on ntsync (newer Wine plus Linux
  6.14+; currently 6.8) or a low-latency kernel. Sample streaming: libraries on
  fast NVMe; consider a casefold ext4 folder (Wine's case-insensitive lookups are
  slow on large libraries).
- **Real-time tuning check.** Read-only report script: real-time priority and
  memlock limits, CPU governor (want `performance`), PipeWire quantum / sample
  rate, kernel preemption model, open-files limit for esync.
- **MIDI reliability.** Multiport through yabridge confirmed (port 2 reaches the
  right player). Still to measure: timing jitter and dropped events under load
  (same benchmark setup), hardware MIDI input through ALSA/PipeWire.
- **Sync reuses the project's own VE Pro plugin.** When a server instance is
  already connected to a VE Pro plugin in this project (e.g. added by hand),
  sync reports it as "connected to another plugin", leaves it untouched and
  creates an unconnected duplicate. It should reuse that plugin and route the
  tracks to it.
- **Optional LTO.** Release builds use `juce_recommended_lto_flags`; GCC
  link-time optimisation makes every Release link slow. Off by default in
  build.sh, `--lto` for final builds.
- **VE Pro Server announces itself on the network (Avahi).** Under Wine the
  server can't announce itself (Wine lacks `DnsServiceRegister`), so other
  machines don't discover it and Linux defaults the host to 127.0.0.1. Plan: the
  VE Pro launcher runs `avahi-publish -s` for `_vepro._tcp` on port 7200 while
  the server runs (check whether plugins also need 6473); needs `avahi-utils`.
  Done when a VE Pro plugin or VSL CLI on another machine finds the server
  without an address. Wine-side tools on this machine would still need
  127.0.0.1:7200.
