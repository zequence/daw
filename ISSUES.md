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

# Integrations

- [x] Syncing to VE Pro with host 127.0.0.1 froze both the app and the VE Pro
      server.
      (three defenses added: the VSL CLI call gets a hard 15 s timeout and is
      killed if it hangs; vepro.sync refuses to run twice at once; and sync
      never force-connects to an instance the server reports as already
      connected - VSL can block inside such a connect, which is the likely
      freeze, since the killed app had left both instances flagged connected.
      Those instances are now reported in the sync notes instead: disconnect
      them on the server, then re-sync. Connect steps are logged for
      diagnosis if it ever happens again.)

# Midi record

- [ ] New mode for record: punch in/out - needs fine-grained settings, not
      implemented yet; placeholder noted in DESIGN.md

## Midi editor

- [ ] CC and note velocity, aftertouch at the bottom. Velocity is default. More
      lanes can be added on top of each other. Requires controls below the piano
      keys. (Milestone-sized - also captured as "Stacked editor lanes" in
      MILESTONES.md; say the word if it should come sooner.)
