#pragma once

#include "../AudioEngine.h"
#include "TimeAxis.h"
#include "../model/PlayheadSteps.h"
#include "KeyCommands.h"
#include "ControllerLanes.h"
#include "../model/PhraseBlocks.h"
#include "CloseButton.h"

class CommandDispatcher;

// The MIDI editor: piano keys on the left, notes as draggable
// rectangles on a bar/beat grid, an editable lane below (velocity, any CC, or pitch
// bend), playhead on top.
//
// All edits go through the command dispatcher (clip.* commands), so the editor,
// scripts and agents perform identical operations and share one undo history.
//
// Interactions:
//   double-click empty    add a note (length dropdown)   drag note          move (snap; vertical = transpose)
//   drag empty            marquee-select                 drag note's right edge   resize
//   right-click note      delete                         Delete                   delete selection
//   Ctrl+Z / Ctrl+Y       undo / redo                    Ctrl+A                   select all
//   wheel                 scroll keys                    shift+wheel              scroll time
//   ctrl+wheel            zoom time                      (the timeline bar above locates)
//   lane drag             velocity: set values; CC/bend: draw a curve (one undo step per stroke)
//   lane right-drag       CC/bend: erase the dragged range
class PianoRollView final : public juce::Component,
                            public juce::SettableTooltipClient,
                            private juce::Timer
{
public:
    CloseButton closeButton;   // the X at the right end of the top bar (MainComponent wires it)

    PianoRollView (AudioEngine&, CommandDispatcher&, TimeAxis&);

    // Draw: click adds a note at the length dropdown's value, dragging stretches it.
    // Edit (select): drag selects, double-click adds.
    void setDrawMode (bool shouldDraw);
    bool isDrawMode() const              { return drawMode; }
    ~PianoRollView() override;

    void setTrack (AudioEngine::TrackId);   // the edited track; shows just it unless it is one of the shown
    AudioEngine::TrackId getTrack() const noexcept { return trackId; }

    // Several tracks in the editor (top to bottom): 'active' is edited, the others' notes are
    // dimmed; the dropdown switches between them
    void setTracks (std::vector<AudioEngine::TrackId> tracks, AudioEngine::TrackId active);
    const std::vector<AudioEngine::TrackId>& getTracks() const noexcept { return shownTracks; }
    std::function<void (AudioEngine::TrackId)> onEditedTrackChanged;   // the dropdown picked another track

    // Esc: selected notes are deselected first; false when nothing was selected
    bool deselectNotes()
    {
        if (! anySelected())
            return false;

        clearAllSelections();
        repaint();
        return true;
    }
    bool isShown (AudioEngine::TrackId id) const { return std::find (shownTracks.begin(), shownTracks.end(), id) != shownTracks.end(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    // Remote control (a key command or a MIDI controller's control): choose an articulation as
    // the articulation panel does - on the selected notes, or the one new notes get. Returns the
    // new-note choice afterwards (what plays live), unless notes were changed instead.
    std::optional<ExpressionMap::Selection> remoteChoose (const juce::String& group, const juce::String& name);

    // Note input's keys (1-9 note length, 0 rest) when Input is on; false when not handled
    bool noteInputKey (const juce::KeyPress&);

private:
    enum class Drag { none, marquee, move, resize, lane, draw };
    enum class LaneMode { velocity, pitchBend, aftertouch, controller };

    //==============================================================================
    // Geometry (time <-> x comes from the shared axis; keysWidth == TimeAxis::gutter)
    juce::Rectangle<int> keysArea() const;
    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> laneArea() const;          // the maximized lane's value area (where editing happens)

    // The controller lanes (Settings > Controller lanes; the track's choice, saved per track): stacked
    // under the grid, one maximized, the others minimized strips (name + a coloured line of their values)
    static constexpr int maximizedLaneHeight = 120, minimizedLaneHeight = 18;
    juce::StringArray shownLanes() const;           // the track's lanes that are available, in order
    juce::String maximizedLaneId() const;
    int lanesHeight() const;
    juce::Rectangle<int> laneRowArea (int index) const;   // full width (names in the keys column)
    int laneIndexAt (juce::Point<int>) const;             // -1 = not in the lane pane
    void showLaneMenu();                                  // right-click: which lanes the track shows
    void paintLanes (juce::Graphics&, const MidiSequence*);
    void paintMinimizedLane (juce::Graphics&, juce::Rectangle<int> strip, const lanes::Lane&, const MidiSequence*);
    juce::String laneValueAt (juce::Point<int>) const;   // the value under the mouse ("" = none)

    juce::String maximizedLane;   // id; empty = the first shown
    juce::String hoverValue;      // drawn by the mouse (always visible, no tooltip delay)
    juce::Point<int> hoverPoint;

    juce::int64 xToTick (int x) const;
    int tickToX (juce::int64 tick) const;
    int yToKey (int y) const;
    int keyToY (int key) const;
    juce::Rectangle<int> noteRect (const MidiSequence::Note&) const;
    juce::int64 snapTick (juce::int64 tick) const;
    void updateCursorAt (juce::Point<int>);   // resize edge, pen (Draw) or the normal pointer
    juce::int64 snapTicksOrZero() const;      // 0 when snapping is off (the transport's Snap button)
    juce::int64 gridTicks() const;            // the grid step at this zoom, regardless of the toggle
    void stepPlayhead (bool forward);         // Left/Right: note to note, or a grid step
    juce::int64 newNoteTicks() const;         // the note-length dropdown

    //==============================================================================
    // Lane (velocity / CC / pitch bend)
    LaneMode laneMode() const;
    int laneControllerNumber() const;
    int laneValueMax() const;                 // 127, or 16383 for pitch bend
    int laneValueFromY (int y) const;
    int laneValueToY (int value) const;
    void commitLaneGesture();

    //==============================================================================
    // Editing (all through clip.* commands)
    void runCommand (const juce::String& cmd, juce::DynamicObject::Ptr params);
    void auditionNote (int key, int velocity);        // hear a note as it's added (toolbar toggle)
    void commitNewNote (const MidiSequence::Note&);   // one clip.addNotes = one history event
    void addNoteAt (juce::int64 tick, int key);
    void deleteSelection();

    // Copy / cut / paste (Ctrl+C/X/V): the notes per track, times relative to the earliest;
    // pasting puts the earliest on the transport line. Shared by every editor in the app.
    static inline std::map<AudioEngine::TrackId, std::vector<MidiSequence::Note>> clipboard;
    void copySelection();
    void undo();
    void redo();
    void pasteAtPlayhead();
    void deleteNote (int index);
    void commitMoveOrResize();
    void commitVelocities();
    void nudgeSelection (juce::int64 tickDelta, int keyDelta);
    void reselectByValue (const std::vector<MidiSequence::Note>& wanted);

    // Articulations (the top bar's dropdown; the rules live in model/ArticulationMenu.h)
    std::vector<ExpressionMap::Selection> articulationTargets (const ExpressionMap&) const;   // the selected notes, else the one for new notes
    std::vector<int> selectedNoteIndices() const;   // the selection, only indices that still exist, ascending
    const ExpressionMap* currentMap();   // cached; nullptr when the track has no (valid) map
    void showArticulationMenu();
    void chooseArticulation (const juce::String& group, const juce::String& name);
    void commitArticulations (const std::vector<ExpressionMap::Selection>& results);
    void refreshArticulationButton();

    int noteIndexAt (juce::Point<int>, bool& onRightEdge) const;
    MidiSequence::Ptr sequence() const { return engine.getTrackSequence (trackId); }

    void timerCallback() override;

    //==============================================================================
    AudioEngine& engine;
    CommandDispatcher& dispatcher;
    TimeAxis& axis;
    AudioEngine::TrackId trackId = 0;
    MidiSequence::Ptr lastSeen;

    // View state (time scroll/zoom live in the shared axis)
    juce::int64 lastPlayheadTick = -1;
    int lastAxisRevision = -1, lastEngineRevision = -1;
    int keyHeight = 12;
    int topKey = 84;                       // highest visible key (C5)
    int hoveredKey = -1;                   // the key under the pointer (lit on the keyboard), -1 = none
    int keyboardKey = -1;                  // the key being played by clicking the keyboard, -1 = none

    void playKey (int key, int x);
    void updateHoveredKey (juce::Point<int>);
    void releaseKey();

    static juce::MouseCursor penCursor();

    // Interaction state
    Drag drag = Drag::none;
    juce::Point<int> dragStart;
    std::set<int> selection;               // indices into the current sequence's notes
    juce::int64 dragTickOffset = 0;        // move/resize preview deltas
    int dragKeyOffset = 0;
    bool dragChangedSomething = false;
    std::map<int, int> velocityPreview;    // index -> velocity during a velocity drag

    // Draw-mode note in progress (committed once, on mouse up)
    MidiSequence::Note pendingNote;

    // CC/bend lane gesture (tick -> value while drawing)
    std::map<juce::int64, int> laneGesture;
    juce::int64 gestureMinTick = -1, gestureMaxTick = -1;
    bool laneErasing = false;

    // Toolbar
    juce::TextButton auditionToggle { "Hear" }, inputToggle { "Input" };
    juce::TextButton dotButton { "." };
    int noteDots = 0;                      // 0, 1 (x1.5) or 2 (x1.75): the note length's dots
    void setNoteDots (int);
    void toggleDots (int);

    // Note input (step entry): notes played within this long after a chord's first note join it
    static constexpr double chordWindowMs = 60.0;
    juce::int64 chordTick = -1;            // where the current chord is written (-1: none yet)
    double chordStartMs = 0.0;             // when its first note arrived

    // What is edited (the dropdown in the toolbar): the track, and where its regions overlap, one
    // of them ("clip-N", the earliest by default). Another region's notes are dimmed and can't be
    // picked; new notes go into the chosen one. -1 = all (no overlaps). Later also several tracks.
    int activeRegion = -1;
    std::vector<int> targetRegions;               // the edited track's overlapping regions
    std::vector<std::pair<AudioEngine::TrackId, int>> editTargets;   // the dropdown: (track, region; -1 = whole track)
    juce::String targetsKey;                      // what the dropdown was built from
    std::vector<AudioEngine::TrackId> shownTracks;

    // Edit one (default) or all of the shown tracks. With all, notes of every shown track can be
    // selected and moved together; the other tracks stay dimmed, and clicking one of their notes
    // moves the focus there (keeping every selection). The others' selections live here.
    bool editAll = false;
    juce::TextButton editAllToggle { "All" };
    std::map<AudioEngine::TrackId, std::set<int>> otherSelections;
    void focusTrack (AudioEngine::TrackId);                                   // keeps every selection
    std::pair<AudioEngine::TrackId, int> otherNoteAt (juce::Point<int>) const;   // (0, -1) = none
    void clearAllSelections()   { selection.clear(); otherSelections.clear(); }
    bool anySelected() const
    {
        if (! selection.empty())
            return true;

        return editAll && std::any_of (otherSelections.begin(), otherSelections.end(), [] (const auto& entry) { return ! entry.second.empty(); });
    }
    // Runs 'edit' for each other shown track that has selected notes (edit all)
    void forOtherSelections (const std::function<void (AudioEngine::TrackId, const MidiSequence&, std::set<int>&)>& edit);
    static std::set<int> reselect (const MidiSequence&, const std::vector<MidiSequence::Note>& wanted);
    juce::ComboBox editTargetBox;
    void rebuildEditTargets();
    bool isEditable (const MidiSequence::Note& note) const   { return activeRegion < 0 || note.region == activeRegion; }

    // Undo/redo of a note written by note input also puts the transport line back: each
    // written note remembers the clip it produced and the line before and after it
    struct InputStep { MidiSequence::Ptr result; juce::int64 lineBefore = 0, lineAfter = 0; };
    std::vector<InputStep> inputSteps;
    void noteInput (const juce::MidiMessage&, double receivedMs);
    bool drawMode = false;   // the top bar's Draw (pen) vs Edit (select)
    juce::ComboBox lengthBox, colourBox;
    juce::TextButton quantizeButton { "Q" };
    juce::TextButton articulationButton { "Articulation" };
    ExpressionMap::Selection newNoteArticulation;   // what new notes are drawn with (nothing selected)
    juce::String articulationKey;                   // what the button was last built for
    std::optional<ExpressionMap> cachedMap;         // the track's map for painting, refreshed when the engine changes
    AudioEngine::TrackId cachedMapTrack = -1;
    int cachedMapRevision = -1;

    static constexpr int keysWidth = TimeAxis::gutter, toolbarHeight = 30;
    static constexpr juce::int64 laneDrawQuantum = Ticks::perQuarterNote / 32;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollView)
};
