#pragma once

#include "../AudioEngine.h"
#include "../model/PhraseBlocks.h"
#include "TimeAxis.h"
#include "SidebarMetrics.h"

class CommandDispatcher;

// The Midi-domain default view: lanes on the SAME Y axis as the track list
// (same row heights and order, folder rows included) sharing its vertical
// scroll, showing computed phrase blocks (meta-regions, see DESIGN.md), marker
// lines and the playhead. Time lives in the TimelineBar above (shared TimeAxis).
//
// Interactions:
//   drag a block              move it (snaps to bars; clip.moveRange)
//   ctrl+drag a block         copy it (clip.copyRange)
//   right-click a block       loop / repeat / open in editor / erase
//   double-click a block      open it in the MIDI editor
//   wheel / shift+wheel       scroll rows (shared with sidebar) / time
//   ctrl+wheel                zoom time
class ArrangementView final : public juce::Component,
                              private juce::Timer
{
public:
    ArrangementView (AudioEngine&, CommandDispatcher&, TimeAxis&, sidebar::VerticalScroll&);
    ~ArrangementView() override;

    std::function<void (AudioEngine::TrackId)> onOpenEditor, onSelectTrack;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void paintOverChildren (juce::Graphics&) override;   // the selection rectangle

private:
    struct BlockRef
    {
        AudioEngine::TrackId trackId = 0;
        juce::int64 startTick = 0, endTick = 0;

        bool valid() const noexcept { return trackId != 0 && endTick > startTick; }
        bool operator== (const BlockRef& other) const noexcept { return trackId == other.trackId && startTick == other.startTick; }
    };

    using Items = std::vector<AudioEngine::SidebarItem>;

    Items itemsNow() const                   { return engine.getSidebarItems (true, true); }
    static int contentHeight (const Items&);
    int rowTop (const Items&, size_t index) const;        // view-local y (scroll applied)
    int itemIndexAt (const Items&, int y) const;          // -1 when below all rows

    juce::int64 xToTick (int x) const        { return axis.xToTick (x); }
    int tickToX (juce::int64 tick) const     { return axis.tickToX (tick); }
    juce::int64 nearestBar (juce::int64 tick) const;

    const std::vector<PhraseBlock>& blocksFor (AudioEngine::TrackId);
    BlockRef blockAt (juce::Point<int>);
    juce::Rectangle<int> blockRect (const BlockRef&, int laneTop) const;

    void runCommand (const juce::String& cmd, juce::DynamicObject::Ptr params);
    void showBlockMenu (const BlockRef&);

    void timerCallback() override;

    AudioEngine& engine;
    CommandDispatcher& dispatcher;
    TimeAxis& axis;
    sidebar::VerticalScroll& vscroll;

    // View state (time scroll/zoom live in the shared axis; vertical in vscroll)
    juce::int64 lastPlayheadTick = -1;
    int lastAxisRevision = -1, lastEngineRevision = -1, lastVScrollRevision = -1;

    // Block cache per track (recomputed when the sequence pointer changes)
    struct CacheEntry
    {
        MidiSequence::Ptr sequence;
        TempoMap::Ptr map;
        std::vector<PhraseBlock> blocks;
    };

    std::map<AudioEngine::TrackId, CacheEntry> cache;

    // Interaction state. A press on a selected block moves the selection; anywhere
    // else draws a selection rectangle, which selects what it touches on release
    // (Ctrl adds to the selection).
    bool isSelected (const BlockRef& block) const   { return std::find (selection.begin(), selection.end(), block) != selection.end(); }
    std::vector<BlockRef> blocksTouching (juce::Rectangle<int>);
    void moveSelection();

    std::vector<BlockRef> selection;
    BlockRef dragging;                     // the pressed block of a move (its snap and its track lead)
    bool marquee = false, marqueeAdds = false;
    juce::Point<int> dragNow;
    juce::Point<int> dragStart;
    juce::int64 dragDeltaTicks = 0;
    AudioEngine::TrackId dragTargetTrack = 0;   // the track under the mouse (up/down moves to it)
    bool dragIsCopy = false, didDrag = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangementView)
};
