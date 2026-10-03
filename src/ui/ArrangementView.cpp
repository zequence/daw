#include "ArrangementView.h"
#include "../api/CommandDispatcher.h"

ArrangementView::ArrangementView (AudioEngine& e, CommandDispatcher& d, TimeAxis& a)
    : engine (e), dispatcher (d), axis (a)
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
    return scrollLane + y / laneHeight;
}

juce::Rectangle<int> ArrangementView::blockRect (const BlockRef& block, int laneIndex) const
{
    const auto x = tickToX (block.startTick);
    const auto right = tickToX (block.endTick);
    const auto y = (laneIndex - scrollLane) * laneHeight;
    return { x, y + 6, juce::jmax (8, right - x), laneHeight - 12 };
}

ArrangementView::BlockRef ArrangementView::blockAt (juce::Point<int> position)
{
    if (position.x < TimeAxis::gutter)
        return {};

    const auto trackIds = engine.getArrangeTrackOrder();
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
        const auto trackIds = engine.getArrangeTrackOrder();

        if (lane >= 0 && lane < (int) trackIds.size() && onSelectTrack)
            onSelectTrack (trackIds[(size_t) lane]);
    }

    repaint();
}

void ArrangementView::mouseDrag (const juce::MouseEvent& event)
{
    if (! dragging.valid())
        return;

    const auto rawDelta = (juce::int64) ((event.x - dragStart.x) * axis.ticksPerPixel);
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
    if (! axis.handleWheel (event, wheel))
    {
        const auto laneCount = (int) engine.getArrangeTrackOrder().size();
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

//==============================================================================
void ArrangementView::timerCallback()
{
    // Playhead, shared axis, or any engine mutation (clips, folders, markers,
    // tempo...) - the engine's state revision covers everything we display.
    const auto playhead = engine.getTransport().getPositionTicks();
    const auto needsRepaint = playhead != lastPlayheadTick || axis.revision != lastAxisRevision
                                || engine.getStateRevision() != lastEngineRevision;

    lastPlayheadTick = playhead;
    lastAxisRevision = axis.revision;
    lastEngineRevision = engine.getStateRevision();

    if (needsRepaint && isShowing())
        repaint();
}

//==============================================================================
void ArrangementView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1c1f));

    const auto map = engine.getTransport().getTempoMap();

    // Lane order follows the sidebar's folder tree; collapsed folders hide their lanes
    const auto trackIds = engine.getArrangeTrackOrder();

    // --- Lanes background ---
    for (int lane = scrollLane; lane < (int) trackIds.size(); ++lane)
    {
        const auto y = (lane - scrollLane) * laneHeight;

        if (y > getHeight())
            break;

        g.setColour (lane % 2 == 0 ? juce::Colour (0xff202327) : juce::Colour (0xff24272c));
        g.fillRect (0, y, getWidth(), laneHeight);
    }

    // --- Bar lines ---
    const auto endTick = xToTick (getWidth());
    auto barTick = map->getBarStart (axis.scrollTick);
    int barCounter = 0;

    while (barTick < endTick && ++barCounter < 3000)
    {
        const auto x = tickToX (barTick);

        if (x >= TimeAxis::gutter)
        {
            g.setColour (juce::Colour (0xff2e3136));
            g.fillRect (x, 0, 1, getHeight());
        }

        barTick += map->getTicksPerBar (barTick);
    }

    // --- Marker lines (names and menus live in the timeline bar) ---
    for (auto& marker : engine.getMarkers())
    {
        const auto x = tickToX (marker.tick);

        if (x >= TimeAxis::gutter && x <= getWidth())
        {
            g.setColour (juce::Colours::gold.withAlpha (0.35f));
            g.fillRect (x, 0, 1, getHeight());
        }
    }

    // --- Phrase blocks ---
    for (int lane = scrollLane; lane < (int) trackIds.size(); ++lane)
    {
        const auto trackId = trackIds[(size_t) lane];
        const auto y = (lane - scrollLane) * laneHeight;

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

            if (rect.getRight() < TimeAxis::gutter || rect.getX() > getWidth())
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
                    const auto nw = juce::jmax (1, (int) ((double) note.lengthTicks / axis.ticksPerPixel));
                    const auto ny = rect.getBottom() - 4 - (note.key - 24) * (rect.getHeight() - 8) / 84;
                    g.fillRect (nx, juce::jlimit (rect.getY() + 2, rect.getBottom() - 3, ny), nw, 2);
                }
            }
        }
    }

    // --- Gutter (shared left column): track names over a solid background ---
    g.setColour (juce::Colour (0xff1d1f23));
    g.fillRect (0, 0, TimeAxis::gutter, getHeight());
    g.setColour (juce::Colour (0xff2e3136));
    g.fillRect (TimeAxis::gutter - 1, 0, 1, getHeight());

    for (int lane = scrollLane; lane < (int) trackIds.size(); ++lane)
    {
        const auto y = (lane - scrollLane) * laneHeight;

        if (y > getHeight())
            break;

        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.setFont (juce::FontOptions (10.0f));
        g.drawFittedText (engine.getTrackName (trackIds[(size_t) lane]),
                          4, y + 4, TimeAxis::gutter - 8, laneHeight - 8,
                          juce::Justification::topLeft, 3);
    }

    // --- Playhead ---
    const auto playheadX = tickToX (engine.getTransport().getPositionTicks());

    if (playheadX >= TimeAxis::gutter && playheadX <= getWidth())
    {
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillRect (playheadX, 0, 1, getHeight());
    }

    // --- Empty hint ---
    if (trackIds.empty())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("Add a track in the sidebar to get started", getLocalBounds(), juce::Justification::centred);
    }
}
