# THEMING (plan)

Goal (ISSUES.md "Settings Window"): selectable themes where **every individual kind
of UI item has its own colors** (border, background, text...), with some colors
unique to one place (arrange view background) and some shared by many things
(panel border). Custom themes can be saved.

Status: plan only, nothing implemented. Settings > Theming already exists and
holds one setting, track color opacity (`colorOpacity`).

## Where we are today

- ~36 distinct `0xAARRGGBB` literals and ~80 `juce::Colours::*` uses are hard-coded
  in `paint()` and constructors across 18 files (PianoRollView.cpp 27,
  MainComponent.cpp 17, TrackList.cpp 15, SettingsView.h 15, ArrangementView.cpp
  14, AudioChannelList.h 14, TimelineBar.cpp 11...).
- Many of them are the *same* color used for the same role in different files:
  `0xff2e3136` is the grid line in the arrange view, the piano roll and the
  timeline bar; `0xff232529` is the panel background in the timeline bar, track
  list, settings category column and piano keys; `0xff39404d` is "selected row"
  in three views. These are the "common" tokens.
- No `LookAndFeel` is installed; buttons and labels are styled one by one with
  `setColour`.
- Track/folder colors are per-item data (`colours::palette`, `#rrggbb` strings) with
  a global opacity. That is *content*, not theme, and stays as is.
- Settings live in `Settings.xml` (`juce::PropertiesFile`) and are read directly
  at paint time (`colours::opacityFrom`). Views already repaint when the engine's
  `stateRevision` changes.

## Model: tokens with inheritance

A **token** is a named color slot, e.g. `arrange.background`. A theme is a map
token -> color. Tokens come in two kinds:

1. **Common tokens**: the shared vocabulary. Concrete values live here.
2. **Specific tokens**: one per UI item. Each declares a **default parent** (a common
   token, or another specific one) and a default value is simply "whatever the
   parent is". A theme may override any token explicitly.

So the user can retheme the whole app by changing ~25 common tokens, *or* change
exactly one thing (arrange view background) without touching anything else. An
unset specific token follows its parent, so changing `surface.panel` changes every
panel that has not been individually overridden.

Resolution: `override ?? resolve(parent) ?? builtinDefault`. Cycles are rejected at
registration (a unit test walks the registry).

### Token naming

`<area>.<element>[.<state>].<property>` where property is `bg`, `border`, `text`,
`fill`, `line`, `accent`. Examples: `arrange.bg`, `arrange.lane.even.bg`,
`topbar.button.on.bg`, `pianoroll.key.black.bg`. Names are stable identifiers
(they end up in theme files and the API), so choose carefully and never reuse.

### Common tokens (first pass, derived from the audit)

| Token | Today's value | Used for |
|---|---|---|
| `surface.window` | 1d1f23 | app/window background, settings page |
| `surface.panel` | 232529 | track list, timeline bar, piano keys, category column |
| `surface.panel.raised` | 2a2d33 | top bar |
| `surface.panel.sunken` | 17191c | status bar, keyboard strip |
| `surface.content` | 1a1c1f | arrange view and piano roll canvases |
| `surface.control` | 3a3e46 | normal button/field background |
| `border.default` | 43464d | panel outlines, separators |
| `border.subtle` | 2e3136 | grid lines, row dividers |
| `border.strong` | 45494f | bar lines, emphasized grid |
| `selection.bg` | 39404d | selected row/list item |
| `selection.border` | 6c87b5 | selected track/folder outline |
| `accent` | 5d8fc4 | notes, active toggles, focus |
| `accent.on` | steelblue | "on" state of toggles (snap, loop...) |
| `highlight` | orange | selected notes, drag, edit cursor |
| `text.primary` | white | main text |
| `text.secondary` | lightgrey | labels, status |
| `text.dim` | grey | hints, disabled |
| `state.record` | darkred | record/arm |
| `state.play` | darkgreen | play |
| `state.solo` / `state.mute` | goldenrod / orange | solo / mute |
| `marker.loop` / `marker.tempo` / `marker.meter` / `marker.marker` | gold / skyblue / mediumpurple... | timeline markers |
| `playhead` | gold | playhead line |

### Specific tokens (examples; the full list is produced by the audit in phase 1)

- Arrange view: `arrange.bg`, `arrange.lane.even.bg`, `arrange.lane.odd.bg`,
  `arrange.lane.folder.bg`, `arrange.gridline`, `arrange.barline`,
  `arrange.region.border`, `arrange.region.selected.border`, `arrange.loop.bg`
- Piano roll: `pianoroll.bg`, `pianoroll.row.black.bg`, `pianoroll.row.white.bg`,
  `pianoroll.row.unplayable.bg`, `pianoroll.note.fill`, `pianoroll.note.selected.fill`,
  `pianoroll.note.border`, `pianoroll.key.white.bg`, `pianoroll.key.black.bg`,
  `pianoroll.lane.bg` (velocity/CC), `pianoroll.rubberband`
- Track list: `tracklist.bg`, `tracklist.row.bg`, `tracklist.row.selected.bg`,
  `tracklist.folder.bg`, `tracklist.row.border`, `tracklist.arm.on`, ...
- Timeline bar: `timeline.bg`, `timeline.ruler.text`, `timeline.loop.bg`, ...
- Top bar: `topbar.bg`, `topbar.panel.bg`, `topbar.panel.border`,
  `topbar.separator`, `topbar.button.bg`, `topbar.play.bg`, `topbar.record.bg`...
- Settings, history, instruments, busy overlay, performance panel: same pattern.

Alpha is part of the color (ARGB); some uses are "this color at 35%" today
(`gold.withAlpha (0.35f)`). Rule: if the *alpha is the design* (a tint over
content) the alpha stays in code and the token is the opaque base; if it is a
plain color it is stored in full. Computed variants (`withBrightness` for note
velocity, hover/pressed `brighter()`) keep being computed *from* the token.

## Architecture

### `src/ui/Theme.h` (+ `Theme.cpp`)

- `enum class Token : uint16_t` generated from one **registry table**
  (X-macro: id, dotted name, parent, builtin default, human label, group). The
  registry is the single source for code, settings UI, API and docs.
- `Theme` value type: `std::array<juce::Colour, N>` of *resolved* colors plus the
  sparse override map it was built from. Immutable once built (same philosophy as
  the rest of the app: swap, don't mutate).
- `ThemeManager` (owned by `AudioEngine` next to the settings file): holds the
  active `Theme`, a revision counter, built-in themes, user themes on disk. Message
  thread only. `ThemeManager::get()` hands views the current resolved theme;
  `theme[Token::arrangeBg]` is an array index, cheap enough for `paint()`.
- Views read `theme[...]` at paint time and bump nothing. A theme change bumps the
  engine's `stateRevision` so existing polling repaints everything; no
  per-component listener plumbing.

### Standard JUCE widgets

Install one `ThemedLookAndFeel` (a `LookAndFeel_V4` subclass) as the default and
map JUCE's color IDs from tokens on every theme change (`TextButton`, `Label`,
`ComboBox`, `TextEditor`, `Slider`, `ScrollBar`, `PopupMenu`, `ToggleButton`
colors). That removes most per-widget `setColour` calls. Per-widget colors that
differ (play/record/arm/solo/mute/loop buttons) keep their own tokens and are set
from `ThemeManager::onChange`.

### Things outside JUCE's graphics

- `NativeBusyWindow.cpp` paints with raw Win32/GDI on its own thread: snapshot
  its few colors into plain `COLORREF`s when the window is created.
- `PluginWindow` takes `ResizableWindow::backgroundColourId` from the default
  LookAndFeel, so it follows automatically.
- Menus (`PopupMenu`) follow the LookAndFeel.

### Persistence

- Built-in themes compiled in: **Dark** (exactly today's colors, the baseline and
  fallback), later **Darker/High contrast/Light** once the token set is stable.
- User themes: `%APPDATA%\OrchestralDAW\Themes\<name>.xml`, containing *only
  overrides*:

  ```xml
  <Theme name="My theme" base="Dark" version="1">
    <Color token="surface.panel" value="ff202328"/>
    <Color token="arrange.bg" value="ff101114"/>
  </Theme>
  ```

  Storing deltas against a named base keeps themes small, lets new tokens appear
  in later versions without breaking old files (they fall back to the base), and
  makes the file diffable. Unknown token names are ignored with a log line (so a
  theme from a newer version degrades gracefully); a missing base falls back to Dark.
- `Settings.xml` stores only `activeTheme` (name). `UserData.h` gets
  `getThemesDir()`.
- Project files do **not** embed the theme (it is a user preference, not music).

## Command-first API (DESIGN.md principle)

New commands, documented in API.md and visible through `describe`:

- `theme.list` - built-in + user themes, which is active
- `theme.get` - active theme: every token with `{ value, overridden, parent }`
- `theme.set` - `{ token, color }` override one token (live, unsaved "working copy")
- `theme.reset` - `{ token }` back to inherited
- `theme.use` - `{ name }` switch theme
- `theme.save` - `{ name }` write the working copy to disk
- `theme.delete` - user themes only

Errors follow "helpful failure": `unknown token 'arrange.backgroud', did you mean:
arrange.bg, arrange.lane.even.bg`. Tests go through the API like the others.
This also lets an agent or script generate a whole palette.

## Settings > Theming UI

Existing opacity slider stays at the top ("Track colors" section).

- **Theme picker**: dropdown of themes, buttons *Save as...*, *Delete*, *Revert*,
  *Import/Export* (copy the XML).
- **Token editor**: grouped, collapsible tree (Common / Arrange view / Piano roll /
  Track list / Timeline / Top bar / ...), one row per token: label, swatch,
  hex field, an "inherits from X" hint while unset, and a reset button when
  overridden. Click the swatch opens JUCE's `ColourSelector` in a callout.
- **Search/filter** box (there will be ~150 tokens).
- **Live preview**: changes apply immediately to the real app behind the window
  (it is a view, not a modal), so no separate preview pane is needed.
- **"Pick from screen"** is a nice-to-have, probably not worth it.
- The picker needs alpha only for tokens flagged as translucent in the registry.

## Phases

1. **Audit + registry (no visible change).** Walk every `paint()`/`setColour`,
   fill the registry with tokens and parents so that the default theme is
   *pixel-identical* to today. Unit test: registry has no cycles, no duplicate
   names, every token resolves. This is the bulk of the thinking.
2. **Theme core.** `Theme`, `ThemeManager`, built-in Dark, settings key, repaint
   hook. Nothing reads it yet.
3. **Convert views, one per commit**: arrange, piano roll, track list, timeline,
   top bar, settings, history, instruments/channel list, busy overlay, perf panel.
   Each commit replaces literals with `theme[...]`; screenshot compare against
   before.
4. **LookAndFeel** for standard widgets; drop redundant `setColour` calls.
5. **API commands + tests** (can run in parallel with 4).
6. **Settings UI**: picker, token tree, color popup, save/delete.
7. **Extra built-in themes** and a lint step: a test (or grep in CI) that fails on
   new hard-coded `Colour (0x...)` / `Colours::` in `src/ui` outside the registry
   and track-colour code, so the system does not rot.

Phases 1-3 are the risky ones; 2 and 5 can be demoed with a single converted view
(arrange) before committing to the rest.

## Decisions to confirm

- **Granularity**: the plan gives one token per *visual element and state* (not per
  widget instance, so all arm buttons share `tracklist.arm.on`). Is that the level
  you want, or should e.g. each topbar button be individually themable?
- **Border/background as separate tokens** (as in your note) is the default, one
  token per property. Alternative: group them as "style" objects with
  bg/border/text. Tokens are simpler to store and script; grouping is nicer in the
  UI. Plan: tokens underneath, UI groups rows by element.
- **Fonts, sizes, corner radii, border widths**: out of scope for v1 (colors only),
  but the token registry could take non-color kinds later.
- **Light theme**: some colors (e.g. `Colours::white.withAlpha` for translucent
  overlays on dark) are written assuming a dark UI. A real light theme needs
  those overlay tokens named in phase 1. Include a light theme as a goal, or
  dark-variants only?
- **Track/folder palette** (the 12 `#rrggbb` swatches): keep fixed, or make the
  palette itself a theme part?
- **Hot-reload of theme files** on disk while editing by hand: cheap with a
  `FileSystemWatcher`; worth it?

## Not doing

- Per-project themes.
- Theme-dependent layout.
- Animated/gradient styles.
