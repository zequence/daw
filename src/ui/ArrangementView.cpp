#include "ArrangementView.h"
#include "../api/CommandDispatcher.h"
#include "ColorPalette.h"
#include "Theme.h"
#include "EditorSettings.h"

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
    return { x, laneTop + 4, juce::jmax (8, right - x), sidebar::trackRowHeight - 8 };   // overlapping ones share it (hatched)
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
    const auto top = rowTop (items, (size_t) index);

    // Where regions overlap, the later one (drawn on top) is hit
    const auto& blocks = blocksFor (trackId);

    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it)
        if (const auto ref = BlockRef::of (trackId, *it); blockRect (ref, top).contains (position))
            return ref;

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
    draggingFolder = {};

    // A selected folder region: dragged sideways, with everything inside it (an unselected one is
    // selected on release, like a region)
    // Alt swaps the two ways of moving (Settings > Editor): with "select first", Alt+drag moves a region
    // straight away; with "in one go", Alt+drag draws a selection rectangle, on a region too (Alt+Ctrl adds)
    const auto moveDirectly = editorSettings::moveRegionsDirectly (engine.getSettingsFile()) != event.mods.isAltDown();

    if (event.mods.isAltDown() && ! moveDirectly && ! event.mods.isPopupMenu())
    {
        marquee = true;
        marqueeAdds = event.mods.isCtrlDown();
        return;
    }

    if (const auto span = folderSpanAt (position); span.folder != 0 && ! event.mods.isPopupMenu()
                                                     && (isSelectedFolderSpan (span) || moveDirectly))
    {
        if (moveDirectly && ! isSelectedFolderSpan (span))   // select and move in one go
        {
            selection.clear();
            selectedFolderSpan = span;
        }

        draggingFolder = span;
        draggingFolderTracks = tracksInFolder (span.folder);
        return;
    }

    // A click on a contact point glues the two regions
    if (const auto glue = gluePointAt (position); glue.trackId != 0 && ! event.mods.isPopupMenu())
    {
        auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
        params->setProperty ("trackId", glue.trackId);
        params->setProperty ("region", glue.region);
        params->setProperty ("into", glue.into);
        runCommand ("clip.glue", params);
        selection.clear();
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }

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

    // Select and move in one go (Settings > Editor): pressing a region selects it (Ctrl adds it)
    if (hit.valid() && moveDirectly && ! isSelected (hit))
    {
        if (! event.mods.isCtrlDown())
            selection.clear();

        selectedFolderSpan = {};
        selection.push_back (hit);

        if (onSelectTrack)
            onSelectTrack (hit.trackId);
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

    if (draggingFolder.folder != 0)   // sideways only, snapped like a region
    {
        const auto rawDelta = (juce::int64) ((event.x - dragStart.x) * axis.ticksPerPixel);
        dragDeltaTicks = axis.snapToGrid (*engine.getTransport().getTempoMap(), draggingFolder.start + rawDelta) - draggingFolder.start;
        repaint();
        return;
    }

    if (marquee)
    {
        repaint();
        return;
    }

    if (! dragging.valid())
        return;

    const auto rawDelta = (juce::int64) ((event.x - dragStart.x) * axis.ticksPerPixel);
    const auto target = axis.snapToGrid (*engine.getTransport().getTempoMap(), dragging.startTick + rawDelta);   // the zoom's grid
    dragDeltaTicks = target - dragging.startTick;

    // Up/down: to the track under the mouse (folders and empty space keep the last one)
    const auto items = itemsNow();

    if (const auto index = itemIndexAt (items, event.y); index >= 0 && items[(size_t) index].member != 0)
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
            const auto ref = BlockRef::of (items[i].member, block);

            if (blockRect (ref, top).intersects (area))
                touched.push_back (ref);
        }
    }

    return touched;
}

void ArrangementView::mouseUp (const juce::MouseEvent& event)
{
    if (draggingFolder.folder != 0)
    {
        // Every track's content in the stretch moves by the same amount: one edit, one undo
        if (didDrag && dragDeltaTicks != 0)
        {
            juce::Array<juce::var> moves;

            for (auto track : draggingFolderTracks)
            {
                auto move = juce::DynamicObject::Ptr (new juce::DynamicObject());
                move->setProperty ("trackId", track);
                move->setProperty ("start", draggingFolder.start);
                move->setProperty ("end", draggingFolder.end);
                move->setProperty ("destStart", juce::jmax ((juce::int64) 0, draggingFolder.start + dragDeltaTicks));
                moves.add (juce::var (move.get()));
            }

            auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
            params->setProperty ("moves", moves);
            runCommand ("clip.moveRanges", params);
            selection.clear();

            const auto landed = juce::jmax ((juce::int64) 0, draggingFolder.start + dragDeltaTicks);
            selectedFolderSpan = { draggingFolder.folder, landed, landed + (draggingFolder.end - draggingFolder.start) };
        }

        draggingFolder = {};
        draggingFolderTracks.clear();
        dragDeltaTicks = 0;
        repaint();
        return;
    }

    if (marquee && ! didDrag)
    {
        // A click on a folder region selects it (Ctrl keeps the region selection)
        if (const auto span = folderSpanAt (dragStart); span.folder != 0)
        {
            if (! marqueeAdds)
                selection.clear();

            selectedFolderSpan = span;
            marquee = false;
            mouseMove (event);
            repaint();
            return;
        }
    }

    if (marquee)
    {
        if (! marqueeAdds)
            selectedFolderSpan = {};

        // A click selects the block under it - where regions overlap, only the top one (or clears);
        // a dragged rectangle selects everything it touches
        std::vector<BlockRef> touched;

        if (! didDrag)
        {
            if (const auto hit = blockAt (dragStart); hit.valid())
                touched.push_back (hit);
        }
        else
        {
            touched = blocksTouching (juce::Rectangle<int> (dragStart, dragNow));
        }

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
    mouseMove (event);   // the new selection may be under the pointer
    repaint();
}

std::map<AudioEngine::TrackId, AudioEngine::TrackId> ArrangementView::trackShift (const Items& items) const
{
    std::vector<AudioEngine::TrackId> order;   // the tracks top to bottom (folders skipped)

    for (auto& item : items)
        if (item.member != 0)
            order.push_back (item.member);

    const auto indexOf = [&order] (AudioEngine::TrackId id)
    {
        return (int) (std::find (order.begin(), order.end(), id) - order.begin());
    };

    const auto anchor = indexOf (dragging.trackId), target = indexOf (dragTargetTrack);

    if (! dragging.valid() || anchor >= (int) order.size() || target >= (int) order.size())
        return {};

    int lowest = (int) order.size(), highest = -1;

    for (auto& block : selection)
    {
        const auto index = indexOf (block.trackId);

        if (index >= (int) order.size())
            return {};

        lowest = juce::jmin (lowest, index);
        highest = juce::jmax (highest, index);
    }

    const auto shift = juce::jlimit (-lowest, (int) order.size() - 1 - highest, target - anchor);
    std::map<AudioEngine::TrackId, AudioEngine::TrackId> result;

    for (auto& block : selection)
        result[block.trackId] = order[(size_t) (indexOf (block.trackId) + shift)];

    return result;
}

// Moves (or copies) every selected block by the drag, in time and up/down across tracks
void ArrangementView::moveSelection()
{
    const auto shift = trackShift (itemsNow());
    const auto changesTrack = std::any_of (shift.begin(), shift.end(), [] (const auto& entry) { return entry.first != entry.second; });

    if (dragDeltaTicks == 0 && ! changesTrack)
        return;

    juce::Array<juce::var> moves;
    std::vector<BlockRef> moved;

    for (auto& block : selection)
    {
        const auto destStart = juce::jmax ((juce::int64) 0, block.startTick + dragDeltaTicks);
        const auto it = shift.find (block.trackId);
        const auto destTrack = it != shift.end() ? it->second : block.trackId;

        auto move = juce::DynamicObject::Ptr (new juce::DynamicObject());
        move->setProperty ("trackId", block.trackId);
        move->setProperty ("start", block.startTick);
        move->setProperty ("end", block.endTick);
        move->setProperty ("destStart", destStart);
        move->setProperty ("destTrackId", destTrack);

        if (block.region >= 0)
            move->setProperty ("region", block.region);   // only this region's notes (others may overlap it)

        moves.add (juce::var (move.get()));

        moved.push_back ({ destTrack, destStart, destStart + (block.endTick - block.startTick) });   // (its new region id: any)
    }

    // One command for the whole selection: one edit per track, one history entry
    auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
    params->setProperty ("moves", moves);
    params->setProperty ("copy", dragIsCopy);
    runCommand ("clip.moveRanges", params);

    selection = moved;   // (blocks recompute from the notes; the refs re-match at their new starts)
}

void ArrangementView::mouseMove (const juce::MouseEvent& event)
{
    if (gluePointAt (event.getPosition()).trackId != 0)
    {
        setMouseCursor (glueCursor());
        return;
    }

    const auto moveDirectly = editorSettings::moveRegionsDirectly (engine.getSettingsFile()) != event.mods.isAltDown();

    if (const auto span = folderSpanAt (event.getPosition());   // a movable folder region: the hand (it drags sideways)
        isSelectedFolderSpan (span) || (moveDirectly && span.folder != 0))
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);   // movable, like any region
        return;
    }

    const auto hit = blockAt (event.getPosition());
    setMouseCursor (hit.valid() && (isSelected (hit) || moveDirectly) ? juce::MouseCursor::DraggingHandCursor
                                                                      : juce::MouseCursor::NormalCursor);
}

ArrangementView::GluePoint ArrangementView::gluePointAt (juce::Point<int> position)
{
    const auto items = itemsNow();
    const auto index = itemIndexAt (items, position.y);

    if (index < 0 || items[(size_t) index].member == 0)
        return {};

    const auto trackId = items[(size_t) index].member;
    const auto top = rowTop (items, (size_t) index);
    const auto& blocks = blocksFor (trackId);
    const auto map = engine.getTransport().getTempoMap();

    for (size_t i = 1; i < blocks.size(); ++i)
    {
        const auto& after = blocks[i];
        const auto rect = blockRect (BlockRef::of (trackId, after), top);

        if (std::abs (position.x - rect.getX()) > 5 || position.y < rect.getY() - 2 || position.y > rect.getBottom() + 2)
            continue;

        // The earlier region it touches or overlaps (closer than the two bars of silence that would part them)
        for (auto j = i; j-- > 0;)
        {
            const auto& before = blocks[j];

            if (before.region != after.region
                 && after.startTick - before.endTick < 2 * map->getTicksPerBar (juce::jmin (before.endTick, after.startTick)))
                return { trackId, after.region, before.region };
        }
    }

    return {};
}

// A glue tube, nozzle down-left; the hotspot is the nozzle's tip
juce::MouseCursor ArrangementView::glueCursor()
{
    static const auto cursor = []() -> juce::MouseCursor
    {
        constexpr int size = 24;
        juce::Image image (juce::Image::ARGB, size, size, true);

        {
            juce::Graphics g (image);
            juce::Path tube;
            tube.addRoundedRectangle (-4.0f, -6.0f, 8.0f, 13.0f, 2.0f);                  // the body
            tube.addTriangle (-2.5f, 7.0f, 2.5f, 7.0f, 0.0f, 12.5f);                        // the nozzle
            tube.addRectangle (-4.5f, -8.0f, 9.0f, 2.0f);                                   // the crimped end
            tube.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f)
                                   .translated (13.0f, 11.0f));

            g.setColour (juce::Colours::black);
            g.strokePath (tube, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved));
            g.setColour (juce::Colours::white);
            g.fillPath (tube);

            g.setColour (juce::Colour (0xff3aa6c4));   // a drop of glue at the tip
            g.fillEllipse (2.0f, 18.0f, 4.5f, 4.5f);
            g.setColour (juce::Colours::black);
            g.drawEllipse (2.0f, 18.0f, 4.5f, 4.5f, 1.0f);
        }

        return juce::MouseCursor (juce::ScaledImage (image), juce::Point<int> (4, 20));
    }();

    return cursor;
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

std::vector<AudioEngine::TrackId> ArrangementView::tracksInFolder (AudioEngine::FolderId folder) const
{
    std::vector<AudioEngine::TrackId> tracks;
    int folderDepth = -1;

    for (auto& item : engine.getSidebarItems (true, false))   // collapsed folders' contents too
    {
        if (folderDepth < 0)
        {
            if (item.folder == folder)
                folderDepth = item.depth;

            continue;
        }

        if (item.depth <= folderDepth)
            break;

        if (item.member != 0)
            tracks.push_back ((AudioEngine::TrackId) item.member);
    }

    return tracks;
}

// The union of the regions of the folder's tracks: overlapping or touching ones join
std::vector<std::pair<juce::int64, juce::int64>> ArrangementView::folderSpans (AudioEngine::FolderId folder)
{
    std::vector<std::pair<juce::int64, juce::int64>> spans;

    for (auto track : tracksInFolder (folder))
        for (auto& block : blocksFor (track))
            spans.push_back ({ block.startTick, block.endTick });

    std::sort (spans.begin(), spans.end());
    std::vector<std::pair<juce::int64, juce::int64>> merged;

    for (auto& span : spans)
    {
        if (! merged.empty() && span.first <= merged.back().second)
            merged.back().second = juce::jmax (merged.back().second, span.second);
        else
            merged.push_back (span);
    }

    return merged;
}

ArrangementView::FolderSpan ArrangementView::folderSpanAt (juce::Point<int> position)
{
    const auto items = itemsNow();
    const auto index = itemIndexAt (items, position.y);

    if (index < 0 || items[(size_t) index].folder == 0 || position.x < TimeAxis::gutter)
        return {};

    const auto tick = xToTick (position.x);

    for (auto& [start, end] : folderSpans (items[(size_t) index].folder))
        if (tick >= start && tick < end)
            return { items[(size_t) index].folder, start, end };

    return {};
}

// Inside a region's box: the stretch before its first note and after its last one, darker
void ArrangementView::dimEmptyEnds (juce::Graphics& g, juce::Rectangle<int> box, juce::int64 firstNote, juce::int64 lastEnd) const
{
    if (firstNote == std::numeric_limits<juce::int64>::max())
        return;

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    const auto inner = box.reduced (1);

    if (const auto x = juce::jmin (inner.getRight(), tickToX (firstNote)); x > inner.getX())
        g.fillRect (inner.withRight (x));

    if (const auto x = juce::jmax (inner.getX(), tickToX (lastEnd)); x < inner.getRight())
        g.fillRect (inner.withLeft (x));
}

void ArrangementView::mouseDoubleClick (const juce::MouseEvent& event)
{
    // A folder's region: the editor on the folder's tracks
    const auto items = itemsNow();

    if (const auto index = itemIndexAt (items, event.y); index >= 0 && items[(size_t) index].folder != 0)
    {
        const auto tick = xToTick (event.x);

        for (auto& [start, end] : folderSpans (items[(size_t) index].folder))
            if (tick >= start && tick < end && onOpenEditorOnTracks)
            {
                if (auto tracks = tracksInFolder (items[(size_t) index].folder); ! tracks.empty())
                    onOpenEditorOnTracks (std::move (tracks));

                return;
            }
    }

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
        const auto ticksPerBar = map->getTicksPerBar (barTick);

        if (x >= TimeAxis::gutter)
        {
            g.setColour (theme::colour (theme::Token::arrangeBarline));
            g.fillRect (x, 0, 1, getHeight());
        }

        // Zoomed in: the finer grid lines (= where regions snap), fainter than the bars
        const auto step = axis.gridStep (*map, barTick);
        g.setColour (theme::colour (theme::Token::arrangeBarline).withMultipliedAlpha (0.45f));

        for (auto lineTick = barTick + step; lineTick < barTick + ticksPerBar && lineTick < endTick; lineTick += step)
            if (const auto lineX = tickToX (lineTick); lineX >= TimeAxis::gutter)
                g.fillRect (lineX, 0, 1, getHeight());

        barTick += ticksPerBar;
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
        // Blocks dragged up/down are drawn in their destination tracks' lanes
        std::map<AudioEngine::TrackId, int> laneTops;

        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].member != 0)
                laneTops[items[i].member] = rowTop (items, i);

        const auto shift = dragging.valid() && didDrag ? trackShift (items)
                                                       : std::map<AudioEngine::TrackId, AudioEngine::TrackId>();

        int y = -vscroll.y;

        for (auto& item : items)
        {
            const auto height = sidebar::heightOf (item);

            if (item.member == 0 && item.folder != 0 && y + height > 0 && y < getHeight())
            {
                // A folder lane: one region per stretch of content inside it
                const auto base = AudioEngine::colourFromHex (engine.getFolderColour (item.folder), juce::Colour (0xff8a8f98));
                const auto style = theme::regionStyle (base, false);
                const auto folderTracks = tracksInFolder (item.folder);

                for (auto [start, end] : folderSpans (item.folder))
                {
                    const auto noteShift = draggingFolder.folder == item.folder && draggingFolder.start == start && didDrag
                                             ? dragDeltaTicks : (juce::int64) 0;
                    const auto originalStart = start;
                    start += noteShift;
                    end += noteShift;
                    const auto x = tickToX (start);
                    const auto right = tickToX (end);

                    if (right < TimeAxis::gutter || x > getWidth())
                        continue;

                    const auto rect = juce::Rectangle<int> (x, y + 3, juce::jmax (8, right - x), height - 6);
                    const auto isSelectedSpan = isSelectedFolderSpan ({ item.folder, originalStart, 0 });
                    const auto look = isSelectedSpan ? theme::regionStyle (base, true) : style;
                    g.setColour (look.fill.withMultipliedAlpha (0.8f));
                    g.fillRoundedRectangle (rect.toFloat(), theme::corner);
                    g.setColour (look.border);
                    g.drawRoundedRectangle (rect.toFloat(), theme::corner, 1.2f);

                    // The empty beginning and end of the folder's stretch: dimmed (as in the tracks' regions)
                    {
                        auto first = std::numeric_limits<juce::int64>::max(), last = (juce::int64) 0;

                        for (auto track : folderTracks)
                            if (auto folderSeq = engine.getTrackSequence (track))
                                for (auto& note : folderSeq->getNotes())
                                    if (note.startTick >= originalStart && note.startTick < originalStart + (end - start))
                                    {
                                        first = juce::jmin (first, note.startTick);
                                        last = juce::jmax (last, note.startTick + note.lengthTicks);
                                    }

                        dimEmptyEnds (g, rect, first + noteShift, last + noteShift);
                    }

                    // The combined notes of the folder's tracks (a mini preview, as in the tracks' regions)
                    g.setColour (juce::Colours::black.withAlpha (0.45f));

                    for (auto track : folderTracks)
                        if (auto folderSeq = engine.getTrackSequence (track))
                            for (auto& note : folderSeq->getNotes())
                            {
                                if (note.startTick < originalStart || note.startTick >= originalStart + (end - start))
                                    continue;

                                const auto nx = tickToX (note.startTick + noteShift);

                                if (nx > getWidth())
                                    break;

                                const auto nw = juce::jmax (1, (int) ((double) note.lengthTicks / axis.ticksPerPixel));
                                const auto ny = rect.getBottom() - 3 - (note.key - 24) * (rect.getHeight() - 6) / 84;
                                g.fillRect (nx, juce::jlimit (rect.getY() + 2, rect.getBottom() - 3, ny), nw, 1);
                            }
                }
            }

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
                auto ref = BlockRef::of (trackId, block);
                const auto selectedBlock = isSelected (ref);
                const auto isDragged = dragging.valid() && didDrag && selectedBlock;
                const auto inDraggedFolder = draggingFolder.folder != 0 && didDrag && block.startTick >= draggingFolder.start
                                               && block.startTick < draggingFolder.end
                                               && std::find (draggingFolderTracks.begin(), draggingFolderTracks.end(), trackId)
                                                    != draggingFolderTracks.end();

                if (isDragged || inDraggedFolder)
                {
                    ref.startTick += dragDeltaTicks;
                    ref.endTick += dragDeltaTicks;
                }

                const auto shifted = isDragged ? shift.find (trackId) : shift.end();

                const auto rect = blockRect (ref, shifted != shift.end() ? laneTops[shifted->second] : y);

                if (rect.getRight() < TimeAxis::gutter || rect.getX() > getWidth())
                    continue;

                // Opacity and brightness of the box and border come from the theme
                const auto style = theme::regionStyle (base, selectedBlock || isDragged);

                g.setColour (style.fill);
                g.fillRoundedRectangle (rect.toFloat(), theme::corner);

                // The empty beginning (from the bar line to the first note) and end (after the last note): dimmed
                if (sequence != nullptr && block.noteCount > 0)
                {
                    auto first = std::numeric_limits<juce::int64>::max(), last = (juce::int64) 0;

                    for (auto& note : sequence->getNotes())
                        if (note.region == block.region && note.startTick >= block.startTick && note.startTick < block.endTick)
                        {
                            first = juce::jmin (first, note.startTick);
                            last = juce::jmax (last, note.startTick + note.lengthTicks);
                        }

                    const auto shiftBy = ref.startTick - block.startTick;   // a dragged block's preview offset
                    dimEmptyEnds (g, rect, first + shiftBy, last + shiftBy);
                }

                g.setColour (style.border);
                g.drawRoundedRectangle (rect.toFloat(), theme::corner, 1.8f);

                // Mini note preview
                if (sequence != nullptr && block.noteCount > 0)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.45f));

                    for (auto& note : sequence->getNotes())
                    {
                        if (note.startTick < block.startTick || note.startTick >= block.endTick || note.region != block.region)
                            continue;

                        const auto tickShift = isDragged || inDraggedFolder ? dragDeltaTicks : 0;   // the notes move with their region
                        const auto nx = tickToX (note.startTick + tickShift);
                        const auto nw = juce::jmax (1, (int) ((double) note.lengthTicks / axis.ticksPerPixel));
                        const auto ny = rect.getBottom() - 4 - (note.key - 24) * (rect.getHeight() - 8) / 84;
                        g.fillRect (nx, juce::jlimit (rect.getY() + 2, rect.getBottom() - 3, ny), nw, 2);
                    }
                }
            }

            // Where regions overlap: a hatched band across the lane, so the overlap shows
            const auto& blocks = blocksFor (trackId);

            for (size_t i = 0; i < blocks.size(); ++i)
                for (size_t j = i + 1; j < blocks.size() && blocks[j].startTick < blocks[i].endTick; ++j)
                {
                    const auto from = tickToX (blocks[j].startTick);
                    const auto to = tickToX (juce::jmin (blocks[i].endTick, blocks[j].endTick));

                    if (to <= from || to < TimeAxis::gutter || from > getWidth())
                        continue;

                    // Slight diagonal lines only: the notes underneath stay readable
                    const auto band = juce::Rectangle<int> (from, y + 4, to - from, height - 8);
                    juce::Graphics::ScopedSaveState state (g);
                    g.reduceClipRegion (band);
                    g.setColour (juce::Colours::white.withAlpha (0.32f));

                    for (int x = band.getX() - band.getHeight(); x < band.getRight(); x += 6)
                        g.drawLine ((float) x, (float) band.getBottom(), (float) (x + band.getHeight()), (float) band.getY(), 1.3f);
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
