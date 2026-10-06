#include "PianoRollView.h"
#include "ThemedLookAndFeel.h"
#include "EditorSettings.h"
#include "../model/ArticulationMenu.h"
#include "../model/NoteNames.h"
#include "ArticulationPanel.h"
#include "Smufl.h"
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
            case 7: return Q / 16;      // 1/64
            case 8: return Q / 32;      // 1/128
            case 9: return Q / 64;      // 1/256
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
        box.addItem ("1/64", 7);
        box.addItem ("1/128", 8);
        box.addItem ("1/256", 9);
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
PianoRollView::PianoRollView (AudioEngine& e, CommandDispatcher& d, TimeAxis& a)
    : engine (e), dispatcher (d), axis (a)
{
    setWantsKeyboardFocus (true);


    snapBox.setTooltip ("Grid division (snapping and quantize)");
    addDivisionItems (snapBox);
    snapBox.setSelectedId (4, juce::dontSendNotification);     // 1/8
    addAndMakeVisible (snapBox);

    lengthBox.setTooltip ("Length of newly added notes");
    addDivisionItems (lengthBox);
    lengthBox.setSelectedId (4, juce::dontSendNotification);   // 1/8
    lengthBox.onChange = [this] { setNoteDots (0); };          // a new length starts undotted
    addAndMakeVisible (lengthBox);

    // Dotted lengths: one dot (x1.5) or two (x1.75); click toggles one, shift-click two.
    // In note input: "." and Shift+"."
    dotButton.setTooltip ("Dotted note length: click for a dot (x1.5), shift-click for a double dot (x1.75). "
                          "With Input on: the . key, Shift+. for double. A new length clears it.");
    dotButton.onClick = [this] { toggleDots (juce::ModifierKeys::getCurrentModifiers().isShiftDown() ? 2 : 1); };
    theme::setButtonRole (dotButton, "accent");
    setNoteDots (0);
    addAndMakeVisible (dotButton);

    laneBox.setTooltip ("What the lane below the grid shows and edits");
    addAndMakeVisible (laneBox);
    rebuildLaneBox();

    colourBox.addItem ("Colour: velocity", 1);
    colourBox.addItem ("Colour: sound slot", 2);
    colourBox.setSelectedId (editorSettings::coloursBySlot (engine.getSettingsFile()) ? 2 : 1, juce::dontSendNotification);
    colourBox.setTooltip ("What colours the notes: their velocity, or the colour of their articulation's sound slot "
                          "(set in the expression map)");
    colourBox.setWantsKeyboardFocus (false);
    colourBox.onChange = [this]
    {
        engine.getSettingsFile().setValue (editorSettings::noteColoursKey, colourBox.getSelectedId() == 2 ? "slot" : "velocity");
        engine.getSettingsFile().saveIfNeeded();
        repaint();
    };
    addAndMakeVisible (colourBox);

    articulationButton.setTooltip ("Articulation");
    articulationButton.onClick = [this] { showArticulationMenu(); };
    addAndMakeVisible (articulationButton);

    auditionToggle.setTooltip ("Play notes when added (through the armed track's instrument)");
    auditionToggle.setClickingTogglesState (true);
    auditionToggle.setToggleState (true, juce::dontSendNotification);
    theme::setButtonRole (auditionToggle, "accent");
    addAndMakeVisible (auditionToggle);

    // Note input: play notes on the MIDI keyboard to write them at the playhead (transport stopped)
    inputToggle.setTooltip ("Note input (N): notes played on the MIDI keyboard are written at the playhead with the note length "
                            "and articulation chosen here, and the playhead moves on. Notes played together (within "
                            + juce::String ((int) chordWindowMs) + " ms of the first) make a chord. Works while stopped.");
    inputToggle.setClickingTogglesState (true);
    inputToggle.setToggleState (false, juce::dontSendNotification);
    theme::setButtonRole (inputToggle, "accent");
    inputToggle.onClick = [this]
    {
        engine.setNoteInputListening (inputToggle.getToggleState());
        chordTick = -1;
    };
    addAndMakeVisible (inputToggle);

    editAllToggle.setTooltip ("Edit all the shown tracks: select and move notes of every track at once (the others stay "
                              "dimmed; clicking one of their notes focuses that track). Off: only the focused track is edited.");
    editAllToggle.setClickingTogglesState (true);
    theme::setButtonRole (editAllToggle, "accent");
    editAllToggle.onClick = [this]
    {
        editAll = editAllToggle.getToggleState();

        if (! editAll)
            otherSelections.clear();

        repaint();
    };
    addAndMakeVisible (editAllToggle);
    addAndMakeVisible (closeButton);

    engine.onNoteInput = [safe = juce::Component::SafePointer<PianoRollView> (this)] (const juce::MidiMessage& message, double receivedMs)
    {
        if (safe != nullptr)
            safe->noteInput (message, receivedMs);
    };

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

    undoButton.setTooltip ("Undo the last clip edit on this track (Ctrl+Z)");
    redoButton.setTooltip ("Redo (Ctrl+Y / Ctrl+Shift+Z)");

    undoButton.onClick = [this]
    {
        const auto before = sequence();
        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        runCommand ("clip.undo", params);
        selection.clear();

        // Undoing a note written by note input: the line goes back to where it was written
        for (auto& step : inputSteps)
            if (before != nullptr && step.result == before && sequence() != before)
            {
                engine.getTransport().locate (step.lineBefore);
                chordTick = -1;
            }
    };
    addAndMakeVisible (undoButton);

    redoButton.onClick = [this]
    {
        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        runCommand ("clip.redo", params);
        selection.clear();

        for (auto& step : inputSteps)
            if (step.result != nullptr && step.result == sequence())
            {
                engine.getTransport().locate (step.lineAfter);
                chordTick = -1;
            }
    };
    addAndMakeVisible (redoButton);

    editTargetBox.setTooltip ("What is edited: the track - and where its regions overlap, which clip (the others are dimmed)");
    editTargetBox.setWantsKeyboardFocus (false);
    editTargetBox.onChange = [this]
    {
        const auto index = editTargetBox.getSelectedId() - 1;

        if (index >= 0 && index < (int) editTargets.size())
        {
            const auto [track, region] = editTargets[(size_t) index];
            setTrack (track);           // one of the shown: they stay shown
            activeRegion = region;
            selection.clear();
            targetsKey.clear();
            rebuildEditTargets();

            if (onEditedTrackChanged)
                onEditedTrackChanged (track);
        }
    };
    addAndMakeVisible (editTargetBox);

    for (auto* c : std::initializer_list<juce::Component*> { &auditionToggle, &inputToggle, &snapBox,
                                                             &lengthBox, &dotButton, &laneBox, &quantizeButton, &undoButton,
                                                             &redoButton, &articulationButton, &colourBox })
        c->setWantsKeyboardFocus (false);

    startTimerHz (30);
}

PianoRollView::~PianoRollView()
{
    engine.setNoteInputListening (false);
    engine.onNoteInput = nullptr;
}

// Note input: a played note is written at the playhead (the chord's position) with the note length
// and the articulation for new notes. The first note of a chord moves the playhead on by the
// length; notes within chordWindowMs of it join the chord at the same position. Nothing is
// selected, so choosing another articulation sets the next notes', not the ones just written.
void PianoRollView::noteInput (const juce::MidiMessage& message, double receivedMs)
{
    auto& transport = engine.getTransport();

    if (! inputToggle.getToggleState() || ! isShowing() || trackId == 0 || transport.isPlaying())
        return;

    const auto length = newNoteTicks();
    const auto joinsChord = chordTick >= 0 && receivedMs - chordStartMs <= chordWindowMs;

    const auto lineBefore = transport.getPositionTicks();

    if (! joinsChord)
    {
        chordTick = transport.getPositionTicks();
        chordStartMs = receivedMs;
        transport.locate (chordTick + length);
        selection.clear();
    }

    auto note = new juce::DynamicObject();
    note->setProperty ("start", chordTick);
    note->setProperty ("length", length);
    note->setProperty ("key", message.getNoteNumber());
    note->setProperty ("velocity", juce::jlimit (1, 127, (int) message.getVelocity()));

    if (activeRegion >= 0)
        note->setProperty ("region", activeRegion);

    if (! newNoteArticulation.isEmpty() && engine.getTrackExpressionMap (trackId).has_value())
        note->setProperty ("articulation", newNoteArticulation.toVar());

    juce::Array<juce::var> notes;
    notes.add (juce::var (note));

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("notes", notes);
    runCommand ("clip.addNotes", params);

    inputSteps.push_back ({ sequence(), lineBefore, transport.getPositionTicks() });

    if (inputSteps.size() > 500)
        inputSteps.erase (inputSteps.begin());

    repaint();
}

void PianoRollView::setTrack (AudioEngine::TrackId id)
{
    if (! isShown (id))
        shownTracks = { id };   // not one of the shown tracks: just this one

    if (trackId != id)
    {
        targetsKey.clear();
        trackId = id;
        selection.clear();
        activeRegion = -1;   // another track: its own regions (the dropdown follows)
        targetRegions.clear();
        newNoteArticulation = {};   // another track, maybe another map
        articulationKey.clear();
        drag = Drag::none;
        rebuildLaneBox();

        // Synchron players: fetch the playable range once (cached in the
        // project); it lands as an engine change, which repaints us
        if (auto info = engine.getTrackChannelInfo (trackId);
            info.has_value() && info->veproChannelAddress.isNotEmpty() && info->keyLow < 0)
        {
            auto params = juce::DynamicObject::Ptr (new juce::DynamicObject());
            params->setProperty ("trackId", trackId);
            auto message = juce::DynamicObject::Ptr (new juce::DynamicObject());
            message->setProperty ("cmd", "vepro.keyRange");
            message->setProperty ("params", juce::var (params.get()));
            dispatcher.dispatchParsed (juce::var (message.get()), [] (const juce::var&) {});
        }
    }

    rebuildEditTargets();
    repaint();
}

//==============================================================================
juce::Rectangle<int> PianoRollView::keysArea() const
{
    return { 0, toolbarHeight, keysWidth,
             getHeight() - toolbarHeight - laneHeight };
}

juce::Rectangle<int> PianoRollView::gridArea() const
{
    return { keysWidth, toolbarHeight, getWidth() - keysWidth,
             getHeight() - toolbarHeight - laneHeight };
}

juce::Rectangle<int> PianoRollView::laneArea() const
{
    return { keysWidth, getHeight() - laneHeight, getWidth() - keysWidth, laneHeight };
}

juce::int64 PianoRollView::xToTick (int x) const
{
    return axis.xToTick (x);
}

int PianoRollView::tickToX (juce::int64 tick) const
{
    return axis.tickToX (tick);
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
    const auto base = divisionToTicks (lengthBox.getSelectedId());
    return base + (noteDots >= 1 ? base / 2 : 0) + (noteDots >= 2 ? base / 4 : 0);
}

juce::int64 PianoRollView::snapTicksOrZero() const
{
    return axis.snap ? gridTicks() : 0;
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

//==============================================================================
// Articulations (the rules are in model/ArticulationMenu.h; this is the view)
const ExpressionMap* PianoRollView::currentMap()
{
    if (cachedMapTrack != trackId || cachedMapRevision != engine.getStateRevision())
    {
        cachedMapTrack = trackId;
        cachedMapRevision = engine.getStateRevision();
        cachedMap = engine.getTrackExpressionMap (trackId);
    }

    return cachedMap.has_value() ? &*cachedMap : nullptr;
}

std::vector<int> PianoRollView::selectedNoteIndices() const
{
    std::vector<int> indices;

    if (auto seq = sequence())
        for (auto index : selection)
            if (index >= 0 && index < (int) seq->getNotes().size())
                indices.push_back (index);

    return indices;
}

std::vector<ExpressionMap::Selection> PianoRollView::articulationTargets (const ExpressionMap& map) const
{
    const auto useFirstRoot = editorSettings::firstRootIsDefault (engine.getSettingsFile());
    std::vector<ExpressionMap::Selection> targets;

    if (auto seq = sequence())
        for (auto index : selectedNoteIndices())
            targets.push_back (articulations::effective (map, seq->getNotes()[(size_t) index].articulation, useFirstRoot));

    if (targets.empty())   // nothing selected: the choice new notes are drawn with
        targets.push_back (articulations::effective (map, newNoteArticulation, useFirstRoot));

    return targets;
}

void PianoRollView::refreshArticulationButton()
{
    const auto info = engine.getTrackChannelInfo (trackId);
    const auto mapName = info.has_value() ? info->expressionMap : juce::String();
    const auto useFirstRoot = editorSettings::firstRootIsDefault (engine.getSettingsFile());

    // Only rebuild when something it depends on changed (this runs on the 30 Hz timer)
    auto key = juce::String (trackId) + "|" + mapName + "|" + juce::String (engine.getStateRevision())
                 + "|" + juce::String ((int) useFirstRoot) + "|" + articulations::label (newNoteArticulation);

    for (auto index : selection)
        key << "," << index;

    if (key == articulationKey)
        return;

    articulationKey = key;

    if (mapName.isEmpty())
    {
        articulationButton.setButtonText ("No expression map");
        articulationButton.setTooltip ("This track's instrument channel has no expression map. "
                                       "Assign one with instrument.setChannelMap.");
        articulationButton.setEnabled (false);
        return;
    }

    const auto map = engine.getTrackExpressionMap (trackId);

    if (! map.has_value())
    {
        articulationButton.setButtonText ("Missing map: " + mapName);
        articulationButton.setTooltip ("The instrument channel uses the expression map '" + mapName
                                       + "', which is not in the project. Notes keep their articulations.");
        articulationButton.setEnabled (false);
        return;
    }

    const auto targets = articulationTargets (*map);
    const auto notesSelected = ! selectedNoteIndices().empty();
    auto text = articulations::allEqual (targets) ? articulations::label (targets.front()) : juce::String ("Mixed");

    if (text.isEmpty())
        text = "No articulation";

    // An articulation the map doesn't have (or that doesn't fit) is a visible error
    if (std::any_of (targets.begin(), targets.end(), [&] (const auto& s) { return ! articulations::resolves (*map, s); }))
        text += "  (!)";

    articulationButton.setButtonText (text);
    articulationButton.setTooltip (notesSelected ? "Articulation of the selected notes (map: " + map->name + ")"
                                                 : "Articulation new notes are drawn with (map: " + map->name
                                                     + "). Select notes to change theirs.");
    articulationButton.setEnabled (true);
}

void PianoRollView::showArticulationMenu()
{
    const auto map = engine.getTrackExpressionMap (trackId);

    if (! map.has_value())
        return;

    // Columns (one per group) that stay open and follow each choice
    const auto safe = juce::Component::SafePointer<PianoRollView> (this);

    auto panel = std::make_unique<ArticulationPanel> (
        [safe]() -> ArticulationPanel::Items
        {
            if (safe == nullptr)
                return {};

            const auto current = safe->engine.getTrackExpressionMap (safe->trackId);
            return current.has_value() ? articulations::buildMenu (*current, safe->articulationTargets (*current))
                                       : ArticulationPanel::Items();
        },
        [safe] (const juce::String& group, const juce::String& name)
        {
            if (safe != nullptr)
                safe->chooseArticulation (group, name);
        });

    juce::CallOutBox::launchAsynchronously (std::move (panel), articulationButton.getScreenBounds(), nullptr);
}

std::optional<ExpressionMap::Selection> PianoRollView::remoteChoose (const juce::String& group, const juce::String& name)
{
    const auto notesSelected = ! selectedNoteIndices().empty();
    chooseArticulation (group, name);

    if (notesSelected)
        return std::nullopt;

    return newNoteArticulation;
}

void PianoRollView::chooseArticulation (const juce::String& group, const juce::String& name)
{
    const auto map = engine.getTrackExpressionMap (trackId);

    if (! map.has_value())
        return;

    const auto applied = articulations::apply (*map, articulationTargets (*map), group, name);

    if (! applied.ok())
    {
        juce::Logger::writeToLog ("PianoRoll: articulation refused: " + applied.error);
        return;
    }

    const auto indices = selectedNoteIndices();

    // Choosing another root can drop modifiers that no longer apply: ask first (the
    // default) or just do it - Settings > Editor > Midi. Not for the pen's own choice.
    if (! applied.dropped.empty() && ! indices.empty() && editorSettings::askBeforeDropping (engine.getSettingsFile()))
    {
        juce::StringArray names;

        for (auto& [droppedGroup, droppedName] : applied.dropped)
            names.add (droppedName + " (" + droppedGroup + ")");

        const auto where = indices.size() == 1 ? juce::String ("the note") : juce::String ((int) indices.size()) + " notes";

        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Drop modifiers?",
                                            "Choosing '" + name + "' removes " + names.joinIntoString (", ") + " from " + where
                                              + ", because " + (applied.dropped.size() == 1 ? "it does" : "they do") + " not apply to it.",
                                            "Drop and change", "Cancel", this,
                                            juce::ModalCallbackFunction::create (
                                                [safe = juce::Component::SafePointer<PianoRollView> (this), results = applied.results] (int result)
                                                {
                                                    if (result == 1 && safe != nullptr)
                                                        safe->commitArticulations (results);
                                                }));
        return;
    }

    commitArticulations (applied.results);
}

void PianoRollView::commitArticulations (const std::vector<ExpressionMap::Selection>& results)
{
    const auto indices = selectedNoteIndices();

    // The player switches at once (its sound slot is sent to the instrument), so the chosen
    // articulation can be played live - from the panel, a key command or a MIDI controller
    if (! results.empty())
        if (const auto map = engine.getTrackExpressionMap (trackId))
            engine.sendLiveArticulation (trackId, map->outputsOf (results.front()));

    if (indices.empty())
    {
        if (! results.empty())
            newNoteArticulation = results.front();

        articulationKey.clear();
        repaint();
        return;
    }

    auto seq = sequence();

    if (seq == nullptr || results.size() != indices.size())
        return;   // the selection changed under the prompt

    // One clip.updateNotes: one undo step for all the notes
    juce::Array<juce::var> edits;

    for (size_t i = 0; i < indices.size(); ++i)
    {
        if (results[i] == seq->getNotes()[(size_t) indices[i]].articulation)
            continue;

        auto edit = new juce::DynamicObject();
        edit->setProperty ("index", indices[i]);
        edit->setProperty ("articulation", results[i].toVar());
        edits.add (juce::var (edit));
    }

    if (edits.isEmpty())
        return;

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("notes", edits);
    runCommand ("clip.updateNotes", params);
    articulationKey.clear();
}

void PianoRollView::auditionNote (int key, int velocity)
{
    if (! auditionToggle.getToggleState())
        return;

    auto on = juce::MidiMessage::noteOn (1, juce::jlimit (0, 127, key), (juce::uint8) juce::jlimit (1, 127, velocity));
    on.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    engine.getLiveMidiCollector().addMessageToQueue (on);

    juce::Timer::callAfterDelay (250,
        [safe = juce::Component::SafePointer<PianoRollView> (this), key]
        {
            if (safe == nullptr)
                return;

            auto off = juce::MidiMessage::noteOff (1, juce::jlimit (0, 127, key));
            off.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
            safe->engine.getLiveMidiCollector().addMessageToQueue (off);
        });
}

void PianoRollView::commitNewNote (const MidiSequence::Note& newNote)
{
    auto note = new juce::DynamicObject();
    note->setProperty ("start", newNote.startTick);
    note->setProperty ("length", newNote.lengthTicks);
    note->setProperty ("key", juce::jlimit (0, 127, newNote.key));
    note->setProperty ("velocity", newNote.velocity);

    if (activeRegion >= 0)
        note->setProperty ("region", activeRegion);

    // New notes get the articulation chosen for them (nothing is written when none is chosen:
    // the default-root setting is implicit)
    if (! newNoteArticulation.isEmpty() && engine.getTrackExpressionMap (trackId).has_value())
        note->setProperty ("articulation", newNoteArticulation.toVar());

    juce::Array<juce::var> notes;
    notes.add (juce::var (note));

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("notes", notes);
    runCommand ("clip.addNotes", params);

    // Select the new note
    if (auto seq = sequence())
    {
        selection.clear();

        for (int i = 0; i < (int) seq->getNotes().size(); ++i)
            if (seq->getNotes()[(size_t) i].startTick == newNote.startTick
                 && seq->getNotes()[(size_t) i].key == newNote.key)
                selection.insert (i);
    }
}

void PianoRollView::addNoteAt (juce::int64 tick, int key)
{
    auditionNote (key, 96);
    commitNewNote ({ snapTick (tick), newNoteTicks(), 1, key, 96 });
}

void PianoRollView::deleteSelection()
{
    if (! anySelected())
        return;

    juce::Array<juce::var> indices;
    for (auto index : selection)
        indices.add (index);

    AudioEngine::ScopedUndoGroup group (engine);   // one undo for every track

    if (! selection.empty())   // the focused track's part (edit all may have only others)
    {
        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        params->setProperty ("indices", indices);
        runCommand ("clip.removeNotes", params);
        selection.clear();
    }

    forOtherSelections ([this] (AudioEngine::TrackId other, const MidiSequence&, std::set<int>& chosen)
    {
        juce::Array<juce::var> otherIndices;

        for (auto index : chosen)
            otherIndices.add (index);

        auto otherParams = new juce::DynamicObject();
        otherParams->setProperty ("trackId", other);
        otherParams->setProperty ("indices", otherIndices);
        runCommand ("clip.removeNotes", otherParams);
        chosen.clear();
    });
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

    if (seq == nullptr || ! anySelected() || (dragTickOffset == 0 && dragKeyOffset == 0))
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

    AudioEngine::ScopedUndoGroup group (engine);   // one undo for every track

    if (! selection.empty())   // the focused track's part (edit all may have only others)
    {
        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        params->setProperty ("notes", edits);
        runCommand ("clip.updateNotes", params);
        reselectByValue (wanted);
    }

    // Edit all: the other tracks' selected notes move (or resize) the same
    forOtherSelections ([this] (AudioEngine::TrackId other, const MidiSequence& otherSeq, std::set<int>& chosen)
    {
        juce::Array<juce::var> otherEdits;
        std::vector<MidiSequence::Note> otherWanted;

        for (auto index : chosen)
        {
            auto note = otherSeq.getNotes()[(size_t) index];
            auto edit = new juce::DynamicObject();
            edit->setProperty ("index", index);

            if (drag == Drag::move)
            {
                note.startTick = juce::jmax ((juce::int64) 0, note.startTick + dragTickOffset);
                note.key = juce::jlimit (0, 127, note.key + dragKeyOffset);
                edit->setProperty ("start", note.startTick);
                edit->setProperty ("key", note.key);
            }
            else
            {
                note.lengthTicks = juce::jmax ((juce::int64) 1, note.lengthTicks + dragTickOffset);
                edit->setProperty ("length", note.lengthTicks);
            }

            otherWanted.push_back (note);
            otherEdits.add (juce::var (edit));
        }

        auto otherParams = new juce::DynamicObject();
        otherParams->setProperty ("trackId", other);
        otherParams->setProperty ("notes", otherEdits);
        runCommand ("clip.updateNotes", otherParams);

        if (auto updated = engine.getTrackSequence (other))
            chosen = reselect (*updated, otherWanted);
    });
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

    if (seq == nullptr || ! anySelected())
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

    AudioEngine::ScopedUndoGroup group (engine);   // one undo for every track

    if (! selection.empty())   // the focused track's part (edit all may have only others)
    {
        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        params->setProperty ("notes", edits);
        runCommand ("clip.updateNotes", params);
        reselectByValue (wanted);
    }

    forOtherSelections ([this, tickDelta, keyDelta] (AudioEngine::TrackId other, const MidiSequence& otherSeq, std::set<int>& chosen)
    {
        juce::Array<juce::var> otherEdits;
        std::vector<MidiSequence::Note> otherWanted;

        for (auto index : chosen)
        {
            auto note = otherSeq.getNotes()[(size_t) index];
            note.startTick = juce::jmax ((juce::int64) 0, note.startTick + tickDelta);
            note.key = juce::jlimit (0, 127, note.key + keyDelta);

            auto edit = new juce::DynamicObject();
            edit->setProperty ("index", index);
            edit->setProperty ("start", note.startTick);
            edit->setProperty ("key", note.key);
            otherEdits.add (juce::var (edit));
            otherWanted.push_back (note);
        }

        auto otherParams = new juce::DynamicObject();
        otherParams->setProperty ("trackId", other);
        otherParams->setProperty ("notes", otherEdits);
        runCommand ("clip.updateNotes", otherParams);

        if (auto updated = engine.getTrackSequence (other))
            chosen = reselect (*updated, otherWanted);
    });

    repaint();
}

std::set<int> PianoRollView::reselect (const MidiSequence& seq, const std::vector<MidiSequence::Note>& wanted)
{
    std::set<int> found;

    for (auto& target : wanted)
        for (int i = 0; i < (int) seq.getNotes().size(); ++i)
        {
            const auto& note = seq.getNotes()[(size_t) i];

            if (note.startTick == target.startTick && note.key == target.key
                 && note.lengthTicks == target.lengthTicks && found.count (i) == 0)
            {
                found.insert (i);
                break;
            }
        }

    return found;
}

void PianoRollView::forOtherSelections (const std::function<void (AudioEngine::TrackId, const MidiSequence&, std::set<int>&)>& edit)
{
    if (! editAll)
        return;

    for (auto& [other, chosen] : otherSelections)
    {
        if (other == trackId || ! isShown (other) || chosen.empty())
            continue;

        if (auto otherSeq = engine.getTrackSequence (other))
        {
            std::erase_if (chosen, [&otherSeq] (int index) { return index >= (int) otherSeq->getNotes().size(); });
            edit (other, *otherSeq, chosen);
        }
    }
}

// Another shown track's note under the mouse (edit all), the topmost first
std::pair<AudioEngine::TrackId, int> PianoRollView::otherNoteAt (juce::Point<int> position) const
{
    for (auto other : shownTracks)
        if (other != trackId)
            if (auto otherSeq = engine.getTrackSequence (other))
                for (int i = (int) otherSeq->getNotes().size(); --i >= 0;)
                    if (noteRect (otherSeq->getNotes()[(size_t) i]).contains (position))
                        return { other, i };

    return { 0, -1 };
}

// The focus moves to another shown track; every selection stays (edit all)
void PianoRollView::focusTrack (AudioEngine::TrackId id)
{
    if (id == trackId)
        return;

    auto kept = otherSelections[id];
    otherSelections.erase (id);
    otherSelections[trackId] = selection;
    setTrack (id);
    selection = kept;

    if (onEditedTrackChanged)
        onEditedTrackChanged (id);
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

        if (rect.contains (position) && isEditable (seq->getNotes()[(size_t) i]))
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

    if (laneArea().contains (position))
    {
        drag = Drag::lane;
        laneGesture.clear();
        gestureMinTick = gestureMaxTick = -1;
        laneErasing = event.mods.isPopupMenu() && laneMode() != LaneMode::velocity;
        mouseDrag (event);
        return;
    }

    // The keyboard plays: the further out on the key (to the right), the louder
    if (keysArea().contains (position) && ! event.mods.isPopupMenu())
    {
        playKey (yToKey (position.y), position.x);
        return;
    }

    if (! gridArea().contains (position))
        return;

    bool onRightEdge = false;
    auto hit = noteIndexAt (position, onRightEdge);

    // Edit all: a note of another shown track focuses that track (no selection is lost)
    if (hit < 0 && editAll && ! event.mods.isPopupMenu())
        if (const auto [other, index] = otherNoteAt (position); other != 0)
        {
            focusTrack (other);
            hit = noteIndexAt (position, onRightEdge);
        }

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
            clearAllSelections();
            selection = { hit };
        }

        // Hear the note you pick (when Hear is on)
        if (auto seq = sequence(); seq != nullptr && hit < (int) seq->getNotes().size())
            auditionNote (seq->getNotes()[(size_t) hit].key, seq->getNotes()[(size_t) hit].velocity);

        drag = onRightEdge ? Drag::resize : Drag::move;

        // Draw mode: drawing on a note resizes it - its end jumps to the pen and follows the drag
        if (drawMode && ! event.mods.isShiftDown())
        {
            if (auto seq = sequence(); seq != nullptr && hit < (int) seq->getNotes().size())
            {
                const auto& note = seq->getNotes()[(size_t) hit];
                selection = { hit };
                dragStart.x = tickToX (note.startTick + note.lengthTicks);
                drag = Drag::resize;
                mouseDrag (event);
            }
        }
    }
    else if (drawMode)
    {
        // Draw mode: preview a note here; stretch while dragging; ONE event on mouse up.
        pendingNote = { snapTick (xToTick (position.x)), newNoteTicks(), 1,
                        juce::jlimit (0, 127, yToKey (position.y)), 96 };
        auditionNote (pendingNote.key, pendingNote.velocity);
        selection.clear();
        drag = Drag::draw;
    }
    else
    {
        if (! event.mods.isShiftDown())
            clearAllSelections();

        drag = Drag::marquee;
    }

    repaint();
}

// Clicking the keyboard: note on while held, velocity by how far out on the key (left edge soft,
// right edge loudest); dragging to another key plays that one instead
void PianoRollView::playKey (int key, int x)
{
    key = juce::jlimit (0, 127, key);
    const auto velocity = juce::jlimit (1, 127, juce::roundToInt (1.0 + 126.0 * juce::jlimit (0.0, 1.0, (double) x / (double) (keysWidth - 2))));

    if (key == keyboardKey)
        return;

    releaseKey();
    keyboardKey = key;
    auto on = juce::MidiMessage::noteOn (1, key, (juce::uint8) velocity);
    on.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    engine.getLiveMidiCollector().addMessageToQueue (on);
}

void PianoRollView::releaseKey()
{
    if (keyboardKey < 0)
        return;

    auto off = juce::MidiMessage::noteOff (1, keyboardKey);
    off.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    engine.getLiveMidiCollector().addMessageToQueue (off);
    keyboardKey = -1;
}

void PianoRollView::mouseDrag (const juce::MouseEvent& event)
{
    updateHoveredKey (event.getPosition());

    if (keyboardKey >= 0)
    {
        playKey (yToKey (event.y), juce::jlimit (0, keysWidth - 2, event.x));
        return;
    }

    const auto position = event.getPosition();

    if (drag == Drag::move || drag == Drag::resize)
    {
        const auto rawTicks = (juce::int64) ((position.x - dragStart.x) * axis.ticksPerPixel);
        const auto grid = snapTicksOrZero();
        dragTickOffset = grid > 0 ? (rawTicks / juce::jmax ((juce::int64) 1, grid)) * grid : rawTicks;
        dragKeyOffset = drag == Drag::move ? (dragStart.y - position.y) / keyHeight : 0;
        dragChangedSomething = true;
        repaint();
    }
    else if (drag == Drag::draw)
    {
        // Keep the dropdown length until the mouse actually crosses the next snap
        // point; from there, size in whole grid steps (or freely with snap off).
        const auto raw = xToTick (position.x) - pendingNote.startTick;
        const auto grid = snapTicksOrZero();

        if (grid > 0)
            pendingNote.lengthTicks = raw < grid ? newNoteTicks() : (raw / grid) * grid;
        else
            pendingNote.lengthTicks = raw < laneDrawQuantum ? newNoteTicks() : raw;

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

                if (best >= 0 && bestDistance < (juce::int64) (20 * axis.ticksPerPixel))
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
    if (keyboardKey >= 0)
    {
        releaseKey();
        return;
    }

    if (drag == Drag::marquee)
    {
        const auto rect = juce::Rectangle<int>::leftTopRightBottom (
            juce::jmin (dragStart.x, event.x), juce::jmin (dragStart.y, event.y),
            juce::jmax (dragStart.x, event.x), juce::jmax (dragStart.y, event.y));

        if (auto seq = sequence())
            for (int i = 0; i < (int) seq->getNotes().size(); ++i)
                if (rect.intersects (noteRect (seq->getNotes()[(size_t) i])) && isEditable (seq->getNotes()[(size_t) i]))
                    selection.insert (i);

        if (editAll)
            for (auto other : shownTracks)
                if (other != trackId)
                    if (auto otherSeq = engine.getTrackSequence (other))
                        for (int i = 0; i < (int) otherSeq->getNotes().size(); ++i)
                            if (rect.intersects (noteRect (otherSeq->getNotes()[(size_t) i])))
                                otherSelections[other].insert (i);
    }
    else if (drag == Drag::move || drag == Drag::resize)
    {
        commitMoveOrResize();
        dragTickOffset = 0;
        dragKeyOffset = 0;
    }
    else if (drag == Drag::draw)
    {
        commitNewNote (pendingNote);
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
    // The keys column explains named keys (the instruction), like a drum map's note names
    juce::String tip;

    if (event.x < keysWidth)
        if (const auto* map = currentMap())
        {
            juce::String instruction;
            const auto label = map->keyLabel (yToKey (event.y), &instruction);
            tip = instruction.isNotEmpty() ? label + ": " + instruction : label;
        }

    if (tip != getTooltip())
        setTooltip (tip);

    updateCursorAt (event.getPosition());
    updateHoveredKey (event.getPosition());
}

// The key under the pointer is lit on the keyboard (and its row, faintly) - while moving and
// while a button is held (dragging a note, playing the keyboard)
void PianoRollView::updateHoveredKey (juce::Point<int> position)
{
    const auto key = (gridArea().contains (position) || keysArea().contains (position)) ? yToKey (position.y) : -1;

    if (key != hoveredKey)
    {
        hoveredKey = key;
        repaint();
    }
}

void PianoRollView::mouseExit (const juce::MouseEvent&)
{
    if (hoveredKey >= 0)
    {
        hoveredKey = -1;
        repaint();
    }
}

// Draw mode's pointer: a pencil, its tip at the hotspot (bottom left)
void PianoRollView::setDrawMode (bool shouldDraw)
{
    drawMode = shouldDraw;
    updateCursorAt (getMouseXYRelative());
    juce::Desktop::getInstance().getMainMouseSource().forceMouseCursorUpdate();   // now, not on the next move
}

void PianoRollView::updateCursorAt (juce::Point<int> position)
{
    bool onRightEdge = false;
    noteIndexAt (position, onRightEdge);

    if (onRightEdge)
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if ((gridArea().contains (position) && drawMode) || laneArea().contains (position))
        setMouseCursor (penCursor());   // the velocity / CC lane is always drawn in
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

juce::MouseCursor PianoRollView::penCursor()
{
    static const auto cursor = []() -> juce::MouseCursor
    {
        constexpr int size = 24;
        juce::Image image (juce::Image::ARGB, size, size, true);

        {
            juce::Graphics g (image);
            juce::Path pen;
            // the pencil along the diagonal: tip at (2, 22), end at (20, 4)
            pen.startNewSubPath (2.0f, 22.0f);
            pen.lineTo (5.5f, 14.5f);
            pen.lineTo (17.0f, 3.0f);
            pen.lineTo (21.0f, 7.0f);
            pen.lineTo (9.5f, 18.5f);
            pen.closeSubPath();

            g.setColour (juce::Colours::black);
            g.strokePath (pen, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved));
            g.setColour (juce::Colours::white);
            g.fillPath (pen);

            juce::Path tip;   // the graphite
            tip.addTriangle (2.0f, 22.0f, 4.0f, 17.5f, 6.5f, 20.0f);
            g.setColour (juce::Colours::black);
            g.fillPath (tip);
            g.drawLine (14.5f, 5.5f, 18.5f, 9.5f, 1.2f);   // where the eraser starts
        }

        return juce::MouseCursor (juce::ScaledImage (image), juce::Point<int> (2, 22));
    }();

    return cursor;
}

void PianoRollView::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (! axis.handleWheel (event, wheel))
        topKey = juce::jlimit (24, 127, topKey + (wheel.deltaY > 0 ? 2 : -2));

    repaint();
}

// Note input: keys 1-9 set the note length (1 = whole, 2 = half ... 9 = 1/256: the length
// box's items in order), 0 enters a rest (the playhead moves on by the length)
void PianoRollView::stepPlayhead (bool forward)
{
    auto& transport = engine.getTransport();
    const auto seq = sequence();
    transport.locate (nextPlayheadStop (transport.getPositionTicks(), forward, seq.get(), juce::jmax ((juce::int64) 1, gridTicks())));
}

bool PianoRollView::noteInputKey (const juce::KeyPress& key)
{
    if (keys::matches ("editor.noteInput", key))
    {
        inputToggle.setToggleState (! inputToggle.getToggleState(), juce::sendNotificationSync);
        return true;
    }

    if (! inputToggle.getToggleState())
        return false;

    if (keys::matches ("input.dot", key))       { toggleDots (1); return true; }
    if (keys::matches ("input.doubleDot", key)) { toggleDots (2); return true; }

    for (int length = 1; length <= 9; ++length)
        if (keys::matches (("input.length" + juce::String (length)).toRawUTF8(), key))
        {
            lengthBox.setSelectedId (length, juce::sendNotificationSync);
            return true;
        }

    if (keys::matches ("input.rest", key))
    {
        auto& transport = engine.getTransport();

        if (! transport.isPlaying())
            transport.locate (transport.getPositionTicks() + newNoteTicks());

        chordTick = -1;
        return true;
    }

    return false;
}

void PianoRollView::setNoteDots (int dots)
{
    noteDots = juce::jlimit (0, 2, dots);
    dotButton.setButtonText (noteDots == 2 ? ".." : ".");
    dotButton.setToggleState (noteDots > 0, juce::dontSendNotification);
}

// Choosing the same dots again removes them (a toggle); the other kind replaces them
void PianoRollView::toggleDots (int dots)
{
    setNoteDots (noteDots == dots ? 0 : dots);
}

bool PianoRollView::keyPressed (const juce::KeyPress& key)
{
    if (noteInputKey (key))
        return true;

    if (keys::matches ("editor.delete", key))
    {
        deleteSelection();
        return true;
    }

    if (keys::matches ("edit.undo", key))
    {
        undoButton.triggerClick();
        return true;
    }

    if (keys::matches ("edit.redo", key))
    {
        redoButton.triggerClick();
        return true;
    }

    if (keys::matches ("editor.selectAll", key))
    {
        if (auto seq = sequence())
        {
            selection.clear();
            for (int i = 0; i < (int) seq->getNotes().size(); ++i)
                if (isEditable (seq->getNotes()[(size_t) i]))
                    selection.insert (i);

            if (editAll)
                for (auto other : shownTracks)
                    if (other != trackId)
                        if (auto otherSeq = engine.getTrackSequence (other))
                            for (int i = 0; i < (int) otherSeq->getNotes().size(); ++i)
                                otherSelections[other].insert (i);

            repaint();
        }
        return true;
    }

    // The transport line: note to note, or a grid step
    if (keys::matches ("editor.playheadLeft", key))  { stepPlayhead (false); return true; }
    if (keys::matches ("editor.playheadRight", key)) { stepPlayhead (true);  return true; }

    // The selected notes: transpose (half step / octave) and move by the grid division
    if (anySelected())
    {
        if (keys::matches ("editor.transposeUp", key))   { nudgeSelection (0, 1);   return true; }
        if (keys::matches ("editor.transposeDown", key)) { nudgeSelection (0, -1);  return true; }
        if (keys::matches ("editor.octaveUp", key))      { nudgeSelection (0, 12);  return true; }
        if (keys::matches ("editor.octaveDown", key))    { nudgeSelection (0, -12); return true; }
        if (keys::matches ("editor.nudgeLeft", key))     { nudgeSelection (-gridTicks(), 0); return true; }
        if (keys::matches ("editor.nudgeRight", key))    { nudgeSelection (gridTicks(), 0);  return true; }
    }

    return false;   // space, Home etc. bubble up to the shell
}

//==============================================================================
// The dropdown: every shown track (top to bottom) - a track whose regions overlap has one entry
// per region ("Name, clip-N" in order of start). The edited track's earliest clip is edited
// unless another is chosen.
void PianoRollView::rebuildEditTargets()
{
    const auto map = engine.getTransport().getTempoMap();
    const auto overlappingRegions = [this, &map] (AudioEngine::TrackId track)
    {
        std::vector<int> regions;

        if (auto seq = engine.getTrackSequence (track))
            for (auto& block : computePhraseBlocks (*seq, *map))
                if (block.layers > 1 && std::find (regions.begin(), regions.end(), block.region) == regions.end())
                    regions.push_back (block.region);

        return regions;
    };

    if (shownTracks.empty() || ! isShown (trackId))
        shownTracks = { trackId };

    std::vector<std::pair<AudioEngine::TrackId, int>> list;
    juce::StringArray names;

    for (auto track : shownTracks)
    {
        const auto name = engine.getTrackName (track);
        const auto regions = overlappingRegions (track);

        if (track == trackId)
        {
            targetRegions = regions;

            if (regions.empty())
                activeRegion = -1;
            else if (std::find (regions.begin(), regions.end(), activeRegion) == regions.end())
                activeRegion = regions.front();   // the earlier clip
        }

        if (regions.empty())
        {
            list.push_back ({ track, -1 });
            names.add (name);
        }
        else
        {
            for (size_t i = 0; i < regions.size(); ++i)
            {
                list.push_back ({ track, regions[i] });
                names.add (name + ", clip-" + juce::String ((int) i + 1));
            }
        }
    }

    const auto key = names.joinIntoString ("|") + "#" + juce::String (trackId) + "#" + juce::String (activeRegion);

    if (key == targetsKey && editTargetBox.getNumItems() > 0)
        return;

    targetsKey = key;
    editTargets = list;
    editTargetBox.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        editTargetBox.addItem (names[i], i + 1);

    for (size_t i = 0; i < editTargets.size(); ++i)
        if (editTargets[i].first == trackId && (editTargets[i].second == activeRegion || editTargets[i].second < 0))
            editTargetBox.setSelectedId ((int) i + 1, juce::dontSendNotification);

    repaint();
}

void PianoRollView::setTracks (std::vector<AudioEngine::TrackId> tracks, AudioEngine::TrackId active)
{
    if (tracks.empty())
        return;

    shownTracks = std::move (tracks);
    setTrack (isShown (active) ? active : shownTracks.front());
    targetsKey.clear();
    rebuildEditTargets();
}

void PianoRollView::timerCallback()
{
    const auto seq = sequence();

    if (seq != lastSeen)
    {
        lastSeen = seq;
        rebuildEditTargets();

        // Drop selection indices that no longer exist
        const auto noteCount = seq != nullptr ? (int) seq->getNotes().size() : 0;
        std::erase_if (selection, [noteCount] (int index) { return index >= noteCount; });
        rebuildLaneBox();
        repaint();
    }

    refreshArticulationButton();
    undoButton.setEnabled (engine.canUndoClip (trackId));
    redoButton.setEnabled (engine.canRedoClip (trackId));

    // Playhead, shared axis, or any engine mutation (grid follows tempo and
    // signature edits too) - the engine's state revision covers it all.
    const auto playhead = engine.getTransport().getPositionTicks();

    if ((playhead != lastPlayheadTick || axis.revision != lastAxisRevision
          || engine.getStateRevision() != lastEngineRevision) && isShowing())
    {
        lastPlayheadTick = playhead;
        lastAxisRevision = axis.revision;
        lastEngineRevision = engine.getStateRevision();
        rebuildEditTargets();
        repaint();
    }
}

//==============================================================================
void PianoRollView::resized()
{
    auto toolbar = juce::Rectangle<int> (0, 0, getWidth(), toolbarHeight).reduced (6, 3);
    closeButton.setBounds (toolbar.removeFromRight (toolbar.getHeight() + 4));
    toolbar.removeFromRight (8);
    snapBox.setBounds (toolbar.removeFromLeft (68));
    toolbar.removeFromLeft (10);
    lengthBox.setBounds (toolbar.removeFromLeft (68));
    toolbar.removeFromLeft (2);
    dotButton.setBounds (toolbar.removeFromLeft (28));
    toolbar.removeFromLeft (10);
    quantizeButton.setBounds (toolbar.removeFromLeft (30));
    toolbar.removeFromLeft (12);
    undoButton.setBounds (toolbar.removeFromLeft (52));
    toolbar.removeFromLeft (4);
    redoButton.setBounds (toolbar.removeFromLeft (52));
    toolbar.removeFromLeft (12);
    auditionToggle.setBounds (toolbar.removeFromLeft (46));
    toolbar.removeFromLeft (4);
    inputToggle.setBounds (toolbar.removeFromLeft (50));
    toolbar.removeFromLeft (4);
    editAllToggle.setBounds (toolbar.removeFromLeft (40));
    toolbar.removeFromLeft (12);
    laneBox.setBounds (toolbar.removeFromLeft (140));
    toolbar.removeFromLeft (10);
    articulationButton.setBounds (toolbar.removeFromLeft (170));
    toolbar.removeFromLeft (10);
    colourBox.setBounds (toolbar.removeFromLeft (150));
    toolbar.removeFromLeft (10);

    editTargetBox.setBounds (toolbar.removeFromRight (juce::jmin (240, juce::jmax (120, toolbar.getWidth()))));
}

void PianoRollView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1c1f));

    const auto grid = gridArea();
    const auto seq = sequence();
    const auto map = engine.getTransport().getTempoMap();

    const auto* articulationMap = currentMap();
    const auto useFirstRoot = editorSettings::firstRootIsDefault (engine.getSettingsFile());
    const auto slotColours = editorSettings::coloursBySlot (engine.getSettingsFile());

    // Playable range: the articulation in effect (the selected notes' if they agree, else the
    // one new notes get) when the map gives it a range, else the track's player (Synchron via
    // VE Pro); unknown = all
    int playableLow = 0, playableHigh = 127;
    bool rangeFromArticulation = false;

    if (articulationMap != nullptr)
    {
        const auto targets = articulationTargets (*articulationMap);

        if (articulations::allEqual (targets))
            rangeFromArticulation = articulationMap->playableRange (targets.front(), playableLow, playableHigh);
    }

    if (! rangeFromArticulation)
        if (auto info = engine.getTrackChannelInfo (trackId); info.has_value() && info->keyLow >= 0)
        {
            playableLow = info->keyLow;
            playableHigh = info->keyHigh;
        }

    const auto playable = [&] (int key) { return key >= playableLow && key <= playableHigh; };

    // --- Key rows ---
    for (int key = topKey; key >= 0; --key)
    {
        const auto y = keyToY (key);

        if (y > grid.getBottom())
            break;

        if (! playable (key))
            g.setColour (juce::Colour (0xff141518));
        else
            g.setColour (isBlackKey (key) ? juce::Colour (0xff202327) : juce::Colour (0xff25282d));
        g.fillRect (grid.getX(), y, grid.getWidth(), keyHeight);

        if (key == hoveredKey)   // the row under the pointer
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillRect (grid.getX(), y, grid.getWidth(), keyHeight);
        }

        if (key % 12 == 0)
        {
            g.setColour (juce::Colour (0xff15171a));
            g.fillRect (grid.getX(), y + keyHeight - 1, grid.getWidth(), 1);
        }
    }

    // --- Bar/beat lines (the timeline bar above shows the numbers) ---
    const auto lane = laneArea();
    const auto endTick = xToTick (getWidth());
    auto barTick = map->getBarStart (axis.scrollTick);
    int guard = 0;

    while (barTick < endTick && ++guard < 3000)
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
        }

        barTick += ticksPerBar;
    }

    // --- The other shown tracks' notes: dimmed, in their track colour, not editable ---
    for (auto other : shownTracks)
    {
        if (other == trackId)
            continue;

        if (auto otherSeq = engine.getTrackSequence (other))
        {
            const auto colour = AudioEngine::colourFromHex (engine.getTrackColour (other), juce::Colour (0xff8a8f98)).withAlpha (0.3f);
            const auto chosen = editAll && otherSelections.count (other) ? otherSelections.at (other) : std::set<int>();

            for (int i = 0; i < (int) otherSeq->getNotes().size(); ++i)
            {
                auto note = otherSeq->getNotes()[(size_t) i];
                const auto isChosen = chosen.count (i) > 0;

                if (isChosen && drag == Drag::move)
                {
                    note.startTick += dragTickOffset;
                    note.key = juce::jlimit (0, 127, note.key + dragKeyOffset);
                }
                else if (isChosen && drag == Drag::resize)
                {
                    note.lengthTicks = juce::jmax ((juce::int64) 1, note.lengthTicks + dragTickOffset);
                }

                if (const auto rect = noteRect (note); rect.intersects (grid))
                {
                    g.setColour (isChosen ? juce::Colours::white.withAlpha (0.55f) : colour);   // selected: white, still dimmed
                    g.fillRoundedRectangle (rect.toFloat().reduced (0.5f), 2.0f);
                }
            }
        }
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

            // Sound slot colours: the colour of the note's slot (notes without one stay neutral grey);
            // a selected note keeps its colour and gets a white outline
            const auto* slot = slotColours && articulationMap != nullptr && articulationMap->hasSlots()
                                 ? articulationMap->findSlot (articulations::effective (*articulationMap, note.articulation, useFirstRoot))
                                 : nullptr;

            if (selected)   // selected notes are white, whatever colours the others
                g.setColour (juce::Colours::white);
            else if (slotColours)
                g.setColour (slot != nullptr && slot->colour.isNotEmpty() ? AudioEngine::colourFromHex (slot->colour, juce::Colours::grey)
                                                                         : juce::Colour (0xff8a8d93));
            else   // velocity: soft = yellow, through orange, loud = red
                g.setColour (juce::Colour::fromHSV ((1.0f / 6.0f) * (1.0f - (float) velocity / 127.0f), 0.8f, 0.95f, 1.0f));

            if (! isEditable (note))
                g.setOpacity (0.25f);   // another region's note (the region tabs)

            g.fillRoundedRectangle (rect.toFloat().reduced (0.5f), 2.0f);
            g.setColour (juce::Colours::black.withAlpha (selected ? 0.7f : 0.4f));
            g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 2.0f, 1.0f);

            // Articulation: its symbol on the note; one the map doesn't have is an error mark
            if (articulationMap != nullptr)
            {
                const auto shown = articulations::effective (*articulationMap, note.articulation, useFirstRoot);

                if (! shown.isEmpty())
                {
                    const auto implicit = note.articulation.isEmpty();   // the default root: not written to the note

                    if (rect.getWidth() >= 12)
                    {
                        // Symbols (SMuFL glyphs or text); a map without any shows the names' starts
                        const auto parts = articulations::symbolParts (*articulationMap, shown);
                        const auto textHeight = juce::jmin (11.0f, (float) rect.getHeight() - 2.0f);
                        g.setColour (juce::Colours::black.withAlpha (implicit ? 0.5f : 0.9f));
                        const juce::Graphics::ScopedSaveState clip (g);
                        g.reduceClipRegion (rect.reduced (2, 0));

                        if (parts.isEmpty())
                        {
                            g.setFont (juce::FontOptions (textHeight));
                            g.drawText (articulations::symbols (*articulationMap, shown), rect.reduced (3, 0),
                                        juce::Justification::centredLeft, true);
                        }
                        else
                        {
                            auto area = rect.reduced (3, 0);

                            for (auto& part : parts)
                                area.removeFromLeft (smufl::draw (g, part, area, textHeight) + 3);
                        }
                    }

                    if (! articulations::resolves (*articulationMap, shown))
                    {
                        g.setColour (juce::Colours::red);
                        g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 2.0f, 1.8f);

                        if (rect.getWidth() >= 16)
                        {
                            const auto badge = juce::Rectangle<float> ((float) rect.getRight() - 9.0f, (float) rect.getY() + 1.0f, 8.0f, 8.0f);
                            g.fillEllipse (badge);
                            g.setColour (juce::Colours::white);
                            g.setFont (juce::FontOptions (8.0f, juce::Font::bold));
                            g.drawText ("!", badge.toNearestInt(), juce::Justification::centred);
                        }
                    }
                }
            }
        }
    }

    // --- Draw-mode preview note ---
    if (drag == Drag::draw)
    {
        const auto rect = noteRect (pendingNote);
        g.setColour (juce::Colours::orange.withAlpha (0.8f));
        g.fillRoundedRectangle (rect.toFloat().reduced (0.5f), 2.0f);
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

        if (! playable (key))
            g.setColour (isBlackKey (key) ? juce::Colour (0xff111214) : juce::Colour (0xff55585e));
        else
            g.setColour (isBlackKey (key) ? juce::Colour (0xff17191c) : juce::Colour (0xffd8d8d8));

        g.fillRect (keys.getX(), y, keys.getWidth() - 2, keyHeight - 1);

        if (key == hoveredKey)   // the key under the pointer
        {
            g.setColour (theme::colour (theme::Token::selectionBorder).withAlpha (0.55f));
            g.fillRect (keys.getX(), y, keys.getWidth() - 2, keyHeight - 1);
        }

        // Keys the map names (a keyswitch is named after its articulation) get their name, and
        // keyswitches a mark on the left
        if (articulationMap != nullptr)
        {
            const auto label = articulationMap->keyLabel (key);

            if (articulationMap->isKeyswitch (key))
            {
                g.setColour (juce::Colours::orange);
                g.fillRect (keys.getX(), y, 3, keyHeight - 1);
            }

            if (label.isNotEmpty())
            {
                g.setColour (isBlackKey (key) || ! playable (key) ? juce::Colours::white.withAlpha (0.85f)
                                                                  : juce::Colour (0xff202225));
                g.setFont (juce::FontOptions (9.0f));
                g.drawText (label, keys.getX() + 5, y, keys.getWidth() - 9, keyHeight,
                            juce::Justification::centredLeft, true);
            }
        }

        if (key % 12 == 0 && (articulationMap == nullptr || articulationMap->keyLabel (key).isEmpty()))   // a name outranks the octave label
        {
            g.setColour (juce::Colours::grey);
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (noteNames::name (key), keys.getX() + 2, y, keys.getWidth() - 8, keyHeight,
                        juce::Justification::centredRight);
        }
    }

    // --- Playhead ---
    const auto playheadX = tickToX (engine.getTransport().getPositionTicks());

    if (playheadX >= grid.getX() && playheadX <= getWidth())
    {
        g.setColour (theme::colour (theme::Token::transportLine));
        g.fillRect (playheadX, grid.getY(), 1, getHeight() - grid.getY());
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
