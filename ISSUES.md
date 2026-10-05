# ISSUES (bugs/feature requests)

Lightweight tracker. Claude checks this file at every prompt: small unambiguous
items get fixed and marked `- [x]` with a note; ambiguous ones get a question;
big ones become milestones (tracked in MILESTONES.md). Add new items as `- [ ]`
under a heading. Unsolved items go at the top of each chapter; solved ones are
moved to file "ISSUES_CLOSED.md", but headers in ISSUES.md always stay. If
Claude is unsure of an issue they will pose a question.

# Test builds

# Global

# Top bar

# Settings Window

- [ ] New category: Theming. Selectable themes. Colors for certain areas, buttons, etc. Ability to save custom themes. (Milestone "Theming" in MILESTONES.md; plan in THEMING.md.)

# Instruments


# Timeline bar

# Sidebar

# Integrations

- [ ] Configuration views: a MIDI track view (port and channel, greyed out when
      immutable) and an instrument view (expression map selection). (Milestone
      "MIDI track and instrument configuration views" in MILESTONES.md.)


# Midi record

- [ ] New mode for record: punch in/out - needs fine-grained settings, not
      implemented yet; placeholder noted in DESIGN.md

# Arrange view

- [ ] Draw regions for folders that span the content inside them.
      (small now that folder rows exist as lanes on the shared Y axis)
- [ ] Double clicking a folder region will load the editor and show the combined midi.
      (Milestone-sized: multi-channel editing. Pairs with the editor's
      channel dropdown below.)

# Midi editor

- [ ] Change pointer to a pen when in draw mode
- [ ] Show midi for multiple tracks. Midi for the selected channel is normal, while other channel midi is greyed out.
- [ ] A dropdown in the top editor bar to select which channel to edit. It will only show the selected midi channels.
- [ ] A button for enabling editing on multi-channel (for example copy paste)
- [ ] CC and note velocity, aftertouch at the bottom. Velocity is default. More
      lanes can be added on top of each other. Requires controls below the piano
      keys. (Milestone-sized - also captured as "Stacked editor lanes" in
      MILESTONES.md; say the word if it should come sooner.)
- [ ] An articulation dropdown in the top editor bar, from the instrument's expression
      map. (Milestone "Articulation / expression maps" in MILESTONES.md.)
