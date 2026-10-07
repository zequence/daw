#include "MainComponent.h"
#include "UserData.h"
#include "model/NoteNames.h"
#include "ui/EditorSettings.h"
#include "ui/ColorPalette.h"
#include "model/DemoSequence.h"
#include "api/CommandDispatcher.h"
#include "api/McpProcess.h"

#if JUCE_WINDOWS
// For the rack: which native window has the keys (a plugin editor embedded in ours can take them)
extern "C" __declspec (dllimport) void* __stdcall GetForegroundWindow();
extern "C" __declspec (dllimport) unsigned long __stdcall GetWindowThreadProcessId (void* window, unsigned long* processId);
extern "C" __declspec (dllimport) unsigned long __stdcall GetCurrentProcessId();

namespace
{
    // A window of this process that isn't one of JUCE's (ours, our plugin windows, menus): a plugin made it
    bool isForeignPluginWindow (void* window)
    {
        unsigned long process = 0;
        GetWindowThreadProcessId (window, &process);

        if (process != GetCurrentProcessId())
            return false;

        for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
            if (juce::ComponentPeer::getPeer (i)->getNativeHandle() == window)
                return false;

        return true;
    }
}
#endif

namespace
{
    constexpr int rightBarWidth  = 120;  // two transport buttons wide
    constexpr int topBarHeight   = 44;   // the transport bar along the top or bottom
    constexpr int statusHeight   = 22;

    // h:mm:ss:ms, hours only when non-zero (same convention as the timeline bar)
    juce::String formatPositionTime (double seconds)
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
}

MainComponent::MainComponent (AudioEngine& e, CommandDispatcher& dispatcher, McpProcess& mcp)
    : engine (e), commandDispatcher (dispatcher), mcpProcess (mcp)
{
    noteNames::middleCOctave() = editorSettings::middleCOctave (engine.getSettingsFile());   // Settings > Editor
    keys::Bindings::get().load (engine.getSettingsFile());                                   // Settings > Key commands
    sidePaneWidth = engine.getSettingsFile().getIntValue ("sidePaneWidth", 0);              // the right pane's width
    dockHeight = engine.getSettingsFile().getIntValue ("dockHeight", 0);
    transportPlace = (TransportPlace) juce::jlimit (0, 2, engine.getSettingsFile().getIntValue ("transportBar", 2));   // 0 top, 1 bottom, 2 right                    // the docked editor's height
    sidebar::trackRowHeightSetting() = juce::jlimit (sidebar::minTrackRowHeight, sidebar::maxTrackRowHeight,
                                                     engine.getSettingsFile().getIntValue ("trackHeight", sidebar::minTrackRowHeight));
    lanes::Settings::get().load (engine.getSettingsFile());                                  // Settings > Controller lanes

    // Keep the window state sane when projects change through the API.
    dispatcher.onBeforeProjectChange = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr)
            safe->pluginWindows.clear();
            safe->insertWindows.clear();
            safe->mixerView.destroyRack();   // its editors go before their plugins
    };

    dispatcher.onAfterProjectChange = [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::File& file)
    {
        if (safe == nullptr)
            return;

        safe->currentProjectFile = file;
        safe->loopButton.setToggleState (safe->engine.getTransport().isLooping(), juce::dontSendNotification);
        safe->bpmLabel.setText (juce::String (safe->engine.getTempoBpm(), 1), juce::dontSendNotification);

        const auto trackIds = safe->engine.getTrackIds();

        if (std::find (trackIds.begin(), trackIds.end(), safe->selectedTrack) == trackIds.end())
            safe->selectTrack (trackIds.empty() ? 0 : trackIds.front(), false);

        safe->updateWindowTitle();
    };

    addChildComponent (busyOverlay);
    engine.getBusyStatus().onChanged = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr)
            safe->busyOverlay.statusChanged();
    };

    dispatcher.onSelectTrack = [safe = juce::Component::SafePointer<MainComponent> (this)] (int id)
    {
        if (safe != nullptr)
            safe->selectTrack (id, false);
    };

    // --- Topbar ---
    menuButton.setButtonText (juce::String::fromUTF8 ("\xE2\x98\xB0"));   // hamburger
    menuButton.setTooltip ("Main menu");
    menuButton.onClick = [this] { showMainMenu(); };

    midiDomainButton.setTooltip ("Midi domain: the arrangement and the track list");
    midiDomainButton.onClick = [this] { setDomain (Domain::midi); };

    audioDomainButton.setTooltip ("Audio domain: audio regions and the channel list");
    audioDomainButton.onClick = [this] { setDomain (Domain::audio); };
    // The Instruments and History buttons toggle: clicking again returns to the
    // domain's arrange view (ISSUES.md "Top bar").
    instrumentsButton.setTooltip ("The instrument rack (I), in a pane on the right (click again or Esc to close)");
    historyButton.setTooltip ("Global history (H), in a pane on the right: click an entry to time-travel (click again or Esc to close)");
    // In the side pane they're its tabs: a click switches to the other (the expand button closes the pane)
    instrumentsButton.onClick = [this] { if (sidePane != SidePane::instruments) toggleSidePane (SidePane::instruments); };
    historyButton.onClick = [this] { if (sidePane != SidePane::history) toggleSidePane (SidePane::history); };

    sidePaneButton.setTooltip ("Expand / collapse the side pane: Instruments (I) and History (H)");
    sidePaneButton.onClick = [this] { toggleSidePane (sidePane != SidePane::none ? sidePane : lastSidePane); };

    rtzButton.setTooltip ("Return to start (Home)");
    theme::setButtonRole (rtzButton, "rtz");
    rtzButton.onClick = [this] { engine.getTransport().returnToZero(); };

    playButton.setTooltip ("Play/Stop (space)");
    theme::setButtonRole (playButton, "play");
    playButton.onClick = [this] { engine.getTransport().togglePlayStop(); };

    recordButton.setTooltip ("Record live MIDI onto the armed track (starts playback if stopped)");
    theme::setButtonRole (recordButton, "record");
    recordButton.onClick = [this]
    {
        if (engine.isRecording())
            engine.stopRecording();
        else if (! engine.startRecording())
            statusLabel.setText ("Add a track before recording", juce::dontSendNotification);
    };

    snapButton.setTooltip ("Snap to grid: notes, clips and the playhead lock to the grid (piano roll division, "
                           "bars in the arrangement, beats on the timeline). Off: free positions");
    snapButton.setClickingTogglesState (true);
    theme::setButtonRole (snapButton, "accent");
    snapButton.setToggleState (engine.getSettingsFile().getBoolValue ("snapToGrid", true), juce::dontSendNotification);
    timeAxis.snap = snapButton.getToggleState();
    snapButton.onClick = [this]
    {
        timeAxis.snap = snapButton.getToggleState();
        engine.getSettingsFile().setValue ("snapToGrid", timeAxis.snap);
        engine.getSettingsFile().saveIfNeeded();
    };

    loopButton.setTooltip ("Loop from the start to the end of the last clip");
    loopButton.setClickingTogglesState (true);
    theme::setButtonRole (loopButton, "loop");
    loopButton.onClick = [this]
    {
        auto& transport = engine.getTransport();
        transport.setLoopRegion (0, engine.getLoopEndTicks());
        transport.setLooping (loopButton.getToggleState());
    };

    // The transport mode: what Stop does (minimal; the tooltip says it)
    returnOnStopButton.setTooltip (juce::String::fromUTF8 ("Stop mode. Lit: return to starting position - Stop goes back to where "
                                                           "playback started. Off: stop at current time - Stop leaves the playhead where it is."));
    returnOnStopButton.setClickingTogglesState (true);
    theme::setButtonRole (returnOnStopButton, "loop");
    returnOnStopButton.setToggleState (engine.getSettingsFile().getBoolValue ("transportReturnOnStop", true), juce::dontSendNotification);
    engine.getTransport().setReturnOnStop (returnOnStopButton.getToggleState());
    returnOnStopButton.onClick = [this]
    {
        engine.getTransport().setReturnOnStop (returnOnStopButton.getToggleState());
        engine.getSettingsFile().setValue ("transportReturnOnStop", returnOnStopButton.getToggleState());
        engine.getSettingsFile().saveIfNeeded();
    };

    bpmLabel.setTooltip ("Tempo (double-click to edit)");
    bpmLabel.setEditable (false, true);
    bpmLabel.setJustificationType (juce::Justification::centred);
    bpmLabel.setColour (juce::Label::outlineColourId, juce::Colour (0xff43464d));
    bpmLabel.setText (juce::String (engine.getTempoBpm(), 1), juce::dontSendNotification);
    bpmLabel.onTextChange = [this]
    {
        engine.setTempoBpm (bpmLabel.getText().getDoubleValue());
        bpmLabel.setText (juce::String (engine.getTempoBpm(), 1), juce::dontSendNotification);
    };

    positionLabel.setJustificationType (juce::Justification::centredRight);
    positionLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    positionLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    positionLabel.setInterceptsMouseClicks (false, false);

    timeLabel.setJustificationType (juce::Justification::centredLeft);
    timeLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    timeLabel.setFont (juce::FontOptions (14.0f));
    timeLabel.setInterceptsMouseClicks (false, false);

    for (auto* b : { &menuButton, &midiDomainButton, &audioDomainButton, &instrumentsButton, &historyButton, &perfButton })
        theme::setButtonRole (*b, "topbar");

    perfButton.setTooltip ("Performance monitor (F12)");
    perfButton.setClickingTogglesState (true);
    perfButton.onClick = [this] { togglePerfPanel(); };

    // --- Timeline bar ---
    timelineBar.onHeightChanged = [this] { resized(); };
    timelineBar.onOpenSettings = [this] { openSettings(); };

    // --- Sidebar ---

    trackList.onAddTrack = [this] { addTrack(); };
    trackList.onAddTrackInFolder = [this] (auto folderId)
    {
        const auto id = engine.addTrack();
        engine.setTrackFolder (id, folderId);
        engine.setFolderCollapsed (folderId, false);
        selectTrack (id, false);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), id]
                                         { if (safe != nullptr) safe->chooseTrackOutput (id); });
    };
    trackList.onSelect = [this] (auto id)
    {
        lastSelectionInArrangement = false;
        selectTrack (id, false);
    };

    // Multi-selection arms every selected track when auto-record is on (ISSUES.md)
    trackList.onSelectionChanged = [this] (const std::set<AudioEngine::TrackId>& selection)
    {
        lastSelectionInArrangement = false;

        // While editing, Ctrl-selected tracks join the editor; the edited track stays edited
        if (isEditorShowing() && selection.size() > 1)
            pianoRollView.setTracks (inSidebarOrder (selection), pianoRollView.getTrack());

        if (! engine.getSettingsFile().getBoolValue (SettingsView::autoRecordOnSelectKey, true))
            return;

        if (selection.empty())
            engine.setArmedTrack (selectedTrack);
        else
            engine.setArmedTracks (selection, selection.count (selectedTrack) ? selectedTrack : *selection.begin());
    };
    trackList.onArm = [this] (auto id) { selectTrack (id, true); };
    trackList.onShowContextMenu = [this] (auto id) { showTrackContextMenu (id); };

    // --- Content views ---
    // The X on every view over the arrangement: back to the arrangement
    for (auto* close : { &pianoRollView.closeButton, &instrumentsView.closeButton, &instrumentEditorView.closeButton,
                         &expressionMapView.closeButton, &historyView.closeButton })
        close->onClick = [this] { showContent (domain == Domain::midi ? ContentView::midiRegions : ContentView::audioRegions); };


    pianoRollView.closeButton.onClick = [this]
    {
        if (isEditorDockedShowing())
            setEditorDocked (false);
        else
            showContent (domain == Domain::midi ? ContentView::midiRegions : ContentView::audioRegions);
    };

    instrumentsView.closeButton.onClick = [this] { toggleSidePane (SidePane::instruments); };
    historyView.closeButton.onClick = [this] { toggleSidePane (SidePane::history); };

    arrangementView.onSelectTrack = [this] (auto id)
    {
        lastSelectionInArrangement = true;
        selectTrack (id, false);
    };
    arrangementView.onOpenEditor = [this] (auto id)
    {
        // Several regions selected (the double-clicked one among them): edit all their tracks
        if (const auto tracks = arrangementView.selectedTracks(); tracks.size() > 1 && tracks.count (id))
        {
            openEditorOn (inSidebarOrder (tracks));
            return;
        }

        selectTrack (id, false);
        openEditorOn ({ id });
    };

    // Track height zoom: Ctrl+Shift+wheel over the list or the arrangement
    arrangementView.onTrackHeightZoom = [this] (int direction) { zoomTrackHeight (direction); };
    trackList.onTrackHeightZoom = [this] (int direction) { zoomTrackHeight (direction); };

    // Double-click on a folder's region: the editor on the folder's tracks
    arrangementView.onOpenEditorOnTracks = [this] (std::vector<AudioEngine::TrackId> tracks) { openEditorOn (std::move (tracks)); };
    trackList.onOpenEditorOnTracks = [this] (std::vector<AudioEngine::TrackId> tracks) { openEditorOn (std::move (tracks)); };
    trackList.onInstrumentMenu = [this] (AudioEngine::InstrumentId instrument)   // an instrument folder or its audio row
    {
        const auto safe = juce::Component::SafePointer<MainComponent> (this);
        juce::PopupMenu menu;
        menu.addItem ("Open the instrument's window", [safe, instrument] { if (safe != nullptr) safe->openPluginWindow (instrument); });
        menu.addSubMenu ("Color", colours::buildMenu (engine.getInstrumentColour (instrument), [safe, instrument] (juce::String hex)
        {
            if (safe != nullptr)
            {
                safe->engine.setInstrumentColour (instrument, hex);
                safe->trackList.refresh();
                safe->arrangementView.repaint();
            }
        }));
        menu.addSeparator();
        menu.addItem ("Remove instrument...", [safe, instrument] { if (safe != nullptr) safe->removeInstrumentAsking (instrument); });
        menu.showMenuAsync (juce::PopupMenu::Options());
    };

    // The editor's dropdown picked another of its tracks: that one is selected
    pianoRollView.onEditedTrackChanged = [this] (auto id) { selectTrack (id, false); };

    instrumentsView.onVeproSync = [this]
    {
        scanStatus = "Syncing to the VE Pro server...";

        auto message = juce::DynamicObject::Ptr (new juce::DynamicObject());
        message->setProperty ("id", 1);
        message->setProperty ("cmd", "vepro.sync");

        commandDispatcher.dispatchParsed (juce::var (message.get()),
            [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::var& reply)
            {
                if (safe == nullptr)
                    return;

                if (! reply.getProperty ("ok", false))
                {
                    safe->scanStatus = "VE Pro sync failed: " + reply.getProperty ("error", {}).toString();
                }
                else
                {
                    const auto result = reply.getProperty ("result", {});
                    safe->scanStatus = "VE Pro sync: " + result.getProperty ("instances", 0).toString()
                                         + " instances, " + result.getProperty ("tracksCreated", 0).toString()
                                         + " new tracks, " + result.getProperty ("channelsSynced", 0).toString()
                                         + " channels";

                    if (auto* notes = result.getProperty ("notes", {}).getArray())
                        for (auto& note : *notes)
                            juce::Logger::writeToLog ("VE Pro sync: " + note.toString());
                }

                juce::Timer::callAfterDelay (10000, [safe] { if (safe != nullptr) safe->scanStatus.clear(); });
            });
    };

    instrumentsView.onOpenPluginGui = [this] (auto id) { openPluginWindow (id); };
    commandDispatcher.onBeforeInstrumentRemove = [this] (int id)
    {
        pluginWindows.erase (id);
        closeInsertWindows (engine.getAudioChannelForInstrument (id), -1);   // its channel's inserts go with it
        mixerView.destroyRack();
    };
    mixerView.onOpenInsert = [this] (auto channel, int slot) { openInsertWindow (channel, slot); };
    mixerView.onChannelSelected = [this] (auto channel) { channelList.selectChannel (channel); };
    mixerView.onBeforeInsertRemove = [this] (auto channel, int slot) { closeInsertWindows (channel, slot); };

    // MIDI controllers (Settings > Audio & MIDI): their assigned controls choose articulations
    engine.onControllerMidi = [this] (const juce::MidiMessage& message)
    {
        triggerArticulation ([&message] (const ExpressionMap& map) { return map.findByTrigger (message); });
    };
    instrumentsView.onRemoveInstrument = [this] (auto id) { removeInstrumentAsking (id); };
    instrumentsView.onEditInstrument = [this] (auto id)
    {
        instrumentEditorView.setInstrument (id);
        showContent (ContentView::instrumentEditor);
    };

    instrumentEditorView.onBack = [this]   // back where it was opened from, the rack still in its pane
    {
        sidePane = SidePane::instruments;
        showContent (mainView);
        resized();
    };
    instrumentEditorView.onEditMap = [this] (const juce::String& mapName)
    {
        expressionMapView.select (mapName);
        showContent (ContentView::expressionMaps);
    };
    expressionMapView.onBack = [this] { showContent (ContentView::instrumentEditor); };
    instrumentEditorView.onOpenPluginGui = [this] (auto id) { openPluginWindow (id); };

    settingsView.onClose = [this] { closeSettings(); };
    settingsView.onStartScan = [this] (auto args) { startPluginScan (std::move (args)); };
    settingsView.onPluginOnTopChanged = [this] (bool onTop)
    {
        for (auto& [id, window] : pluginWindows)
            window->setAlwaysOnTop (onTop);
    };

    settingsView.onMcpToggled = [this] (bool enabled)
    {
        if (enabled)
        {
            const auto port = engine.getSettingsFile().getIntValue (SettingsView::mcpPortKey, 53218);
            settingsView.setMcpStatus (mcpProcess.start (port)
                                           ? "Running at " + mcpProcess.getUrl()
                                           : "Failed to start - is Python on PATH and the script in tools/mcp?");
        }
        else
        {
            mcpProcess.stop();
            settingsView.setMcpStatus ("Off");
        }
    };

    settingsView.setMcpStatus (mcpProcess.isRunning() ? "Running at " + mcpProcess.getUrl() : "Off");

    // --- Bottom ---
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setFont (juce::FontOptions (12.0f));

    for (auto* c : std::initializer_list<juce::Component*> {
             &menuButton, &midiDomainButton, &audioDomainButton, &instrumentsButton, &historyButton, &sidePaneButton,
             &rtzButton, &playButton, &recordButton, &loopButton, &returnOnStopButton, &snapButton, &bpmLabel, &positionLabel, &timeLabel, &perfButton,
             &sidebarHeader, &masterMeter, &trackList, &channelList, &sidebarResizer, &sidePaneResizer, &dockHandle,
             &timelineBar, &arrangementView, &audioRegionsView, &pianoRollView, &mixerView,
             &instrumentsView, &instrumentEditorView, &expressionMapView, &historyView, &settingsView,
             &statusLabel })
        addAndMakeVisible (c);

    for (auto* b : std::initializer_list<juce::Component*> { &menuButton, &midiDomainButton, &audioDomainButton,
                                                             &instrumentsButton, &historyButton, &sidePaneButton, &rtzButton,
                                                             &playButton, &recordButton, &loopButton, &returnOnStopButton, &snapButton, &perfButton })
        b->setWantsKeyboardFocus (false);

    addChildComponent (perfPanel);
    setWantsKeyboardFocus (true);

    engine.getDeviceManager().addChangeListener (this);
    engine.getKnownPlugins().addChangeListener (this);

    updateViewVisibility();
    setSize (1100, 700);
    startTimerHz (30);

    // A fresh, empty start gets one playable track with the default instrument.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
                                     { if (safe != nullptr) safe->createDefaultTrack(); });
}

void MainComponent::createDefaultTrack()
{
    if (! engine.getTrackIds().empty() || ! engine.getInstruments().empty())
        return;

    // The startup project (hamburger menu > "Save as startup project"), opened untitled
    if (const auto startup = startupProjectFile(); startup.existsAsFile())
    {
        statusLabel.setText ("Opening the startup project...", juce::dontSendNotification);

        engine.loadProject (startup, [safe = juce::Component::SafePointer<MainComponent> (this), startup]
                                     (bool ok, const juce::String& warnings)
        {
            if (safe == nullptr)
                return;

            safe->applyLoadedProject (startup, ok, warnings);
            safe->currentProjectFile = {};   // a starting point: saving asks for a name
            safe->updateWindowTitle();
            safe->engine.markProjectClean();

            if (ok)
                safe->statusLabel.setText ("Started from the startup project", juce::dontSendNotification);
        });
        return;
    }

    const auto wanted = engine.getSettingsFile().getValue ("defaultInstrument", "FabFilter Twin");

    if (wanted.isEmpty())   // empty setting disables the default track's instrument
        return;
    const auto track = engine.addTrack();
    selectTrack (track, false);

    juce::PluginDescription description;
    bool found = false;

    for (auto& type : engine.getInstrumentTypes())
    {
        // Plugins often report their name without the maker ("Twin 3" by "FabFilter"),
        // so match against the combined label as well.
        if (type.name.containsIgnoreCase (wanted)
             || (type.manufacturerName + " " + type.name).containsIgnoreCase (wanted))
        {
            description = type;
            found = true;
            break;
        }
    }

    if (! found)
    {
        juce::Logger::writeToLog ("Default instrument '" + wanted + "' not in the plugin cache; track left unrouted");
        engine.markProjectClean();   // the untouched startup state shouldn't nag about saving
        return;
    }

    statusLabel.setText ("Loading " + description.name + "...", juce::dontSendNotification);

    engine.addInstrument (description,
        [safe = juce::Component::SafePointer<MainComponent> (this), track, name = description.name]
        (auto instrumentId, const juce::String& error)
        {
            if (safe == nullptr)
                return;

            safe->statusLabel.setText ("", juce::dontSendNotification);

            if (instrumentId == 0)
            {
                juce::Logger::writeToLog ("Default instrument failed to load: " + error);
                return;
            }

            safe->engine.addTrackOutput (track, instrumentId, 1);
            safe->autoNameTrackForOutput (track, instrumentId);

            // The untouched startup state shouldn't nag about saving - but only if the
            // user hasn't done anything while the instrument was loading. The history
            // knows: more entries than Start + track + instrument means real work.
            const auto entries = safe->commandDispatcher.run ("history.list")["result"];

            if (entries.getArray() == nullptr || entries.getArray()->size() <= 3)
                safe->engine.markProjectClean();
        });
}

// Remote control of articulations: a key command or a MIDI controller's control chooses
// an articulation of the current track's map, as the articulation panel does (MILESTONES.md
// "Articulation remote control"). With no notes selected it becomes what new notes get; either
// way the instrument switches at once (PianoRollView sends the sound slot), so it plays live.
bool MainComponent::triggerArticulation (const std::function<std::optional<ExpressionMap::Target> (const ExpressionMap&)>& find)
{
    const auto track = pianoRollView.getTrack() != 0 ? pianoRollView.getTrack() : selectedTrack;
    const auto map = track != 0 ? engine.getTrackExpressionMap (track) : std::nullopt;

    if (! map.has_value())
        return false;

    const auto target = find (*map);

    if (! target.has_value())
        return false;

    if (pianoRollView.getTrack() != track)
        pianoRollView.setTrack (track);

    if (const auto live = pianoRollView.remoteChoose (target->group, target->name))   // (the editor sends it to the player)
        statusLabel.setText ("Articulation: " + ExpressionMap::labelOf (*live), juce::dontSendNotification);
    else
    {
        statusLabel.setText ("Articulation of the selected notes: " + target->name, juce::dontSendNotification);
    }

    return true;
}

// Remove an instrument and all its MIDI (the tracks that play it). Asks first - "Are you sure?" with a
// "Don't ask again" toggle (the setting askRemoveInstrument)
void MainComponent::removeInstrumentAsking (AudioEngine::InstrumentId id)
{
    const auto name = engine.getInstrumentName (id);
    juce::StringArray playing;

    for (auto trackId : engine.getTrackIds())
        for (auto& output : engine.getTrackOutputs (trackId))
            if (output.instrument == id)
            {
                playing.addIfNotAlreadyThere (engine.getTrackName (trackId));
                break;
            }

    const auto remove = [safe = juce::Component::SafePointer<MainComponent> (this), id, name]
    {
        if (safe == nullptr)
            return;

        auto params = new juce::DynamicObject();
        params->setProperty ("instrumentId", id);
        params->setProperty ("removeTracks", true);
        const auto reply = safe->commandDispatcher.run ("instrument.remove", juce::var (params));

        if (! (bool) reply["ok"])
        {
            safe->statusLabel.setText ("Couldn't remove " + name + ": " + reply["error"].toString(), juce::dontSendNotification);
            return;
        }

        const auto trackIds = safe->engine.getTrackIds();

        if (std::find (trackIds.begin(), trackIds.end(), safe->selectedTrack) == trackIds.end())
            safe->selectTrack (trackIds.empty() ? 0 : trackIds.front(), false);

        safe->trackList.refresh();
        safe->statusLabel.setText ("Removed " + name, juce::dontSendNotification);
    };

    if (! engine.getSettingsFile().getBoolValue ("askRemoveInstrument", true))
    {
        remove();
        return;
    }

    const auto message = "Remove '" + name + "'"
                           + (playing.isEmpty() ? juce::String ("?")
                                                : " and its " + juce::String (playing.size()) + (playing.size() == 1 ? " track" : " tracks")
                                                    + " (" + playing.joinIntoString (", ") + ")?")
                           + "\n\nThe instrument's settings and its MIDI go with it.";

    auto* window = new juce::AlertWindow ("Remove instrument", message, juce::MessageBoxIconType::QuestionIcon, this);
    auto* neverAsk = new juce::ToggleButton ("Don't ask again");
    neverAsk->setSize (240, 24);
    window->addCustomComponent (neverAsk);
    window->addButton ("Remove", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer<MainComponent> (this), remove, neverAsk] (int result)
        {
            const auto dontAsk = neverAsk->getToggleState();
            delete neverAsk;   // (not owned by the window)

            if (safe == nullptr || result != 1)
                return;

            if (dontAsk)
            {
                safe->engine.getSettingsFile().setValue ("askRemoveInstrument", false);
                safe->engine.getSettingsFile().saveIfNeeded();
            }

            remove();
        }), true);
}

MainComponent::~MainComponent()
{
    engine.getBusyStatus().onChanged = nullptr;   // the engine outlives this window
    stopTimer();
    engine.getDeviceManager().removeChangeListener (this);
    engine.getKnownPlugins().removeChangeListener (this);

    pluginWindows.clear();

    insertWindows.clear();

    mixerView.destroyRack();   // its editors go before their plugins
}

//==============================================================================
void MainComponent::addTrack()
{
    const auto id = engine.addTrack();
    selectTrack (id, false);

    // Ask where the new track should send its MIDI.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), id]
                                     { if (safe != nullptr) safe->chooseTrackOutput (id); });
}

void MainComponent::removeTrack (AudioEngine::TrackId id)
{
    engine.removeTrack (id);

    if (selectedTrack == id)
    {
        const auto remaining = engine.getTrackIds();
        selectTrack (remaining.empty() ? 0 : remaining.front(), false);
    }

    trackList.refresh();
}

void MainComponent::selectTrack (AudioEngine::TrackId id, bool forceArm)
{
    // Step timing goes to the log when a selection is slow (big projects)
    auto last = juce::Time::getMillisecondCounterHiRes();
    juce::StringArray slowSteps;

    const auto lap = [&last, &slowSteps] (const char* step)
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (now - last > 20.0)
            slowSteps.add (juce::String (step) + " " + juce::String (juce::roundToInt (now - last)) + " ms");

        last = now;
    };

    selectedTrack = id;
    trackList.setSelectedTrack (id);
    lap ("trackList");

    const auto autoArm = engine.getSettingsFile().getBoolValue (SettingsView::autoRecordOnSelectKey, true);

    if (id != 0 && (autoArm || forceArm) && engine.getArmedTrack() != id)
    {
        engine.setArmedTrack (id);
    }

    lap ("arm");

    if (sidePane == SidePane::instruments)
        instrumentsView.focusTrack (id);

    if (isEditorShowing())
        pianoRollView.setTrack (id);   // the editor (full or docked) follows the selected track

    lap ("views");

    historyView.setSelectedTrack (id);
    updatePlaceholders();
    lap ("rest");

    if (! slowSteps.isEmpty())
        juce::Logger::writeToLog ("Slow track selection: " + slowSteps.joinIntoString (", "));
}

void MainComponent::showTrackContextMenu (AudioEngine::TrackId id)
{
    selectTrack (id, false);

    const auto safe = juce::Component::SafePointer<MainComponent> (this);
    juce::PopupMenu menu;

    // A track in an instrument folder: its routing is the instrument's (no output or port choices here),
    // and removing it removes the instrument (with all its tracks)
    const auto instrument = engine.getTrackInstrument (id);

    if (instrument == 0)
        menu.addItem ("Set output...", [safe, id] { if (safe != nullptr) safe->chooseTrackOutput (id); });

    menu.addItem ("Open / close instrument GUI (" + keys::Bindings::describe (keys::Bindings::get().keysFor ("track.instrumentGui")) + ")",
                  ! engine.getTrackOutputs (id).empty(), false,
                  [safe, id] { if (safe != nullptr) safe->openTrackPluginWindow (id); });
    menu.addItem ("Record mode: replace", true, engine.isTrackRecordReplace (id), [safe, id]
    {
        // Off = Add (new takes merge into the clip); on = from the first played note,
        // existing material is replaced until you stop
        if (safe != nullptr)
            safe->engine.setTrackRecordReplace (id, ! safe->engine.isTrackRecordReplace (id));
    });
    if (instrument == 0)
        menu.addItem ("Port and channel...", [safe, id] { if (safe != nullptr) safe->showTrackOutputConfig (id); });

    menu.addSeparator();

    // Contextual add (ISSUES.md "Sidebar"): the new track lands right below this one
    menu.addItem ("New track below", [safe, id]
    {
        if (safe == nullptr)
            return;

        const auto parent = safe->engine.getTrackFolder (id);
        int index = 0;

        for (auto& item : safe->engine.getSidebarItems (true, false))
        {
            if (item.parent == parent && item.isTreeChild())
                ++index;

            if (item.member == id)
                break;
        }

        const auto newId = safe->engine.addTrack();
        safe->engine.moveSidebarItems (true, {}, { newId }, parent, index);
        safe->selectTrack (newId, false);
        juce::MessageManager::callAsync ([safe, newId] { if (safe != nullptr) safe->chooseTrackOutput (newId); });
    });

    // Folders (Cubase-style grouping in the sidebar)
    juce::PopupMenu moveTo;
    moveTo.addItem ("Top level", true, engine.getTrackFolder (id) == 0,
                    [safe, id] { if (safe != nullptr) safe->engine.setTrackFolder (id, 0); });

    for (auto folderId : engine.getFolderIds (true))
        moveTo.addItem (engine.getFolderName (folderId), true, engine.getTrackFolder (id) == folderId,
                        [safe, id, folderId] { if (safe != nullptr) safe->engine.setTrackFolder (id, folderId); });

    moveTo.addSeparator();
    moveTo.addItem ("New folder", [safe, id]
    {
        if (safe != nullptr)
            safe->engine.setTrackFolder (id, safe->engine.addFolder (true));
    });

    menu.addSubMenu ("Move to folder", moveTo);
    if (instrument == 0)   // (in an instrument folder only the folder is coloured)
        menu.addSubMenu ("Color", colours::buildMenu (engine.getTrackColour (id),
                                                      [safe, id] (juce::String hex)
                                                      { if (safe != nullptr) safe->engine.setTrackColour (id, hex); }));
    menu.addSeparator();
    menu.addItem ("Demo clip", [safe, id]
    {
        if (safe == nullptr)
            return;

        safe->engine.setTrackSequence (id, makeDemoSequence());

        if (! safe->engine.getTransport().isPlaying())
            safe->engine.getTransport().returnToZero();
    });
    menu.addItem ("Clear clip", engine.getTrackSequence (id) != nullptr, false,
                  [safe, id] { if (safe != nullptr) safe->engine.setTrackSequence (id, nullptr); });
    menu.addSeparator();
    if (instrument != 0)
        menu.addItem ("Remove instrument...", [safe, instrument] { if (safe != nullptr) safe->removeInstrumentAsking (instrument); });
    else
        menu.addItem ("Remove track", [safe, id]
        {
            juce::MessageManager::callAsync ([safe, id] { if (safe != nullptr) safe->removeTrack (id); });
        });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

// Tracks that still carry an automatic name ("Track 3", or a previous output's name)
// follow their output; manually renamed tracks are left alone.
void MainComponent::autoNameTrackForOutput (AudioEngine::TrackId trackId, AudioEngine::InstrumentId instrumentId)
{
    const auto current = engine.getTrackName (trackId);

    bool isAutomatic = current.startsWith ("Track ")
                         && current.fromFirstOccurrenceOf ("Track ", false, false).containsOnly ("0123456789");

    if (! isAutomatic)
        for (auto& [id, name] : engine.getInstruments())
            if (current == name)
            {
                isAutomatic = true;
                break;
            }

    if (isAutomatic)
        engine.setTrackName (trackId, engine.getInstrumentName (instrumentId));
}

// The MIDI track configuration: port and channel of the track's output (greyed out for a synced track)
void MainComponent::showTrackOutputConfig (AudioEngine::TrackId trackId)
{
    auto panel = std::make_unique<TrackOutputPanel> (engine, commandDispatcher, trackId);
    const auto mouse = juce::Desktop::getMousePosition();
    juce::CallOutBox::launchAsynchronously (std::move (panel), juce::Rectangle<int> (mouse.x, mouse.y, 1, 1), nullptr);
}

void MainComponent::chooseTrackOutput (AudioEngine::TrackId trackId)
{
    const auto outputs = engine.getTrackOutputs (trackId);
    const auto safe = juce::Component::SafePointer<MainComponent> (this);

    juce::PopupMenu menu;

    for (auto& [instrumentId, name] : engine.getInstruments())
    {
        juce::PopupMenu channels;

        for (int ch = 1; ch <= 16; ++ch)
        {
            const auto current = ! outputs.empty()
                                   && outputs.front().instrument == instrumentId
                                   && outputs.front().midiChannel == ch
                                   && outputs.front().midiPort == 1;
            const auto channelName = engine.getInstrumentChannelName (instrumentId, ch);

            channels.addItem ("Channel " + juce::String (ch) + (channelName.isNotEmpty() ? "  (" + channelName + ")" : ""),
                              true, current,
                              [safe, trackId, id = instrumentId, ch]
                              {
                                  if (safe != nullptr)
                                  {
                                      safe->engine.clearTrackOutputs (trackId);
                                      safe->engine.addTrackOutput (trackId, id, ch);
                                      safe->autoNameTrackForOutput (trackId, id);
                                  }
                              });
        }

        // Named channels on further ports (multiport instruments like VE Pro)
        bool addedPortSeparator = false;

        for (auto& info : engine.getInstrumentMidiChannels (instrumentId))
        {
            if (info.midiPort <= 1)
                continue;

            if (! addedPortSeparator)
            {
                channels.addSeparator();
                addedPortSeparator = true;
            }

            const auto current = ! outputs.empty()
                                   && outputs.front().instrument == instrumentId
                                   && outputs.front().midiChannel == info.midiChannel
                                   && outputs.front().midiPort == info.midiPort;

            channels.addItem ("Port " + juce::String (info.midiPort) + " ch " + juce::String (info.midiChannel)
                                + (info.name.isNotEmpty() ? "  (" + info.name + ")" : ""),
                              true, current,
                              [safe, trackId, id = instrumentId, ch = info.midiChannel, port = info.midiPort]
                              {
                                  if (safe != nullptr)
                                  {
                                      safe->engine.clearTrackOutputs (trackId);
                                      safe->engine.addTrackOutput (trackId, id, ch, port);
                                      safe->autoNameTrackForOutput (trackId, id);
                                  }
                              });
        }

        menu.addSubMenu (name, channels);
    }

    if (! engine.getInstruments().empty())
        menu.addSeparator();

    menu.addItem ("New instrument...", [safe, trackId] { if (safe != nullptr) safe->chooseNewInstrumentFor (trackId); });

    if (! outputs.empty())
        menu.addItem ("No output", [safe, trackId] { if (safe != nullptr) safe->engine.clearTrackOutputs (trackId); });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

void MainComponent::chooseNewInstrumentFor (AudioEngine::TrackId trackId)
{
    const auto types = engine.getInstrumentTypes();

    juce::PopupMenu menu;

    if (types.isEmpty())
        menu.addItem (1, "No instruments found - scan in Settings > Plugins first", false, false);
    else
        juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortByManufacturer);

    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [safe = juce::Component::SafePointer<MainComponent> (this), trackId, types] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;

                            const auto index = juce::KnownPluginList::getIndexChosenByMenu (types, result);

                            if (! juce::isPositiveAndBelow (index, types.size()))
                                return;

                            const auto description = types.getReference (index);
                            safe->statusLabel.setText ("Loading " + description.name + "...", juce::dontSendNotification);

                            safe->engine.addInstrument (description,
                                [safe, trackId, name = description.name] (auto instrumentId, const juce::String& error)
                                {
                                    if (safe == nullptr)
                                        return;

                                    safe->statusLabel.setText ("", juce::dontSendNotification);

                                    if (instrumentId == 0)
                                    {
                                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                                "Couldn't load plugin", name + "\n\n" + error);
                                        return;
                                    }

                                    safe->engine.clearTrackOutputs (trackId);
                                    safe->engine.addTrackOutput (trackId, instrumentId, 1);
                                    safe->autoNameTrackForOutput (trackId, instrumentId);
                                    safe->openPluginWindow (instrumentId);
                                });
                        });
}

void MainComponent::openPluginWindow (AudioEngine::InstrumentId instrumentId)
{
    auto& window = pluginWindows[instrumentId];

    if (window != nullptr)
    {
        window->setVisible (true);
        window->toFront (true);
        window->ensureOnScreen();   // it may have been left off-screen, or the displays changed
        return;
    }

    auto* plugin = engine.getInstrumentPlugin (instrumentId);

    if (plugin == nullptr)
    {
        pluginWindows.erase (instrumentId);
        return;
    }

    const auto onTop = engine.getSettingsFile().getBoolValue (SettingsView::pluginWindowsOnTopKey, true);

    window = std::make_unique<PluginWindow> (*plugin, engine.getInstrumentName (instrumentId), onTop);
    window->onKey = [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::KeyPress& key)
    {
        return safe != nullptr && safe->keyPressed (key);   // the main window's keys, from the plugin window too
    };
    window->onClose = [safe = juce::Component::SafePointer<MainComponent> (this), instrumentId]
    {
        // Defer deletion: we're inside the window's own callback.
        juce::MessageManager::callAsync ([safe, instrumentId]
                                         { if (safe != nullptr) safe->pluginWindows.erase (instrumentId); });
    };
}

// An insert's window: like an instrument's (one per insert, kept until closed)
void MainComponent::openInsertWindow (AudioEngine::AudioChannelId channel, int slot)
{
    const auto key = std::pair (channel, slot);
    auto& window = insertWindows[key];

    if (window != nullptr)
    {
        window->setVisible (true);
        window->toFront (true);
        window->ensureOnScreen();
        return;
    }

    auto* plugin = engine.getInsertPlugin (channel, slot);

    if (plugin == nullptr)
    {
        insertWindows.erase (key);
        return;
    }

    const auto onTop = engine.getSettingsFile().getBoolValue (SettingsView::pluginWindowsOnTopKey, true);
    window = std::make_unique<PluginWindow> (*plugin, engine.getAudioChannelName (channel) + " - " + plugin->getName(), onTop);
    window->onKey = [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::KeyPress& key)
    {
        return safe != nullptr && safe->keyPressed (key);
    };
    window->onClose = [safe = juce::Component::SafePointer<MainComponent> (this), key]
    {
        juce::MessageManager::callAsync ([safe, key] { if (safe != nullptr) safe->insertWindows.erase (key); });
    };
}

// Closes a channel's insert windows (slot -1: all of them) - before their plugins are deleted
void MainComponent::closeInsertWindows (AudioEngine::AudioChannelId channel, int slot)
{
    for (auto it = insertWindows.begin(); it != insertWindows.end();)
        it = it->first.first == channel && (slot < 0 || it->first.second == slot) ? insertWindows.erase (it) : std::next (it);
}

//==============================================================================
void MainComponent::setDomain (Domain newDomain)
{
    domain = newDomain;
    showContent (domain == Domain::midi ? ContentView::midiRegions : ContentView::audioRegions);
}

void MainComponent::showContent (ContentView view)
{
    contentView = view;

    if (view == ContentView::midiRegions || view == ContentView::midiEditor || view == ContentView::audioRegions)
        mainView = view;

    if (view == ContentView::midiEditor)
        pianoRollView.setTrack (selectedTrack);

    updatePlaceholders();
    updateViewVisibility();
    resized();   // the arrangement shares its area with the docked editor

    if (view == ContentView::midiEditor)
        pianoRollView.grabKeyboardFocus();
}

// The MIDI editor docked under the arrangement (the handle opens, closes and sizes it)
void MainComponent::setEditorDocked (bool docked)
{
    if (docked && ! editorDocked)
        pianoRollView.setTrack (selectedTrack);   // it shows the selected track, as the full editor does

    editorDocked = docked;
    updateViewVisibility();
    resized();
    repaint();
    dockHandle.repaint();
}

// The docked editor's border: rounded at the top, down both sides
void MainComponent::paintOverChildren (juce::Graphics& g)
{
    if (! isEditorDockedShowing() || ! pianoRollView.isVisible())
        return;

    const auto box = pianoRollView.getBounds().expanded (2, 0).withTop (pianoRollView.getY() - 2).toFloat().reduced (0.5f);
    constexpr float radius = 7.0f;
    juce::Path border;
    border.startNewSubPath (box.getX(), box.getBottom());
    border.lineTo (box.getX(), box.getY() + radius);
    border.quadraticTo (box.getX(), box.getY(), box.getX() + radius, box.getY());
    border.lineTo (box.getRight() - radius, box.getY());
    border.quadraticTo (box.getRight(), box.getY(), box.getRight(), box.getY() + radius);
    border.lineTo (box.getRight(), box.getBottom());
    g.setColour (juce::Colour (0xff6c7380));
    g.strokePath (border, juce::PathStrokeType (1.5f));
}

void MainComponent::setTransportPlace (TransportPlace where)
{
    transportPlace = where;
    engine.getSettingsFile().setValue ("transportBar", (int) where);
    engine.getSettingsFile().saveIfNeeded();
    resized();
    repaint();
}

void MainComponent::showMainMenu()
{
    const auto safe = juce::Component::SafePointer<MainComponent> (this);
    juce::PopupMenu menu;

    menu.addItem ("New project", [safe] { if (safe != nullptr) safe->newProject(); });
    menu.addItem ("Load project...", [safe] { if (safe != nullptr) safe->loadProjectDialog(); });
    menu.addItem ("Save project", [safe] { if (safe != nullptr) safe->saveProject (false); });
    menu.addItem ("Save project as...", [safe] { if (safe != nullptr) safe->saveProject (true); });
    menu.addSeparator();

    juce::PopupMenu place;

    for (auto [name, where] : { std::pair ("Top", TransportPlace::top), std::pair ("Bottom", TransportPlace::bottom), std::pair ("Right", TransportPlace::right) })
        place.addItem (name, true, transportPlace == where, [safe, where] { if (safe != nullptr) safe->setTransportPlace (where); });

    menu.addSubMenu ("Transport bar", place);
    menu.addSeparator();
    menu.addItem ("Save as startup project", [safe]
    {
        if (safe == nullptr)
            return;

        const auto ok = safe->engine.saveProject (startupProjectFile());
        safe->statusLabel.setText (ok ? "Saved as the startup project: the app starts with it from now on"
                                      : "Saving the startup project FAILED", juce::dontSendNotification);
    });
    menu.addItem ("Stop using the startup project", startupProjectFile().existsAsFile(), false, [safe]
    {
        if (safe == nullptr)
            return;

        startupProjectFile().moveFileTo (startupProjectFile().withFileExtension (".odaw.bak"));
        safe->statusLabel.setText ("The app starts with the default instrument again", juce::dontSendNotification);
    });
    menu.addSeparator();
    menu.addItem ("Settings...", [safe] { if (safe != nullptr) safe->openSettings(); });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuButton));
}

//==============================================================================
juce::File MainComponent::getProjectsDirectory()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("OrchestralDAW Projects");
    dir.createDirectory();
    return dir;
}

void MainComponent::confirmDiscard (const juce::String& action, std::function<void()> proceed)
{
    if (engine.getTrackIds().empty() && engine.getInstruments().empty())
    {
        proceed();
        return;
    }

    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon,
                                        action, "The current project will be closed. Anything not saved is lost.",
                                        action, "Cancel", this,
                                        juce::ModalCallbackFunction::create (
                                            [proceed = std::move (proceed)] (int result)
                                            {
                                                if (result == 1)
                                                    proceed();
                                            }));
}

void MainComponent::newProject()
{
    confirmDiscard ("New project", [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe == nullptr)
            return;

        safe->pluginWindows.clear();

        safe->insertWindows.clear();

        safe->mixerView.destroyRack();   // its editors go before their plugins
        safe->engine.clearProject();
        safe->currentProjectFile = juce::File();
        safe->selectedTrack = 0;
        safe->loopButton.setToggleState (false, juce::dontSendNotification);
        safe->bpmLabel.setText (juce::String (safe->engine.getTempoBpm(), 1), juce::dontSendNotification);
        safe->trackList.setSelectedTrack (0);
        safe->setDomain (Domain::midi);
        safe->updateWindowTitle();
        safe->statusLabel.setText ("New project", juce::dontSendNotification);
    });
}

void MainComponent::loadProjectDialog()
{
    confirmDiscard ("Load project", [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe == nullptr)
            return;

        safe->fileChooser = std::make_unique<juce::FileChooser> ("Load project",
                                                                 getProjectsDirectory(), "*.odaw");

        safe->fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe] (const juce::FileChooser& chooser)
            {
                if (safe == nullptr || chooser.getResult() == juce::File())
                    return;

                const auto file = chooser.getResult();
                safe->pluginWindows.clear();
                safe->insertWindows.clear();
                safe->mixerView.destroyRack();   // its editors go before their plugins
                safe->statusLabel.setText ("Loading " + file.getFileName() + "...", juce::dontSendNotification);

                safe->engine.loadProject (file, [safe, file] (bool ok, const juce::String& warnings)
                {
                    if (safe != nullptr)
                        safe->applyLoadedProject (file, ok, warnings);
                });
            });
    });
}

void MainComponent::applyLoadedProject (const juce::File& file, bool ok, const juce::String& warnings)
{
    currentProjectFile = ok ? file : juce::File();

    loopButton.setToggleState (false, juce::dontSendNotification);
    bpmLabel.setText (juce::String (engine.getTempoBpm(), 1), juce::dontSendNotification);

    const auto trackIds = engine.getTrackIds();
    selectTrack (trackIds.empty() ? 0 : trackIds.front(), false);
    setDomain (Domain::midi);
    updateWindowTitle();

    if (isShowing())
        grabKeyboardFocus();   // the selected track's keys work without a click first

    if (! ok)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't load project", warnings);
        statusLabel.setText ("Load failed", juce::dontSendNotification);
        return;
    }

    statusLabel.setText ("Loaded " + file.getFileName(), juce::dontSendNotification);

    if (warnings.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                "Project loaded with warnings", warnings);
}

void MainComponent::saveProject (bool saveAs, std::function<void()> onSaved)
{
    if (! saveAs && currentProjectFile != juce::File())
    {
        const auto ok = engine.saveProject (currentProjectFile);
        statusLabel.setText (ok ? "Saved " + currentProjectFile.getFileName()
                                : "Save FAILED", juce::dontSendNotification);

        if (ok && onSaved)
            onSaved();

        return;
    }

    fileChooser = std::make_unique<juce::FileChooser> ("Save project",
                                                       getProjectsDirectory().getChildFile ("Untitled.odaw"), "*.odaw");

    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<MainComponent> (this), onSaved = std::move (onSaved)]
        (const juce::FileChooser& chooser)
        {
            if (safe == nullptr || chooser.getResult() == juce::File())
                return;

            const auto file = chooser.getResult().withFileExtension ("odaw");

            if (safe->engine.saveProject (file))
            {
                safe->currentProjectFile = file;
                safe->updateWindowTitle();
                safe->statusLabel.setText ("Saved " + file.getFileName(), juce::dontSendNotification);

                if (onSaved)
                    onSaved();
            }
            else
            {
                safe->statusLabel.setText ("Save FAILED", juce::dontSendNotification);
            }
        });
}

void MainComponent::confirmQuit()
{
    if (! engine.isProjectDirty())
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
        return;
    }

    juce::AlertWindow::showYesNoCancelBox (juce::MessageBoxIconType::QuestionIcon,
        "Save before closing?", "The project has unsaved changes.",
        "Save", "Discard", "Cancel", this,
        juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<MainComponent> (this)] (int result)
            {
                if (safe == nullptr)
                    return;

                if (result == 1)        // Save, then quit
                    safe->saveProject (false, [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
                else if (result == 2)   // Discard
                    juce::JUCEApplication::getInstance()->systemRequestedQuit();
            }));
}

void MainComponent::updateWindowTitle()
{
    if (auto* window = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))
        window->setName (currentProjectFile != juce::File()
                             ? "DAW+ - " + currentProjectFile.getFileNameWithoutExtension()
                             : juce::String ("DAW+"));
}

void MainComponent::openSettings()
{
    settingsOpen = true;
    updateViewVisibility();
}

void MainComponent::closeSettings()
{
    settingsOpen = false;
    engine.saveSettings();
    updateViewVisibility();
}

void MainComponent::updateViewVisibility()
{
    arrangementView.setVisible (contentView == ContentView::midiRegions);
    pianoRollView.setVisible (isEditorShowing());
    dockHandle.setVisible (contentView == ContentView::midiRegions);
    mixerView.setVisible (contentView == ContentView::mixer);
    audioRegionsView.setVisible (contentView == ContentView::audioRegions);
    instrumentsView.setVisible (sidePane == SidePane::instruments);
    instrumentEditorView.setVisible (contentView == ContentView::instrumentEditor);
    expressionMapView.setVisible (contentView == ContentView::expressionMaps);
    historyView.setVisible (sidePane == SidePane::history);
    historyButton.setToggleState (sidePane == SidePane::history, juce::dontSendNotification);
    instrumentsButton.setVisible (sidePane != SidePane::none);   // the pane's tabs
    historyButton.setVisible (sidePane != SidePane::none);
    sidePaneButton.setButtonText (sidePane != SidePane::none ? juce::String::fromUTF8 ("\xc2\xbb") : juce::String::fromUTF8 ("\xc2\xab"));
    sidePaneButton.setToggleState (sidePane != SidePane::none, juce::dontSendNotification);

    trackList.setVisible (domain == Domain::midi);
    channelList.setVisible (domain == Domain::audio);
    // Midi: dark blue/cyan; Audio: dried blood. Black text on both
    sidebarHeader.set (domain == Domain::midi ? "MIDI" : "AUDIO",
                       juce::Colour (domain == Domain::midi ? 0xff3aa6c4 : 0xffc0504a).withMultipliedBrightness (0.8f).withAlpha (0.57f));   // a bit darker, see-through

    midiDomainButton.setToggleState (domain == Domain::midi, juce::dontSendNotification);
    audioDomainButton.setToggleState (domain == Domain::audio, juce::dontSendNotification);
    instrumentsButton.setToggleState (sidePane == SidePane::instruments, juce::dontSendNotification);

    // The domain buttons (the sidebar's top) show the domain in its colour: Midi blue, Audio red
    for (auto [button, colour] : { std::pair (&midiDomainButton, 0xff3aa6c4u), std::pair (&audioDomainButton, 0xffc0504au) })
        button->setColour (juce::TextButton::buttonOnColourId, juce::Colour (colour).withMultipliedBrightness (0.8f));

    settingsView.setVisible (settingsOpen);

    if (settingsOpen)
        settingsView.toFront (false);
}

void MainComponent::updatePlaceholders()
{
    const auto channelCount = (int) engine.getAudioChannelIds().size();
    audioRegionsView.setDetails ({ "Coming: audio regions and automation lanes for the audio channels.",
                                   "",
                                   juce::String (channelCount) + (channelCount == 1 ? " audio channel" : " audio channels")
                                     + " - their strips are in the sidebar and the mixer (F4)." });
}

void MainComponent::togglePerfPanel()
{
    perfPanel.setVisible (! perfPanel.isVisible());
    perfButton.setToggleState (perfPanel.isVisible(), juce::dontSendNotification);
    resized();
}

//==============================================================================
void MainComponent::startPluginScan (juce::StringArray args)
{
    const auto exe = AudioEngine::getScannerExecutable();

    if (! exe.existsAsFile())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Plugin scanner missing",
                                                "Couldn't find " + exe.getFullPathName());
        return;
    }

    args.insert (0, "--scan");
    juce::Logger::writeToLog ("Plugin scan started: " + exe.getFullPathName() + " " + args.joinIntoString (" "));
    // Replacing a running scan abandons its onFinished, so close its overlay here
    if (pluginScan != nullptr && pluginScan->isScanning())
        engine.getBusyStatus().end();

    pluginScan = std::make_unique<PluginScanProcess> (exe, std::move (args));
    scanStatus = "Scanning plugins...";
    engine.getBusyStatus().begin ("Scanning plugins");
    engine.getBusyStatus().update ("Starting the scanner...");

    pluginScan->onOutput = [this] (const juce::String& line)
    {
        scanStatus = "Plugin scan: " + line;
        engine.getBusyStatus().update (line);
    };

    pluginScan->onFinished = [this] (bool success)
    {
        engine.getBusyStatus().update ("Reading the plugin list...");
        reloadingPluginCache = true;
        engine.reloadPluginCache();
        reloadingPluginCache = false;
        engine.getBusyStatus().end();

        const auto numInstruments = engine.getInstrumentTypes().size();
        scanStatus = success ? "Plugin scan finished: " + juce::String (numInstruments) + " instruments available"
                             : "Plugin scan failed - see the scan log in the user data folder";

        juce::Timer::callAfterDelay (8000, [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safe != nullptr && (safe->pluginScan == nullptr || ! safe->pluginScan->isScanning()))
                safe->scanStatus.clear();
        });
    };

    pluginScan->start();
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &engine.getKnownPlugins())
    {
        // Edits made in the plugin list (removals etc.), not our own reloads.
        if (! reloadingPluginCache)
            engine.savePluginCache();
    }
    else
    {
        engine.saveSettings();
    }
}

void MainComponent::timerCallback()
{
#if JUCE_WINDOWS
    // A click in a plugin editor inside the rack gives that plugin the keyboard, and the transport's
    // keys stop working. Once the mouse is up, the keys come back to the main window. (Typing into a
    // plugin's own text field in the rack does not work because of this; its own window does.)
    if (contentView == ContentView::mixer && mixerView.isRackOpen()
        && ! juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown())
    {
        // Our window is the active one, but the keyboard is not ours: a plugin inside it has it (some
        // plugins run their windows on their own thread, so asking "who has the focus" can't see them)
        // A plugin editor the rack just created may make a window of its own the active one (seen:
        // Softube's), and then no key reaches us: for a moment after, we take the activation back
        if (auto* peer = getPeer())
            if (auto* fg = GetForegroundWindow(); fg != nullptr && fg != peer->getNativeHandle()
                  && isForeignPluginWindow (fg)
                  && juce::Time::getMillisecondCounter() - mixerView.rackEditorsOpenedAt < 2000)
            {
                getTopLevelComponent()->toFront (true);
                peer->grabFocus();
                grabKeyboardFocus();
            }

        // Also when nothing of ours has the keys: opening the rack hides the strips that had them
        if (auto* peer = getPeer())
            if (auto* focused = juce::Component::getCurrentlyFocusedComponent();
                GetForegroundWindow() == peer->getNativeHandle()
                  && (! peer->isFocused() || focused == nullptr || (focused != this && ! isParentOf (focused))))
            {
                peer->grabFocus();      // the native focus (JUCE may still think it has it)
                grabKeyboardFocus();
            }
    }
#endif

    engine.pollRecording();

    if (auto* master = engine.getMasterChannel(); master != nullptr && masterMeter.isVisible())
        masterMeter.update (master->getLastPeak());


    // The mixer highlights the selected audio channel (the sidebar's audio list and the mixer share
    // it); while none is selected there, the selected track's channel (its first output's instrument)
    {
        auto channel = channelList.getCurrentChannel();

        if (channel == 0)
            if (const auto outputs = engine.getTrackOutputs (selectedTrack); ! outputs.empty())
                channel = engine.getAudioChannelForInstrument (outputs.front().instrument);

        mixerView.setHighlightedChannel (channel);
    }

    // The side list marks the editor's tracks (none when it is closed), full or docked
    if (isEditorShowing())
        trackList.setEditedTracks (pianoRollView.getTracks(), pianoRollView.getTrack());
    else
        trackList.setEditedTracks ({}, 0);

    auto& transport = engine.getTransport();
    playButton.setToggleState (transport.isPlaying(), juce::dontSendNotification);
    playButton.setButtonText (transport.isPlaying() ? "Stop" : "Play");
    recordButton.setToggleState (engine.isRecording(), juce::dontSendNotification);
    loopButton.setToggleState (transport.isLooping(), juce::dontSendNotification);

    // Transport unit readout (Label::setText only repaints when the text changed)
    const auto position = transport.getTempoMap()->ticksToBarsBeats (transport.getPositionTicks());
    positionLabel.setText (juce::String (position.bar) + "." + juce::String (position.beat),
                           juce::dontSendNotification);
    timeLabel.setText (formatPositionTime (transport.getPositionSeconds()), juce::dontSendNotification);

    // Any engine mutation (from the UI, the API, or history travel) refreshes the
    // topbar widgets that mirror engine state.
    if (engine.getStateRevision() != lastEngineRevision)
    {
        lastEngineRevision = engine.getStateRevision();

        if (! bpmLabel.isBeingEdited())
            bpmLabel.setText (juce::String (engine.getTempoBpm(), 1), juce::dontSendNotification);
    }

    if (trackList.isShowing())
        trackList.refresh();

    if (channelList.isShowing())
        channelList.refresh();

    if (instrumentsView.isShowing())
        instrumentsView.refresh();

    if (instrumentEditorView.isShowing())
        instrumentEditorView.refresh();

    if (expressionMapView.isShowing())
        expressionMapView.refresh();

    if (audioRegionsView.isShowing())
        updatePlaceholders();

    // Status line
    auto& dm = engine.getDeviceManager();

    if (scanStatus.isNotEmpty())
    {
        statusLabel.setText (scanStatus, juce::dontSendNotification);
    }
    else if (auto* device = dm.getCurrentAudioDevice())
    {
        const auto sampleRate = device->getCurrentSampleRate();
        const auto latencyMs  = sampleRate > 0 ? 1000.0 * (device->getOutputLatencyInSamples()
                                                           + device->getCurrentBufferSizeSamples()) / sampleRate
                                               : 0.0;

        statusLabel.setText (device->getTypeName() + ": " + device->getName()
                               + "  |  " + juce::String (sampleRate / 1000.0, 1) + " kHz"
                               + "  |  " + juce::String (device->getCurrentBufferSizeSamples()) + " smp"
                               + " (" + juce::String (latencyMs, 1) + " ms)"
                               + "  |  CPU " + juce::String (juce::roundToInt (dm.getCpuUsage() * 100.0)) + "%",
                             juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText ("No audio device - open Settings", juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::openTrackPluginWindow (AudioEngine::TrackId id)
{
    const auto outputs = engine.getTrackOutputs (id);

    if (outputs.empty())
        return;

    // A toggle: a showing GUI closes
    const auto instrument = outputs.front().instrument;

    if (const auto it = pluginWindows.find (instrument); it != pluginWindows.end() && it->second != nullptr
                                                           && it->second->isVisible())
    {
        // Hidden now, deleted a moment later: the key may have come from this very window
        auto* closing = it->second.release();
        pluginWindows.erase (it);
        closing->setVisible (false);
        juce::MessageManager::callAsync ([closing] { delete closing; });
        return;
    }

    openPluginWindow (instrument);

    // Keep the keys here (I again closes it); the plugin window still shows on top
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr)
        {
            if (auto* top = safe->getTopLevelComponent())
                top->toFront (true);

            safe->grabKeyboardFocus();
        }
    });
}

// Edit and Draw both open the editor, each with its own pointer. The lit one
// closes it again; the other switches the pointer.
// Taller or shorter track rows (the side list and the arrangement lanes together); the one-line
// height is the smallest
void MainComponent::zoomTrackHeight (int direction)
{
    auto& height = sidebar::trackRowHeightSetting();
    const auto next = direction > 0 ? juce::roundToInt (height * 1.25) : juce::roundToInt (height / 1.25);
    const auto limited = juce::jlimit (sidebar::minTrackRowHeight, sidebar::maxTrackRowHeight, next);

    if (limited == height)
        return;

    height = limited;
    trackList.rowHeightsChanged();
    arrangementView.repaint();
    engine.getSettingsFile().setValue ("trackHeight", height);
    engine.getSettingsFile().saveIfNeeded();
}

// The Instruments / History pane: the button opens it (or switches it), again closes it
void MainComponent::toggleSidePane (SidePane pane)
{
    sidePane = sidePane == pane ? SidePane::none : pane;

    if (sidePane != SidePane::none)
        lastSidePane = sidePane;

    if (sidePane == SidePane::instruments)
        instrumentsView.focusTrack (selectedTrack);

    updateViewVisibility();
    resized();
}

void MainComponent::toggleEditor (bool draw)
{
    if (contentView == ContentView::midiEditor && pianoRollView.isDrawMode() == draw)
    {
        showContent (domain == Domain::midi ? ContentView::midiRegions : ContentView::audioRegions);
        return;
    }

    pianoRollView.setDrawMode (draw);

    if (contentView == ContentView::midiEditor)
        updateViewVisibility();
    else
        openEditorOn (tracksToEdit(), selectedTrack);
}

std::vector<AudioEngine::TrackId> MainComponent::inSidebarOrder (const std::set<AudioEngine::TrackId>& tracks) const
{
    std::vector<AudioEngine::TrackId> ordered;

    for (auto& item : engine.getSidebarItems (true, false))
        if (item.member != 0 && tracks.count ((AudioEngine::TrackId) item.member))
            ordered.push_back ((AudioEngine::TrackId) item.member);

    return ordered;
}

// What E/D edit: the latest selection - several regions in the arrangement (their tracks), several
// tracks Ctrl-selected in the side list, a folder (every track inside it), else the selected track
std::vector<AudioEngine::TrackId> MainComponent::tracksToEdit() const
{
    if (lastSelectionInArrangement)
        if (const auto tracks = arrangementView.selectedTracks(); tracks.size() > 1)
            return inSidebarOrder (tracks);

    if (! lastSelectionInArrangement && trackList.getMultiSelection().size() > 1)
        return inSidebarOrder (trackList.getMultiSelection());

    if (const auto instrument = trackList.getSelectedInstrument(); instrument != 0)   // an instrument folder: all its tracks
        if (auto tracks = engine.getInstrumentTracks (instrument); ! tracks.empty())
            return tracks;

    if (const auto folder = trackList.getSelectedFolder(); folder != 0)
    {
        std::vector<AudioEngine::TrackId> inside;
        const auto items = engine.getSidebarItems (true, false);
        int folderDepth = -1;

        for (auto& item : items)
        {
            if (folderDepth < 0)
            {
                if (item.folder == folder)
                    folderDepth = item.depth;

                continue;
            }

            if (item.depth <= folderDepth)
                break;   // past the folder's contents

            if (item.member != 0)
                inside.push_back ((AudioEngine::TrackId) item.member);
        }

        if (! inside.empty())
            return inside;
    }

    // A track in an instrument folder: all the instrument's tracks (multi-edit; it stays the edited one)
    if (selectedTrack != 0)
        if (const auto instrument = engine.getTrackInstrument (selectedTrack); instrument != 0)
            if (auto tracks = engine.getInstrumentTracks (instrument); tracks.size() > 1)
                return tracks;

    if (selectedTrack != 0)
        return { selectedTrack };

    return {};
}

// The editor on these tracks (top to bottom); the top one is edited
void MainComponent::openEditorOn (std::vector<AudioEngine::TrackId> tracks, AudioEngine::TrackId edited)
{
    if (tracks.empty())
        return;

    if (std::find (tracks.begin(), tracks.end(), edited) == tracks.end())
        edited = tracks.front();

    selectTrack (edited, false);   // the top track is the selected one (a folder: its first track)

    // The instrument folders of the edited tracks open (in the sidebar and the arrangement)
    for (auto track : tracks)
        if (const auto instrument = engine.getTrackInstrument (track); instrument != 0)
            engine.setInstrumentExpanded (instrument, true);

    showContent (ContentView::midiEditor);
    pianoRollView.setTracks (std::move (tracks), edited);
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (keys::matches ("view.back", key))
    {
        if (settingsOpen)
        {
            closeSettings();
            return true;
        }

        if (contentView == ContentView::instrumentEditor)
        {
            instrumentEditorView.onBack();
            return true;
        }

        if (contentView == ContentView::expressionMaps)
        {
            showContent (ContentView::instrumentEditor);
            return true;
        }

        if (contentView == ContentView::mixer && mixerView.isRackOpen())   // the rack first, then the mixer
        {
            mixerView.closeRack();
            return true;
        }

        if (contentView == ContentView::mixer)   // the mixer: back where it came from
        {
            showContent (mainView);
            return true;
        }

        // The side pane closes first
        if (sidePane != SidePane::none)
        {
            toggleSidePane (sidePane);
            return true;
        }

        if (contentView == ContentView::mixer && mixerView.isRackOpen())   // the rack, back to the mixer
        {
            mixerView.closeRack();
            return true;
        }

        if (contentView == ContentView::mixer)   // then the mixer itself
        {
            showContent (mainView);
            return true;
        }

        // The MIDI editor: Esc first deselects the notes, then closes
        if (isEditorShowing() && pianoRollView.deselectNotes())
            return true;

        // Then draw mode steps back to edit mode, before the editor closes
        if (isEditorShowing() && pianoRollView.isDrawMode())
        {
            pianoRollView.setDrawMode (false);
            updateViewVisibility();
            return true;
        }

        if (contentView == ContentView::midiEditor)
        {
            showContent (domain == Domain::midi ? ContentView::midiRegions : ContentView::audioRegions);
            return true;
        }

        if (contentView == ContentView::midiRegions)   // the arrangement: nothing selected
        {
            arrangementView.clearSelection();
            return true;
        }

        return false;
    }

    // Note input's keys (note length, rest) wherever the focus is, while the editor shows
    if (pianoRollView.isShowing() && pianoRollView.noteInputKey (key))
        return true;

    // An articulation's key command (in the current track's expression map)
    if (triggerArticulation ([&key] (const ExpressionMap& map) { return map.findByKeyCommand (key.getTextDescription()); }))
        return true;

    // The MIDI editor's keys work while it shows, even when the focus is elsewhere (opened with E or D)
    if (pianoRollView.isShowing() && ! pianoRollView.hasKeyboardFocus (true) && pianoRollView.keyPressed (key))
        return true;

    // (KeyCommands.h lists these; Settings > Key commands changes them; KEY_COMMANDS.md documents them)
    if (keys::matches ("view.edit", key) || keys::matches ("view.draw", key))
    {
        toggleEditor (keys::matches ("view.draw", key));
        return true;
    }

    if (selectedTrack != 0 && keys::matches ("track.instrumentGui", key))
    {
        openTrackPluginWindow (selectedTrack);
        return true;
    }

    // The track above / below (as the side list shows them; collapsed folders' tracks are skipped)
    if (keys::matches ("track.previous", key) || keys::matches ("track.next", key))
    {
        std::vector<AudioEngine::TrackId> visible;

        for (auto& item : engine.getSidebarItems (true, true))
            if (item.member != 0)
                visible.push_back ((AudioEngine::TrackId) item.member);

        if (! visible.empty())
        {
            const auto at = std::find (visible.begin(), visible.end(), selectedTrack);
            const auto step = keys::matches ("track.next", key) ? 1 : -1;
            const auto index = at == visible.end() ? 0 : juce::jlimit (0, (int) visible.size() - 1, (int) (at - visible.begin()) + step);
            lastSelectionInArrangement = false;
            selectTrack (visible[(size_t) index], false);
        }

        return true;
    }

    if (selectedTrack != 0 && keys::matches ("track.solo", key))
    {
        engine.setTrackSoloed (selectedTrack, ! engine.isTrackSoloed (selectedTrack));
        return true;
    }

    if (selectedTrack != 0 && keys::matches ("track.mute", key))
    {
        engine.setTrackMuted (selectedTrack, ! engine.isTrackMuted (selectedTrack));
        return true;
    }

    // The views: F1 MIDI arrangement, F2 MIDI editor, F3 audio arrangement, F4 mixer (again: back)
    if (keys::matches ("view.midiArrange", key))
    {
        setDomain (Domain::midi);
        return true;
    }

    if (keys::matches ("view.midiEditor", key))
    {
        if (domain != Domain::midi)
            domain = Domain::midi;

        if (contentView != ContentView::midiEditor)
            openEditorOn (tracksToEdit(), selectedTrack);

        return true;
    }

    if (keys::matches ("view.audioArrange", key))
    {
        setDomain (Domain::audio);
        return true;
    }

    if (keys::matches ("view.mixer", key))
    {
        showContent (contentView == ContentView::mixer ? mainView : ContentView::mixer);
        return true;
    }

    if (keys::matches ("view.tracksTaller", key) || keys::matches ("view.tracksShorter", key))
    {
        zoomTrackHeight (keys::matches ("view.tracksTaller", key) ? 1 : -1);
        return true;
    }

    if (keys::matches ("view.instruments", key))
    {
        toggleSidePane (SidePane::instruments);
        return true;
    }

    if (keys::matches ("view.history", key))
    {
        toggleSidePane (SidePane::history);
        return true;
    }

    if (keys::matches ("view.performance", key))
    {
        togglePerfPanel();
        return true;
    }

    if (keys::matches ("transport.playStop", key))
    {

        engine.getTransport().togglePlayStop();
        return true;
    }

    if (keys::matches ("transport.home", key))
    {
        engine.getTransport().returnToZero();
        return true;
    }

    // Global clip undo/redo on the selected track (the piano roll consumes its own first)
    if (keys::matches ("edit.undo", key))   // in the mixer: its own undo (the strips' settings)
        return contentView == ContentView::mixer ? mixerView.undo()
                                                 : selectedTrack != 0 && engine.undoTrackSequence (selectedTrack);

    if (keys::matches ("edit.redo", key))
        return contentView == ContentView::mixer ? mixerView.redo()
                                                 : selectedTrack != 0 && engine.redoTrackSequence (selectedTrack);

    return false;
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1d1f23));

    // The transport bar on the right, and the side pane's tab row
    g.setColour (juce::Colour (0xff2a2d33));
    g.fillRect (rightBar);
    g.fillRect (paneTabs);
    g.setColour (juce::Colour (0xff43464d));

    if (transportPlace == TransportPlace::right)
        g.fillRect (rightBar.getX(), rightBar.getY(), 1, rightBar.getHeight());
    else
        g.fillRect (rightBar.getX(), transportPlace == TransportPlace::top ? rightBar.getBottom() - 1 : rightBar.getY(), rightBar.getWidth(), 1);

    g.setColour (juce::Colour (0xff17191c));
    g.fillRect (getLocalBounds().removeFromBottom (statusHeight));

    if (sidePaneEdge >= 0)   // the side pane's edge
    {
        g.setColour (juce::Colour (0xff43464d));
        g.fillRect (sidePaneEdge, paneTabs.getY(), 1, getHeight() - statusHeight - paneTabs.getY());
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    // Bottom
    statusLabel.setBounds (area.removeFromBottom (statusHeight).reduced (8, 1));

    // The transport bar: on the right (one button wide, stacked) or along the top or bottom. The
    // transport buttons are all one size; the expand button (the side pane) and Perf at its ends.
    const auto vertical = transportPlace == TransportPlace::right;
    rightBar = vertical ? area.removeFromRight (rightBarWidth)
                        : (transportPlace == TransportPlace::top ? area.removeFromTop (topBarHeight) : area.removeFromBottom (topBarHeight));
    const std::initializer_list<juce::Component*> transportButtons { &rtzButton, &playButton, &recordButton, &loopButton, &returnOnStopButton, &snapButton };

    if (vertical)
    {
        // The master control surface: the side pane's expander and Perf at the top, the readouts, the
        // output meter filling the middle, the transport at the bottom (two buttons a row)
        auto bar = rightBar.reduced (6, 6).withTrimmedLeft (1);
        auto top = bar.removeFromTop (24);
        sidePaneButton.setBounds (top.removeFromLeft (top.getWidth() / 2).withTrimmedRight (2));
        perfButton.setBounds (top.withTrimmedLeft (2));
        bar.removeFromTop (10);

        positionLabel.setBounds (bar.removeFromTop (24));
        timeLabel.setBounds (bar.removeFromTop (18));
        bar.removeFromTop (4);
        bpmLabel.setBounds (bar.removeFromTop (24).withSizeKeepingCentre (64, 24));
        bar.removeFromTop (10);

        const std::array<std::pair<juce::Component*, juce::Component*>, 3> rows {{ { &playButton, &recordButton },
                                                                                     { &rtzButton, &loopButton },
                                                                                     { &returnOnStopButton, &snapButton } }};

        for (auto it = rows.rbegin(); it != rows.rend(); ++it)   // from the bottom up
        {
            auto row = bar.removeFromBottom (30);
            it->first->setBounds (row.removeFromLeft (row.getWidth() / 2).withTrimmedRight (2));
            it->second->setBounds (row.withTrimmedLeft (2));
            bar.removeFromBottom (4);
        }

        bar.removeFromBottom (6);
        masterMeter.setBounds (bar.withSizeKeepingCentre (56, bar.getHeight()));   // the audio out (with its scale)
        masterMeter.setVisible (true);
    }
    else
    {
        auto bar = rightBar.reduced (8, 7);
        masterMeter.setVisible (false);   // (only on the right bar for now)
        sidePaneButton.setBounds (bar.removeFromRight (36));
        bar.removeFromRight (8);
        perfButton.setBounds (bar.removeFromRight (50));

        for (auto* b : transportButtons)
        {
            b->setBounds (bar.removeFromLeft (50));
            bar.removeFromLeft (4);
        }

        bar.removeFromLeft (10);
        positionLabel.setBounds (bar.removeFromLeft (76));
        bar.removeFromLeft (6);
        timeLabel.setBounds (bar.removeFromLeft (92));
        bar.removeFromLeft (10);
        bpmLabel.setBounds (bar.removeFromLeft (56));
    }

    positionLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    positionLabel.setJustificationType (vertical ? juce::Justification::centred : juce::Justification::centredRight);
    timeLabel.setFont (juce::FontOptions (14.0f));
    timeLabel.setJustificationType (vertical ? juce::Justification::centred : juce::Justification::centredLeft);
    timeLabel.setMinimumHorizontalScale (0.7f);


    // The side pane (Instruments / History) on the right, beside everything else
    if (sidePane != SidePane::none)
    {
        auto pane = area.removeFromRight (currentSidePaneWidth());
        sidePaneEdge = pane.getX();
        sidePaneResizer.setBounds (pane.removeFromLeft (5));   // drag to resize
        sidePaneResizer.setVisible (true);
        sidePaneResizer.toFront (false);

        paneTabs = pane.removeFromTop (30);   // the tabs: Instruments | History
        auto tabs = paneTabs.reduced (6, 4);
        instrumentsButton.setBounds (tabs.removeFromLeft (tabs.getWidth() / 2).withTrimmedRight (2));
        historyButton.setBounds (tabs.withTrimmedLeft (2));
        instrumentsView.setBounds (pane);
        historyView.setBounds (pane);
    }
    else
    {
        sidePaneEdge = -1;
        sidePaneResizer.setVisible (false);
        paneTabs = {};
    }

    if (perfPanel.isVisible())
        perfPanel.setBounds (area.removeFromBottom (160));

    // Sidebar + content
    if (sidebarWidth == 0)
        sidebarWidth = juce::jmax (180, getWidth() * 15 / 100);

    // The top strip: the timeline bar's rows; the sidebar header fits into the same height
    const auto timelineHeight = timelineBar.getPreferredHeight();   // the header block fits into it
    auto sidebar = area.removeFromLeft (sidebarWidth);

    // The sidebar lists start at the same y as the content views (below the
    // timeline bar), so their rows share the arrangement's Y axis exactly.
    // Its top (the timeline bar's height): Menu, then the Midi / Audio buttons (the domain in its colour)
    auto sidebarTop = sidebar.removeFromTop (timelineHeight).reduced (6, 0);
    sidebarTop = sidebarTop.withSizeKeepingCentre (sidebarTop.getWidth(), juce::jmin (28, sidebarTop.getHeight() - 4));
    sidebarHeader.setVisible (false);
    menuButton.setBounds (sidebarTop.removeFromLeft (40));

    // One page for everything (MIDI and audio together): no domain buttons
    midiDomainButton.setVisible (false);
    audioDomainButton.setVisible (false);

    trackList.setBounds (sidebar);
    channelList.setBounds (sidebar);

    sidebarResizer.setBounds (area.removeFromLeft (6));

    // Timeline bar + content container. The bar spans exactly the content area, so
    // its local x coordinates (and the shared TimeAxis gutter) line up with the
    // arrangement lanes and the piano roll grid below it.
    timelineBar.setBounds (area.removeFromTop (timelineHeight));

    for (auto* view : std::initializer_list<juce::Component*> { &arrangementView, &pianoRollView, &audioRegionsView,
                                                                &instrumentEditorView, &expressionMapView, &mixerView })
        view->setBounds (area);

    // The arrangement: the editor handle along its bottom, the docked editor under it when open
    contentAreaHeight = area.getHeight();

    if (contentView == ContentView::midiRegions)
    {
        auto arrangeArea = area;

        if (editorDocked)
        {
            auto editorArea = arrangeArea.removeFromBottom (currentDockHeight());
            pianoRollView.setBounds (editorArea.withTrimmedLeft (2).withTrimmedRight (2).withTrimmedTop (2));   // inside its border
        }

        dockHandle.setBounds (arrangeArea.removeFromBottom (dockHandleHeight));
        arrangementView.setBounds (arrangeArea);
    }

    // Settings replaces the whole UI; the busy overlay covers everything
    settingsView.setBounds (getLocalBounds());
    busyOverlay.setBounds (getLocalBounds());
}
