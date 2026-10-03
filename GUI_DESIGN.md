# The escape button

Very important function for resetting focus in the GUI.


# Default GUI size

The default when opening the app should be maximized window. And, we probably want a smallest possible size (the current default window size is ok as minimal default for now).

This app should have no additional floating windows, except for plugin GUIs.

# Topbar

Main menu, audio, midi, instruments, transport controls.

Main menu items:
* new project
* load project
* save project
* settings

the "Instruments" button opens the instruments UI.

The Audio button opens the Audio UI.
The Midi button opens the midi UI.

# Settings

opens in the main gui container, replacing
the whole UI.
Has a close X button. Can be closed with ESC.

* Audio
    * audio driver
* Midi
    * enable/disable midi inputs
* Tracks
    * Auto-record on select
* Plugins
    * Stays on top true/false
* Key / Controller commands
    * Transport controls (midi editor gui, track region view gui)
        * space: start/stop
        * home: back to the beginning
        * end: to the end of audio/midi data

# Instruments

This gui opens in the content GUI container.

Shows a list of loaded instruments.
Midi tracks can send midi data to these.

Clicking on midi channels in the sidebar will focus the correct instruments at the top of the list.

Each instrument has an edit button to open a Instrument Editor ui.

## Instrument editor UI

This gui replaces the instruments UI.
Here we can see which midi channels are assigned to this instrument and we can create more.
Auto-creation is another method.
We can also name each channel from here, or use an auto-naming method (for later).

Clicking ESC or clicking the "<- back" button goes back to Instruments UI.

# Midi GUI

## Top transport data

Similar in design to what is used for ardour.
We can set time signatures, set tempo,
markers.

## Track sidebar

Tracks in a list on the left side in a container. The container can be collapsed with a button at the top.
The width of the container is resizable but defaults to about 15% of the width of the screen.

Each track has small size buttons for:

* R (Record)
* E (Midi Editor)
* S (Solo)
* M (Mute)
* I (instrument - opens the VST GUI and the particular instrument UI)

When a track is selected, the GUI on the left follows. If the midi editor is opened,
the contents of the midi editor will change
to that channel.

## content GUI container

This is on the right side of the track sidebar and takes up most of the space in the GUI.

Default view is showing track region content.
For now, we'll have a placeholder gui for that.

The main container opens different ui's depending on what we activate from top or side panels.

### Midi editor

This shows in the content GUI.
Pianoroll on the left. Grid view displaying notes as drawable rectangular boxes.

# Audio GUI

Audio channels in the left sidebar, similar to midi channels.
An audio channel has input and output. The input is either an instrument, an audio device input, or none. The output for now is just the stereo out (but in the future we will do summing as well)

Right side will show audio wave regions by default. Other UIs will follow (audio editing, channel settings, etc.)

# Plugin GUIs

These should stay on top by default.