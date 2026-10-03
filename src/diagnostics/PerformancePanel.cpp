#include "PerformancePanel.h"
#include "../AudioEngine.h"
#include "../UserData.h"

namespace
{
    constexpr int uiTickHz = 30;
}

PerformanceTracker::PerformanceTracker (AudioEngine& e)
    : engine (e),
      csv (UserData::getAppPerfLog(), { "audio_load_pct", "audio_peak_load_pct", "xruns", "instruments", "ui_stall_ms" })
{
    lastTickMs = lastSampleMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (uiTickHz);
}

PerformanceTracker::~PerformanceTracker()
{
    stopTimer();
}

void PerformanceTracker::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();

    // Anything much beyond the timer period means the message thread was blocked.
    maxGapMs = juce::jmax (maxGapMs, now - lastTickMs - 1000.0 / uiTickHz);
    lastTickMs = now;

    auto& dm = engine.getDeviceManager();
    peakAudioLoad = juce::jmax (peakAudioLoad, dm.getCpuUsage() * 100.0);

    if (now - lastSampleMs < 1000.0)
        return;

    lastSampleMs = now;

    Sample s;
    s.system = monitor.sample();
    s.audioLoadPercent = dm.getCpuUsage() * 100.0;
    s.peakAudioLoadPercent = peakAudioLoad;
    s.loadedInstruments = engine.getNumLoadedInstruments();
    s.maxUiStallMs = juce::jmax (0.0, maxGapMs);

    if (auto* device = dm.getCurrentAudioDevice())
        s.xruns = device->getXRunCount();

    maxGapMs = 0.0;
    peakAudioLoad = 0.0;

    latest = s;
    history.push_back (s);

    while (history.size() > historyLength)
        history.pop_front();

    csv.write (s.system, { juce::String (s.audioLoadPercent, 1), juce::String (s.peakAudioLoadPercent, 1),
                           juce::String (s.xruns), juce::String (s.loadedInstruments),
                           juce::String (juce::roundToInt (s.maxUiStallMs)) });

    if (s.maxUiStallMs > 250.0)
        juce::Logger::writeToLog ("UI stall: message thread blocked for ~" + juce::String (juce::roundToInt (s.maxUiStallMs)) + " ms");

    if (onNewSample)
        onNewSample();
}

//==============================================================================
PerformancePanel::PerformancePanel (PerformanceTracker& t) : tracker (t)
{
    tracker.onNewSample = [this] { repaint(); };
}

PerformancePanel::~PerformancePanel()
{
    tracker.onNewSample = nullptr;
}

void PerformancePanel::drawGraph (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& title,
                                  std::function<double (const PerformanceTracker::Sample&)> value,
                                  double maxValue, juce::Colour colour) const
{
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (area, 3.0f);

    const auto& history = tracker.getHistory();
    const auto plot = area.reduced (4.0f, 16.0f).withTrimmedBottom (-12.0f);

    if (history.size() >= 2)
    {
        juce::Path path;
        const auto step = plot.getWidth() / (float) (PerformanceTracker::historyLength - 1);
        auto x = plot.getRight() - step * (float) (history.size() - 1);

        for (size_t i = 0; i < history.size(); ++i, x += step)
        {
            const auto proportion = (float) juce::jlimit (0.0, 1.0, value (history[i]) / maxValue);
            const auto y = plot.getBottom() - proportion * plot.getHeight();

            if (i == 0) path.startNewSubPath (x, y);
            else        path.lineTo (x, y);
        }

        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (1.5f));
    }

    g.setColour (juce::Colours::lightgrey);
    g.setFont (12.0f);
    const auto current = history.empty() ? 0.0 : value (history.back());
    g.drawText (title + ": " + juce::String (current, 1), area.reduced (6.0f, 2.0f).removeFromTop (14.0f),
                juce::Justification::topLeft);
}

void PerformancePanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff17191c));

    const auto& s = tracker.getLatest();
    const auto& sys = s.system;
    auto area = getLocalBounds().reduced (8, 6);

    // Text column
    auto text = area.removeFromLeft (330);
    g.setFont (juce::FontOptions (13.0f));

    const juce::StringArray lines {
        "Audio load " + juce::String (s.audioLoadPercent, 1) + "%   (peak " + juce::String (juce::roundToInt (s.peakAudioLoadPercent)) + "%)",
        "Dropouts (xruns) " + (s.xruns >= 0 ? juce::String (s.xruns) : juce::String ("n/a for this driver")),
        "Instruments loaded " + juce::String (s.loadedInstruments),
        "Process CPU " + juce::String (sys.processCpuPercent, 1) + "%   system " + juce::String (juce::roundToInt (sys.systemCpuPercent)) + "%",
        "RAM " + PerformanceMonitor::formatBytes (sys.workingSetBytes)
            + "   committed " + PerformanceMonitor::formatBytes (sys.privateBytes),
        "System free RAM " + PerformanceMonitor::formatBytes (sys.systemAvailableBytes)
            + " of " + PerformanceMonitor::formatBytes (sys.systemTotalBytes),
        "Threads " + juce::String (sys.threadCount) + "   handles " + juce::String (sys.handleCount),
        "Child processes " + juce::String (sys.descendantProcessCount)
            + (sys.descendantProcessCount > 0 ? "  (" + PerformanceMonitor::formatBytes (sys.descendantWorkingSetBytes) + ")" : juce::String()),
        "UI stall " + juce::String (juce::roundToInt (s.maxUiStallMs)) + " ms"
    };

    const auto lineHeight = juce::jmin (16, text.getHeight() / lines.size());

    for (auto& line : lines)
    {
        const auto warn = (line.startsWith ("UI stall") && s.maxUiStallMs > 100.0)
                       || (line.startsWith ("Audio load") && s.peakAudioLoadPercent > 80.0);
        g.setColour (warn ? juce::Colours::orange : juce::Colours::lightgrey);
        g.drawText (line, text.removeFromTop (lineHeight), juce::Justification::centredLeft);
    }

    // Graphs
    area.removeFromLeft (8);
    const auto graphWidth = (area.getWidth() - 16) / 3;

    drawGraph (g, area.removeFromLeft (graphWidth).toFloat(), "Audio load %",
               [] (const auto& x) { return x.peakAudioLoadPercent; }, 100.0, juce::Colours::limegreen);
    area.removeFromLeft (8);
    drawGraph (g, area.removeFromLeft (graphWidth).toFloat(), "Process CPU %",
               [] (const auto& x) { return x.system.processCpuPercent; }, 100.0, juce::Colours::skyblue);
    area.removeFromLeft (8);

    const auto totalMb = (double) juce::jmax ((juce::int64) 1, sys.systemTotalBytes) / (1024.0 * 1024.0);
    drawGraph (g, area.toFloat(), "RAM MB",
               [] (const auto& x) { return (double) x.system.workingSetBytes / (1024.0 * 1024.0); },
               totalMb, juce::Colours::orange);
}
