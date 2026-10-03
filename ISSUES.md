# ISSUES (bugs/feature requests)

Lightweight tracker. Claude checks this file at every prompt: small unambiguous
items get fixed and marked `- [x]` with a note; ambiguous ones get a question;
big ones become milestones (tracked in MILESTONES.md). Add new items as `- [ ]`
under a heading. Unsolved items go at the top of each chapter; solved ones are
moved to file "ISSUES_CLOSED.md", but headers in ISSUES.md always stay. If
Claude is unsure of an issue they will pose a question.

# Test builds

- [x] Only test the things that were edited. Do not play audio if no audio
      related code was changed
      (test runner has --quiet which skips the audible tests; Claude runs the
      full audible suite only when audio-path code changed)

# Global

- [ ] Help text when hovering over buttons for a certain time. Key commands,
      what the thing does, how it can be controlled.
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

- [ ] Make the transport controls its own unit in the top bar. Colorize the buttons. Add the current point in time there and remove it from the right side.
- [x] Make the Main menu a hamburger menu
      (the Menu button shows the hamburger glyph, tooltip "Main menu")
- [x] Clicking on UI buttons toggles that UI between the UI and arrange mode.
      (Instruments and History toggle back to the current domain's arrange view
      when clicked while open; Escape still works too)

# Settings Window

- [ ] Use a tab system for displaying the different options. On the left, a column with all the categories (Audio, Midi, etc), on the right, the actual settings
- [ ] New category: Theming. Selectable themes. Colors for certain areas, buttons, etc. Ability to save custom themes.

# Timeline bar

- [ ] Right-click menu for enabling, disabling the different rows. At the bottom of the menu, add "Preferences", which opens a sub-UI for the global "Settings".
- [ ] Move bar to the top by default.
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

- [ ] right menu everywhere in the right panel. Contextualize.
  - [ ] Option to add a channel anywhere. If right-clicking on a channel, add the new channel after. If clicking on a folder, add it inside the folder.
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

# Midi record

- [ ] New mode for record: punch in/out - needs fine-grained settings, not
      implemented yet; placeholder noted in DESIGN.md
- [x] Ctrl-Z, Shift-Ctrl-Z works on recorded midi (not only inside editor)
      (global shortcut acts on the selected track's clip history; Ctrl-Y too)
- [x] When recording with replace, no midi data is played from that track during
      the take (live monitoring unaffected; semantics updated in DESIGN.md)

## Midi editor

- [ ] Add small text descriptions to editor top panel items where needed.
- [x] Play notes when added. Toggle button for it at the top editor panel.
      ("Hear" toggle in the editor toolbar, on by default: double-click adds
      and draw-mode clicks play the note for 250 ms through the armed track's
      instrument)
- [ ] CC and note velocity, aftertouch at the bottom. Velocity is default. More
      lanes can be added on top of each other. Requires controls below the piano
      keys. (Milestone-sized - also captured as "Stacked editor lanes" in
      MILESTONES.md; say the word if it should come sooner.)
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
