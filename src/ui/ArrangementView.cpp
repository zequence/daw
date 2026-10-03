#include "ArrangementView.h"
#include "../api/CommandDispatcher.h"

ArrangementView::ArrangementView (AudioEngine& e, CommandDispatcher& d)
    : engine (e), dispatcher (d)
{
    setWantsKeyboardFocus (false);
    startTimerHz (30);
}

ArrangementView::~ArrangementView() = default;

//==============================================================================
juce::int64 ArrangementView::nearestBar (juce::int64 tick) const
{
    const auto map = engine.getTransport().getTempoMap();
    const auto clamped = juce::jmax ((juce::int64) 0, tick);
    return map->getBarStart (clamped + map->getTicksPerBar (clamped) / 2);
}

const std::vector<PhraseBlock>& ArrangementView::blocksFor (AudioEngine::TrackId trackId)
{
    auto& entry = cache[trackId];
    const auto sequence = engine.getTrackSequence (trackId);
    const auto map = engine.getTransport().getTempoMap();

    if (entry.sequence != sequence || entry.map != map)
    {
        entry.sequence = sequence;
        entry.map = map;
        entry.blocks = sequence != nullptr ? computePhraseBlocks (*sequence, *map) : std::vector<PhraseBlock>();
    }

    return entry.blocks;
}

int ArrangementView::laneIndexAt (int y) const
{
    if (y < rulerHeight)
        return -1;

    return scrollLane + (y - rulerHeight) / laneHeight;
}

juce::Rectangle<int> ArrangementView::blockRect (const BlockRef& block, int laneIndex) const
{
    const auto x = tickToX (block.startTick);
    const auto right = tickToX (block.endTick);
    const auto y = rulerHeight + (laneIndex - scrollLane) * laneHeight;
    return { x, y + 6, juce::jmax (8, right - x), laneHeight - 12 };
}

ArrangementView::BlockRef ArrangementView::blockAt (juce::Point<int> position)
{
    const auto trackIds = engine.getTrackIds();
    const auto lane = laneIndexAt (position.y);

    if (lane < 0 || lane >= (int) trackIds.size())
        return {};

    const auto trackId = trackIds[(size_t) lane];
    const auto tick = xToTick (position.x);

    for (auto& block : blocksFor (trackId))
        if (tick >= block.startTick && tick < block.endTick)
            return { trackId, block.startTick, block.endTick };

    return {};
}

//==============================================================================
void ArrangementView::runCommand (const juce::String& cmd, juce::DynamicObject::Ptr params)
{
    const auto reply = dispatcher.run (cmd, juce::var (params.get()));

    if (! reply.getProperty ("ok", false))
        juce::Logger::writeToLog ("Arrangement: " + cmd + " failed: "
                                  + reply.getProperty ("error", {}).toString());
}

void ArrangementView::mouseDown (const juce::MouseEvent& event)
{
    const auto position = event.getPosition();
    dragStart = position;
    dragDeltaTicks = 0;
    didDrag = false;
    dragging = {};

    if (rulerArea().contains (position))
    {
        const auto tick = nearestBar (xToTick (position.x));

        if (event.mods.isPopupMenu())
            showRulerMenu (tick);
        else
        {
            auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
            params->setProperty ("tick", tick);
            runCommand ("transport.locate", params);
        }

        return;
    }

    const auto hit = blockAt (position);

    if (event.mods.isPopupMenu())
    {
        if (hit.valid())
        {
            selected = hit;
            showBlockMenu (hit);
        }

        return;
    }

    selected = hit;

    if (hit.valid())
    {
        dragging = hit;
        dragIsCopy = event.mods.isCtrlDown();

        if (onSelectTrack)
            onSelectTrack (hit.trackId);
    }
    else
    {
        const auto lane = laneIndexAt (position.y);
        const auto trackIds = engine.getTrackIds();

        if (lane >= 0 && lane < (int) trackIds.size() && onSelectTrack)
            onSelectTrack (trackIds[(size_t) lane]);
    }

    repaint();
}

void ArrangementView::mouseDrag (const juce::MouseEvent& event)
{
    if (! dragging.valid())
        return;

    const auto rawDelta = (juce::int64) ((event.x - dragStart.x) * ticksPerPixel);
    const auto target = nearestBar (dragging.startTick + rawDelta);
    dragDeltaTicks = target - dragging.startTick;
    didDrag = true;
    repaint();
}

void ArrangementView::mouseUp (const juce::MouseEvent&)
{
    if (dragging.valid() && didDrag && dragDeltaTicks != 0)
    {
        const auto destStart = juce::jmax ((juce::int64) 0, dragging.startTick + dragDeltaTicks);

        auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
        params->setProperty ("trackId", dragging.trackId);
        params->setProperty ("start", dragging.startTick);
        params->setProperty ("end", dragging.endTick);
        params->setProperty ("destStart", destStart);
        runCommand (dragIsCopy ? "clip.copyRange" : "clip.moveRange", params);

        selected = { dragging.trackId, destStart, destStart + (dragging.endTick - dragging.startTick) };
    }

    dragging = {};
    dragDeltaTicks = 0;
    repaint();
}

void ArrangementView::mouseDoubleClick (const juce::MouseEvent& event)
{
    const auto hit = blockAt (event.getPosition());

    if (hit.valid() && onOpenEditor)
        onOpenEditor (hit.trackId);
}

void ArrangementView::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.mods.isCtrlDown())
    {
        const auto mouseTick = xToTick (event.x);
        ticksPerPixel = juce::jlimit (800.0, 400000.0, ticksPerPixel * (wheel.deltaY > 0 ? 0.8 : 1.25));
        scrollTick = juce::jmax ((juce::int64) 0, mouseTick - (juce::int64) (event.x * ticksPerPixel));
    }
    else if (event.mods.isShiftDown())
    {
        scrollTick = juce::jmax ((juce::int64) 0,
                                 scrollTick - (juce::int64) (wheel.deltaY * 40 * ticksPerPixel * 8));
    }
    else
    {
        const auto laneCount = (int) engine.getTrackIds().size();
        scrollLane = juce::jlimit (0, juce::jmax (0, laneCount - 1),
                                   scrollLane + (wheel.deltaY > 0 ? -1 : 1));
    }

    repaint();
}

//==============================================================================
void ArrangementView::showBlockMenu (const BlockRef& block)
{
    const auto safe = juce::Component::SafePointer<ArrangementView> (this);
    juce::PopupMenu menu;

    menu.addItem ("Loop this block", [safe, block]
    {
        if (safe == nullptr)
            return;

        auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
        params->setProperty ("enabled", true);
        params->setProperty ("startTick", safe->nearestBar (block.startTick));
        params->setProperty ("endTick", safe->nearestBar (block.endTick));
        safe->runCommand ("transport.setLoop", params);
    });

    juce::PopupMenu repeat;

    for (auto times : { 1, 2, 4, 8 })
        repeat.addItem ("x " + juce::String (times), [safe, block, times]
        {
            if (safe == nullptr)
                return;

            // Repeat the block bar-aligned, starting where its bar-span ends.
            const auto start = safe->nearestBar (block.startTick);
            auto end = safe->nearestBar (block.endTick);

            if (end <= start)
                end = start + safe->engine.getTransport().getTempoMap()->getTicksPerBar (start);

            auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
            params->setProperty ("trackId", block.trackId);
            params->setProperty ("start", start);
            params->setProperty ("end", end);
            params->setProperty ("destStart", end);
            params->setProperty ("times", times);
            safe->runCommand ("clip.copyRange", params);
        });

    menu.addSubMenu ("Repeat after itself", repeat);
    menu.addSeparator();

    menu.addItem ("Open in MIDI editor", [safe, block]
    {
        if (safe != nullptr && safe->onOpenEditor)
            safe->onOpenEditor (block.trackId);
    });

    menu.addSeparator();
    menu.addItem ("Erase block", [safe, block]
    {
        if (safe == nullptr)
            return;

        auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
        params->setProperty ("trackId", block.trackId);
        params->setProperty ("start", block.startTick);
        params->setProperty ("end", block.endTick);
        safe->runCommand ("clip.eraseRange", params);
        safe->selected = {};
    });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

void ArrangementView::showRulerMenu (juce::int64 tick)
{
    const auto safe = juce::Component::SafePointer<ArrangementView> (this);

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

void ArrangementView::promptForMarker (juce::int64 tick, const juce::String& existingName)
{
    auto* window = new juce::AlertWindow (existingName.isEmpty() ? "Add marker" : "Rename marker",
                                          "Name:", juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", existingName);
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<ArrangementView> (this), window, tick] (int result)
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
void ArrangementView::timerCallback()
{
    const auto playhead = engine.getTransport().getPositionTicks();
    bool needsRepaint = playhead != lastPlayheadTick;
    lastPlayheadTick = playhead;

    // Repaint when any visible sequence changed (the cache notices pointer changes)
    for (auto trackId : engine.getTrackIds())
    {
        const auto it = cache.find (trackId);

        if (it == cache.end() || it->second.sequence != engine.getTrackSequence (trackId))
        {
            needsRepaint = true;
            break;
        }
    }

    if (needsRepaint && isShowing())
        repaint();
}

//==============================================================================
void ArrangementView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1c1f));

    const auto map = engine.getTransport().getTempoMap();
    const auto trackIds = engine.getTrackIds();
    const auto lanes = lanesArea();

    // --- Lanes background ---
    for (int lane = scrollLane; lane < (int) trackIds.size(); ++lane)
    {
        const auto y = rulerHeight + (lane - scrollLane) * laneHeight;

        if (y > getHeight())
            break;

        g.setColour (lane % 2 == 0 ? juce::Colour (0xff202327) : juce::Colour (0xff24272c));
        g.fillRect (0, y, getWidth(), laneHeight);

        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (engine.getTrackName (trackIds[(size_t) lane]), 6, y + 2, 200, 14, juce::Justification::left);
    }

    // --- Bar lines + ruler ---
    g.setColour (juce::Colour (0xff232529));
    g.fillRect (rulerArea());

    const auto endTick = xToTick (getWidth());
    auto barTick = map->getBarStart (scrollTick);
    int barCounter = 0;

    while (barTick < endTick && ++barCounter < 2000)
    {
        const auto x = tickToX (barTick);
        const auto bar = map->ticksToBarsBeats (barTick).bar;

        g.setColour (juce::Colour (0xff2e3136));
        g.fillRect (x, rulerHeight, 1, lanes.getHeight());

        g.setColour (juce::Colours::lightgrey);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String (bar), x + 3, rulerHeight - 18, 40, 14, juce::Justification::left);

        barTick += map->getTicksPerBar (barTick);
    }

    // --- Loop region band ---
    auto& transport = engine.getTransport();

    if (transport.isLooping() && transport.getLoopEnd() > transport.getLoopStart())
    {
        const auto x1 = tickToX (transport.getLoopStart());
        const auto x2 = tickToX (transport.getLoopEnd());
        g.setColour (juce::Colours::steelblue.withAlpha (0.35f));
        g.fillRect (x1, rulerHeight - 6, juce::jmax (2, x2 - x1), 6);
    }

    // --- Markers ---
    for (auto& marker : engine.getMarkers())
    {
        const auto x = tickToX (marker.tick);

        if (x < -100 || x > getWidth())
            continue;

        g.setColour (juce::Colours::gold.withAlpha (0.85f));
        g.fillRect (x, 0, 1, getHeight());
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (marker.name, x + 4, 2, 140, 14, juce::Justification::left);
    }

    // --- Phrase blocks ---
    for (int lane = scrollLane; lane < (int) trackIds.size(); ++lane)
    {
        const auto trackId = trackIds[(size_t) lane];
        const auto y = rulerHeight + (lane - scrollLane) * laneHeight;

        if (y > getHeight())
            break;

        const auto sequence = engine.getTrackSequence (trackId);

        for (auto& block : blocksFor (trackId))
        {
            BlockRef ref { trackId, block.startTick, block.endTick };
            const auto isSelected = selected.valid() && selected.trackId == trackId
                                      && selected.startTick == block.startTick;
            const auto isDragged = dragging.valid() && dragging.trackId == trackId
                                     && dragging.startTick == block.startTick && didDrag;

            if (isDragged)
            {
                ref.startTick += dragDeltaTicks;
                ref.endTick += dragDeltaTicks;
            }

            const auto rect = blockRect (ref, lane);

            if (rect.getRight() < 0 || rect.getX() > getWidth())
                continue;

            g.setColour (isSelected || isDragged ? juce::Colour (0xcc7aa3d4) : juce::Colour (0x995d8fc4));
            g.fillRoundedRectangle (rect.toFloat(), 4.0f);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.drawRoundedRectangle (rect.toFloat(), 4.0f, 1.0f);

            // Mini note preview
            if (sequence != nullptr && block.noteCount > 0)
            {
                g.setColour (juce::Colours::white.withAlpha (0.5f));

                for (auto& note : sequence->getNotes())
                {
                    if (note.startTick < block.startTick || note.startTick >= block.endTick)
                        continue;

                    const auto tickShift = isDragged ? dragDeltaTicks : 0;
                    const auto nx = tickToX (note.startTick + tickShift);
                    const auto nw = juce::jmax (1, (int) ((double) note.lengthTicks / ticksPerPixel));
                    const auto ny = rect.getBottom() - 4 - (note.key - 24) * (rect.getHeight() - 8) / 84;
                    g.fillRect (nx, juce::jlimit (rect.getY() + 2, rect.getBottom() - 3, ny), nw, 2);
                }
            }
        }
    }

    // --- Playhead ---
    const auto playheadX = tickToX (engine.getTransport().getPositionTicks());

    if (playheadX >= 0 && playheadX <= getWidth())
    {
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillRect (playheadX, 0, 1, getHeight());
    }

    // --- Empty hint ---
    if (trackIds.empty())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("Add a track in the sidebar to get started", lanes, juce::Justification::centred);
    }
}
