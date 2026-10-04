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
from the player's state on the server; today it caches only the range of the
first loaded slot, fetched once; empty "Custom" slots report 0-127 and are
skipped). When our keyswitches or program changes switch
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
