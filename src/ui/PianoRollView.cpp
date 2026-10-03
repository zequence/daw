#include "PianoRollView.h"
#include "../api/CommandDispatcher.h"

namespace
{
    constexpr auto Q = Ticks::perQuarterNote;

    bool isBlackKey (int key)
    {
        const auto n = key % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }

    juce::var toVar (juce::DynamicObject::Ptr o)
    {
        return juce::var (o.get());
    }
}

//==============================================================================
PianoRollView::PianoRollView (AudioEngine& e, CommandDispatcher& d)
    : engine (e), dispatcher (d)
{
    setWantsKeyboardFocus (true);

    snapBox.addItem ("Snap off", 1);
    snapBox.addItem ("1/1", 2);
    snapBox.addItem ("1/2", 3);
    snapBox.addItem ("1/4", 4);
    snapBox.addItem ("1/8", 5);
    snapBox.addItem ("1/16", 6);
    snapBox.addItem ("1/32", 7);
    snapBox.setSelectedId (5, juce::dontSendNotification);   // 1/8
    snapBox.setWantsKeyboardFocus (false);
    addAndMakeVisible (snapBox);

    quantizeButton.setTooltip ("Quantize selected notes (or all) to the snap grid");
    quantizeButton.setWantsKeyboardFocus (false);
    quantizeButton.onClick = [this]
    {
        const auto grid = snapTicksOrZero();

        if (grid <= 0 || sequence() == nullptr)
            return;

        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        params->setProperty ("grid", grid);

        if (! selection.empty())
        {
            // Quantize only the selected notes by narrowing to their exact range
            // would catch bystanders; instead update them by index.
            const auto seq = sequence();
            juce::Array<juce::var> edits;

            for (auto index : selection)
            {
                if (index >= (int) seq->getNotes().size())
                    continue;

                const auto& note = seq->getNotes()[(size_t) index];
                auto edit = new juce::DynamicObject();
                edit->setProperty ("index", index);
                edit->setProperty ("start", ((note.startTick + grid / 2) / grid) * grid);
                edits.add (juce::var (edit));
            }

            auto updateParams = new juce::DynamicObject();
            updateParams->setProperty ("trackId", trackId);
            updateParams->setProperty ("notes", edits);
            runCommand ("clip.updateNotes", updateParams);
            selection.clear();
            return;
        }

        runCommand ("clip.quantize", params);
    };
    addAndMakeVisible (quantizeButton);

    undoButton.setWantsKeyboardFocus (false);
    undoButton.onClick = [this]
    {
        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        runCommand ("clip.undo", params);
        selection.clear();
    };
    addAndMakeVisible (undoButton);

    redoButton.setWantsKeyboardFocus (false);
    redoButton.onClick = [this]
    {
        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        runCommand ("clip.redo", params);
        selection.clear();
    };
    addAndMakeVisible (redoButton);

    trackLabel.setJustificationType (juce::Justification::centredRight);
    trackLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (trackLabel);

    startTimerHz (30);
}

PianoRollView::~PianoRollView() = default;

void PianoRollView::setTrack (AudioEngine::TrackId id)
{
    if (trackId != id)
    {
        trackId = id;
        selection.clear();
        drag = Drag::none;
    }

    trackLabel.setText (engine.getTrackName (trackId), juce::dontSendNotification);
    repaint();
}

//==============================================================================
juce::Rectangle<int> PianoRollView::rulerArea() const
{
    return { keysWidth, toolbarHeight, getWidth() - keysWidth, rulerHeight };
}

juce::Rectangle<int> PianoRollView::keysArea() const
{
    return { 0, toolbarHeight + rulerHeight, keysWidth,
             getHeight() - toolbarHeight - rulerHeight - velocityHeight };
}

juce::Rectangle<int> PianoRollView::gridArea() const
{
    return { keysWidth, toolbarHeight + rulerHeight, getWidth() - keysWidth,
             getHeight() - toolbarHeight - rulerHeight - velocityHeight };
}

juce::Rectangle<int> PianoRollView::velocityArea() const
{
    return { keysWidth, getHeight() - velocityHeight, getWidth() - keysWidth, velocityHeight };
}

juce::int64 PianoRollView::xToTick (int x) const
{
    return scrollTick + (juce::int64) juce::jmax (0.0, (x - keysWidth) * ticksPerPixel);
}

int PianoRollView::tickToX (juce::int64 tick) const
{
    return keysWidth + (int) ((double) (tick - scrollTick) / ticksPerPixel);
}

int PianoRollView::yToKey (int y) const
{
    return topKey - (y - gridArea().getY()) / keyHeight;
}

int PianoRollView::keyToY (int key) const
{
    return gridArea().getY() + (topKey - key) * keyHeight;
}

juce::Rectangle<int> PianoRollView::noteRect (const MidiSequence::Note& note) const
{
    const auto x = tickToX (note.startTick);
    const auto right = tickToX (note.startTick + note.lengthTicks);
    return { x, keyToY (note.key), juce::jmax (3, right - x), keyHeight };
}

juce::int64 PianoRollView::snapTicksOrZero() const
{
    switch (snapBox.getSelectedId())
    {
        case 2: return 4 * Q;
        case 3: return 2 * Q;
        case 4: return Q;
        case 5: return Q / 2;
        case 6: return Q / 4;
        case 7: return Q / 8;
        default: return 0;
    }
}

juce::int64 PianoRollView::snapTick (juce::int64 tick) const
{
    const auto grid = snapTicksOrZero();
    return grid > 0 ? ((tick + grid / 2) / grid) * grid : tick;
}

//==============================================================================
void PianoRollView::runCommand (const juce::String& cmd, juce::DynamicObject::Ptr params)
{
    const auto reply = dispatcher.run (cmd, toVar (params));

    if (! reply.getProperty ("ok", false))
        juce::Logger::writeToLog ("PianoRoll: " + cmd + " failed: "
                                  + reply.getProperty ("error", {}).toString());
}

void PianoRollView::addNoteAt (juce::int64 tick, int key)
{
    const auto grid = snapTicksOrZero();
    const auto length = grid > 0 ? grid : Q / 2;

    auto note = new juce::DynamicObject();
    note->setProperty ("start", snapTick (tick));
    note->setProperty ("length", length);
    note->setProperty ("key", juce::jlimit (0, 127, key));
    note->setProperty ("velocity", 96);

    juce::Array<juce::var> notes;
    notes.add (juce::var (note));

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("notes", notes);
    runCommand ("clip.addNotes", params);

    // Select the new note
    if (auto seq = sequence())
    {
        const auto snapped = snapTick (tick);
        selection.clear();

        for (int i = 0; i < (int) seq->getNotes().size(); ++i)
            if (seq->getNotes()[(size_t) i].startTick == snapped && seq->getNotes()[(size_t) i].key == key)
                selection.insert (i);
    }
}

void PianoRollView::deleteSelection()
{
    if (selection.empty())
        return;

    juce::Array<juce::var> indices;
    for (auto index : selection)
        indices.add (index);

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("indices", indices);
    runCommand ("clip.removeNotes", params);
    selection.clear();
}

void PianoRollView::deleteNote (int index)
{
    juce::Array<juce::var> indices;
    indices.add (index);

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("indices", indices);
    runCommand ("clip.removeNotes", params);
    selection.clear();
}

void PianoRollView::commitMoveOrResize()
{
    const auto seq = sequence();

    if (seq == nullptr || selection.empty() || (dragTickOffset == 0 && dragKeyOffset == 0))
        return;

    juce::Array<juce::var> edits;
    std::vector<MidiSequence::Note> wanted;

    for (auto index : selection)
    {
        if (index >= (int) seq->getNotes().size())
            continue;

        auto note = seq->getNotes()[(size_t) index];
        auto edit = new juce::DynamicObject();
        edit->setProperty ("index", index);

        if (drag == Drag::move)
        {
            note.startTick = juce::jmax ((juce::int64) 0, note.startTick + dragTickOffset);
            note.key = juce::jlimit (0, 127, note.key + dragKeyOffset);
            edit->setProperty ("start", note.startTick);
            edit->setProperty ("key", note.key);
        }
        else   // resize
        {
            note.lengthTicks = juce::jmax ((juce::int64) 1, note.lengthTicks + dragTickOffset);
            edit->setProperty ("length", note.lengthTicks);
        }

        wanted.push_back (note);
        edits.add (juce::var (edit));
    }

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("notes", edits);
    runCommand ("clip.updateNotes", params);
    reselectByValue (wanted);
}

void PianoRollView::commitVelocities()
{
    if (velocityPreview.empty())
        return;

    juce::Array<juce::var> edits;

    for (auto& [index, velocity] : velocityPreview)
    {
        auto edit = new juce::DynamicObject();
        edit->setProperty ("index", index);
        edit->setProperty ("velocity", velocity);
        edits.add (juce::var (edit));
    }

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("notes", edits);
    runCommand ("clip.updateNotes", params);
    velocityPreview.clear();
}

void PianoRollView::reselectByValue (const std::vector<MidiSequence::Note>& wanted)
{
    selection.clear();
    const auto seq = sequence();

    if (seq == nullptr)
        return;

    for (auto& target : wanted)
        for (int i = 0; i < (int) seq->getNotes().size(); ++i)
        {
            const auto& note = seq->getNotes()[(size_t) i];

            if (note.startTick == target.startTick && note.key == target.key
                 && note.lengthTicks == target.lengthTicks && selection.count (i) == 0)
            {
                selection.insert (i);
                break;
            }
        }
}

int PianoRollView::noteIndexAt (juce::Point<int> position, bool& onRightEdge) const
{
    onRightEdge = false;
    const auto seq = sequence();

    if (seq == nullptr)
        return -1;

    for (int i = (int) seq->getNotes().size(); --i >= 0;)
    {
        const auto rect = noteRect (seq->getNotes()[(size_t) i]);

        if (rect.contains (position))
        {
            onRightEdge = position.x >= rect.getRight() - juce::jmin (6, rect.getWidth() / 3);
            return i;
        }
    }

    return -1;
}

//==============================================================================
void PianoRollView::mouseDown (const juce::MouseEvent& event)
{
    grabKeyboardFocus();
    const auto position = event.getPosition();
    dragStart = position;
    dragTickOffset = 0;
    dragKeyOffset = 0;
    dragChangedSomething = false;

    if (rulerArea().contains (position))
    {
        dispatcher.run ("transport.locate", toVar ([&]
        {
            auto p = juce::DynamicObject::Ptr (new juce::DynamicObject());
            p->setProperty ("tick", snapTick (xToTick (position.x)));
            return p;
        }()));
        return;
    }

    if (velocityArea().contains (position))
    {
        drag = Drag::velocity;
        mouseDrag (event);
        return;
    }

    if (! gridArea().contains (position))
        return;

    bool onRightEdge = false;
    const auto hit = noteIndexAt (position, onRightEdge);

    if (event.mods.isPopupMenu())
    {
        if (hit >= 0)
            deleteNote (hit);
        return;
    }

    if (hit >= 0)
    {
        if (event.mods.isShiftDown())
        {
            if (selection.count (hit)) selection.erase (hit);
            else                       selection.insert (hit);
        }
        else if (selection.count (hit) == 0)
        {
            selection = { hit };
        }

        drag = onRightEdge ? Drag::resize : Drag::move;
    }
    else
    {
        if (! event.mods.isShiftDown())
            selection.clear();

        drag = Drag::marquee;
    }

    repaint();
}

void PianoRollView::mouseDrag (const juce::MouseEvent& event)
{
    const auto position = event.getPosition();

    if (drag == Drag::move || drag == Drag::resize)
    {
        const auto rawTicks = (juce::int64) ((position.x - dragStart.x) * ticksPerPixel);
        const auto grid = snapTicksOrZero();
        dragTickOffset = grid > 0 ? (rawTicks / juce::jmax ((juce::int64) 1, grid)) * grid : rawTicks;
        dragKeyOffset = drag == Drag::move ? (dragStart.y - position.y) / keyHeight : 0;
        dragChangedSomething = true;
        repaint();
    }
    else if (drag == Drag::marquee)
    {
        repaint();
    }
    else if (drag == Drag::velocity)
    {
        const auto seq = sequence();

        if (seq == nullptr)
            return;

        const auto area = velocityArea();
        const auto velocity = juce::jlimit (1, 127, 127 - (position.y - area.getY()) * 127 / juce::jmax (1, area.getHeight()));
        const auto tick = xToTick (position.x);

        // Selection-aware: with a selection, dragging edits selected notes; without,
        // it edits the note whose start is nearest the mouse x.
        if (! selection.empty())
        {
            for (auto index : selection)
                velocityPreview[index] = velocity;
        }
        else
        {
            int best = -1;
            juce::int64 bestDistance = std::numeric_limits<juce::int64>::max();

            for (int i = 0; i < (int) seq->getNotes().size(); ++i)
            {
                const auto distance = std::abs (seq->getNotes()[(size_t) i].startTick - tick);

                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = i;
                }
            }

            if (best >= 0 && bestDistance < (juce::int64) (20 * ticksPerPixel))
                velocityPreview[best] = velocity;
        }

        dragChangedSomething = true;
        repaint();
    }
}

void PianoRollView::mouseUp (const juce::MouseEvent& event)
{
    if (drag == Drag::marquee && dragChangedSomething == false)
    {
        // plain click on empty space: selection already cleared in mouseDown
    }

    if (drag == Drag::marquee)
    {
        const auto rect = juce::Rectangle<int>::leftTopRightBottom (
            juce::jmin (dragStart.x, event.x), juce::jmin (dragStart.y, event.y),
            juce::jmax (dragStart.x, event.x), juce::jmax (dragStart.y, event.y));

        if (auto seq = sequence())
            for (int i = 0; i < (int) seq->getNotes().size(); ++i)
                if (rect.intersects (noteRect (seq->getNotes()[(size_t) i])))
                    selection.insert (i);
    }
    else if (drag == Drag::move || drag == Drag::resize)
    {
        commitMoveOrResize();
        dragTickOffset = 0;
        dragKeyOffset = 0;
    }
    else if (drag == Drag::velocity)
    {
        commitVelocities();
    }

    drag = Drag::none;
    repaint();
}

void PianoRollView::mouseDoubleClick (const juce::MouseEvent& event)
{
    bool onRightEdge = false;

    if (gridArea().contains (event.getPosition()) && noteIndexAt (event.getPosition(), onRightEdge) < 0)
        addNoteAt (xToTick (event.x), yToKey (event.y));
}

void PianoRollView::mouseMove (const juce::MouseEvent& event)
{
    bool onRightEdge = false;
    noteIndexAt (event.getPosition(), onRightEdge);
    setMouseCursor (onRightEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void PianoRollView::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.mods.isCtrlDown())
    {
        const auto mouseTick = xToTick (event.x);
        ticksPerPixel = juce::jlimit (400.0, 200000.0, ticksPerPixel * (wheel.deltaY > 0 ? 0.8 : 1.25));
        scrollTick = juce::jmax ((juce::int64) 0, mouseTick - (juce::int64) ((event.x - keysWidth) * ticksPerPixel));
    }
    else if (event.mods.isShiftDown())
    {
        scrollTick = juce::jmax ((juce::int64) 0,
                                 scrollTick - (juce::int64) (wheel.deltaY * 40 * ticksPerPixel * 8));
    }
    else
    {
        topKey = juce::jlimit (24, 127, topKey + (wheel.deltaY > 0 ? 2 : -2));
    }

    repaint();
}

bool PianoRollView::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        deleteSelection();
        return true;
    }

    if (key == juce::KeyPress ('z', juce::ModifierKeys::ctrlModifier, 0))
    {
        undoButton.triggerClick();
        return true;
    }

    if (key == juce::KeyPress ('y', juce::ModifierKeys::ctrlModifier, 0)
        || key == juce::KeyPress ('z', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        redoButton.triggerClick();
        return true;
    }

    if (key == juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 0))
    {
        if (auto seq = sequence())
        {
            selection.clear();
            for (int i = 0; i < (int) seq->getNotes().size(); ++i)
                selection.insert (i);
            repaint();
        }
        return true;
    }

    return false;   // space, Home etc. bubble up to the shell
}

//==============================================================================
void PianoRollView::timerCallback()
{
    const auto seq = sequence();

    if (seq != lastSeen)
    {
        lastSeen = seq;

        // Drop selection indices that no longer exist
        const auto noteCount = seq != nullptr ? (int) seq->getNotes().size() : 0;
        std::erase_if (selection, [noteCount] (int index) { return index >= noteCount; });
        repaint();
    }

    undoButton.setEnabled (engine.canUndoClip (trackId));
    redoButton.setEnabled (engine.canRedoClip (trackId));

    // Follow the playhead whenever it moves - during playback or a locate while stopped.
    const auto playhead = engine.getTransport().getPositionTicks();

    if (playhead != lastPlayheadTick && isShowing())
    {
        lastPlayheadTick = playhead;
        repaint();
    }
}

//==============================================================================
void PianoRollView::resized()
{
    auto toolbar = juce::Rectangle<int> (0, 0, getWidth(), toolbarHeight).reduced (6, 3);
    snapBox.setBounds (toolbar.removeFromLeft (90));
    toolbar.removeFromLeft (6);
    quantizeButton.setBounds (toolbar.removeFromLeft (30));
    toolbar.removeFromLeft (12);
    undoButton.setBounds (toolbar.removeFromLeft (52));
    toolbar.removeFromLeft (4);
    redoButton.setBounds (toolbar.removeFromLeft (52));
    trackLabel.setBounds (toolbar);
}

void PianoRollView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1c1f));

    const auto grid = gridArea();
    const auto seq = sequence();
    const auto map = engine.getTransport().getTempoMap();

    // --- Key rows ---
    for (int key = topKey; key >= 0; --key)
    {
        const auto y = keyToY (key);

        if (y > grid.getBottom())
            break;

        g.setColour (isBlackKey (key) ? juce::Colour (0xff202327) : juce::Colour (0xff25282d));
        g.fillRect (grid.getX(), y, grid.getWidth(), keyHeight);

        if (key % 12 == 0)   // C: slightly stronger line
        {
            g.setColour (juce::Colour (0xff15171a));
            g.fillRect (grid.getX(), y + keyHeight - 1, grid.getWidth(), 1);
        }
    }

    // --- Bar/beat lines + ruler ---
    const auto ruler = rulerArea();
    g.setColour (juce::Colour (0xff232529));
    g.fillRect (ruler);

    const auto endTick = xToTick (getWidth());
    auto barTick = map->getBarStart (scrollTick);

    while (barTick < endTick)
    {
        const auto ticksPerBeat = map->getTicksPerBeat (barTick);
        const auto ticksPerBar = map->getTicksPerBar (barTick);

        for (auto beatTick = barTick; beatTick < barTick + ticksPerBar && beatTick < endTick; beatTick += ticksPerBeat)
        {
            const auto x = tickToX (beatTick);

            if (x < grid.getX())
                continue;

            const auto isBar = beatTick == barTick;
            g.setColour (isBar ? juce::Colour (0xff45494f) : juce::Colour (0xff2e3136));
            g.fillRect (x, grid.getY(), 1, grid.getHeight());
            g.fillRect (x, velocityArea().getY(), 1, velocityArea().getHeight());

            if (isBar)
            {
                g.setColour (juce::Colours::lightgrey);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (juce::String (map->ticksToBarsBeats (beatTick).bar),
                            x + 3, ruler.getY() + 6, 40, 14, juce::Justification::left);
            }
        }

        barTick += ticksPerBar;
    }

    // --- Notes ---
    if (seq != nullptr)
    {
        const auto& notes = seq->getNotes();

        for (int i = 0; i < (int) notes.size(); ++i)
        {
            auto note = notes[(size_t) i];
            const auto selected = selection.count (i) > 0;

            if (selected && drag == Drag::move)
            {
                note.startTick += dragTickOffset;
                note.key = juce::jlimit (0, 127, note.key + dragKeyOffset);
            }
            else if (selected && drag == Drag::resize)
            {
                note.lengthTicks = juce::jmax ((juce::int64) 1, note.lengthTicks + dragTickOffset);
            }

            const auto rect = noteRect (note);

            if (! rect.intersects (grid))
                continue;

            const auto velocity = velocityPreview.count (i) ? velocityPreview.at (i) : note.velocity;
            const auto brightness = 0.45f + 0.55f * (float) velocity / 127.0f;

            g.setColour (selected ? juce::Colours::orange.withBrightness (brightness)
                                  : juce::Colour (0xff5d8fc4).withBrightness (brightness));
            g.fillRoundedRectangle (rect.toFloat().reduced (0.5f), 2.0f);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 2.0f, 1.0f);
        }
    }

    // --- Marquee ---
    if (drag == Drag::marquee)
    {
        const auto mouse = getMouseXYRelative();
        const auto rect = juce::Rectangle<int>::leftTopRightBottom (
            juce::jmin (dragStart.x, mouse.x), juce::jmin (dragStart.y, mouse.y),
            juce::jmax (dragStart.x, mouse.x), juce::jmax (dragStart.y, mouse.y));
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (rect);
        g.setColour (juce::Colours::white.withAlpha (0.3f));
        g.drawRect (rect);
    }

    // --- Velocity lane ---
    const auto velocityLane = velocityArea();
    g.setColour (juce::Colour (0xff202227));
    g.fillRect (velocityLane);
    g.setColour (juce::Colour (0xff2e3136));
    g.fillRect (velocityLane.getX(), velocityLane.getY(), velocityLane.getWidth(), 1);

    if (seq != nullptr)
    {
        const auto& notes = seq->getNotes();

        for (int i = 0; i < (int) notes.size(); ++i)
        {
            const auto& note = notes[(size_t) i];
            const auto x = tickToX (note.startTick);

            if (x < velocityLane.getX() || x > velocityLane.getRight())
                continue;

            const auto velocity = velocityPreview.count (i) ? velocityPreview.at (i) : note.velocity;
            const auto height = velocityLane.getHeight() * velocity / 127;

            g.setColour (selection.empty() || selection.count (i)
                             ? (selection.count (i) ? juce::Colours::orange : juce::Colour (0xff5d8fc4))
                             : juce::Colour (0xff3a4654));
            g.fillRect (x, velocityLane.getBottom() - height, 3, height);
        }
    }

    // --- Keys column ---
    const auto keys = keysArea();
    g.setColour (juce::Colour (0xff232529));
    g.fillRect (keys);

    for (int key = topKey; key >= 0; --key)
    {
        const auto y = keyToY (key);

        if (y > keys.getBottom())
            break;

        g.setColour (isBlackKey (key) ? juce::Colour (0xff17191c) : juce::Colour (0xffd8d8d8));
        g.fillRect (keys.getX(), y, keys.getWidth() - 2, keyHeight - 1);

        if (key % 12 == 0)
        {
            g.setColour (juce::Colours::grey);
            g.setFont (juce::FontOptions (10.0f));
            g.drawText ("C" + juce::String (key / 12 - 1), keys.getX() + 2, y, keys.getWidth() - 8, keyHeight,
                        juce::Justification::centredRight);
        }
    }

    // --- Playhead ---
    const auto playheadX = tickToX (engine.getTransport().getPositionTicks());

    if (playheadX >= grid.getX() && playheadX <= getWidth())
    {
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillRect (playheadX, ruler.getY(), 1, getHeight() - ruler.getY() );
    }

    // --- Empty hint ---
    if (seq == nullptr || seq->getNotes().empty())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("Double-click to add notes - or record something",
                    grid, juce::Justification::centred);
    }
}
