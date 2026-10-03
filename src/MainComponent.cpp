#include "MainComponent.h"
#include "UserData.h"
#include "model/DemoSequence.h"
#include "api/CommandDispatcher.h"
#include "api/McpProcess.h"

namespace
{
    constexpr int topbarHeight   = 44;
    constexpr int statusHeight   = 22;
    constexpr int keyboardHeight = 90;
    constexpr int collapsedSidebarWidth = 26;
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

    // --- Topbar ---
    menuButton.onClick = [this] { showMainMenu(); };
    midiDomainButton.onClick = [this] { setDomain (Domain::midi); };
    audioDomainButton.onClick = [this] { setDomain (Domain::audio); };
    instrumentsButton.onClick = [this]
    {
        instrumentsView.focusTrack (selectedTrack);
        showContent (ContentView::instruments);
    };

    rtzButton.setTooltip ("Return to start (Home)");
    rtzButton.onClick = [this] { engine.getTransport().returnToZero(); };

    playButton.setTooltip ("Play/Stop (space)");
    playButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::darkgreen);
    playButton.onClick = [this] { engine.getTransport().togglePlayStop(); };

    recordButton.setTooltip ("Record live MIDI onto the armed track (starts playback if stopped)");
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

    positionLabel.setJustificationType (juce::Justification::centredLeft);
    positionLabel.setColour (juce::Label::textColourId, juce::Colours::white);

    perfButton.setTooltip ("Performance monitor (F12)");
    perfButton.setClickingTogglesState (true);
    perfButton.onClick = [this] { togglePerfPanel(); };

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
    trackList.onSelect = [this] (auto id) { selectTrack (id, false); };
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
             &menuButton, &midiDomainButton, &audioDomainButton, &instrumentsButton,
             &rtzButton, &playButton, &recordButton, &loopButton, &bpmLabel, &positionLabel, &perfButton,
             &collapseButton, &trackList, &channelList, &sidebarResizer,
             &arrangementView, &audioRegionsView, &pianoRollView,
             &instrumentsView, &instrumentEditorView, &settingsView,
             &statusLabel, &keyboard })
        addAndMakeVisible (c);

    for (auto* b : std::initializer_list<juce::Component*> { &menuButton, &midiDomainButton, &audioDomainButton,
                                                             &instrumentsButton, &rtzButton, &playButton,
                                                             &recordButton, &loopButton, &perfButton,
                                                             &collapseButton, &keyboard })
        b->setWantsKeyboardFocus (false);

    addChildComponent (perfPanel);
    setWantsKeyboardFocus (true);

    engine.getDeviceManager().addChangeListener (this);
    engine.getKnownPlugins().addChangeListener (this);

    updateViewVisibility();
    setSize (1100, 700);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
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
    selectedTrack = id;
    trackList.setSelectedTrack (id);

    const auto autoArm = engine.getSettingsFile().getBoolValue (SettingsView::autoRecordOnSelectKey, true);

    if (id != 0 && (autoArm || forceArm) && engine.getArmedTrack() != id)
    {
        keyboardState.allNotesOff (0);   // release on-screen keys aimed at the old instrument
        engine.setArmedTrack (id);
    }

    if (contentView == ContentView::instruments)
        instrumentsView.focusTrack (id);

    if (contentView == ContentView::midiEditor)
        pianoRollView.setTrack (id);   // the editor follows the selected track

    updatePlaceholders();
}

void MainComponent::showTrackContextMenu (AudioEngine::TrackId id)
{
    selectTrack (id, false);

    const auto safe = juce::Component::SafePointer<MainComponent> (this);
    juce::PopupMenu menu;

    menu.addItem ("Set output...", [safe, id] { if (safe != nullptr) safe->chooseTrackOutput (id); });
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
                                   && outputs.front().midiChannel == ch;
            const auto channelName = engine.getInstrumentChannelName (instrumentId, ch);

            channels.addItem ("Channel " + juce::String (ch) + (channelName.isNotEmpty() ? "  (" + channelName + ")" : ""),
                              true, current,
                              [safe, trackId, id = instrumentId, ch]
                              {
                                  if (safe != nullptr)
                                  {
                                      safe->engine.clearTrackOutputs (trackId);
                                      safe->engine.addTrackOutput (trackId, id, ch);
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

void MainComponent::saveProject (bool saveAs)
{
    if (! saveAs && currentProjectFile != juce::File())
    {
        statusLabel.setText (engine.saveProject (currentProjectFile)
                                 ? "Saved " + currentProjectFile.getFileName()
                                 : "Save FAILED", juce::dontSendNotification);
        return;
    }

    fileChooser = std::make_unique<juce::FileChooser> ("Save project",
                                                       getProjectsDirectory().getChildFile ("Untitled.odaw"), "*.odaw");

    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<MainComponent> (this)] (const juce::FileChooser& chooser)
        {
            if (safe == nullptr || chooser.getResult() == juce::File())
                return;

            const auto file = chooser.getResult().withFileExtension ("odaw");

            if (safe->engine.saveProject (file))
            {
                safe->currentProjectFile = file;
                safe->updateWindowTitle();
                safe->statusLabel.setText ("Saved " + file.getFileName(), juce::dontSendNotification);
            }
            else
            {
                safe->statusLabel.setText ("Save FAILED", juce::dontSendNotification);
            }
        });
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

    const auto map = transport.getTempoMap();
    const auto position = map->ticksToBarsBeats (transport.getPositionTicks());
    const auto seconds = transport.getPositionSeconds();
    const auto minutes = (int) (seconds / 60.0);

    positionLabel.setText (juce::String (position.bar) + "." + juce::String (position.beat)
                             + "   " + juce::String (minutes)
                             + ":" + juce::String (seconds - minutes * 60.0, 1).paddedLeft ('0', 4),
                           juce::dontSendNotification);

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

        if (contentView == ContentView::midiEditor || contentView == ContentView::instruments)
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

    return false;
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1d1f23));

    g.setColour (juce::Colour (0xff2a2d33));
    g.fillRect (getLocalBounds().removeFromTop (topbarHeight));

    g.setColour (juce::Colour (0xff17191c));
    auto bottom = getLocalBounds().removeFromBottom (statusHeight + keyboardHeight);
    g.fillRect (bottom.removeFromBottom (statusHeight));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    // Topbar
    auto toolbar = area.removeFromTop (topbarHeight).reduced (8, 7);
    menuButton.setBounds (toolbar.removeFromLeft (56));
    toolbar.removeFromLeft (10);
    midiDomainButton.setBounds (toolbar.removeFromLeft (52));
    toolbar.removeFromLeft (4);
    audioDomainButton.setBounds (toolbar.removeFromLeft (56));
    toolbar.removeFromLeft (4);
    instrumentsButton.setBounds (toolbar.removeFromLeft (94));
    toolbar.removeFromLeft (14);
    rtzButton.setBounds (toolbar.removeFromLeft (34));
    toolbar.removeFromLeft (4);
    playButton.setBounds (toolbar.removeFromLeft (54));
    toolbar.removeFromLeft (4);
    recordButton.setBounds (toolbar.removeFromLeft (46));
    toolbar.removeFromLeft (4);
    loopButton.setBounds (toolbar.removeFromLeft (48));
    toolbar.removeFromLeft (8);
    bpmLabel.setBounds (toolbar.removeFromLeft (56));
    toolbar.removeFromLeft (8);
    perfButton.setBounds (toolbar.removeFromRight (50));
    toolbar.removeFromRight (6);
    positionLabel.setBounds (toolbar);

    // Bottom
    statusLabel.setBounds (area.removeFromBottom (statusHeight).reduced (8, 1));
    keyboard.setBounds (area.removeFromBottom (keyboardHeight));

    if (perfPanel.isVisible())
        perfPanel.setBounds (area.removeFromBottom (160));

    // Sidebar + content
    if (sidebarWidth == 0)
        sidebarWidth = juce::jmax (180, getWidth() * 15 / 100);

    const auto currentSidebarWidth = sidebarCollapsed ? collapsedSidebarWidth : sidebarWidth;
    auto sidebar = area.removeFromLeft (currentSidebarWidth);

    collapseButton.setBounds (sidebar.removeFromTop (24).reduced (2, 1));

    trackList.setBounds (sidebar);
    channelList.setBounds (sidebar);

    sidebarResizer.setBounds (area.removeFromLeft (6));
    sidebarResizer.setVisible (! sidebarCollapsed);

    // Content container
    for (auto* view : std::initializer_list<juce::Component*> { &arrangementView, &pianoRollView, &audioRegionsView,
                                                                &instrumentsView, &instrumentEditorView })
        view->setBounds (area);

    // Settings replaces the whole UI
    settingsView.setBounds (getLocalBounds());
}
