# Key commands

Every key command in DAW+ and its default keys. All of them can be changed in
**Settings > Key commands**: click a command's keys and press the new key, use **+**
to add another key, and **↺** to go back to the default. Changed keys are saved in
the settings file.

The commands are defined in [src/ui/KeyCommands.h](src/ui/KeyCommands.h), under the
ids shown below. A test fails if a command there is missing from this file.

## Which command wins

1. **Note input** keys, while the MIDI editor shows and note input is on.
   The note input toggle (N) works whenever the MIDI editor shows.
2. **Articulation keys** from the selected track's expression map. These are set
   per map in the expression map editor, not here.
3. **MIDI editor** keys, while the MIDI editor shows.
4. **Global** keys.

A key that is typed into a text field goes to the field.

## Global

| Command | Default | Id |
|---|---|---|
| Start / stop playback | Space | `transport.playStop` |
| Back to the beginning | Home | `transport.home` |
| Close the open view / go back (deselects first in the MIDI editor and arrangement) | Esc | `view.back` |
| MIDI editor with the select pointer (toggle) | E | `view.edit` |
| MIDI editor with the pen (toggle) | D | `view.draw` |
| Performance monitor | F12 | `view.performance` |
| History pane on / off | H | `view.history` |
| Instruments pane on / off | I | `view.instruments` |
| Solo the selected track | S | `track.solo` |
| Mute the selected track | M | `track.mute` |
| Open / close the selected track's instrument GUI | G | `track.instrumentGui` |
| Undo (the editor's own, else the selected track's) | Ctrl+Z | `edit.undo` |
| Redo | Ctrl+Y, Ctrl+Shift+Z | `edit.redo` |

Esc steps back one level: the expression map editor goes back to the instrument
editor, the instrument editor to the instrument list, and other views to the
arrangement. It also closes Settings. In the MIDI editor, Esc first deselects the
selected notes and closes the editor only when nothing is selected. In the arrangement
it clears the region selection.

## MIDI editor

| Command | Default | Id |
|---|---|---|
| Note input on / off | N | `editor.noteInput` |
| Delete the selected notes (or CC points) | Delete, Backspace | `editor.delete` |
| Select all notes | Ctrl+A | `editor.selectAll` |
| Copy the selected notes | Ctrl+C | `editor.copy` |
| Cut the selected notes | Ctrl+X | `editor.cut` |
| Paste at the transport line | Ctrl+V | `editor.paste` |
| Transport line to the previous note start (else the next grid line) | Left | `editor.playheadLeft` |
| Transport line to the next note end (else the next grid line) | Right | `editor.playheadRight` |
| Move the selected notes a grid step earlier | Alt+Left | `editor.nudgeLeft` |
| Move the selected notes a grid step later | Alt+Right | `editor.nudgeRight` |
| Transpose the selected notes a half step up | Up | `editor.transposeUp` |
| Transpose the selected notes a half step down | Down | `editor.transposeDown` |
| Transpose the selected notes an octave up | Ctrl+Up | `editor.octaveUp` |
| Transpose the selected notes an octave down | Ctrl+Down | `editor.octaveDown` |

Pasted notes keep their spacing; the earliest lands on the transport line, and the
pasted notes become the selection. With "All" on, notes copied from several tracks go
back to those tracks when they are shown (else to the edited track).

With parallel notes, Left and Right stop at the closest start or end first. With no note
that way, they move to the next grid line. The grid follows the zoom: bars when zoomed
out, then half notes, quarters and so on as you zoom in. Moving notes with Alt+Left/Right
uses the same grid step.

## Note input

These work while note input is on (the editor's Input button, or N).

| Command | Default | Id |
|---|---|---|
| Note length 1/1 | 1, Numpad 1 | `input.length1` |
| Note length 1/2 | 2, Numpad 2 | `input.length2` |
| Note length 1/4 | 3, Numpad 3 | `input.length3` |
| Note length 1/8 | 4, Numpad 4 | `input.length4` |
| Note length 1/16 | 5, Numpad 5 | `input.length5` |
| Note length 1/32 | 6, Numpad 6 | `input.length6` |
| Note length 1/64 | 7, Numpad 7 | `input.length7` |
| Note length 1/128 | 8, Numpad 8 | `input.length8` |
| Note length 1/256 | 9, Numpad 9 | `input.length9` |
| Rest (move on by the note length) | 0, Numpad 0 | `input.rest` |
| Dotted note (toggle) | . | `input.dot` |
| Double-dotted note (toggle) | Shift+. | `input.doubleDot` |

Choosing a new note length removes the dot.
