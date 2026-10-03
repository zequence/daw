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

    juce::int64 divisionToTicks (int comboId)
    {
        switch (comboId)
        {
            case 1: return 4 * Q;       // 1/1
            case 2: return 2 * Q;
            case 3: return Q;
            case 4: return Q / 2;
            case 5: return Q / 4;
            case 6: return Q / 8;       // 1/32
            default: return Q / 2;
        }
    }

    void addDivisionItems (juce::ComboBox& box)
    {
        box.addItem ("1/1", 1);
        box.addItem ("1/2", 2);
        box.addItem ("1/4", 3);
        box.addItem ("1/8", 4);
        box.addItem ("1/16", 5);
        box.addItem ("1/32", 6);
    }

    juce::String controllerName (int cc)
    {
        switch (cc)
        {
            case 1:  return "CC1 Mod Wheel";
            case 2:  return "CC2 Breath";
            case 7:  return "CC7 Volume";
            case 10: return "CC10 Pan";
            case 11: return "CC11 Expression";
            case 64: return "CC64 Sustain";
            default: return "CC" + juce::String (cc);
        }
    }
}

//==============================================================================
PianoRollView::PianoRollView (AudioEngine& e, CommandDispatcher& d)
    : engine (e), dispatcher (d)
{
    setWantsKeyboardFocus (true);

    modeBox.setTooltip ("Edit mode (keys: S = select, D = draw). Select: drag selects, double-click adds. "
                        "Draw: click adds a note at the length dropdown's value, keep dragging to stretch it.");
    modeBox.addItem ("Select", 1);
    modeBox.addItem ("Draw", 2);
    modeBox.setSelectedId (1, juce::dontSendNotification);
    addAndMakeVisible (modeBox);

    snapToggle.setTooltip ("Snap to grid: new notes, moves and resizes lock to the grid division. "
                           "Off: notes can be drawn and moved freely between grid lines");
    snapToggle.setClickingTogglesState (true);
    snapToggle.setToggleState (true, juce::dontSendNotification);
    snapToggle.setColour (juce::TextButton::buttonOnColourId, juce::Colours::steelblue);
    addAndMakeVisible (snapToggle);

    snapBox.setTooltip ("Grid division (snapping and quantize)");
    addDivisionItems (snapBox);
    snapBox.setSelectedId (4, juce::dontSendNotification);     // 1/8
    addAndMakeVisible (snapBox);

    lengthBox.setTooltip ("Length of newly added notes");
    addDivisionItems (lengthBox);
    lengthBox.setSelectedId (4, juce::dontSendNotification);   // 1/8
    addAndMakeVisible (lengthBox);

    laneBox.setTooltip ("What the lane below the grid shows and edits");
    addAndMakeVisible (laneBox);
    rebuildLaneBox();

    quantizeButton.setTooltip ("Quantize selected notes (or all) to the grid division");
    quantizeButton.onClick = [this]
    {
        const auto grid = gridTicks();

        if (grid <= 0 || sequence() == nullptr)
            return;

        if (! selection.empty())
        {
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

        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        params->setProperty ("grid", grid);
        runCommand ("clip.quantize", params);
    };
    addAndMakeVisible (quantizeButton);

    undoButton.onClick = [this]
    {
        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        runCommand ("clip.undo", params);
        selection.clear();
    };
    addAndMakeVisible (undoButton);

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

    for (auto* c : std::initializer_list<juce::Component*> { &modeBox, &snapToggle, &snapBox, &lengthBox,
                                                             &laneBox, &quantizeButton, &undoButton, &redoButton })
        c->setWantsKeyboardFocus (false);

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
        rebuildLaneBox();
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
             getHeight() - toolbarHeight - rulerHeight - laneHeight };
}

juce::Rectangle<int> PianoRollView::gridArea() const
{
    return { keysWidth, toolbarHeight + rulerHeight, getWidth() - keysWidth,
             getHeight() - toolbarHeight - rulerHeight - laneHeight };
}

juce::Rectangle<int> PianoRollView::laneArea() const
{
    return { keysWidth, getHeight() - laneHeight, getWidth() - keysWidth, laneHeight };
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

juce::int64 PianoRollView::gridTicks() const
{
    return divisionToTicks (snapBox.getSelectedId());
}

juce::int64 PianoRollView::newNoteTicks() const
{
    return divisionToTicks (lengthBox.getSelectedId());
}

juce::int64 PianoRollView::snapTicksOrZero() const
{
    return snapToggle.getToggleState() ? gridTicks() : 0;
}

juce::int64 PianoRollView::snapTick (juce::int64 tick) const
{
    const auto grid = snapTicksOrZero();
    return grid > 0 ? ((tick + grid / 2) / grid) * grid : tick;
}

//==============================================================================
PianoRollView::LaneMode PianoRollView::laneMode() const
{
    const auto id = laneBox.getSelectedId();
    return id == 1 ? LaneMode::velocity : (id == 2 ? LaneMode::pitchBend : LaneMode::controller);
}

int PianoRollView::laneControllerNumber() const
{
    return laneBox.getSelectedId() >= 100 ? laneBox.getSelectedId() - 100 : 1;
}

int PianoRollView::laneValueMax() const
{
    return laneMode() == LaneMode::pitchBend ? 16383 : 127;
}

int PianoRollView::laneValueFromY (int y) const
{
    const auto area = laneArea();
    return juce::jlimit (0, laneValueMax(),
                         laneValueMax() - (y - area.getY()) * laneValueMax() / juce::jmax (1, area.getHeight()));
}

int PianoRollView::laneValueToY (int value) const
{
    const auto area = laneArea();
    return area.getBottom() - area.getHeight() * value / juce::jmax (1, laneValueMax());
}

void PianoRollView::rebuildLaneBox()
{
    // Standard orchestral set plus whatever the clip already contains
    std::vector<int> ccs { 1, 2, 7, 10, 11, 64 };

    if (auto seq = sequence())
        for (auto& control : seq->getControls())
            if (control.type == MidiSequence::ControlType::controller
                 && std::find (ccs.begin(), ccs.end(), control.number) == ccs.end())
                ccs.push_back (control.number);

    std::sort (ccs.begin() + 6, ccs.end());

    if (ccs == lastCcList && laneBox.getNumItems() > 0)
        return;

    lastCcList = ccs;
    const auto previous = laneBox.getSelectedId();

    laneBox.clear (juce::dontSendNotification);
    laneBox.addItem ("Velocity", 1);
    laneBox.addItem ("Pitch Bend", 2);

    for (auto cc : ccs)
        laneBox.addItem (controllerName (cc), 100 + cc);

    laneBox.setSelectedId (previous != 0 && laneBox.indexOfItemId (previous) >= 0 ? previous : 1,
                           juce::dontSendNotification);
}

void PianoRollView::commitLaneGesture()
{
    if (laneMode() == LaneMode::velocity || gestureMinTick < 0)
        return;

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("type", laneMode() == LaneMode::pitchBend ? 1 : 0);
    params->setProperty ("number", laneControllerNumber());
    params->setProperty ("start", gestureMinTick);
    params->setProperty ("end", gestureMaxTick + laneDrawQuantum);

    juce::Array<juce::var> events;

    if (! laneErasing)
    {
        for (auto& [tick, value] : laneGesture)
        {
            auto event = new juce::DynamicObject();
            event->setProperty ("tick", tick);
            event->setProperty ("value", value);
            events.add (juce::var (event));
        }
    }

    params->setProperty ("events", events);
    runCommand ("clip.setControlRange", params);

    laneGesture.clear();
    gestureMinTick = gestureMaxTick = -1;
    laneErasing = false;
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
    auto note = new juce::DynamicObject();
    note->setProperty ("start", snapTick (tick));
    note->setProperty ("length", newNoteTicks());
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

void PianoRollView::nudgeSelection (juce::int64 tickDelta, int keyDelta)
{
    const auto seq = sequence();

    if (seq == nullptr || selection.empty())
        return;

    juce::Array<juce::var> edits;
    std::vector<MidiSequence::Note> wanted;

    for (auto index : selection)
    {
        if (index >= (int) seq->getNotes().size())
            continue;

        auto note = seq->getNotes()[(size_t) index];
        note.startTick = juce::jmax ((juce::int64) 0, note.startTick + tickDelta);
        note.key = juce::jlimit (0, 127, note.key + keyDelta);

        auto edit = new juce::DynamicObject();
        edit->setProperty ("index", index);
        edit->setProperty ("start", note.startTick);
        edit->setProperty ("key", note.key);
        edits.add (juce::var (edit));
        wanted.push_back (note);
    }

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("notes", edits);
    runCommand ("clip.updateNotes", params);
    reselectByValue (wanted);
    repaint();
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

    if (laneArea().contains (position))
    {
        drag = Drag::lane;
        laneGesture.clear();
        gestureMinTick = gestureMaxTick = -1;
        laneErasing = event.mods.isPopupMenu() && laneMode() != LaneMode::velocity;
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
    else if (modeBox.getSelectedId() == 2)
    {
        // Draw mode: add a note right here; keep dragging to stretch it.
        addNoteAt (xToTick (position.x), yToKey (position.y));
        drag = Drag::resize;
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
    else if (drag == Drag::lane)
    {
        if (laneMode() == LaneMode::velocity)
        {
            const auto seq = sequence();

            if (seq == nullptr)
                return;

            const auto velocity = laneValueFromY (position.y);
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
        }
        else
        {
            // CC / pitch bend drawing (or erasing with the right button)
            const auto tick = juce::jmax ((juce::int64) 0,
                                          (xToTick (position.x) / laneDrawQuantum) * laneDrawQuantum);

            gestureMinTick = gestureMinTick < 0 ? tick : juce::jmin (gestureMinTick, tick);
            gestureMaxTick = juce::jmax (gestureMaxTick, tick);

            if (! laneErasing)
                laneGesture[tick] = laneValueFromY (position.y);
        }

        dragChangedSomething = true;
        repaint();
    }
}

void PianoRollView::mouseUp (const juce::MouseEvent& event)
{
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
    else if (drag == Drag::lane)
    {
        if (laneMode() == LaneMode::velocity)
            commitVelocities();
        else
            commitLaneGesture();
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

    // Arrow keys nudge the selection: up/down transpose a half step (Ctrl = octave),
    // left/right move by the grid division.
    if (! selection.empty())
    {
        const auto octave = key.getModifiers().isCtrlDown() ? 12 : 1;

        if (key.isKeyCode (juce::KeyPress::upKey))    { nudgeSelection (0, octave);  return true; }
        if (key.isKeyCode (juce::KeyPress::downKey))  { nudgeSelection (0, -octave); return true; }
        if (key.isKeyCode (juce::KeyPress::leftKey))  { nudgeSelection (-gridTicks(), 0); return true; }
        if (key.isKeyCode (juce::KeyPress::rightKey)) { nudgeSelection (gridTicks(), 0);  return true; }
    }

    if (key == juce::KeyPress ('s'))
    {
        modeBox.setSelectedId (1);
        return true;
    }

    if (key == juce::KeyPress ('d'))
    {
        modeBox.setSelectedId (2);
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
        rebuildLaneBox();
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
    modeBox.setBounds (toolbar.removeFromLeft (78));
    toolbar.removeFromLeft (10);
    snapToggle.setBounds (toolbar.removeFromLeft (52));
    toolbar.removeFromLeft (4);
    snapBox.setBounds (toolbar.removeFromLeft (68));
    toolbar.removeFromLeft (10);
    lengthBox.setBounds (toolbar.removeFromLeft (68));
    toolbar.removeFromLeft (10);
    quantizeButton.setBounds (toolbar.removeFromLeft (30));
    toolbar.removeFromLeft (12);
    undoButton.setBounds (toolbar.removeFromLeft (52));
    toolbar.removeFromLeft (4);
    redoButton.setBounds (toolbar.removeFromLeft (52));
    toolbar.removeFromLeft (12);
    laneBox.setBounds (toolbar.removeFromLeft (140));
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

        if (key % 12 == 0)
        {
            g.setColour (juce::Colour (0xff15171a));
            g.fillRect (grid.getX(), y + keyHeight - 1, grid.getWidth(), 1);
        }
    }

    // --- Bar/beat lines + ruler ---
    const auto ruler = rulerArea();
    g.setColour (juce::Colour (0xff232529));
    g.fillRect (ruler);

    const auto lane = laneArea();
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
            g.fillRect (x, lane.getY(), 1, lane.getHeight());

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

    // --- Lane ---
    g.setColour (juce::Colour (0xff202227));
    g.fillRect (lane);
    g.setColour (juce::Colour (0xff2e3136));
    g.fillRect (lane.getX(), lane.getY(), lane.getWidth(), 1);

    if (laneMode() == LaneMode::velocity)
    {
        if (seq != nullptr)
        {
            const auto& notes = seq->getNotes();

            for (int i = 0; i < (int) notes.size(); ++i)
            {
                const auto& note = notes[(size_t) i];
                const auto x = tickToX (note.startTick);

                if (x < lane.getX() || x > lane.getRight())
                    continue;

                const auto velocity = velocityPreview.count (i) ? velocityPreview.at (i) : note.velocity;
                const auto height = lane.getHeight() * velocity / 127;

                g.setColour (selection.empty() || selection.count (i)
                                 ? (selection.count (i) ? juce::Colours::orange : juce::Colour (0xff5d8fc4))
                                 : juce::Colour (0xff3a4654));
                g.fillRect (x, lane.getBottom() - height, 3, height);
            }
        }
    }
    else
    {
        // CC / pitch bend: step line with points
        if (seq != nullptr)
        {
            const auto wantedType = laneMode() == LaneMode::pitchBend ? MidiSequence::ControlType::pitchBend
                                                                      : MidiSequence::ControlType::controller;
            const auto wantedNumber = laneControllerNumber();

            int previousX = -1, previousY = -1;
            g.setColour (juce::Colour (0xff5d8fc4));

            for (auto& control : seq->getControls())
            {
                if (control.type != wantedType
                     || (wantedType == MidiSequence::ControlType::controller && control.number != wantedNumber))
                    continue;

                const auto x = tickToX (control.tick);
                const auto y = laneValueToY (control.value);

                if (previousX >= 0 && x >= lane.getX())
                {
                    g.fillRect (juce::jmax (lane.getX(), previousX), previousY, juce::jmax (1, x - previousX), 2);
                    g.fillRect (x, juce::jmin (previousY, y), 2, std::abs (y - previousY) + 2);
                }

                if (x >= lane.getX() && x <= lane.getRight())
                    g.fillRect (x - 1, y - 1, 4, 4);

                previousX = x;
                previousY = y;

                if (x > lane.getRight())
                    break;
            }

            // Hold the last value to the right edge
            if (previousX >= 0 && previousX < lane.getRight())
                g.fillRect (juce::jmax (lane.getX(), previousX), previousY,
                            lane.getRight() - juce::jmax (lane.getX(), previousX), 2);
        }

        // Gesture overlay
        if (drag == Drag::lane && ! laneErasing)
        {
            g.setColour (juce::Colours::orange);

            for (auto& [tick, value] : laneGesture)
                g.fillRect (tickToX (tick) - 1, laneValueToY (value) - 1, 3, 3);
        }
        else if (drag == Drag::lane && laneErasing && gestureMinTick >= 0)
        {
            g.setColour (juce::Colours::red.withAlpha (0.25f));
            g.fillRect (tickToX (gestureMinTick), lane.getY(),
                        juce::jmax (2, tickToX (gestureMaxTick) - tickToX (gestureMinTick)), lane.getHeight());
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
        g.fillRect (playheadX, ruler.getY(), 1, getHeight() - ruler.getY());
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
