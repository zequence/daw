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

- [ ] New category: Theming. Selectable themes. Colors for certain areas, buttons, etc. Ability to save custom themes.

# Timeline bar

# Sidebar

- [x] Remove Add track/folder (not needed anymore)
      (both sidebars' header buttons removed; right-click menus cover adding)
- [ ] Colored tracks (and regions)
  - Selectable colors in right-click menu
  - The color is shown only as a left border
  - Regions in arrange view get colored borders (all around)
  - a slide control for opacity in settings/theming with visible examples next to it


# Integrations

- [ ] Syncing should fetch colors from the server. Use the instance colors for instance folders and player colors for midi tracks.

# Midi record

- [ ] New mode for record: punch in/out - needs fine-grained settings, not
      implemented yet; placeholder noted in DESIGN.md

# Arrange view

- [ ] Region height and Y-coordinate should follow channels.
      (Milestone-sized as part of "Unified track area" in MILESTONES.md: the
      sidebar and arrange view become one unit, which solves this, the
      scroll sync and the folder spans by construction.)
- [ ] Scrolling should sync with sidebar
      (part of "Unified track area", see above)
- [ ] Draw regions for folders that span the content inside them.
      (part of "Unified track area", see above)
- [ ] Double clicking a folder in the midi UI will load the editor and show the combined midi. This also selects all midi channels inside it.
      (Milestone-sized: multi-channel editing; builds on the unified track
      area. Pairs with the editor's channel dropdown below.)

# Midi editor

- [ ] A dropdown in the top editor bar to select which channel to edit. It will only show the selected midi channels.
- [ ] CC and note velocity, aftertouch at the bottom. Velocity is default. More
      lanes can be added on top of each other. Requires controls below the piano
      keys. (Milestone-sized - also captured as "Stacked editor lanes" in
      MILESTONES.md; say the word if it should come sooner.)
