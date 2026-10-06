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

}

//==============================================================================
PianoRollView::PianoRollView (AudioEngine& e, CommandDispatcher& d, TimeAxis& a)
    : engine (e), dispatcher (d), axis (a)
{
    setWantsKeyboardFocus (true);



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

    quantizeButton.setTooltip ("Quantize selected notes (or all) to the grid (it follows the zoom)");
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


    editTargetBox.setTooltip ("What is edited: the track - and where its regions overlap, which clip (the others are dimmed)");
    editTargetBox.setWantsKeyboardFocus (false);
    theme::setPopupDownwards (editTargetBox);   // the list always opens below it
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

    for (auto* c : std::initializer_list<juce::Component*> { &auditionToggle, &inputToggle,
                                                             &lengthBox, &dotButton, &quantizeButton,
                                                             &articulationButton, &colourBox })
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
             getHeight() - toolbarHeight - lanesHeight() };
}

juce::Rectangle<int> PianoRollView::gridArea() const
{
    return { keysWidth, toolbarHeight, getWidth() - keysWidth,
             getHeight() - toolbarHeight - lanesHeight() };
}

// The maximized lane's value area
juce::Rectangle<int> PianoRollView::laneArea() const
{
    const auto ids = shownLanes();
    const auto index = ids.indexOf (maximizedLaneId());
    return index < 0 ? juce::Rectangle<int>() : laneRowArea (index).withTrimmedLeft (keysWidth).withTrimmedTop (1);
}

juce::StringArray PianoRollView::shownLanes() const
{
    auto chosen = engine.getTrackEditorLanes (trackId);

    if (chosen.isEmpty())
        chosen = lanes::defaultTrackLanes();

    juce::StringArray ids;

    for (auto& id : chosen)
        if (lanes::Settings::get().isAvailable (id))
            ids.add (id);

    return ids;
}

juce::String PianoRollView::maximizedLaneId() const
{
    const auto ids = shownLanes();
    return ids.contains (maximizedLane) ? maximizedLane : ids[0];
}

int PianoRollView::lanesHeight() const
{
    const auto count = shownLanes().size();
    return count == 0 ? minimizedLaneHeight   // a strip to right-click to bring lanes back
                      : maximizedLaneHeight + (count - 1) * minimizedLaneHeight;
}

juce::Rectangle<int> PianoRollView::laneRowArea (int index) const
{
    const auto ids = shownLanes();
    const auto maximized = ids.indexOf (maximizedLaneId());
    auto y = getHeight() - lanesHeight();

    for (int i = 0; i < index; ++i)
        y += i == maximized ? maximizedLaneHeight : minimizedLaneHeight;

    return { 0, y, getWidth(), index == maximized ? maximizedLaneHeight : minimizedLaneHeight };
}

int PianoRollView::laneIndexAt (juce::Point<int> position) const
{
    for (int i = 0; i < shownLanes().size(); ++i)
        if (laneRowArea (i).contains (position))
            return i;

    return -1;
}

// Right-click on a lane's name or a minimized lane: tick the lanes this track shows
void PianoRollView::showLaneMenu()
{
    auto chosen = engine.getTrackEditorLanes (trackId);

    if (chosen.isEmpty())
        chosen = lanes::defaultTrackLanes();

    juce::PopupMenu menu;
    menu.addSectionHeader ("Lanes for " + engine.getTrackName (trackId));
    const auto safe = juce::Component::SafePointer<PianoRollView> (this);

    for (auto& id : lanes::Settings::get().availableIds())
        menu.addItem (lanes::Settings::get().displayName (id), true, chosen.contains (id), [safe, id, chosen]
        {
            if (safe == nullptr)
                return;

            auto lanesNow = chosen;

            if (lanesNow.contains (id))
                lanesNow.removeString (id);
            else
            {
                // Kept in the settings' order
                juce::StringArray ordered;

                for (auto& each : lanes::allIds())
                    if (lanesNow.contains (each) || each == id)
                        ordered.add (each);

                lanesNow = ordered;
                safe->maximizedLane = id;   // a lane just added is the one to edit
            }

            safe->engine.setTrackEditorLanes (safe->trackId, lanesNow.isEmpty() ? juce::StringArray { "none" } : lanesNow);
            safe->repaint();
        });

    menu.addSeparator();
    menu.addItem ("Choose the available lanes in Settings > Controller lanes", false, false, [] {});
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

// The value under the mouse in a lane: the note's velocity there, or the controller's value
juce::String PianoRollView::laneValueAt (juce::Point<int> position) const
{
    const auto index = laneIndexAt (position);
    const auto seq = sequence();

    if (index < 0 || position.x < keysWidth)
        return {};

    if (seq == nullptr)
        return laneArea().contains (position) ? lanes::Settings::get().displayName (shownLanes()[index]) + ": "
                                                    + juce::String (laneValueFromY (position.y))
                                              : juce::String();

    const auto lane = lanes::parse (shownLanes()[index]);
    const auto tick = xToTick (position.x);
    const auto name = lane.kind == lanes::Kind::velocity ? juce::String ("Velocity")
                                                         : lanes::Settings::get().displayName (shownLanes()[index]);
    const auto shown = [&lane] (int v) { return lane.kind == lanes::Kind::pitchBend ? juce::String (v - 8192) : juce::String (v); };

    // What is there now at the mouse's x: the note's velocity, or the controller's value
    int current = -1;

    if (lane.kind == lanes::Kind::velocity)
    {
        for (auto& note : seq->getNotes())
            if (tick >= note.startTick && tick < note.startTick + note.lengthTicks)
            {
                current = note.velocity;
                break;
            }
    }
    else
    {
        // The value in effect there: the last point's, or along its ramp
        for (auto& control : seq->getControls())
            if (lane.shows (control))
            {
                current = MidiSequence::laneValueAt (seq->getControls(), control, tick);
                break;
            }
    }

    // The maximized lane: the value at the mouse's height - what a drag sets - and what is there now
    if (laneArea().contains (position))
    {
        const auto atHeight = laneValueFromY (position.y);
        return name + " " + shown (atHeight) + (current >= 0 && current != atHeight ? "  (now " + shown (current) + ")" : juce::String());
    }

    return current >= 0 ? name + " " + shown (current) : juce::String();   // a minimized lane: what is there
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

// The grid follows the zoom (TimeAxis::gridStep), where the view starts
juce::int64 PianoRollView::gridTicks() const
{
    return axis.gridStep (*engine.getTransport().getTempoMap(), axis.scrollTick);
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
    return axis.snapToGrid (*engine.getTransport().getTempoMap(), tick);
}

//==============================================================================
PianoRollView::LaneMode PianoRollView::laneMode() const
{
    switch (lanes::parse (maximizedLaneId()).kind)
    {
        case lanes::Kind::pitchBend:  return LaneMode::pitchBend;
        case lanes::Kind::aftertouch: return LaneMode::aftertouch;
        case lanes::Kind::controller: return LaneMode::controller;
        case lanes::Kind::velocity:   break;
    }

    return LaneMode::velocity;
}

int PianoRollView::laneControllerNumber() const
{
    return lanes::parse (maximizedLaneId()).cc;
}

int PianoRollView::laneValueMax() const
{
    return lanes::parse (maximizedLaneId()).maxValue();
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

void PianoRollView::commitLaneGesture()
{
    if (laneMode() == LaneMode::velocity || gestureMinTick < 0)
        return;

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("type", lanes::parse (maximizedLaneId()).controlType());
    params->setProperty ("number", laneControllerNumber());
    params->setProperty ("start", gestureMinTick);
    params->setProperty ("end", gestureMaxTick + laneDrawQuantum);

    juce::Array<juce::var> events;

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

// Undo / redo of this track's clip (Ctrl+Z / Ctrl+Y)
void PianoRollView::undo()
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
}

void PianoRollView::redo()
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
}

void PianoRollView::copySelection()
{
    std::map<AudioEngine::TrackId, std::vector<MidiSequence::Note>> copied;

    if (auto seq = sequence())
        for (auto index : selection)
            if (index < (int) seq->getNotes().size())
                copied[trackId].push_back (seq->getNotes()[(size_t) index]);

    forOtherSelections ([&copied] (AudioEngine::TrackId other, const MidiSequence& otherSeq, std::set<int>& chosen)
    {
        for (auto index : chosen)
            copied[other].push_back (otherSeq.getNotes()[(size_t) index]);
    });

    if (copied.empty())
        return;

    auto earliest = std::numeric_limits<juce::int64>::max();

    for (auto& [track, notes] : copied)
        for (auto& note : notes)
            earliest = juce::jmin (earliest, note.startTick);

    for (auto& [track, notes] : copied)
        for (auto& note : notes)
        {
            note.startTick -= earliest;
            note.region = 0;   // the paste decides the region
        }

    clipboard = std::move (copied);
}

void PianoRollView::pasteAtPlayhead()
{
    if (clipboard.empty() || trackId == 0)
        return;

    const auto at = engine.getTransport().getPositionTicks();
    std::map<AudioEngine::TrackId, std::vector<MidiSequence::Note>> wanted;

    AudioEngine::ScopedUndoGroup group (engine);   // one undo for every track

    // Each copied track's notes go back to it when it is shown, else to the edited track
    std::map<AudioEngine::TrackId, juce::Array<juce::var>> adds;

    for (auto& [source, notes] : clipboard)
    {
        const auto target = isShown (source) ? source : trackId;

        for (auto note : notes)
        {
            note.startTick += at;

            auto n = new juce::DynamicObject();
            n->setProperty ("start", note.startTick);
            n->setProperty ("length", note.lengthTicks);
            n->setProperty ("key", note.key);
            n->setProperty ("velocity", note.velocity);
            n->setProperty ("channel", note.channel);

            if (! note.articulation.isEmpty())
                n->setProperty ("articulation", note.articulation.toVar());

            if (target == trackId && activeRegion >= 0)
                n->setProperty ("region", activeRegion);   // into the clip being edited

            adds[target].add (juce::var (n));
            wanted[target].push_back (note);
        }
    }

    for (auto& [target, notes] : adds)
    {
        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", target);
        params->setProperty ("notes", notes);
        runCommand ("clip.addNotes", params);
    }

    // The pasted notes become the selection
    clearAllSelections();

    for (auto& [target, notes] : wanted)
        if (auto seq = engine.getTrackSequence (target))
        {
            if (target == trackId)
                selection = reselect (*seq, notes);
            else if (editAll)
                otherSelections[target] = reselect (*seq, notes);
        }

    repaint();
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

    // No lanes shown: the strip left there brings up the lane menu
    if (shownLanes().isEmpty() && position.y >= getHeight() - lanesHeight())
    {
        showLaneMenu();
        return;
    }

    // The lane pane: right-click a name or a minimized lane for the lane menu; click a minimized
    // lane to maximize it; the maximized lane is drawn in
    if (const auto laneIndex = laneIndexAt (position); laneIndex >= 0 && ! laneArea().contains (position))
    {
        if (event.mods.isPopupMenu())
            showLaneMenu();
        else if (position.x >= keysWidth || laneRowArea (laneIndex).getHeight() == minimizedLaneHeight)
            maximizedLane = shownLanes()[laneIndex];

        repaint();
        return;
    }

    if (laneArea().contains (position))
    {
        if (event.mods.isPopupMenu())   // right-click: the lane menu (never erasing)
        {
            showLaneMenu();
            return;
        }

        if (isPointLane())
        {
            pointValueDelta = 0;
            pointTickDelta = 0;
            handleMoved = false;

            if (const auto handle = handleAt (position); handle >= 0)
            {
                dragHandle = handle;
                const auto seq = sequence();
                const auto ramped = seq != nullptr && seq->getControls()[(size_t) handle].ramp;
                previewBend = ramped ? seq->getControls()[(size_t) handle].bend : 0.5f;
                previewBendAt = ramped ? seq->getControls()[(size_t) handle].bendAt : 0.5f;
                drag = Drag::handle;
                return;
            }

            if (const auto point = pointAt (position); point >= 0)
            {
                if (event.mods.isShiftDown())
                {
                    if (pointSelection.count (point)) pointSelection.erase (point);
                    else                              pointSelection.insert (point);
                }
                else if (effectivePoints().count (point) == 0)
                {
                    pointSelection = { point };
                    clearAllSelections();   // the notes' span no longer picks points
                }

                dragPoint = point;
                drag = Drag::point;
                repaint();
                return;
            }

            if (! event.mods.isShiftDown())
                pointSelection.clear();

            addPointAt (position);
            repaint();
            return;
        }

        drag = Drag::lane;
        laneGesture.clear();
        gestureMinTick = gestureMaxTick = -1;
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

    if (event.mods.isPopupMenu())   // right-click never deletes: it just picks the note
    {
        if (hit >= 0 && selection.count (hit) == 0)
        {
            clearAllSelections();
            selection = { hit };
            repaint();
        }

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
    else if (drag == Drag::point)
    {
        if (const auto seq = sequence(); seq != nullptr && dragPoint >= 0 && dragPoint < (int) seq->getControls().size())
        {
            const auto& pressed = seq->getControls()[(size_t) dragPoint];
            pointValueDelta = laneValueFromY (position.y) - pressed.value;

            if (effectivePoints().size() == 1)   // one point also moves in time (snapped)
                pointTickDelta = snapPointTick (pressed.tick + (juce::int64) ((position.x - dragStart.x) * axis.ticksPerPixel)) - pressed.tick;

            const auto lane = lanes::parse (maximizedLaneId());
            const auto value = juce::jlimit (0, lane.maxValue(), pressed.value + pointValueDelta);
            hoverValue = lanes::Settings::get().displayName (maximizedLaneId()) + " "
                           + juce::String (lane.kind == lanes::Kind::pitchBend ? value - 8192 : value);
            hoverPoint = position;
        }

        repaint();
    }
    else if (drag == Drag::handle)
    {
        if (const auto seq = sequence(); seq != nullptr && dragHandle >= 0)
        {
            const auto points = lanePoints();
            const auto at = std::find (points.begin(), points.end(), dragHandle);

            if (at != points.end() && at + 1 != points.end())
            {
                const auto& from = seq->getControls()[(size_t) *at];
                const auto& to = seq->getControls()[(size_t) *(at + 1)];

                // The handle moves both ways: up/down sets the height, left/right where the curve bends
                if (position.getDistanceFrom (dragStart) > 2)
                {
                    handleMoved = true;

                    if (from.value != to.value)
                        previewBend = juce::jlimit (0.02f, 0.98f, (float) (laneValueFromY (position.y) - from.value) / (float) (to.value - from.value));

                    const auto fromX = tickToX (from.tick), toX = tickToX (to.tick);
                    previewBendAt = juce::jlimit (0.05f, 0.95f, (float) (position.x - fromX) / (float) juce::jmax (1, toX - fromX));
                }
            }
        }

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
            hoverValue = "Velocity " + juce::String (velocity);
            hoverPoint = position;

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

            {
                laneGesture[tick] = laneValueFromY (position.y);
                hoverValue = lanes::Settings::get().displayName (maximizedLaneId()) + " "
                               + juce::String (laneMode() == LaneMode::pitchBend ? laneGesture[tick] - 8192 : laneGesture[tick]);
                hoverPoint = position;
            }
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
    else if (drag == Drag::point)
    {
        commitPointDrag();
        dragPoint = -1;
        pointValueDelta = 0;
        pointTickDelta = 0;
    }
    else if (drag == Drag::handle)
    {
        commitHandle (! handleMoved);
        dragHandle = -1;
        previewBend = -1.0f;
    }

    drag = Drag::none;
    repaint();
}

void PianoRollView::mouseDoubleClick (const juce::MouseEvent& event)
{
    if (isPointLane())
        if (const auto handle = handleAt (event.getPosition()); handle >= 0)
        {
            auto change = new juce::DynamicObject();
            change->setProperty ("index", handle);
            change->setProperty ("ramp", false);
            auto params = new juce::DynamicObject();
            params->setProperty ("trackId", trackId);
            params->setProperty ("controls", juce::Array<juce::var> { juce::var (change) });
            runCommand ("clip.updateControls", params);
            repaint();
            return;
        }

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

    // A minimized lane under the mouse is lit subtly
    {
        const auto index = laneIndexAt (event.getPosition());
        const auto hovered = index >= 0 && shownLanes()[index] != maximizedLaneId() ? index : -1;

        if (hovered != hoveredLane)
        {
            hoveredLane = hovered;
            repaint();
        }
    }

    // The value under the mouse in a lane, always shown by the pointer
    const auto value = laneValueAt (event.getPosition());

    if (value != hoverValue || (value.isNotEmpty() && event.getPosition() != hoverPoint))
    {
        hoverValue = value;
        hoverPoint = event.getPosition();
        repaint();
    }
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
    if (hoverValue.isNotEmpty() || hoveredLane >= 0)
    {
        hoverValue.clear();
        hoveredLane = -1;
        repaint();
    }

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

    const auto overHandle = isPointLane() && handleAt (position) >= 0;

    if (overHandle != (hoveredHandle >= 0))
    {
        hoveredHandle = overHandle ? handleAt (position) : -1;
        repaint();
    }

    if (onRightEdge)
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if (overHandle)
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    else if (isPointLane() && pointAt (position) >= 0)
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
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
        if (! pointSelection.empty())
            deletePoints();
        else
            deleteSelection();

        repaint();
        return true;
    }

    if (keys::matches ("edit.undo", key))
    {
        undo();
        return true;
    }

    if (keys::matches ("edit.redo", key))
    {
        redo();
        return true;
    }

    if (keys::matches ("editor.copy", key))
    {
        copySelection();
        return true;
    }

    if (keys::matches ("editor.cut", key))
    {
        copySelection();
        deleteSelection();
        return true;
    }

    if (keys::matches ("editor.paste", key))
    {
        pasteAtPlayhead();
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
        repaint();
    }

    refreshArticulationButton();

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
    lengthBox.setBounds (toolbar.removeFromLeft (68));
    toolbar.removeFromLeft (2);
    dotButton.setBounds (toolbar.removeFromLeft (28));
    toolbar.removeFromLeft (10);
    quantizeButton.setBounds (toolbar.removeFromLeft (30));
    toolbar.removeFromLeft (12);
    auditionToggle.setBounds (toolbar.removeFromLeft (46));
    toolbar.removeFromLeft (4);
    inputToggle.setBounds (toolbar.removeFromLeft (50));
    toolbar.removeFromLeft (4);
    editAllToggle.setBounds (toolbar.removeFromLeft (40));
    toolbar.removeFromLeft (12);
    toolbar.removeFromLeft (10);
    articulationButton.setBounds (toolbar.removeFromLeft (170));
    toolbar.removeFromLeft (10);
    colourBox.setBounds (toolbar.removeFromLeft (150));
    toolbar.removeFromLeft (10);

    editTargetBox.setBounds (toolbar.removeFromRight (juce::jmin (240, juce::jmax (120, toolbar.getWidth()))));
}

//==============================================================================
// CC points
std::vector<int> PianoRollView::lanePoints() const
{
    std::vector<int> points;

    if (auto seq = sequence())
    {
        const auto lane = lanes::parse (maximizedLaneId());

        for (int i = 0; i < (int) seq->getControls().size(); ++i)
            if (lane.shows (seq->getControls()[(size_t) i]))
                points.push_back (i);
    }

    return points;
}

// The selected points, plus every point of the lane under the selected notes' span (dragged together)
std::set<int> PianoRollView::effectivePoints() const
{
    auto points = pointSelection;
    const auto seq = sequence();

    if (seq == nullptr || selection.empty())
        return points;

    auto from = std::numeric_limits<juce::int64>::max(), to = (juce::int64) 0;

    for (auto index : selection)
        if (index < (int) seq->getNotes().size())
        {
            const auto& note = seq->getNotes()[(size_t) index];
            from = juce::jmin (from, note.startTick);
            to = juce::jmax (to, note.startTick + note.lengthTicks);
        }

    for (auto index : lanePoints())
        if (const auto tick = seq->getControls()[(size_t) index].tick; tick >= from && tick <= to)
            points.insert (index);

    return points;
}

juce::Point<int> PianoRollView::pointPosition (const MidiSequence::Control& c) const
{
    return { tickToX (c.tick), laneValueToY (c.value) };
}

juce::Point<int> PianoRollView::handlePosition (const MidiSequence::Control& from, const MidiSequence::Control& to) const
{
    const auto at = from.ramp ? from.bendAt : 0.5f;
    const auto height = from.ramp ? from.bend : 0.5f;
    const auto x = tickToX (from.tick + (juce::int64) ((double) (to.tick - from.tick) * at));
    return { x, laneValueToY (juce::roundToInt ((float) from.value + (float) (to.value - from.value) * height)) };
}

int PianoRollView::pointAt (juce::Point<int> position) const
{
    const auto seq = sequence();

    if (seq == nullptr || ! laneArea().contains (position))
        return -1;

    for (auto index : lanePoints())
        if (pointPosition (seq->getControls()[(size_t) index]).getDistanceFrom (position) <= 5)
            return index;

    return -1;
}

int PianoRollView::handleAt (juce::Point<int> position) const
{
    const auto seq = sequence();

    if (seq == nullptr || ! laneArea().contains (position))
        return -1;

    const auto points = lanePoints();

    for (size_t i = 0; i + 1 < points.size(); ++i)
    {
        const auto& from = seq->getControls()[(size_t) points[i]];
        const auto& to = seq->getControls()[(size_t) points[i + 1]];

        if (tickToX (to.tick) - tickToX (from.tick) >= 14 && handlePosition (from, to).getDistanceFrom (position) <= 5)
            return points[i];
    }

    return -1;
}

// With Snap on: the nearest of the grid line and the edited track's note starts and ends
juce::int64 PianoRollView::snapPointTick (juce::int64 tick) const
{
    tick = juce::jmax ((juce::int64) 0, tick);

    if (! axis.snap)
        return tick;

    auto best = axis.snapToGrid (*engine.getTransport().getTempoMap(), tick);

    if (auto seq = sequence())
        for (auto& note : seq->getNotes())
            for (auto edge : { note.startTick, note.startTick + note.lengthTicks })
                if (std::abs (edge - tick) < std::abs (best - tick))
                    best = edge;

    return best;
}

// A click on the lane's empty space: one point there (a point at that very time takes the value)
void PianoRollView::addPointAt (juce::Point<int> position)
{
    const auto lane = lanes::parse (maximizedLaneId());
    const auto tick = snapPointTick (xToTick (position.x));
    const auto value = laneValueFromY (position.y);
    const auto seq = sequence();

    if (seq != nullptr)
        for (auto index : lanePoints())
            if (seq->getControls()[(size_t) index].tick == tick)
            {
                auto change = new juce::DynamicObject();
                change->setProperty ("index", index);
                change->setProperty ("value", value);
                auto params = new juce::DynamicObject();
                params->setProperty ("trackId", trackId);
                params->setProperty ("controls", juce::Array<juce::var> { juce::var (change) });
                runCommand ("clip.updateControls", params);
                pointSelection = { index };
                return;
            }

    auto point = new juce::DynamicObject();
    point->setProperty ("tick", tick);
    point->setProperty ("type", lane.controlType());
    point->setProperty ("number", lane.cc);
    point->setProperty ("value", value);

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("controls", juce::Array<juce::var> { juce::var (point) });
    runCommand ("clip.addControls", params);

    // The new point is selected
    pointSelection.clear();

    if (auto updated = sequence())
        for (int i = 0; i < (int) updated->getControls().size(); ++i)
            if (const auto& c = updated->getControls()[(size_t) i]; lane.shows (c) && c.tick == tick && c.value == value)
                pointSelection = { i };
}

// The dragged points: up/down together (one alone also moves in time)
void PianoRollView::commitPointDrag()
{
    const auto seq = sequence();
    const auto points = effectivePoints();

    if (seq == nullptr || (pointValueDelta == 0 && pointTickDelta == 0))
        return;

    const auto lane = lanes::parse (maximizedLaneId());
    juce::Array<juce::var> changes;
    std::vector<std::pair<juce::int64, int>> wanted;

    for (auto index : points)
    {
        const auto& c = seq->getControls()[(size_t) index];
        const auto value = juce::jlimit (0, lane.maxValue(), c.value + pointValueDelta);
        const auto tick = points.size() == 1 ? juce::jmax ((juce::int64) 0, c.tick + pointTickDelta) : c.tick;

        auto change = new juce::DynamicObject();
        change->setProperty ("index", index);
        change->setProperty ("value", value);
        change->setProperty ("tick", tick);
        changes.add (juce::var (change));
        wanted.push_back ({ tick, value });
    }

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("controls", changes);
    runCommand ("clip.updateControls", params);

    // The moved points stay selected (they may have been picked through the notes)
    if (pointSelection.size() == points.size() || ! pointSelection.empty())
    {
        pointSelection.clear();

        if (auto updated = sequence())
            for (int i = 0; i < (int) updated->getControls().size(); ++i)
                for (auto& [tick, value] : wanted)
                    if (const auto& c = updated->getControls()[(size_t) i]; lane.shows (c) && c.tick == tick && c.value == value)
                        pointSelection.insert (i);
    }
}

// The handle: a click turns a step into a straight ramp; a drag bends the ramp
void PianoRollView::commitHandle (bool click)
{
    const auto seq = sequence();

    if (seq == nullptr || dragHandle < 0 || dragHandle >= (int) seq->getControls().size())
        return;

    const auto& from = seq->getControls()[(size_t) dragHandle];
    auto change = new juce::DynamicObject();
    change->setProperty ("index", dragHandle);
    change->setProperty ("ramp", true);
    change->setProperty ("bend", click ? (from.ramp ? (double) from.bend : 0.5) : (double) previewBend);
    change->setProperty ("bendAt", click ? (from.ramp ? (double) from.bendAt : 0.5) : (double) previewBendAt);

    if (click && from.ramp)
        return;   // a click on a ramp's handle changes nothing (drag to bend, double-click for a step)

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("controls", juce::Array<juce::var> { juce::var (change) });
    runCommand ("clip.updateControls", params);
}

void PianoRollView::deletePoints()
{
    if (pointSelection.empty())
        return;

    juce::Array<juce::var> indices;

    for (auto index : pointSelection)
        indices.add (index);

    auto params = new juce::DynamicObject();
    params->setProperty ("trackId", trackId);
    params->setProperty ("indices", indices);
    runCommand ("clip.removeControls", params);
    pointSelection.clear();
}

void PianoRollView::paintLanes (juce::Graphics& g, const MidiSequence* seq)
{
    const auto ids = shownLanes();
    const auto maximized = maximizedLaneId();

    if (ids.isEmpty())
    {
        const auto strip = juce::Rectangle<int> (0, getHeight() - lanesHeight(), getWidth(), lanesHeight());
        g.setColour (juce::Colour (0xff1d1f23));
        g.fillRect (strip);
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("No lanes - click to choose", strip.reduced (6, 0), juce::Justification::centredLeft, true);
        return;
    }

    for (int laneIndex = 0; laneIndex < ids.size(); ++laneIndex)
    {
        const auto row = laneRowArea (laneIndex);
        const auto laneKind = lanes::parse (ids[laneIndex]);
        const auto isMaximized = ids[laneIndex] == maximized;

        // The name, on the left (the keys column)
        const auto nameArea = row.withWidth (keysWidth);
        g.setColour (juce::Colour (0xff232529));
        g.fillRect (nameArea);
        g.setColour (juce::Colours::white.withAlpha (isMaximized ? 0.85f : 0.55f));
        g.setFont (juce::FontOptions (11.5f, isMaximized ? juce::Font::bold : juce::Font::plain));
        g.drawFittedText (lanes::Settings::get().displayName (ids[laneIndex]), nameArea.reduced (4, 2),
                          isMaximized ? juce::Justification::topLeft : juce::Justification::centredLeft, isMaximized ? 4 : 1, 0.8f);

        g.setColour (juce::Colour (0xff2e3136));
        g.fillRect (row.getX(), row.getY(), row.getWidth(), 1);

        if (! isMaximized)
        {
            paintMinimizedLane (g, row.withTrimmedLeft (keysWidth).withTrimmedTop (1), laneKind, seq);

            if (laneIndex == hoveredLane)   // hover: a subtle lift (click maximizes it)
            {
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.fillRect (row.withTrimmedTop (1));
            }

            continue;
        }

        const auto lane = laneArea();

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

                    g.setColour (selection.count (i) ? juce::Colours::white
                                                     : lanes::valueColour (lanes::Kind::velocity, (float) velocity / 127.0f)
                                                           .withAlpha (selection.empty() ? 1.0f : 0.35f));
                    g.fillRect (x, lane.getBottom() - height, 3, height);
                }
            }
        }
        else
        {
            if (seq != nullptr)
            {
                // CC / pitch bend / aftertouch: the points, the curve between them, the handles
                std::vector<MidiSequence::Control> shown;
                std::vector<int> indices;
                const auto picked = effectivePoints();

                for (int i = 0; i < (int) seq->getControls().size(); ++i)
                    if (auto c = seq->getControls()[(size_t) i]; laneKind.shows (c))
                    {
                        if (drag == Drag::point && picked.count (i))   // the drag's preview
                        {
                            c.value = juce::jlimit (0, laneKind.maxValue(), c.value + pointValueDelta);

                            if (picked.size() == 1)
                                c.tick = juce::jmax ((juce::int64) 0, c.tick + pointTickDelta);
                        }

                        if (drag == Drag::handle && i == dragHandle)
                        {
                            c.ramp = true;
                            c.bend = previewBend;
                            c.bendAt = previewBendAt;
                        }

                        shown.push_back (c);
                        indices.push_back (i);
                    }

                const auto colourOf = [&laneKind] (int value)
                {
                    return lanes::valueColour (lanes::Kind::controller, (float) value / (float) laneKind.maxValue());
                };

                for (size_t i = 0; i < shown.size(); ++i)
                {
                    const auto& from = shown[i];
                    const auto a = pointPosition (from);
                    const auto rightX = i + 1 < shown.size() ? tickToX (shown[i + 1].tick) : lane.getRight();

                    if (rightX < lane.getX() || a.x > lane.getRight())
                        continue;

                    if (i + 1 < shown.size() && from.ramp)
                    {
                        // The ramp: drawn as a polyline along its curve
                        const auto& to = shown[i + 1];
                        juce::Path path;
                        path.startNewSubPath (a.toFloat());

                        for (auto x = a.x + 3; x < rightX; x += 3)
                        {
                            const auto t = (float) (x - a.x) / (float) juce::jmax (1, rightX - a.x);
                            const auto v = (float) from.value + (float) (to.value - from.value) * MidiSequence::rampShape (t, from.bend, from.bendAt);
                            path.lineTo ((float) x, (float) laneValueToY (juce::roundToInt (v)));
                        }

                        path.lineTo (pointPosition (to).toFloat());
                        g.setColour (colourOf ((from.value + to.value) / 2));
                        g.strokePath (path, juce::PathStrokeType (2.0f));
                    }
                    else
                    {
                        // A step: hold, then jump at the next point
                        g.setColour (colourOf (from.value));
                        g.fillRect (juce::jmax (lane.getX(), a.x), a.y, juce::jmax (1, rightX - juce::jmax (lane.getX(), a.x)), 2);

                        if (i + 1 < shown.size())
                        {
                            const auto b = pointPosition (shown[i + 1]);
                            g.fillRect (b.x, juce::jmin (a.y, b.y), 2, std::abs (b.y - a.y) + 2);
                        }
                    }

                    // The segment's handle (click: ramp; drag: bend; double-click: step)
                    if (i + 1 < shown.size() && rightX - a.x >= 14)
                    {
                        const auto h = handlePosition (from, shown[i + 1]);
                        const auto box = juce::Rectangle<int> (6, 6).withCentre (h).toFloat();
                        g.setColour (juce::Colours::white.withAlpha (indices[i] == hoveredHandle || indices[i] == dragHandle ? 0.95f : 0.55f));

                        if (indices[i] == hoveredHandle || indices[i] == dragHandle)
                            g.fillRect (box);
                        else
                            g.drawRect (box, 1.0f);
                    }
                }

                // The points (selected: white)
                for (size_t i = 0; i < shown.size(); ++i)
                {
                    const auto p = pointPosition (shown[i]);

                    if (p.x < lane.getX() - 4 || p.x > lane.getRight() + 4)
                        continue;

                    g.setColour (picked.count (indices[i]) ? juce::Colours::white : colourOf (shown[i].value));
                    g.fillRect (juce::Rectangle<int> (6, 6).withCentre (p));
                }
            }

            // Gesture overlay
            if (drag == Drag::lane)
            {
                g.setColour (juce::Colours::orange);

                for (auto& [tick, value] : laneGesture)
                    g.fillRect (tickToX (tick) - 1, laneValueToY (value) - 1, 3, 3);
            }
        }
    }
}

// A minimized lane: the background, and a line (60% opaque) coloured by the values where there are any
// (velocity per note; controllers held until the next point, or along a ramp)
void PianoRollView::paintMinimizedLane (juce::Graphics& g, juce::Rectangle<int> strip, const lanes::Lane& lane,
                                        const MidiSequence* seq)
{
    g.setColour (juce::Colour (0xff1d1f23));
    g.fillRect (strip);

    if (seq == nullptr)
        return;

    const auto line = strip.withSizeKeepingCentre (strip.getWidth(), 4);

    if (lane.kind == lanes::Kind::velocity)
    {
        for (auto& note : seq->getNotes())
        {
            const auto x = juce::jmax (strip.getX(), tickToX (note.startTick));
            const auto right = juce::jmin (strip.getRight(), tickToX (note.startTick + note.lengthTicks));

            if (right > x)
            {
                g.setColour (lanes::valueColour (lane.kind, (float) note.velocity / 127.0f).withAlpha (0.6f));
                g.fillRect (x, line.getY(), juce::jmax (1, right - x), line.getHeight());
            }
        }

        return;
    }

    const MidiSequence::Control* previous = nullptr;
    const auto colourFor = [&lane] (int value) { return lanes::valueColour (lane.kind, (float) value / (float) lane.maxValue()).withAlpha (0.6f); };

    // A step holds its colour; a ramp's colour follows its curve (in small pieces)
    const auto drawSegment = [&] (const MidiSequence::Control& from, const MidiSequence::Control* to, int untilX)
    {
        const auto x = juce::jmax (strip.getX(), tickToX (from.tick));
        const auto right = juce::jmin (strip.getRight(), untilX);

        if (right <= x)
            return;

        if (to == nullptr || ! from.ramp)
        {
            g.setColour (colourFor (from.value));
            g.fillRect (x, line.getY(), right - x, line.getHeight());
            return;
        }

        const auto startX = tickToX (from.tick), endX = tickToX (to->tick);

        for (auto px = x; px < right; px += 3)
        {
            const auto t = (float) (px - startX) / (float) juce::jmax (1, endX - startX);
            const auto v = (float) from.value + (float) (to->value - from.value) * MidiSequence::rampShape (t, from.bend, from.bendAt);
            g.setColour (colourFor (juce::roundToInt (v)));
            g.fillRect (px, line.getY(), juce::jmin (3, right - px), line.getHeight());
        }
    };

    for (auto& control : seq->getControls())
    {
        if (! lane.shows (control))
            continue;

        if (previous != nullptr)
            drawSegment (*previous, &control, tickToX (control.tick));

        previous = &control;
    }

    if (previous != nullptr)
        drawSegment (*previous, nullptr, strip.getRight());
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
        // The grid (= the snap positions) follows the zoom: bars, then halves, quarters...
        const auto ticksPerBar = map->getTicksPerBar (barTick);
        const auto step = axis.gridStep (*map, barTick);
        const auto ticksPerBeat = map->getTicksPerBeat (barTick);

        for (auto lineTick = barTick; lineTick < barTick + ticksPerBar && lineTick < endTick; lineTick += step)
        {
            const auto x = tickToX (lineTick);

            if (x < grid.getX())
                continue;

            const auto isBar = lineTick == barTick;
            const auto isBeat = (lineTick - barTick) % ticksPerBeat == 0;
            g.setColour (isBar ? juce::Colour (0xff45494f) : isBeat ? juce::Colour (0xff33373d) : juce::Colour (0xff2a2d32));
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
            else   // velocity: soft = blue, loud = cyan (lanes::valueColour)
                g.setColour (lanes::valueColour (lanes::Kind::velocity, (float) velocity / 127.0f));

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

    paintLanes (g, seq.get());

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

    // The lane value under the mouse
    if (hoverValue.isNotEmpty())
    {
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        const auto width = juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), hoverValue)) + 10;
        auto bubble = juce::Rectangle<int> (hoverPoint.x + 12, hoverPoint.y - 20, width, 16);

        if (bubble.getRight() > getWidth())
            bubble.setX (hoverPoint.x - 12 - width);

        g.setColour (juce::Colour (0xe0101113));
        g.fillRoundedRectangle (bubble.toFloat(), theme::corner);
        g.setColour (juce::Colours::white);
        g.drawText (hoverValue, bubble, juce::Justification::centred, false);
    }
}
