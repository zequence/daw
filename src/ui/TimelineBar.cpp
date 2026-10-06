#include "TimelineBar.h"
#include "Theme.h"
#include "../api/CommandDispatcher.h"

namespace
{
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

//==============================================================================
const char* TimelineBar::settingsKeyFor (RowKind kind)
{
    switch (kind)
    {
        case RowKind::bars:      return "timelineRowBars";
        case RowKind::time:      return "timelineRowTime";
        case RowKind::tempo:     return "timelineRowTempo";
        case RowKind::signature: return "timelineRowSignature";
        case RowKind::markers:   return "timelineRowMarkers";
    }

    return "";
}

const char* TimelineBar::nameFor (RowKind kind)
{
    switch (kind)
    {
        case RowKind::bars:      return "Bars";
        case RowKind::time:      return "Time";
        case RowKind::tempo:     return "Tempo";
        case RowKind::signature: return "Time signature";
        case RowKind::markers:   return "Markers";
    }

    return "";
}

bool TimelineBar::isRowVisible (RowKind kind) const
{
    return engine.getSettingsFile().getBoolValue (settingsKeyFor (kind), true);
}

void TimelineBar::setRowVisible (RowKind kind, bool visible)
{
    engine.getSettingsFile().setValue (settingsKeyFor (kind), visible);
    engine.saveSettings();

    if (onHeightChanged)
        onHeightChanged();

    repaint();
}

int TimelineBar::rowY (RowKind kind) const
{
    int y = 0;

    for (auto row : rowOrder)
    {
        if (! isRowVisible (row))
            continue;

        if (row == kind)
            return y;

        y += rowHeight;
    }

    return -1;
}

int TimelineBar::getPreferredHeight() const
{
    int rows = 0;

    for (auto row : rowOrder)
        if (isRowVisible (row))
            ++rows;

    return juce::jmax (16, rows * rowHeight + 1);
}

//==============================================================================
juce::int64 TimelineBar::nearestBeat (juce::int64 tick) const
{
    return axis.snapToGrid (*engine.getTransport().getTempoMap(), tick);   // the zoom's grid
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
    if (event.mods.isPopupMenu())
    {
        if (event.x >= TimeAxis::gutter)
            showContextMenu (nearestBar (axis.xToTick (event.x)));
        else
            showContextMenu (-1);   // gutter: row toggles and Preferences only

        return;
    }

    if (event.x >= TimeAxis::gutter)
        locateAt (event.x);
}

void TimelineBar::mouseDrag (const juce::MouseEvent& event)
{
    if (! event.mods.isPopupMenu() && event.x >= TimeAxis::gutter)
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
    juce::PopupMenu menu;

    // --- Marker section (contextual: only for clicks on the timeline itself) ---
    if (tick >= 0)
    {
        const AudioEngine::Marker* nearby = nullptr;
        const auto map = engine.getTransport().getTempoMap();
        const auto tolerance = map->getTicksPerBar (tick) / 2;

        for (auto& marker : engine.getMarkers())
            if (std::abs (marker.tick - tick) <= tolerance)
                nearby = &marker;

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
        menu.addSeparator();
    }

    // --- Row visibility (ISSUES.md "Timeline bar") ---
    for (auto row : rowOrder)
    {
        const auto visible = isRowVisible (row);
        menu.addItem (juce::String ("Show ") + nameFor (row), true, visible,
                      [safe, row, visible] { if (safe != nullptr) safe->setRowVisible (row, ! visible); });
    }

    menu.addSeparator();
    menu.addItem ("Preferences...", [safe] { if (safe != nullptr && safe->onOpenSettings) safe->onOpenSettings(); });

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
    // Playhead, shared axis, or any engine mutation (tempo, signature, markers,
    // loop...) - the engine's state revision covers everything we display.
    const auto playhead = engine.getTransport().getPositionTicks();

    if (playhead != lastPlayheadTick || axis.revision != lastAxisRevision
         || engine.getStateRevision() != lastEngineRevision)
    {
        lastPlayheadTick = playhead;
        lastAxisRevision = axis.revision;
        lastEngineRevision = engine.getStateRevision();
        repaint();
    }
}

void TimelineBar::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232529));

    auto& transport = engine.getTransport();
    const auto map = transport.getTempoMap();
    const auto endTick = axis.xToTick (getWidth());

    const auto barsY = rowY (RowKind::bars);
    const auto timeY = rowY (RowKind::time);
    const auto tempoY = rowY (RowKind::tempo);
    const auto sigY = rowY (RowKind::signature);
    const auto markerY = rowY (RowKind::markers);

    // --- Row separators + gutter labels ---
    g.setColour (juce::Colour (0xff2e3136));

    for (auto y : { barsY, timeY, tempoY, sigY, markerY })
        if (y > 0)
            g.fillRect (0, y, getWidth(), 1);

    g.setColour (juce::Colours::grey.withAlpha (0.6f));
    g.setFont (juce::FontOptions (9.0f));

    const auto shortNameFor = [] (RowKind kind)
    {
        switch (kind)
        {
            case RowKind::bars:      return "BARS";
            case RowKind::time:      return "TIME";
            case RowKind::tempo:     return "TEMPO";
            case RowKind::signature: return "SIG";
            case RowKind::markers:   return "MARK";
        }

        return "";
    };

    for (auto row : rowOrder)
        if (const auto y = rowY (row); y >= 0)
            g.drawText (shortNameFor (row), 2, y + 2, TimeAxis::gutter - 6, 11, juce::Justification::centredRight);

    // --- Loop band (on the bars row) ---
    if (barsY >= 0 && transport.isLooping() && transport.getLoopEnd() > transport.getLoopStart())
    {
        const auto x1 = juce::jmax (TimeAxis::gutter, axis.tickToX (transport.getLoopStart()));
        const auto x2 = axis.tickToX (transport.getLoopEnd());
        g.setColour (juce::Colours::steelblue.withAlpha (0.35f));
        g.fillRect (x1, barsY + 1, juce::jmax (2, x2 - x1), rowHeight - 1);
    }

    // --- Bars: full-height lines; numbers on the bars row, wall-clock time on the
    //     time row (computed per bar from the tempo and signature timelines) ---
    auto barTick = map->getBarStart (axis.scrollTick);
    int guard = 0, lastTimeLabelRight = -1;

    while (barTick < endTick && ++guard < 3000)
    {
        const auto x = axis.tickToX (barTick);

        if (x >= TimeAxis::gutter && x < getWidth())
        {
            g.setColour (juce::Colour (0xff45494f));
            g.fillRect (x, 0, 1, getHeight());

            if (barsY >= 0)
            {
                g.setColour (juce::Colours::lightgrey);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (juce::String (map->ticksToBarsBeats (barTick).bar),
                            x + 3, barsY + 2, 44, 13, juce::Justification::left);
            }

            // Skip time labels that would overlap the previous one.
            if (timeY >= 0 && x + 2 > lastTimeLabelRight)
            {
                g.setColour (juce::Colours::grey);
                g.setFont (juce::FontOptions (10.0f));
                g.drawText (formatBarTime (map->ticksToSeconds (barTick)),
                            x + 3, timeY + 2, 52, 12, juce::Justification::left);
                lastTimeLabelRight = x + 3 + 52;
            }
        }

        barTick += map->getTicksPerBar (barTick);
    }

    // --- Markers ---
    if (markerY >= 0)
    {
        for (auto& marker : engine.getMarkers())
        {
            const auto x = axis.tickToX (marker.tick);

            if (x < TimeAxis::gutter - 2 || x > getWidth())
                continue;

            g.setColour (juce::Colours::gold.withAlpha (0.9f));
            g.fillRect (x, 0, 1, getHeight());
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (marker.name, x + 3, markerY + 2, 120, 12, juce::Justification::left);
        }
    }

    // --- Tempo track ---
    if (tempoY >= 0)
    {
        g.setFont (juce::FontOptions (10.0f));

        for (auto& tempo : map->getTempoChanges())
        {
            const auto x = axis.tickToX (tempo.tick);

            if (x < TimeAxis::gutter - 2 || x > getWidth())
                continue;

            g.setColour (juce::Colours::skyblue.withAlpha (0.9f));
            g.fillEllipse ((float) x - 1.5f, (float) tempoY + 3.0f, 3.0f, 3.0f);
            g.drawText (juce::String (tempo.bpm, tempo.bpm == (int) tempo.bpm ? 0 : 1),
                        x + 4, tempoY + 1, 48, 12, juce::Justification::left);
        }
    }

    // --- Time signature track ---
    if (sigY >= 0)
    {
        g.setFont (juce::FontOptions (10.0f));

        for (auto& meter : map->getMeterChanges())
        {
            const auto x = axis.tickToX (meter.tick);

            if (x < TimeAxis::gutter - 2 || x > getWidth())
                continue;

            g.setColour (juce::Colours::mediumpurple.withAlpha (0.95f));
            g.drawText (juce::String (meter.numerator) + "/" + juce::String (meter.denominator),
                        x + 2, sigY + 1, 44, 12, juce::Justification::left);
        }
    }

    // --- Playhead ---
    const auto playheadX = axis.tickToX (transport.getPositionTicks());

    if (playheadX >= TimeAxis::gutter && playheadX < getWidth())
    {
        g.setColour (theme::colour (theme::Token::transportLine));
        g.fillRect (playheadX, 0, 1, getHeight());
    }
}
