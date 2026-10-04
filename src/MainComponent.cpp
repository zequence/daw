#include "MainComponent.h"
#include "UserData.h"
#include "ui/ColorPalette.h"
#include "model/DemoSequence.h"
#include "api/CommandDispatcher.h"
#include "api/McpProcess.h"

namespace
{
    constexpr int topbarHeight   = 44;
    constexpr int statusHeight   = 22;
    constexpr int keyboardHeight = 90;
    constexpr int collapsedSidebarWidth = 26;

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
    // Keep the window state sane when projects change through the API.
    dispatcher.onBeforeProjectChange = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr)
            safe->pluginWindows.clear();
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
    instrumentsButton.setTooltip ("The instrument rack (click again or Esc to return to arrange)");
    historyButton.setTooltip ("Global history: click an entry to time-travel (click again or Esc to return)");

    instrumentsButton.onClick = [this]
    {
        if (contentView == ContentView::instruments || contentView == ContentView::instrumentEditor)
        {
            setDomain (domain);
            return;
        }

        instrumentsView.focusTrack (selectedTrack);
        showContent (ContentView::instruments);
    };

    historyButton.onClick = [this]
    {
        if (contentView == ContentView::history)
            setDomain (domain);
        else
            showContent (ContentView::history);
    };

    rtzButton.setTooltip ("Return to start (Home)");
    rtzButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a3e46));
    rtzButton.onClick = [this] { engine.getTransport().returnToZero(); };

    playButton.setTooltip ("Play/Stop (space)");
    playButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2b4634));
    playButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::darkgreen);
    playButton.onClick = [this] { engine.getTransport().togglePlayStop(); };

    recordButton.setTooltip ("Record live MIDI onto the armed track (starts playback if stopped)");
    recordButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff4a2e2e));
    recordButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::darkred);
    recordButton.onClick = [this]
    {
        if (engine.isRecording())
            engine.stopRecording();
        else if (! engine.startRecording())
            statusLabel.setText ("Add a track before recording", juce::dontSendNotification);
    };

    loopButton.setTooltip ("Loop from the start to the end of the last clip");
    loopButton.setClickingTogglesState (true);
    loopButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2e3b4a));
    loopButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::steelblue);
    loopButton.onClick = [this]
    {
        auto& transport = engine.getTransport();
        transport.setLoopRegion (0, engine.getLoopEndTicks());
        transport.setLooping (loopButton.getToggleState());
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

    perfButton.setTooltip ("Performance monitor (F12)");
    perfButton.setClickingTogglesState (true);
    perfButton.onClick = [this] { togglePerfPanel(); };

    // --- Timeline bar ---
    timelineBar.onHeightChanged = [this] { resized(); };
    timelineBar.onOpenSettings = [this] { openSettings(); };

    // --- Sidebar ---
    collapseButton.setTooltip ("Collapse/expand the track list");
    collapseButton.onClick = [this]
    {
        sidebarCollapsed = ! sidebarCollapsed;
        collapseButton.setButtonText (sidebarCollapsed ? ">>" : "<<");
        updateViewVisibility();
        resized();
    };

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
    trackList.onSelect = [this] (auto id) { selectTrack (id, false); };

    // Multi-selection arms every selected track when auto-record is on (ISSUES.md)
    trackList.onSelectionChanged = [this] (const std::set<AudioEngine::TrackId>& selection)
    {
        if (! engine.getSettingsFile().getBoolValue (SettingsView::autoRecordOnSelectKey, true))
            return;

        keyboardState.allNotesOff (0);

        if (selection.empty())
            engine.setArmedTrack (selectedTrack);
        else
            engine.setArmedTracks (selection, selection.count (selectedTrack) ? selectedTrack : *selection.begin());
    };
    trackList.onArm = [this] (auto id) { selectTrack (id, true); };
    trackList.onOpenEditor = [this] (auto id)
    {
        selectTrack (id, false);
        showContent (ContentView::midiEditor);
    };
    trackList.onOpenInstrument = [this] (auto id)
    {
        selectTrack (id, false);

        const auto outputs = engine.getTrackOutputs (id);

        if (! outputs.empty())
            openPluginWindow (outputs.front().instrument);

        instrumentsView.focusTrack (id);
        showContent (ContentView::instruments);
    };
    trackList.onShowContextMenu = [this] (auto id) { showTrackContextMenu (id); };

    // --- Content views ---
    arrangementView.onSelectTrack = [this] (auto id) { selectTrack (id, false); };
    arrangementView.onOpenEditor = [this] (auto id)
    {
        selectTrack (id, false);
        showContent (ContentView::midiEditor);
    };

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
    instrumentsView.onEditInstrument = [this] (auto id)
    {
        instrumentEditorView.setInstrument (id);
        showContent (ContentView::instrumentEditor);
    };

    instrumentEditorView.onBack = [this] { showContent (ContentView::instruments); };
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

    keyboard.setAvailableRange (21, 108);
    keyboard.setLowestVisibleKey (36);
    keyboardState.addListener (this);

    for (auto* c : std::initializer_list<juce::Component*> {
             &menuButton, &midiDomainButton, &audioDomainButton, &instrumentsButton, &historyButton,
             &rtzButton, &playButton, &recordButton, &loopButton, &bpmLabel, &positionLabel, &timeLabel, &perfButton,
             &collapseButton, &trackList, &channelList, &sidebarResizer,
             &timelineBar, &arrangementView, &audioRegionsView, &pianoRollView,
             &instrumentsView, &instrumentEditorView, &historyView, &settingsView,
             &statusLabel, &keyboard })
        addAndMakeVisible (c);

    for (auto* b : std::initializer_list<juce::Component*> { &menuButton, &midiDomainButton, &audioDomainButton,
                                                             &instrumentsButton, &historyButton, &rtzButton,
                                                             &playButton, &recordButton, &loopButton, &perfButton,
                                                             &collapseButton, &keyboard })
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

MainComponent::~MainComponent()
{
    engine.getBusyStatus().onChanged = nullptr;   // the engine outlives this window
    stopTimer();
    keyboardState.removeListener (this);
    engine.getDeviceManager().removeChangeListener (this);
    engine.getKnownPlugins().removeChangeListener (this);

    pluginWindows.clear();
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
        keyboardState.allNotesOff (0);   // release on-screen keys aimed at the old instrument
        engine.setArmedTrack (id);
    }

    lap ("arm");

    if (contentView == ContentView::instruments)
        instrumentsView.focusTrack (id);

    if (contentView == ContentView::midiEditor)
        pianoRollView.setTrack (id);   // the editor follows the selected track

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

    menu.addItem ("Set output...", [safe, id] { if (safe != nullptr) safe->chooseTrackOutput (id); });
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
            if (item.parent == parent)
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
    window->onClose = [safe = juce::Component::SafePointer<MainComponent> (this), instrumentId]
    {
        // Defer deletion: we're inside the window's own callback.
        juce::MessageManager::callAsync ([safe, instrumentId]
                                         { if (safe != nullptr) safe->pluginWindows.erase (instrumentId); });
    };
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

    if (view == ContentView::midiEditor)
        pianoRollView.setTrack (selectedTrack);

    updatePlaceholders();
    updateViewVisibility();

    if (view == ContentView::midiEditor)
        pianoRollView.grabKeyboardFocus();
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
        safe->engine.clearProject();
        safe->currentProjectFile = {};
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
                             ? "Orchestral DAW - " + currentProjectFile.getFileNameWithoutExtension()
                             : juce::String ("Orchestral DAW"));
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
    pianoRollView.setVisible (contentView == ContentView::midiEditor);
    audioRegionsView.setVisible (contentView == ContentView::audioRegions);
    instrumentsView.setVisible (contentView == ContentView::instruments);
    instrumentEditorView.setVisible (contentView == ContentView::instrumentEditor);
    historyView.setVisible (contentView == ContentView::history);
    historyButton.setToggleState (contentView == ContentView::history, juce::dontSendNotification);

    trackList.setVisible (! sidebarCollapsed && domain == Domain::midi);
    channelList.setVisible (! sidebarCollapsed && domain == Domain::audio);

    midiDomainButton.setToggleState (domain == Domain::midi, juce::dontSendNotification);
    audioDomainButton.setToggleState (domain == Domain::audio, juce::dontSendNotification);
    instrumentsButton.setToggleState (contentView == ContentView::instruments
                                        || contentView == ContentView::instrumentEditor, juce::dontSendNotification);

    settingsView.setVisible (settingsOpen);

    if (settingsOpen)
        settingsView.toFront (false);
}

void MainComponent::updatePlaceholders()
{
    const auto channelCount = (int) engine.getAudioChannelIds().size();
    audioRegionsView.setDetails ({ juce::String (channelCount) + (channelCount == 1 ? " audio channel" : " audio channels")
                                     + " - strips are in the sidebar.",
                                   "",
                                   "Audio regions and editing arrive later." });
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
    pluginScan = std::make_unique<PluginScanProcess> (exe, std::move (args));
    scanStatus = "Scanning plugins...";

    pluginScan->onOutput = [this] (const juce::String& line)
    {
        scanStatus = "Plugin scan: " + line;
    };

    pluginScan->onFinished = [this] (bool success)
    {
        reloadingPluginCache = true;
        engine.reloadPluginCache();
        reloadingPluginCache = false;

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
void MainComponent::handleNoteOn (juce::MidiKeyboardState*, int channel, int note, float velocity)
{
    auto message = juce::MidiMessage::noteOn (channel, note, velocity);
    message.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    engine.getLiveMidiCollector().addMessageToQueue (message);
}

void MainComponent::handleNoteOff (juce::MidiKeyboardState*, int channel, int note, float velocity)
{
    auto message = juce::MidiMessage::noteOff (channel, note, velocity);
    message.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    engine.getLiveMidiCollector().addMessageToQueue (message);
}

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
    engine.pollRecording();

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
bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (settingsOpen)
        {
            closeSettings();
            return true;
        }

        if (contentView == ContentView::instrumentEditor)
        {
            showContent (ContentView::instruments);
            return true;
        }

        if (contentView == ContentView::midiEditor || contentView == ContentView::instruments
             || contentView == ContentView::history)
        {
            showContent (domain == Domain::midi ? ContentView::midiRegions : ContentView::audioRegions);
            return true;
        }

        return false;
    }

    if (key == juce::KeyPress::F12Key)
    {
        togglePerfPanel();
        return true;
    }

    if (key == juce::KeyPress::spaceKey)
    {
        engine.getTransport().togglePlayStop();
        return true;
    }

    if (key == juce::KeyPress::homeKey)
    {
        engine.getTransport().returnToZero();
        return true;
    }

    // Global clip undo/redo on the selected track (the piano roll consumes its own first)
    if (key == juce::KeyPress ('z', juce::ModifierKeys::ctrlModifier, 0))
        return selectedTrack != 0 && engine.undoTrackSequence (selectedTrack);

    if (key == juce::KeyPress ('y', juce::ModifierKeys::ctrlModifier, 0)
        || key == juce::KeyPress ('z', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0))
        return selectedTrack != 0 && engine.redoTrackSequence (selectedTrack);

    return false;
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1d1f23));

    g.setColour (juce::Colour (0xff2a2d33));
    g.fillRect (getLocalBounds().removeFromTop (topbarHeight));

    // The transport unit's panel
    g.setColour (juce::Colour (0xff1f2227));
    g.fillRoundedRectangle (transportPanel.toFloat(), 6.0f);
    g.setColour (juce::Colour (0xff43464d));
    g.drawRoundedRectangle (transportPanel.toFloat(), 6.0f, 1.0f);

    // Separators between the topbar's groups (hamburger | view buttons | ... | Perf)
    g.setColour (juce::Colour (0xff43464d));

    for (auto x : topbarSeparators)
        if (x > 0)
            g.fillRect (x, 10, 1, topbarHeight - 20);

    g.setColour (juce::Colour (0xff17191c));
    auto bottom = getLocalBounds().removeFromBottom (statusHeight + keyboardHeight);
    g.fillRect (bottom.removeFromBottom (statusHeight));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    // Topbar
    // Topbar: four distinct groups (ISSUES.md) - hamburger | view buttons |
    // transport unit | right-side buttons - with separators painted between them.
    auto toolbar = area.removeFromTop (topbarHeight).reduced (8, 7);
    menuButton.setBounds (toolbar.removeFromLeft (36));
    topbarSeparators[0] = toolbar.getX() + 7;
    toolbar.removeFromLeft (14);
    midiDomainButton.setBounds (toolbar.removeFromLeft (52));
    toolbar.removeFromLeft (4);
    audioDomainButton.setBounds (toolbar.removeFromLeft (56));
    toolbar.removeFromLeft (4);
    instrumentsButton.setBounds (toolbar.removeFromLeft (94));
    toolbar.removeFromLeft (4);
    historyButton.setBounds (toolbar.removeFromLeft (62));
    toolbar.removeFromLeft (14);

    perfButton.setBounds (getWidth() - 8 - 50, toolbar.getY(), 50, toolbar.getHeight());
    topbarSeparators[1] = perfButton.getX() - 8;

    // The transport unit: buttons + position readout + tempo, PERFECTLY centered
    // in the window. If it would collide, it shifts right of the view buttons and
    // the Perf button hides - the window's minimum width normally prevents both.
    constexpr auto unitWidth = 34 + 4 + 54 + 4 + 46 + 4 + 48 + 14 + 76 + 6 + 92 + 10 + 56;
    auto unit = juce::Rectangle<int> ((getWidth() - unitWidth) / 2, toolbar.getY(),
                                      unitWidth, toolbar.getHeight());

    if (unit.getX() < toolbar.getX())
        unit.setX (toolbar.getX());

    const auto perfVisible = unit.getRight() + 12 <= perfButton.getX();
    perfButton.setVisible (perfVisible);

    const auto rightEdge = perfVisible ? perfButton.getX() - 8 : getWidth() - 8;

    if (unit.getRight() > rightEdge)
        unit.setX (juce::jmax (toolbar.getX(), rightEdge - unitWidth));

    transportPanel = unit.expanded (8, 4)
                         .getIntersection (getLocalBounds().removeFromTop (topbarHeight).reduced (0, 2));

    rtzButton.setBounds (unit.removeFromLeft (34));
    unit.removeFromLeft (4);
    playButton.setBounds (unit.removeFromLeft (54));
    unit.removeFromLeft (4);
    recordButton.setBounds (unit.removeFromLeft (46));
    unit.removeFromLeft (4);
    loopButton.setBounds (unit.removeFromLeft (48));
    unit.removeFromLeft (14);
    positionLabel.setBounds (unit.removeFromLeft (76));
    unit.removeFromLeft (6);
    timeLabel.setBounds (unit.removeFromLeft (92));
    unit.removeFromLeft (10);
    bpmLabel.setBounds (unit.removeFromLeft (56));

    // Bottom
    statusLabel.setBounds (area.removeFromBottom (statusHeight).reduced (8, 1));
    keyboard.setBounds (area.removeFromBottom (keyboardHeight));

    if (perfPanel.isVisible())
        perfPanel.setBounds (area.removeFromBottom (160));

    // Sidebar + content
    if (sidebarWidth == 0)
        sidebarWidth = juce::jmax (180, getWidth() * 15 / 100);

    const auto currentSidebarWidth = sidebarCollapsed ? collapsedSidebarWidth : sidebarWidth;
    const auto timelineHeight = timelineBar.getPreferredHeight();
    auto sidebar = area.removeFromLeft (currentSidebarWidth);

    // The sidebar lists start at the same y as the content views (below the
    // timeline bar), so their rows share the arrangement's Y axis exactly.
    auto sidebarTop = sidebar.removeFromTop (timelineHeight);
    collapseButton.setBounds (sidebarTop.removeFromTop (24).reduced (2, 1));

    trackList.setBounds (sidebar);
    channelList.setBounds (sidebar);

    sidebarResizer.setBounds (area.removeFromLeft (6));
    sidebarResizer.setVisible (! sidebarCollapsed);

    // Timeline bar + content container. The bar spans exactly the content area, so
    // its local x coordinates (and the shared TimeAxis gutter) line up with the
    // arrangement lanes and the piano roll grid below it.
    timelineBar.setBounds (area.removeFromTop (timelineHeight));

    for (auto* view : std::initializer_list<juce::Component*> { &arrangementView, &pianoRollView, &audioRegionsView,
                                                                &instrumentsView, &instrumentEditorView, &historyView })
        view->setBounds (area);

    // Settings replaces the whole UI; the busy overlay covers everything
    settingsView.setBounds (getLocalBounds());
    busyOverlay.setBounds (getLocalBounds());
}
