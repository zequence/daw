#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PerformanceMonitor.h"

class AudioEngine;

// Samples app performance once per second and logs it to Logs/perf-app.csv.
// Runs whether or not the panel is visible, so the log is always there for debugging.
class PerformanceTracker final : private juce::Timer
{
public:
    struct Sample
    {
        PerformanceMonitor::Snapshot system;
        double audioLoadPercent = 0.0;   // audio callback time as % of the buffer period
        double peakAudioLoadPercent = 0.0;
        int xruns = -1;                  // -1 if the driver doesn't report them
        int loadedInstruments = 0;
        double maxUiStallMs = 0.0;       // longest message-thread gap during the last second
    };

    explicit PerformanceTracker (AudioEngine&);
    ~PerformanceTracker() override;

    const Sample& getLatest() const noexcept               { return latest; }
    const std::deque<Sample>& getHistory() const noexcept  { return history; }

    std::function<void()> onNewSample;

    static constexpr size_t historyLength = 120;

private:
    void timerCallback() override;

    AudioEngine& engine;
    PerformanceMonitor monitor;
    PerformanceCsvLog csv;
    Sample latest;
    std::deque<Sample> history;

    double lastTickMs = 0.0, lastSampleMs = 0.0, maxGapMs = 0.0, peakAudioLoad = 0.0;
};

//==============================================================================
class PerformancePanel final : public juce::Component
{
public:
    explicit PerformancePanel (PerformanceTracker&);
    ~PerformancePanel() override;

    void paint (juce::Graphics&) override;

private:
    void drawGraph (juce::Graphics&, juce::Rectangle<float> area, const juce::String& title,
                    std::function<double (const PerformanceTracker::Sample&)> value,
                    double maxValue, juce::Colour colour) const;

    PerformanceTracker& tracker;
};
