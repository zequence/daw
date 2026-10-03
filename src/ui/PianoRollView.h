#pragma once

#include "../AudioEngine.h"

class CommandDispatcher;

// The MIDI editor (GUI_DESIGN.md): piano keys on the left, notes as draggable
// rectangles on a bar/beat grid, velocity lane below, playhead on top.
//
// All edits go through the command dispatcher (clip.* commands), so the editor,
// scripts and agents perform identical operations and share one undo history.
//
// Interactions:
//   double-click empty    add a note (snap-sized)      drag note          move (snap; vertical = transpose)
//   drag empty            marquee-select               drag note's right edge   resize
//   right-click note      delete                       drag in velocity lane    set velocity (selection-aware)
//   Delete                delete selection             Ctrl+Z / Ctrl+Y          undo / redo
//   wheel                 scroll keys                  shift+wheel              scroll time
//   ctrl+wheel            zoom time                    ruler click              locate
class PianoRollView final : public juce::Component,
                            private juce::Timer
{
public:
    PianoRollView (AudioEngine&, CommandDispatcher&);
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
    enum class Drag { none, marquee, move, resize, velocity };

    //==============================================================================
    // Geometry
    juce::Rectangle<int> rulerArea() const;
    juce::Rectangle<int> keysArea() const;
    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> velocityArea() const;

    juce::int64 xToTick (int x) const;
    int tickToX (juce::int64 tick) const;
    int yToKey (int y) const;
    int keyToY (int key) const;
    juce::Rectangle<int> noteRect (const MidiSequence::Note&) const;
    juce::int64 snapTick (juce::int64 tick) const;
    juce::int64 snapTicksOrZero() const;

    //==============================================================================
    // Editing (all through clip.* commands)
    void runCommand (const juce::String& cmd, juce::DynamicObject::Ptr params);
    void addNoteAt (juce::int64 tick, int key);
    void deleteSelection();
    void deleteNote (int index);
    void commitMoveOrResize();
    void commitVelocities();
    void reselectByValue (const std::vector<MidiSequence::Note>& wanted);

    int noteIndexAt (juce::Point<int>, bool& onRightEdge) const;
    MidiSequence::Ptr sequence() const { return engine.getTrackSequence (trackId); }

    void timerCallback() override;

    //==============================================================================
    AudioEngine& engine;
    CommandDispatcher& dispatcher;
    AudioEngine::TrackId trackId = 0;
    MidiSequence::Ptr lastSeen;

    // View state
    double ticksPerPixel = 16000.0;        // ~60 px per quarter note initially
    juce::int64 scrollTick = 0;
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

    // Toolbar
    juce::ComboBox snapBox;
    juce::TextButton quantizeButton { "Q" }, undoButton { "Undo" }, redoButton { "Redo" };
    juce::Label trackLabel;

    static constexpr int rulerHeight = 26, keysWidth = 56, velocityHeight = 64, toolbarHeight = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollView)
};
