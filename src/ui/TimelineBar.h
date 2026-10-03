#pragma once

#include "../AudioEngine.h"
#include "TimeAxis.h"

class CommandDispatcher;

// The timeline bar (MILESTONES.md): sits under the topbar, above the content views,
// and owns the shared time axis. Rows, top to bottom: time (h:m:s per visible bar,
// computed from tempo and signature), tempo track, time-signature track, markers,
// bars; bar lines run the full height. A readout panel on the right shows bars.beats
// and h:mm:ss:ms (hours only when non-zero). Click/drag locates (beat-snapped);
// right-click manages markers; ctrl/shift wheel zooms and scrolls every timeline
// view at once.
class TimelineBar final : public juce::Component,
                          private juce::Timer
{
public:
    TimelineBar (AudioEngine&, CommandDispatcher&, TimeAxis&);
    ~TimelineBar() override;

    static constexpr int barHeight = 76;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    juce::Rectangle<int> lanesArea() const;    // everything left of the readout panel
    juce::int64 nearestBeat (juce::int64 tick) const;
    juce::int64 nearestBar (juce::int64 tick) const;

    void runCommand (const juce::String& cmd, juce::DynamicObject::Ptr params);
    void locateAt (int x);
    void showContextMenu (juce::int64 tick);
    void promptForMarker (juce::int64 tick, const juce::String& existingName);

    void timerCallback() override;

    AudioEngine& engine;
    CommandDispatcher& dispatcher;
    TimeAxis& axis;

    juce::int64 lastPlayheadTick = -1;
    int lastAxisRevision = -1, lastEngineRevision = -1;

    static constexpr int readoutWidth = 148;
    static constexpr int timeRow = 0, tempoRow = 15, sigRow = 30, markerRow = 45, barRow = 60;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineBar)
};
