# ISSUES (bugs/feature requests)

Lightweight tracker. Claude checks this file at every prompt: small unambiguous
items get fixed and marked `- [x]` with a note; ambiguous ones get a question;
big ones become milestones. Add new items as `- [ ]` under a heading.
Unsolved items go at the top of each chapter; solved ones sink below.
If Claude is unsure of an issue they will pose a question.

# Global

- [ ] history UI. Button for it at the top bar. Show all actions there. Ability to time-travel backwards to an earlier edit.
      (MILESTONE, scoped 2026-10-03: global history showing all actions, with a
      filter to switch between scopes - all / clip edits per track / categories.
      Requires global undo built on the command layer.)
- [x] Closing the app should ask if wanting to save first
      (asks only when there are unsaved changes: Save / Discard / Cancel; the
      untouched startup state doesn't count as unsaved)

# Timeline bar

- [ ] Create timeline bar that sits under the menu, on top of the different
      non-full-window gui modes. Has:
      - time (hours:min:sec:ms) (hours only when non-zero)
      - Tempo track
      - time signature
      - markers
      - bars (already implemented)
      (MILESTONE, scoped 2026-10-03: one shared time axis - the bar owns
      scroll/zoom, arrangement and piano roll align to it and lose their own
      rulers; markers/tempo/signature displayed and later edited here.)

# Midi record

- [ ] New mode for record: punch in/out - needs fine-grained settings, not
      implemented yet; placeholder noted in DESIGN.md
- [x] Ctrl-Z, Shift-Ctrl-Z works on recorded midi (not only inside editor)
      (global shortcut acts on the selected track's clip history; Ctrl-Y too)
- [x] When recording with replace, no midi data is played from that track during
      the take (live monitoring unaffected; semantics updated in DESIGN.md)

## Midi editing

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
