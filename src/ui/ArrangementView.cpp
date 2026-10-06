#include "ArrangementView.h"
#include "../api/CommandDispatcher.h"
#include "ColorPalette.h"
#include "Theme.h"

ArrangementView::ArrangementView (AudioEngine& e, CommandDispatcher& d, TimeAxis& a, sidebar::VerticalScroll& v)
    : engine (e), dispatcher (d), axis (a), vscroll (v)
{
    setWantsKeyboardFocus (false);
    startTimerHz (30);
}

ArrangementView::~ArrangementView() = default;

//==============================================================================
int ArrangementView::contentHeight (const Items& items)
{
    int total = 0;

    for (auto& item : items)
        total += sidebar::heightOf (item);

    return total;
}

int ArrangementView::rowTop (const Items& items, size_t index) const
{
    int y = -vscroll.y;

    for (size_t i = 0; i < index && i < items.size(); ++i)
        y += sidebar::heightOf (items[i]);

    return y;
}

int ArrangementView::itemIndexAt (const Items& items, int targetY) const
{
    int y = -vscroll.y;

    for (size_t i = 0; i < items.size(); ++i)
    {
        const auto height = sidebar::heightOf (items[i]);

        if (targetY >= y && targetY < y + height)
            return (int) i;

        y += height;
    }

    return -1;
}

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

juce::Rectangle<int> ArrangementView::blockRect (const BlockRef& block, int laneTop) const
{
    const auto x = tickToX (block.startTick);
    const auto right = tickToX (block.endTick);
    return { x, laneTop + 4, juce::jmax (8, right - x), sidebar::trackRowHeight - 8 };
}

ArrangementView::BlockRef ArrangementView::blockAt (juce::Point<int> position)
{
    if (position.x < TimeAxis::gutter)
        return {};

    const auto items = itemsNow();
    const auto index = itemIndexAt (items, position.y);

    if (index < 0 || items[(size_t) index].member == 0)
        return {};

    const auto trackId = items[(size_t) index].member;
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
    dragStart = dragNow = position;
    dragDeltaTicks = 0;
    didDrag = false;
    dragging = {};
    dragTargetTrack = 0;
    marquee = false;

    const auto hit = blockAt (position);

    if (event.mods.isPopupMenu())
    {
        if (hit.valid())
        {
            if (! isSelected (hit))
                selection = { hit };

            showBlockMenu (hit);
        }

        repaint();
        return;
    }

    if (hit.valid() && isSelected (hit))
    {
        // Moving starts only on something already selected (Ctrl = copy)
        dragging = hit;
        dragTargetTrack = hit.trackId;
        dragIsCopy = event.mods.isCtrlDown();
    }
    else
    {
        marquee = true;
        marqueeAdds = event.mods.isCtrlDown();
    }

    repaint();
}

void ArrangementView::mouseDrag (const juce::MouseEvent& event)
{
    dragNow = event.getPosition();

    if (event.getDistanceFromDragStart() > 2)
        didDrag = true;

    if (marquee)
    {
        repaint();
        return;
    }

    if (! dragging.valid())
        return;

    const auto rawDelta = (juce::int64) ((event.x - dragStart.x) * axis.ticksPerPixel);
    const auto target = axis.snap ? nearestBar (dragging.startTick + rawDelta)
                                  : juce::jmax ((juce::int64) 0, dragging.startTick + rawDelta);
    dragDeltaTicks = target - dragging.startTick;

    // Up/down (a single block): to the track under the mouse (folders and empty space keep the last one)
    const auto items = itemsNow();

    if (const auto index = itemIndexAt (items, event.y); selection.size() == 1 && index >= 0 && items[(size_t) index].member != 0)
        dragTargetTrack = items[(size_t) index].member;

    repaint();
}

std::vector<ArrangementView::BlockRef> ArrangementView::blocksTouching (juce::Rectangle<int> area)
{
    std::vector<BlockRef> touched;
    const auto items = itemsNow();

    for (size_t i = 0; i < items.size(); ++i)
    {
        if (items[i].member == 0)
            continue;

        const auto top = rowTop (items, i);

        for (auto& block : blocksFor (items[i].member))
        {
            const BlockRef ref { items[i].member, block.startTick, block.endTick };

            if (blockRect (ref, top).intersects (area))
                touched.push_back (ref);
        }
    }

    return touched;
}

void ArrangementView::mouseUp (const juce::MouseEvent& event)
{
    if (marquee)
    {
        // A click is a 1-pixel rectangle: it selects the block under it (or clears)
        auto area = juce::Rectangle<int> (dragStart, dragNow);
        area.setSize (juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()));
        const auto touched = blocksTouching (area);

        if (! marqueeAdds)
            selection.clear();

        for (auto& block : touched)
            if (! isSelected (block))
                selection.push_back (block);

        // The track follows the click (or the first block touched)
        const auto items = itemsNow();
        const auto index = itemIndexAt (items, dragStart.y);

        if (onSelectTrack)
        {
            if (! touched.empty())
                onSelectTrack (touched.front().trackId);
            else if (index >= 0 && items[(size_t) index].member != 0)
                onSelectTrack (items[(size_t) index].member);
        }
    }
    else if (dragging.valid())
    {
        if (didDrag)
            moveSelection();
        else if (event.mods.isCtrlDown())
            std::erase (selection, dragging);   // Ctrl-click on a selected block deselects it
        else
            selection = { dragging };           // a plain click narrows to this block

        if (onSelectTrack && ! selection.empty())
            onSelectTrack (selection.front().trackId);
    }

    marquee = false;
    dragging = {};
    dragDeltaTicks = 0;
    dragTargetTrack = 0;
    repaint();
}

// Moves (or copies) every selected block by the drag; a single block may also change track.
// Blocks on one track are processed so a moved range never lands on one still to move.
void ArrangementView::moveSelection()
{
    const auto toTrack = selection.size() == 1 && dragTargetTrack != 0 ? dragTargetTrack : 0;

    if (dragDeltaTicks == 0 && (toTrack == 0 || toTrack == selection.front().trackId))
        return;

    auto order = selection;
    std::sort (order.begin(), order.end(), [forward = dragDeltaTicks > 0] (const BlockRef& a, const BlockRef& b)
               { return forward ? a.startTick > b.startTick : a.startTick < b.startTick; });

    std::vector<BlockRef> moved;

    for (auto& block : order)
    {
        const auto destStart = juce::jmax ((juce::int64) 0, block.startTick + dragDeltaTicks);
        const auto destTrack = toTrack != 0 ? toTrack : block.trackId;

        auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
        params->setProperty ("trackId", block.trackId);
        params->setProperty ("start", block.startTick);
        params->setProperty ("end", block.endTick);
        params->setProperty ("destStart", destStart);
        params->setProperty ("destTrackId", destTrack);
        runCommand (dragIsCopy ? "clip.copyRange" : "clip.moveRange", params);

        moved.push_back ({ destTrack, destStart, destStart + (block.endTick - block.startTick) });
    }

    selection = moved;   // (blocks recompute from the notes; the refs re-match at their new starts)
}

void ArrangementView::paintOverChildren (juce::Graphics& g)
{
    if (! marquee || ! didDrag)
        return;

    const auto area = juce::Rectangle<int> (dragStart, dragNow).toFloat();
    g.setColour (theme::colour (theme::Token::selectionBorder).withAlpha (0.15f));
    g.fillRect (area);
    g.setColour (theme::colour (theme::Token::selectionBorder));
    g.drawRect (area, 1.0f);
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
        // Plain wheel scrolls the rows - shared with the sidebar
        const auto items = itemsNow();
        const auto maxScroll = juce::jmax (0, contentHeight (items) - getHeight());
        vscroll.set (juce::jlimit (0, maxScroll, vscroll.y - (int) (wheel.deltaY * 160.0f)));
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
        std::erase (safe->selection, block);
    });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

//==============================================================================
void ArrangementView::timerCallback()
{
    // Playhead, shared axis, shared vertical scroll, or any engine mutation.
    const auto playhead = engine.getTransport().getPositionTicks();
    const auto needsRepaint = playhead != lastPlayheadTick || axis.revision != lastAxisRevision
                                || engine.getStateRevision() != lastEngineRevision
                                || vscroll.revision != lastVScrollRevision;

    lastPlayheadTick = playhead;
    lastAxisRevision = axis.revision;
    lastEngineRevision = engine.getStateRevision();
    lastVScrollRevision = vscroll.revision;

    if (needsRepaint && isShowing())
        repaint();
}

//==============================================================================
void ArrangementView::paint (juce::Graphics& g)
{
    g.fillAll (theme::colour (theme::Token::arrangeBg));

    const auto map = engine.getTransport().getTempoMap();
    const auto items = itemsNow();


    // --- Row backgrounds (same Y axis as the sidebar rows) ---
    {
        int y = -vscroll.y, trackParity = 0;

        for (auto& item : items)
        {
            const auto height = sidebar::heightOf (item);

            if (y + height > 0 && y < getHeight())
            {
                if (item.folder != 0)
                    g.setColour (theme::colour (theme::Token::arrangeLaneFolder));
                else
                    g.setColour (theme::colour (trackParity % 2 == 0 ? theme::Token::arrangeLaneEven : theme::Token::arrangeLaneOdd));

                g.fillRect (0, y, getWidth(), height);
            }

            if (item.member != 0)
                ++trackParity;

            y += height;
        }
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
            g.setColour (theme::colour (theme::Token::arrangeBarline));
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
            g.setColour (theme::colour (theme::Token::arrangeMarkerLine));
            g.fillRect (x, 0, 1, getHeight());
        }
    }

    // --- Phrase blocks ---
    {
        // A block dragged up/down is drawn in the target track's lane
        int targetLaneTop = 0;

        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].member != 0 && items[i].member == dragTargetTrack)
                targetLaneTop = rowTop (items, i);

        int y = -vscroll.y;

        for (auto& item : items)
        {
            const auto height = sidebar::heightOf (item);

            if (item.member == 0 || y + height <= 0 || y >= getHeight())
            {
                y += height;
                continue;
            }

            const auto trackId = item.member;
            const auto sequence = engine.getTrackSequence (trackId);

            // The whole region inherits the track color (ISSUES.md): the border is
            // pronounced and colorful, the box brighter and less colorful.
            const auto base = AudioEngine::colourFromHex (engine.getTrackColour (trackId),
                                                          juce::Colour (0xff8a8f98));

            for (auto& block : blocksFor (trackId))
            {
                BlockRef ref { trackId, block.startTick, block.endTick };
                const auto selectedBlock = isSelected (ref);
                const auto isDragged = dragging.valid() && didDrag && selectedBlock;

                if (isDragged)
                {
                    ref.startTick += dragDeltaTicks;
                    ref.endTick += dragDeltaTicks;
                }

                const auto rect = blockRect (ref, isDragged && selection.size() == 1 && dragTargetTrack != 0 ? targetLaneTop : y);

                if (rect.getRight() < TimeAxis::gutter || rect.getX() > getWidth())
                    continue;

                // Opacity and brightness of the box and border come from the theme
                const auto style = theme::regionStyle (base, selectedBlock || isDragged);

                g.setColour (style.fill);
                g.fillRoundedRectangle (rect.toFloat(), theme::corner);

                g.setColour (style.border);
                g.drawRoundedRectangle (rect.toFloat(), theme::corner, 1.8f);

                // Mini note preview
                if (sequence != nullptr && block.noteCount > 0)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.45f));

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

            y += height;
        }
    }

    // --- Gutter (shared left column): names over a solid background ---
    g.setColour (theme::colour (theme::Token::arrangeGutterBg));
    g.fillRect (0, 0, TimeAxis::gutter, getHeight());
    g.setColour (theme::colour (theme::Token::arrangeGutterBorder));
    g.fillRect (TimeAxis::gutter - 1, 0, 1, getHeight());

    {
        int y = -vscroll.y;

        for (auto& item : items)
        {
            const auto height = sidebar::heightOf (item);

            if (y + height > 0 && y < getHeight())
            {
                if (item.folder != 0)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.55f));
                    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                    g.drawFittedText (engine.getFolderName (item.folder),
                                      4, y + 2, TimeAxis::gutter - 8, height - 4,
                                      juce::Justification::centredLeft, 2);
                }
                else
                {
                    g.setColour (juce::Colours::white.withAlpha (0.45f));
                    g.setFont (juce::FontOptions (10.0f));
                    g.drawFittedText (engine.getTrackName (item.member),
                                      4, y + 4, TimeAxis::gutter - 8, height - 8,
                                      juce::Justification::topLeft, 3);
                }
            }

            y += height;
        }
    }

    // --- Playhead ---
    const auto playheadX = tickToX (engine.getTransport().getPositionTicks());

    if (playheadX >= TimeAxis::gutter && playheadX <= getWidth())
    {
        g.setColour (theme::colour (theme::Token::transportLine));
        g.fillRect (playheadX, 0, 1, getHeight());
    }

    // --- Empty hint ---
    if (items.empty())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("Add a track in the sidebar to get started", getLocalBounds(), juce::Justification::centred);
    }
}
