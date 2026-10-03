#pragma once

#include "../AudioEngine.h"
#include "TimeAxis.h"

class CommandDispatcher;

// The timeline bar (MILESTONES.md): sits under the topbar, above the content views,
// and owns the shared time axis. Rows, top to bottom by default: bars, time
// (h:m:s per visible bar, computed from tempo and signature), tempo track,
// time-signature track, markers; bar lines run the full height. Rows can be shown/
// hidden from the right-click menu (persisted in settings), which also manages
// markers and opens Preferences. Click/drag locates (beat-snapped); ctrl/shift
// wheel zooms and scrolls every timeline view at once. The position readout lives
// in the topbar's transport unit.
class TimelineBar final : public juce::Component,
                          private juce::Timer
{
public:
    TimelineBar (AudioEngine&, CommandDispatcher&, TimeAxis&);
    ~TimelineBar() override;

    // Height depends on which rows are visible; the owner re-lays-out via onHeightChanged.
    int getPreferredHeight() const;
    std::function<void()> onHeightChanged;
    std::function<void()> onOpenSettings;   // the menu's "Preferences..."

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum class RowKind { bars, time, tempo, signature, markers };
    static constexpr RowKind rowOrder[] = { RowKind::bars, RowKind::time, RowKind::tempo,
                                            RowKind::signature, RowKind::markers };
    static constexpr int rowHeight = 15;

    static const char* settingsKeyFor (RowKind);
    static const char* nameFor (RowKind);
    bool isRowVisible (RowKind) const;
    void setRowVisible (RowKind, bool);     // persists, re-lays-out, repaints
    int rowY (RowKind) const;               // top of the row; -1 when hidden

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineBar)
};
