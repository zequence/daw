#include "TimelineBar.h"
#include "../api/CommandDispatcher.h"

namespace
{
    juce::String formatTime (double seconds)
    {
        const auto totalMs = (juce::int64) std::llround (juce::jmax (0.0, seconds) * 1000.0);
        const auto ms = (int) (totalMs % 1000);
        const auto s = (int) ((totalMs / 1000) % 60);
        const auto m = (int) ((totalMs / 60000) % 60);
        const auto h = (int) (totalMs / 3600000);

        auto text = juce::String (m).paddedLeft ('0', 2) + ":" + juce::String (s).paddedLeft ('0', 2)
                      + ":" + juce::String (ms).paddedLeft ('0', 3);

        return h > 0 ? juce::String (h) + ":" + text : text;
    }

    // Compact h:m:s for the per-bar time row (hours only when non-zero).
    juce::String formatBarTime (double seconds)
    {
        const auto total = (juce::int64) std::llround (juce::jmax (0.0, seconds));
        const auto s = (int) (total % 60);
        const auto m = (int) ((total / 60) % 60);
        const auto h = (int) (total / 3600);

        auto text = juce::String (m) + ":" + juce::String (s).paddedLeft ('0', 2);
        return h > 0 ? juce::String (h) + ":" + juce::String (m).paddedLeft ('0', 2)
                         + ":" + juce::String (s).paddedLeft ('0', 2)
                     : text;
    }
}

TimelineBar::TimelineBar (AudioEngine& e, CommandDispatcher& d, TimeAxis& a)
    : engine (e), dispatcher (d), axis (a)
{
    setWantsKeyboardFocus (false);
    startTimerHz (30);
}

TimelineBar::~TimelineBar() = default;

juce::Rectangle<int> TimelineBar::lanesArea() const
{
    return getLocalBounds().withTrimmedRight (readoutWidth);
}

juce::int64 TimelineBar::nearestBeat (juce::int64 tick) const
{
    const auto map = engine.getTransport().getTempoMap();
    const auto clamped = juce::jmax ((juce::int64) 0, tick);
    const auto beat = map->getTicksPerBeat (clamped);
    return ((clamped + beat / 2) / beat) * beat;
}

juce::int64 TimelineBar::nearestBar (juce::int64 tick) const
{
    const auto map = engine.getTransport().getTempoMap();
    const auto clamped = juce::jmax ((juce::int64) 0, tick);
    return map->getBarStart (clamped + map->getTicksPerBar (clamped) / 2);
}

void TimelineBar::runCommand (const juce::String& cmd, juce::DynamicObject::Ptr params)
{
    const auto reply = dispatcher.run (cmd, juce::var (params.get()));

    if (! reply.getProperty ("ok", false))
        juce::Logger::writeToLog ("Timeline: " + cmd + " failed: " + reply.getProperty ("error", {}).toString());
}

void TimelineBar::locateAt (int x)
{
    auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
    params->setProperty ("tick", nearestBeat (axis.xToTick (x)));
    runCommand ("transport.locate", params);
}

void TimelineBar::mouseDown (const juce::MouseEvent& event)
{
    if (! lanesArea().contains (event.getPosition()) || event.x < TimeAxis::gutter)
        return;

    if (event.mods.isPopupMenu())
        showContextMenu (nearestBar (axis.xToTick (event.x)));
    else
        locateAt (event.x);
}

void TimelineBar::mouseDrag (const juce::MouseEvent& event)
{
    if (! event.mods.isPopupMenu() && event.x >= TimeAxis::gutter && lanesArea().contains (event.getPosition()))
        locateAt (event.x);
}

void TimelineBar::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    axis.handleWheel (event, wheel);
}

//==============================================================================
void TimelineBar::showContextMenu (juce::int64 tick)
{
    const auto safe = juce::Component::SafePointer<TimelineBar> (this);

    // A marker near the click (within half a bar)?
    const AudioEngine::Marker* nearby = nullptr;
    const auto map = engine.getTransport().getTempoMap();
    const auto tolerance = map->getTicksPerBar (tick) / 2;

    for (auto& marker : engine.getMarkers())
        if (std::abs (marker.tick - tick) <= tolerance)
            nearby = &marker;

    juce::PopupMenu menu;

    if (nearby != nullptr)
    {
        const auto markerTick = nearby->tick;
        const auto markerName = nearby->name;

        menu.addItem ("Rename \"" + markerName + "\"...", [safe, markerTick, markerName]
        {
            if (safe != nullptr)
                safe->promptForMarker (markerTick, markerName);
        });

        menu.addItem ("Remove \"" + markerName + "\"", [safe, markerTick]
        {
            if (safe == nullptr)
                return;

            auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
            params->setProperty ("tick", markerTick);
            safe->runCommand ("marker.remove", params);
        });

        menu.addItem ("Loop part (to next marker)", [safe, markerTick]
        {
            if (safe == nullptr)
                return;

            juce::int64 partEnd = -1;

            for (auto& marker : safe->engine.getMarkers())
                if (marker.tick > markerTick && (partEnd < 0 || marker.tick < partEnd))
                    partEnd = marker.tick;

            if (partEnd < 0)
                partEnd = juce::jmax (safe->engine.getLoopEndTicks(),
                                      markerTick + safe->engine.getTransport().getTempoMap()->getTicksPerBar (markerTick));

            auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
            params->setProperty ("enabled", true);
            params->setProperty ("startTick", markerTick);
            params->setProperty ("endTick", partEnd);
            safe->runCommand ("transport.setLoop", params);
        });

        menu.addSeparator();
    }

    menu.addItem ("Add marker here...", [safe, tick] { if (safe != nullptr) safe->promptForMarker (tick, {}); });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

void TimelineBar::promptForMarker (juce::int64 tick, const juce::String& existingName)
{
    auto* window = new juce::AlertWindow (existingName.isEmpty() ? "Add marker" : "Rename marker",
                                          "Name:", juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", existingName);
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<TimelineBar> (this), window, tick] (int result)
            {
                if (safe != nullptr && result == 1)
                {
                    auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
                    params->setProperty ("tick", tick);
                    params->setProperty ("name", window->getTextEditorContents ("name"));
                    safe->runCommand ("marker.add", params);
                }
            }),
        true);
}

//==============================================================================
void TimelineBar::timerCallback()
{
    const auto playhead = engine.getTransport().getPositionTicks();

    if (playhead != lastPlayheadTick || axis.revision != lastAxisRevision)
    {
        lastPlayheadTick = playhead;
        lastAxisRevision = axis.revision;
        repaint();
    }
}

void TimelineBar::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232529));

    const auto lanes = lanesArea();
    auto& transport = engine.getTransport();
    const auto map = transport.getTempoMap();
    const auto endTick = axis.xToTick (lanes.getRight());

    // --- Row separators + gutter labels (top to bottom: time, tempo, sig, markers, bars) ---
    g.setColour (juce::Colour (0xff2e3136));

    for (auto y : { tempoRow, sigRow, markerRow, barRow })
        g.fillRect (0, y, lanes.getWidth(), 1);

    g.setColour (juce::Colours::grey.withAlpha (0.6f));
    g.setFont (juce::FontOptions (9.0f));
    g.drawText ("TIME",  2, timeRow + 2,   TimeAxis::gutter - 6, 11, juce::Justification::centredRight);
    g.drawText ("TEMPO", 2, tempoRow + 2,  TimeAxis::gutter - 6, 11, juce::Justification::centredRight);
    g.drawText ("SIG",   2, sigRow + 2,    TimeAxis::gutter - 6, 11, juce::Justification::centredRight);
    g.drawText ("MARK",  2, markerRow + 2, TimeAxis::gutter - 6, 11, juce::Justification::centredRight);
    g.drawText ("BARS",  2, barRow + 3,    TimeAxis::gutter - 6, 11, juce::Justification::centredRight);

    // --- Loop band (on the bars row) ---
    if (transport.isLooping() && transport.getLoopEnd() > transport.getLoopStart())
    {
        const auto x1 = juce::jmax (TimeAxis::gutter, axis.tickToX (transport.getLoopStart()));
        const auto x2 = axis.tickToX (transport.getLoopEnd());
        g.setColour (juce::Colours::steelblue.withAlpha (0.35f));
        g.fillRect (x1, barRow + 1, juce::jmax (2, x2 - x1), getHeight() - barRow - 1);
    }

    // --- Bars: full-height lines, numbers at the bottom, wall-clock time at the top
    //     (computed per bar from the tempo and signature timelines) ---
    auto barTick = map->getBarStart (axis.scrollTick);
    int guard = 0, lastTimeLabelRight = -1;

    while (barTick < endTick && ++guard < 3000)
    {
        const auto x = axis.tickToX (barTick);

        if (x >= TimeAxis::gutter && x < lanes.getRight())
        {
            g.setColour (juce::Colour (0xff45494f));
            g.fillRect (x, 0, 1, getHeight());

            g.setColour (juce::Colours::lightgrey);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (juce::String (map->ticksToBarsBeats (barTick).bar),
                        x + 3, barRow + 2, 44, 13, juce::Justification::left);

            // Skip time labels that would overlap the previous one.
            if (x + 2 > lastTimeLabelRight)
            {
                g.setColour (juce::Colours::grey);
                g.setFont (juce::FontOptions (10.0f));
                g.drawText (formatBarTime (map->ticksToSeconds (barTick)),
                            x + 3, timeRow + 2, 52, 12, juce::Justification::left);
                lastTimeLabelRight = x + 3 + 52;
            }
        }

        barTick += map->getTicksPerBar (barTick);
    }

    // --- Markers ---
    for (auto& marker : engine.getMarkers())
    {
        const auto x = axis.tickToX (marker.tick);

        if (x < TimeAxis::gutter - 2 || x > lanes.getRight())
            continue;

        g.setColour (juce::Colours::gold.withAlpha (0.9f));
        g.fillRect (x, markerRow, 1, getHeight() - markerRow);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText (marker.name, x + 3, markerRow + 2, 120, 12, juce::Justification::left);
    }

    // --- Tempo track ---
    g.setFont (juce::FontOptions (10.0f));

    for (auto& tempo : map->getTempoChanges())
    {
        const auto x = axis.tickToX (tempo.tick);

        if (x < TimeAxis::gutter - 2 || x > lanes.getRight())
            continue;

        g.setColour (juce::Colours::skyblue.withAlpha (0.9f));
        g.fillEllipse ((float) x - 1.5f, (float) tempoRow + 3.0f, 3.0f, 3.0f);
        g.drawText (juce::String (tempo.bpm, tempo.bpm == (int) tempo.bpm ? 0 : 1),
                    x + 4, tempoRow + 1, 48, 12, juce::Justification::left);
    }

    // --- Time signature track ---
    for (auto& meter : map->getMeterChanges())
    {
        const auto x = axis.tickToX (meter.tick);

        if (x < TimeAxis::gutter - 2 || x > lanes.getRight())
            continue;

        g.setColour (juce::Colours::mediumpurple.withAlpha (0.95f));
        g.drawText (juce::String (meter.numerator) + "/" + juce::String (meter.denominator),
                    x + 2, sigRow + 1, 44, 12, juce::Justification::left);
    }

    // --- Playhead ---
    const auto playheadX = axis.tickToX (transport.getPositionTicks());

    if (playheadX >= TimeAxis::gutter && playheadX < lanes.getRight())
    {
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.fillRect (playheadX, 0, 1, getHeight());
    }

    // --- Readout panel ---
    const auto panel = getLocalBounds().removeFromRight (readoutWidth);
    g.setColour (juce::Colour (0xff1d1f23));
    g.fillRect (panel);
    g.setColour (juce::Colour (0xff2e3136));
    g.fillRect (panel.getX(), 0, 1, getHeight());

    const auto position = map->ticksToBarsBeats (transport.getPositionTicks());
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText (juce::String (position.bar) + "." + juce::String (position.beat),
                panel.reduced (10, 4).removeFromTop (28), juce::Justification::centredLeft);

    g.setColour (juce::Colours::lightgrey);
    g.setFont (juce::FontOptions (15.0f));
    g.drawText (formatTime (transport.getPositionSeconds()),
                panel.reduced (10, 4).removeFromBottom (24), juce::Justification::centredLeft);
}
