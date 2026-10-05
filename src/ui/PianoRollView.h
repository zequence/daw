#pragma once

#include "../AudioEngine.h"
#include "TimeAxis.h"

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
                            private juce::Timer
{
public:
    PianoRollView (AudioEngine&, CommandDispatcher&, TimeAxis&);
    ~PianoRollView() override;

    void setTrack (AudioEngine::TrackId);
    AudioEngine::TrackId getTrack() const noexcept { return trackId; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Drag { none, marquee, move, resize, lane, draw };
    enum class LaneMode { velocity, pitchBend, controller };

    //==============================================================================
    // Geometry (time <-> x comes from the shared axis; keysWidth == TimeAxis::gutter)
    juce::Rectangle<int> keysArea() const;
    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> laneArea() const;

    juce::int64 xToTick (int x) const;
    int tickToX (juce::int64 tick) const;
    int yToKey (int y) const;
    int keyToY (int key) const;
    juce::Rectangle<int> noteRect (const MidiSequence::Note&) const;
    juce::int64 snapTick (juce::int64 tick) const;
    juce::int64 snapTicksOrZero() const;      // 0 when the Snap toggle is off
    juce::int64 gridTicks() const;            // the grid dropdown, regardless of the toggle
    juce::int64 newNoteTicks() const;         // the note-length dropdown

    //==============================================================================
    // Lane (velocity / CC / pitch bend)
    LaneMode laneMode() const;
    int laneControllerNumber() const;
    int laneValueMax() const;                 // 127, or 16383 for pitch bend
    int laneValueFromY (int y) const;
    int laneValueToY (int value) const;
    void rebuildLaneBox();
    void commitLaneGesture();

    //==============================================================================
    // Editing (all through clip.* commands)
    void runCommand (const juce::String& cmd, juce::DynamicObject::Ptr params);
    void auditionNote (int key, int velocity);        // hear a note as it's added (toolbar toggle)
    void commitNewNote (const MidiSequence::Note&);   // one clip.addNotes = one history event
    void addNoteAt (juce::int64 tick, int key);
    void deleteSelection();
    void deleteNote (int index);
    void commitMoveOrResize();
    void commitVelocities();
    void nudgeSelection (juce::int64 tickDelta, int keyDelta);
    void reselectByValue (const std::vector<MidiSequence::Note>& wanted);

    // Articulations (the top bar's dropdown; the rules live in model/ArticulationMenu.h)
    std::vector<ExpressionMap::Selection> articulationTargets (const ExpressionMap&) const;   // the selected notes, else the one for new notes
    std::vector<int> selectedNoteIndices() const;   // the selection, only indices that still exist, ascending
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
    int topKey = 84;                       // highest visible key (C6)

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
    juce::TextButton snapToggle { "Snap" }, auditionToggle { "Hear" };
    juce::ComboBox modeBox, snapBox, lengthBox, laneBox;
    juce::TextButton quantizeButton { "Q" }, undoButton { "Undo" }, redoButton { "Redo" };
    juce::TextButton articulationButton { "Articulation" };
    ExpressionMap::Selection newNoteArticulation;   // what new notes are drawn with (nothing selected)
    juce::String articulationKey;                   // what the button was last built for
    juce::Label trackLabel;
    std::vector<int> lastCcList;           // CCs currently offered by laneBox

    static constexpr int keysWidth = TimeAxis::gutter, laneHeight = 80, toolbarHeight = 30;
    static constexpr juce::int64 laneDrawQuantum = Ticks::perQuarterNote / 32;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollView)
};
