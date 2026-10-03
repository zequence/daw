#pragma once

#include "../AudioEngine.h"
#include "../model/PhraseBlocks.h"

class CommandDispatcher;

// The Midi-domain default view: one lane per track, showing computed phrase blocks
// (meta-regions, see DESIGN.md), markers in the ruler, the loop region and playhead.
//
// Interactions:
//   drag a block              move it (snaps to bars; clip.moveRange)
//   ctrl+drag a block         copy it (clip.copyRange)
//   right-click a block       loop / repeat / open in editor / erase
//   double-click a block      open it in the MIDI editor
//   ruler click               locate (bar-snapped)   ruler right-click   marker menu
//   wheel / shift+wheel       scroll lanes / time    ctrl+wheel          zoom time
class ArrangementView final : public juce::Component,
                              private juce::Timer
{
public:
    ArrangementView (AudioEngine&, CommandDispatcher&);
    ~ArrangementView() override;

    std::function<void (AudioEngine::TrackId)> onOpenEditor, onSelectTrack;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct BlockRef
    {
        AudioEngine::TrackId trackId = 0;
        juce::int64 startTick = 0, endTick = 0;

        bool valid() const noexcept { return trackId != 0 && endTick > startTick; }
    };

    juce::Rectangle<int> rulerArea() const   { return { 0, 0, getWidth(), rulerHeight }; }
    juce::Rectangle<int> lanesArea() const   { return { 0, rulerHeight, getWidth(), getHeight() - rulerHeight }; }

    juce::int64 xToTick (int x) const        { return scrollTick + (juce::int64) juce::jmax (0.0, x * ticksPerPixel); }
    int tickToX (juce::int64 tick) const     { return (int) ((double) (tick - scrollTick) / ticksPerPixel); }
    juce::int64 nearestBar (juce::int64 tick) const;

    const std::vector<PhraseBlock>& blocksFor (AudioEngine::TrackId);
    int laneIndexAt (int y) const;
    BlockRef blockAt (juce::Point<int>);
    juce::Rectangle<int> blockRect (const BlockRef&, int laneIndex) const;

    void runCommand (const juce::String& cmd, juce::DynamicObject::Ptr params);
    void showBlockMenu (const BlockRef&);
    void showRulerMenu (juce::int64 tick);
    void promptForMarker (juce::int64 tick, const juce::String& existingName);

    void timerCallback() override;

    AudioEngine& engine;
    CommandDispatcher& dispatcher;

    // View state
    double ticksPerPixel = 32000.0;          // ~30 px per quarter note
    juce::int64 scrollTick = 0;
    int scrollLane = 0;
    juce::int64 lastPlayheadTick = -1;

    // Block cache per track (recomputed when the sequence pointer changes)
    struct CacheEntry
    {
        MidiSequence::Ptr sequence;
        TempoMap::Ptr map;
        std::vector<PhraseBlock> blocks;
    };

    std::map<AudioEngine::TrackId, CacheEntry> cache;

    // Interaction state
    BlockRef selected, dragging;
    juce::Point<int> dragStart;
    juce::int64 dragDeltaTicks = 0;
    bool dragIsCopy = false, didDrag = false;

    static constexpr int rulerHeight = 40, laneHeight = 52;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangementView)
};
