# ISSUES_CLOSED (solved items from ISSUES.md)

Solved items move here from ISSUES.md, keeping their chapter headings and
resolution notes. Newest additions go at the top of each chapter.

# Test builds

- [x] Only test the things that were edited. Do not play audio if no audio
      related code was changed
      (test runner has --quiet which skips the audible tests; Claude runs the
      full audible suite only when audio-path code changed)

# Global

- [x] Help text when hovering over buttons for a certain time. Key commands,
      what the thing does, how it can be controlled.
      (the app never had a TooltipWindow, so the tooltips that were already
      set were invisible - added one (700 ms hover); topbar view buttons and
      editor undo/redo gained tooltips with their key commands)
- [x] All gui elements that affect other gui elements need an update mechanism.
      (systematic now: every engine mutation bumps a state revision counter
      via emitEvent - the same channel that feeds the API stream, history and
      the dirty flag. Each view's 30 Hz timer compares that one number and
      repaints; no more hand-picking which state to watch. Topbar bpm/loop
      follow it too.)
- [x] history UI. Button for it at the top bar. Show all actions there. Ability
      to time-travel backwards to an earlier edit.
      (History button in the topbar opens the view: every action as an entry
      with time/description/category, filter by category or selected track,
      click an entry to time-travel back and forward; also history.list and
      history.travel commands for agents. One gesture = one entry. Limits: the
      plugin rack itself is not rewound, and project load/new reset the
      timeline.)
- [x] Closing the app should ask if wanting to save first
      (asks only when there are unsaved changes: Save / Discard / Cancel; the
      untouched startup state doesn't count as unsaved)

# Top bar

- [x] Resizing to smallest possible width makes the Perf button disappear.
      (the hide threshold had almost no slack at the minimum width; the
      minimum is now 890 with margin, so Perf stays at the smallest size and
      hiding remains only a last-resort fallback)
- [x] Make the top bar have four distinct groups: hamburger on the very left,
      window buttons aligned left, transport section, right side buttons.
      (separator lines now divide hamburger | view buttons | transport unit |
      Perf; the transport keeps its centered panel)

# Settings Window

- [x] controller or key commands for specific articulations
      (phase 1 of milestone "Articulation remote control": an articulation has
      an optional key command and an optional MIDI trigger, set in the map
      editor (key capture, MIDI Learn); Settings > Audio & MIDI adds MIDI
      inputs as MIDI controllers - control surfaces whose MIDI never reaches
      tracks or recording. A trigger chooses the articulation as the panel
      does (selected notes, or new notes) and switches the instrument live.
      Still to come: recorded notes carry the articulation in effect.)
- [x] Use a tab system for displaying the different options. On the left, a column with all the categories (Audio, Midi, etc), on the right, the actual settings
      (category column on the left - Audio & MIDI, Plugins, Tracks,
      Agents (MCP), Key commands - with the selected category's settings on
      the right; no more one long scroll page; the selected category
      persists across sessions)

- [x] Make the transport controls its own unit in the top bar. Colorize the buttons. Add the current point in time there and remove it from the right side.
      (rounded panel perfectly centered in the window; shrinking the window
      keeps it centered until it meets the view buttons, and the minimum
      window width is exactly where all topbar controls sit next to each
      other; Play/Rec/Loop tinted green/red/blue; bars.beats + h:mm:ss:ms
      readout and tempo live in the unit; the timeline bar's right-side
      readout panel is gone)
- [x] Make the Main menu a hamburger menu
      (the Menu button shows the hamburger glyph, tooltip "Main menu")
- [x] Clicking on UI buttons toggles that UI between the UI and arrange mode.
      (Instruments and History toggle back to the current domain's arrange view
      when clicked while open; Escape still works too)

# Instruments

- [x] Need a way to delete instruments. Ask to remove connected midi channels, if any.
      (Instruments view: a Remove button per instrument. When tracks play it,
      a prompt lists them and offers "Remove the tracks too" (tracks that play
      only this instrument go; tracks with other outputs just lose this one)
      or "Keep the tracks" (they stay, without that output). Its plugin window
      closes. API: instrument.remove {instrumentId, removeTracks?}.)

# Transport

- [x] Add button for transport mode. The two stopping choices are "return to starting position" and "stop at current time". Reword those to tooltips and make the button something minimal.
  - Resolved: a small toggle button (an arrow) next to Loop. Lit = return to starting position (Stop goes back to where playback started), off = stop at current time; the tooltip explains both. Return is the default; the choice is saved in settings ("transportReturnOnStop").

# Timeline bar

- [x] Right-click menu for enabling, disabling the different rows. At the bottom of the menu, add "Preferences", which opens a sub-UI for the global "Settings".
      (right-click anywhere in the bar: marker items when on the timeline,
      then Show Bars/Time/Tempo/Time signature/Markers toggles, then
      "Preferences..." opening Settings; visibility persists in settings and
      the bar's height adapts to the visible rows)
- [x] Move bar to the top by default.
      (the bars row is now the top row: Bars, Time, Tempo, Time signature,
      Markers)
- [x] Update UI when edited. Time is not updated when editing tempo.
      (fixed by the global state-revision mechanism: tempo edits now redraw
      the time row, and tempo set over the API updates the topbar bpm label)
- [x] Time [h:m:s] per visible bar should be the top row in the timeline bar. This means we calculate time for every bar according tempo and signature.
      (computed per bar via TempoMap::ticksToSeconds, so tempo and signature
      changes are respected; hours shown only when non-zero; labels that would
      overlap at far zoom are skipped)
- [x] Order should be (the bar lines go all the way up to the top of the timeline view):
    - Time
    - Tempo
    - Time signature
    - Marker
    - bars
      (rows reordered exactly so; bar lines now run the full bar height)

# Sidebar

- [x] Selecting a folder should unselect any channels
      (selecting a folder clears the track multi-selection and removes the
      track highlight)
- [x] Selecting multiple channels should put them all in record mode if we
      have the auto-record settings on
      (the engine has an armed SET now: Ctrl/Shift-selecting arms every
      selected track; live input plays through all of them and a take records
      into all of them, each by its own Add/Replace mode - replace targets
      also keep notes committed on loop passes. Ctrl-click keeps the current
      track as the first group member. Agents: track.arm takes trackIds;
      track.list reports armed + primaryArmed. Covered by a new recording
      test with one Add and one Replace track.)
- [x] Remove the scrollbar. Scrolling is unaffected.
      (both sidebar lists hide the scrollbar; the wheel still scrolls)
- [x] Switching tracks took over a second in a big project (reported in chat,
      1032-track VE Pro sync).
      (arming used to rewire live MIDI in the graph, and any connection change
      rebuilds JUCE's render sequence - >1 s with ~2100 nodes. Live input is
      now wired to every track's source permanently and gated by an atomic
      arm flag, so selecting is 1.3 s -> 0.01 s. Arming also no longer
      creates history entries. New end-to-end test proves live input reaches
      the armed track only.)
- [x] Selecting a folder should not select all the channels inside it
      (removed: clicking a folder row only highlights the folder)
- [x] Collapse/expand all folders in right click menu. Shows only when right
      clicking folders and collapses/expands all its subfolders.
      (folder menu: "Collapse all (with subfolders)" / "Expand all (with
      subfolders)")
- [x] Collapsing folders should only be done from the arrows. Most of the
      folder area is for selecting the folder.
      (the arrow toggles collapse; clicking elsewhere on the folder row selects
      the folder - highlighted - and every track inside it, collapsed
      subfolders included, ready for multi-channel work)
- [x] Collapsing folders is very slow when there are a lot of visible tracks.
      (the track list is virtualized: only rows near the visible area get
      components - ~30 instead of 1000+ - so collapse/expand and every UI tick
      cost the same in any project size; the per-frame folder-tree walk is
      one pass instead of a rescan of all tracks per folder. Removes the
      constant ~300 ms UI stalls the log showed with 1032 tracks.)
- [x] Uncolored tracks should be grey
      (tracks and folders without a color draw a grey left border)
- [x] Remove the yellow lines that show where expanded folders begin and end.
      Track side colors do this already.
      (gold indent guides removed from both sidebars; the gold folder text
      and triangle went neutral too)
- [x] Folder name text should be about the same color as the tracks. The font
      size, and/or the font puts them apart visually.
      (folder labels use the same text color as tracks; bold 13 px vs the
      tracks' regular 14 px keeps them distinct)
- [x] Colored tracks (and regions): selectable colors in right-click menu; the
      color shown only as a left border; regions get colored borders (all
      around); a slide control for opacity in settings/theming with visible
      examples next to it.
      (tracks and folders carry a '#rrggbb' color: right-click > Color offers
      a 12-swatch palette + None; sidebar rows show it as a 4 px left border;
      arrange regions draw it as the all-around border; Settings > Theming
      has the opacity slider with a live mock track row and region beside it.
      Persisted, in history snapshots, and track.setColor/folder.setColor +
      color in track.list/folder.list for agents.)
- [x] Remove Add track/folder (not needed anymore)
      (both sidebars' header buttons removed; right-click menus cover adding)
- [x] When Rec is disabled (for example when selecting another channel) any playing notes on that channel should be stopped.
      (un-arming releases held notes: the previously armed track's routes get
      a kill request and emit note-offs on the next block, so switching
      channels mid-note never leaves notes ringing)
- [x] Dragging a group of selected channels is re-selecting the selected channel that is clicked on. Re-selecting should only work on channels that are not selected.
      (clicking an already-selected row no longer re-selects on mouse-down;
      it keeps the group for the drag and only becomes the single selection
      on mouse-up when no drag happened - both sidebars)
- [x] right menu everywhere in the right panel. Contextualize.
      (right-clicking the empty sidebar area offers Add track / Add folder
      in the track list, Add folder in the audio list - channels themselves
      come from instruments)
  - [x] Option to add a channel anywhere. If right-clicking on a channel, add the new channel after. If clicking on a folder, add it inside the folder.
        (track row menu gains "New track below" - the new track lands right
        after it among its siblings; folder menu gains "New track inside";
        both select the new track and ask for its output as usual)
- [x] Drag channels to re-order them. Select multiple channels by holding Shift or Ctrl (the usual functionality) and drag those together. Multiple will be put in order (as one group) once moved out of the current position. Moving only happens when mouse moves outside of the channel being dragged, and move is complete only after dropping.
      (both sidebars: explicit ordering model in the engine; Ctrl toggles,
      Shift range-selects; dragging any selected row moves the group in
      visual order; the drag arms only once the mouse leaves the pressed row
      and commits on drop - a gold line shows the insertion point. One drop
      = one history entry; order persists in the project and the arrangement
      lanes follow it. sidebar.move command for agents.)
- [x] Drag tracks/channels/folders onto a folder row to move them into it
      (drop on a folder row's middle - the row highlights - to drop inside;
      the row's top/bottom edges insert before/after the folder instead.
      Folders can be dragged too; into-own-subtree is rejected.)
- [x] Folders for grouping channels, like in Cubase; nested; half channel
      height; indented area on the left shows membership
      (both sidebars: "+ Folder" button, collapse/expand on click, right-click
      menus to move/rename/remove, gold indent guides; arrangement lane order
      follows the tree and collapsed folders hide their lanes; folder.* API
      commands; saved in the project and covered by history time-travel)

# Integrations

- [x] Can we find out about a vsl players keyboard range? If so, the midi editor should show which keys are disabled.
      (Not in the VST parameters: all 2369 were searched. It is available live
      from the server. channel/instrument/state/export returns the Synchron
      Player state, whose core is zstd-compressed JSON with rangeFrom/rangeTo
      per articulation node. Done as the union range: when the editor opens a
      synced Synchron track, vepro.keyRange fetches it in the background once
      and caches it in the project. Keys and rows outside the range are greyed
      out. Sync now records each player's server channel and plugin; tracks
      synced before this change need one re-sync. Synchron Player only, since
      Pianos and third-party players don't expose ranges. Per-articulation
      ranges come with expression maps. The decoder is the vendored zstd 1.5.6
      reference decoder (BSD, decompression only, external/zstd).)
- [x] Syncing the huge VE Pro project still slow (chat, 2026-10-04; 20
      instances / 1032 players).
      (measured by phase: each plugin load waited behind an async rebuild of
      the growing graph - 61 s of a 65 s sync. Graph edits during a sync or
      project load now defer all rebuilding to ONE rebuild at the end
      (JUCE UpdateKind::none batch), instrument loads included; the server
      fetch runs 8 instances in parallel (2.0 -> 0.67 s, results identical
      across runs); two quadratic per-track steps removed. Sync: 65 s -> 6 s.
      The reply now reports timing per phase.)
- [x] A loading overlay showing why the app isn't responding (chat request).
      (dims the window, blocks clicks, shows the operation, a detail line
      ("Instance 3 of 20: Stu WW (54 players)") and a progress bar; painted
      immediately on each step since the message thread is busy; used by
      VE Pro sync and project loads)

- [x] Syncing should fetch colors from the server. Use the instance colors for
      instance folders and player colors for midi tracks.
      (instance colors come with instance/list, player colors via
      channel/color/get per player; applied when the folder/track is CREATED
      only, so later color edits in the app are never overwritten by re-sync)
- [x] When syncing create renamable folders: one for VE Pro Server, and inside
      it, one folder for each instance; inside those, the midi channels whose
      names are immutable. Folders stay freely renamable and movable.
      (first sync of an instance creates "VE Pro Server" > <instance> and puts
      the player tracks inside; later syncs place NEW tracks next to the
      instrument's existing ones - wherever the user moved them - so
      re-organisation and renames are never fought. Only the synced channel
      NAMES are immutable; folders and tracks are ordinary.)

# Arrange view

- [x] Draw regions for folders that span the content inside them.
  - Resolved: a folder's lane shows one region per stretch of content in its tracks (subfolders too, collapsed or not), in the folder's colour.
- [x] Double clicking a folder region will load the editor and show the combined midi.
  - Resolved: the editor opens on all the folder's tracks (the top one edited, the others dimmed; the dropdown and the All toggle switch between them).
- [x] The entire region inherits the track color. The borders are more
      pronounced and colorful, while the box itself is brighter and less
      colorful.
      (region fill = the track color desaturated and brightened, border =
      the full color at the Theming opacity; selection brightens the fill
      and whitens the border; uncolored tracks render grey)
- [x] Region height and Y-coordinate should follow channels.
      (the arrangement lanes now sit on the SAME Y axis as the track list:
      same row order and heights, folder rows included as bands)
- [x] Scrolling should sync with sidebar
      (one shared vertical scroll: wheel in either the track list or the
      arrangement moves both; in the editor, the wheel scrolls whatever it
      is over - piano roll inside the editor, the channel list over the
      sidebar. The sidebar lists also start below the timeline bar now, so
      rows align pixel-exactly.)

# Midi record

- [x] Ctrl-Z, Shift-Ctrl-Z works on recorded midi (not only inside editor)
      (global shortcut acts on the selected track's clip history; Ctrl-Y too)
- [x] When recording with replace, no midi data is played from that track during
      the take (live monitoring unaffected; semantics updated in DESIGN.md)

## Midi editor

- [x] Note input mode. Note length is controlled with keys 1-9. 0 is pause. 1 - whole note, 2 - half note, etc.
      (the MIDI editor's Input toggle (off by default): notes played on the MIDI
      keyboard are written at the playhead, chords within 60 ms; with Input on,
      keys 1-9 set the note length (1/1 ... 1/256 - the length box got 1/64,
      1/128, 1/256), 0 moves the playhead on by the length (a rest). Plain
      digits only, so key commands with modifiers still work.)
- [x] Change pointer to a pen when in draw mode
      (a pencil cursor drawn in code, its tip at the hotspot, over the grid in
      Draw mode; the resize cursor still wins at a note's right edge.)
- [x] When hovering over the grid highlight the hovered key in the piano roll
      (the key under the pointer is lit on the keyboard, its row faintly across
      the grid; over the keys column too; cleared when the pointer leaves.)
- [x] Articulations: show available modifiers on the right side of a selected articulation, ordered by group
      (the articulation button opens columns, one per group in map order -
      Color | Main | Legato | Release | Tempo - each with what is offered for
      the current choice; choosing doesn't close it, the columns follow.
      ui/ArticulationPanel.h.)
- [x] Articulations: do not show any modifiers until having selected a prerequisite
      (a modifier is offered only when a sound slot has it together with
      exactly the choices already made in the groups before it: Tempo appears
      after Rep., the legato types after Long; with only a colour chosen, only
      Main is offered. ExpressionMap::offers.)
- [x] Add small text descriptions to editor top panel items where needed.
      (every editor toolbar item now has a hover tooltip - visible since the
      app gained a TooltipWindow; undo/redo got theirs with key commands)
- [x] Play notes when added. Toggle button for it at the top editor panel.
      ("Hear" toggle in the editor toolbar, on by default: double-click adds
      and draw-mode clicks play the note for 250 ms through the armed track's
      instrument)
- [x] Drawing notes when snap is larger than note-length should not resize the
      note before the mouse moves to the next snap point
      (draw keeps the dropdown length until the mouse crosses the next grid
      point, then grows in whole grid steps)
- [x] Drawing notes and "re-sizing" should not create two separate events in
      history; the event ends when lifting the mouse button
      (Draw mode now previews the note locally and commits once on mouse up)
- [x] Edit modes (dropdown): select (key S), draw (key D)
      (Draw: click adds a note at the length dropdown's value, dragging stretches
      it; Select: marquee/move/resize as before, double-click still adds)
- [x] Button for snap to grid, named "Snap"
      (toolbar toggle; when off, notes draw and move freely between grid lines)
- [x] Two note-length dropdowns: one for note-length (new notes), one for
      "snap to grid" (snapping + quantize share it)
- [x] Move selected notes with arrows: Up/Down a half step, Ctrl+Up/Down an
      octave, Left/Right by the snap grid

# Midi editor

- [x] CC edits are done by adding points rather then drawing. CC is either in staircase steps, ramps or "bent" ramps. The possible position of points is determined by Q and grid settings.
  - Resolved: a controller lane is a list of points; a click adds one (snapped to the zoom's grid and the notes' starts and ends when Snap is on). Between two points a step with a handle in the middle: click it for a ramp, drag it to bend the ramp, double-click it for a step again. Points drag (one also in time; several, e.g. picked by selecting notes over them, up/down together); Delete removes selected points. Playback renders ramps into messages. Old CC data loads as steps.

# Expression maps

- [x] Add new groups for dynamic lengths, trill intervals, ricochet amount
      (the DAW+ map, vsl-manager tools/daw_maps.py: Main "Trill" + group
      Interval (Half-tone, Whole tone, Minor third, Major third), Main
      "Ricochet" + Amount (a1-a4), Cresc. / Dim. + Length (Short, Medium,
      Long); each Main defaults to its first / default value.)
- [x] For harmonics, add a subgroup with modifiers for long, stacc, tremolo
      (Main "Harmonics" + its own group Harmonics: Long, Staccato, Tremolo.)
- [x] Each articulation has a symbol, a name and a description. For now, no symbols. Nothing else. Showin in "sub-columns". Width decided by the widestt content.
      (the articulation panel: in each group's column every articulation is a
      row of sub-columns - symbol (only when the group has any), name,
      description - each as wide as its widest content. The generated map has
      no symbols.)
- [x] Implement colors for sound slots
      (a slot has a colour, set in the map editor's slot details; the MIDI
      editor's top bar chooses what colours the notes: "Colour: velocity" or
      "Colour: sound slot" (a selected note keeps its colour, white outline).
      The vsl-manager generator colours slots by main articulation, the Cubase
      maps' hues.)
