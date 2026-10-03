#include "MainComponent.h"
#include "UserData.h"
#include "model/DemoSequence.h"

namespace
{
    constexpr int toolbarHeight  = 44;
    constexpr int trackRowHeight = 44;
    constexpr int keyboardHeight = 90;

    void launchDialog (juce::Component::SafePointer<juce::DialogWindow>& window,
                       std::unique_ptr<juce::Component> content, const juce::String& title,
                       juce::Component* parent)
    {
        if (window != nullptr)
        {
            window->toFront (true);
            return;
        }

        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (content.release());
        options.dialogTitle = title;
        options.dialogBackgroundColour = juce::Colour (0xff23262b);
        options.componentToCentreAround = parent;
        options.useNativeTitleBar = true;
        options.resizable = true;
        window = options.launchAsync();
    }
}

MainComponent::MainComponent (AudioEngine& e)
    : engine (e)
{
    audioButton.onClick    = [this] { showAudioSettings(); };
    pluginsButton.onClick  = [this] { showPluginsMenu(); };
    addTrackButton.onClick = [this] { addTrack(); };

    perfButton.setTooltip ("Performance monitor (F12)");
    perfButton.setClickingTogglesState (true);
    perfButton.onClick = [this] { togglePerfPanel(); };
    addChildComponent (perfPanel);
    setWantsKeyboardFocus (true);

    rtzButton.setTooltip ("Return to start");
    rtzButton.onClick = [this] { engine.getTransport().returnToZero(); };

    playButton.setTooltip ("Play/Stop (space)");
    playButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::darkgreen);
    playButton.onClick = [this] { engine.getTransport().togglePlayStop(); };

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

    // Keep keyboard focus on the main component so the space bar reaches the transport.
    for (auto* b : std::initializer_list<juce::Component*> { &audioButton, &pluginsButton, &addTrackButton,
                                                             &perfButton, &rtzButton, &playButton, &loopButton,
                                                             &keyboard })
        b->setWantsKeyboardFocus (false);

    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    trackViewport.setViewedComponent (&trackContainer, false);
    trackViewport.setScrollBarsShown (true, false);

    keyboard.setAvailableRange (21, 108);   // 88-key range
    keyboard.setLowestVisibleKey (36);
    keyboardState.addListener (this);

    for (auto* c : std::initializer_list<juce::Component*> { &audioButton, &pluginsButton, &addTrackButton, &perfButton,
                                                             &rtzButton, &playButton, &loopButton, &bpmLabel,
                                                             &positionLabel, &statusLabel, &trackViewport, &keyboard })
        addAndMakeVisible (c);

    engine.getDeviceManager().addChangeListener (this);
    engine.getKnownPlugins().addChangeListener (this);

    addTrack();

    setSize (1000, 640);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    keyboardState.removeListener (this);
    engine.getDeviceManager().removeChangeListener (this);
    engine.getKnownPlugins().removeChangeListener (this);

    if (audioDialog != nullptr)  delete audioDialog.getComponent();
    if (pluginDialog != nullptr) delete pluginDialog.getComponent();

    for (auto& row : trackRows)
        engine.removeTrack (row->getTrackId());

    trackRows.clear();
}

//==============================================================================
void MainComponent::addTrack()
{
    const auto id = engine.addTrack();

    auto row = std::make_unique<TrackRow> (engine, id, "Track " + juce::String (++trackCounter));
    row->onArmClicked    = [this] (auto trackId) { armTrack (trackId); };
    row->onDemoToggled   = [this] (auto trackId, bool enabled)
    {
        engine.setTrackSequence (trackId, enabled ? makeDemoSequence() : nullptr);

        // The clip starts at bar 1; make sure the playhead isn't already beyond it.
        if (enabled && ! engine.getTransport().isPlaying())
            engine.getTransport().returnToZero();
    };
    row->onRemoveClicked = [this] (auto trackId)
    {
        // Defer: the click came from a button inside the row we're about to delete.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), trackId]
                                         { if (safe != nullptr) safe->removeTrack (trackId); });
    };

    trackContainer.addAndMakeVisible (*row);
    trackRows.push_back (std::move (row));

    armTrack (engine.getArmedTrack());
    layoutTracks();
}

void MainComponent::removeTrack (AudioEngine::TrackId id)
{
    auto it = std::find_if (trackRows.begin(), trackRows.end(), [id] (auto& r) { return r->getTrackId() == id; });

    if (it == trackRows.end())
        return;

    trackRows.erase (it);   // closes the editor before the plugin is destroyed
    engine.removeTrack (id);
    armTrack (engine.getArmedTrack());
    layoutTracks();
}

void MainComponent::armTrack (AudioEngine::TrackId id)
{
    // Release held on-screen keys so the previously armed instrument doesn't hang.
    keyboardState.allNotesOff (0);

    engine.setArmedTrack (id);

    for (auto& row : trackRows)
        row->setArmed (row->getTrackId() == id);
}

void MainComponent::layoutTracks()
{
    const auto width = trackViewport.getMaximumVisibleWidth();
    trackContainer.setSize (width, juce::jmax (1, (int) trackRows.size() * trackRowHeight));

    for (size_t i = 0; i < trackRows.size(); ++i)
        trackRows[i]->setBounds (0, (int) i * trackRowHeight, width, trackRowHeight);
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (engine.getDeviceManager(),
                                                                          0, 0,     // audio inputs
                                                                          2, 64,    // audio outputs
                                                                          true,     // MIDI inputs
                                                                          false,    // MIDI output
                                                                          true,     // stereo pairs
                                                                          false);   // advanced options
    selector->setSize (520, 480);
    launchDialog (audioDialog, std::move (selector), "Audio & MIDI Settings", this);
}

void MainComponent::showPluginsMenu()
{
    const auto scanning = pluginScan != nullptr && pluginScan->isScanning();

    juce::PopupMenu menu;
    menu.addItem ("Scan for new plugins", ! scanning, false, [this] { startPluginScan ({}); });
    menu.addItem ("Retry failed plugins", ! scanning, false, [this] { startPluginScan ({ "--retry-failed" }); });
    menu.addItem ("Rescan everything", ! scanning, false, [this] { startPluginScan ({ "--rescan-all" }); });
    menu.addSeparator();
    menu.addItem ("Plugin list...", [this] { showPluginManager(); });
    menu.addItem ("Show scan log", [] { UserData::getPluginScanLog().startAsProcess(); });
    menu.addItem ("Open user data folder", [] { UserData::getDir().startAsProcess(); });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (pluginsButton));
}

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
                             : "Plugin scan failed - see Plugins > Show scan log";

        juce::Timer::callAfterDelay (8000, [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safe != nullptr && (safe->pluginScan == nullptr || ! safe->pluginScan->isScanning()))
                safe->scanStatus.clear();
        });
    };

    pluginScan->start();
}

void MainComponent::showPluginManager()
{
    auto list = std::make_unique<juce::PluginListComponent> (engine.getFormatManager(),
                                                             engine.getKnownPlugins(),
                                                             engine.getDeadMansPedalFile(),
                                                             nullptr,
                                                             true);
    list->setSize (760, 520);
    launchDialog (pluginDialog, std::move (list), "Plugins", this);
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
        // Edits made in the plugin list window (removals etc.), not our own reloads.
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
    for (auto& row : trackRows)
        row->updateMeter();

    auto& transport = engine.getTransport();
    playButton.setToggleState (transport.isPlaying(), juce::dontSendNotification);
    playButton.setButtonText (transport.isPlaying() ? "Stop" : "Play");

    const auto map = transport.getTempoMap();
    const auto position = map->ticksToBarsBeats (transport.getPositionTicks());
    const auto seconds = transport.getPositionSeconds();
    const auto minutes = (int) (seconds / 60.0);

    positionLabel.setText (juce::String (position.bar) + "." + juce::String (position.beat)
                             + "   " + juce::String (minutes)
                             + ":" + juce::String (seconds - minutes * 60.0, 1).paddedLeft ('0', 4),
                           juce::dontSendNotification);

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
        statusLabel.setText ("No audio device - open Audio Settings", juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::togglePerfPanel()
{
    perfPanel.setVisible (! perfPanel.isVisible());
    perfButton.setToggleState (perfPanel.isVisible(), juce::dontSendNotification);
    resized();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
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

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1d1f23));

    g.setColour (juce::Colour (0xff2a2d33));
    g.fillRect (getLocalBounds().removeFromTop (toolbarHeight));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    auto toolbar = area.removeFromTop (toolbarHeight).reduced (8, 7);
    audioButton.setBounds (toolbar.removeFromLeft (60));
    toolbar.removeFromLeft (6);
    pluginsButton.setBounds (toolbar.removeFromLeft (70));
    toolbar.removeFromLeft (6);
    addTrackButton.setBounds (toolbar.removeFromLeft (70));
    toolbar.removeFromLeft (14);
    rtzButton.setBounds (toolbar.removeFromLeft (34));
    toolbar.removeFromLeft (4);
    playButton.setBounds (toolbar.removeFromLeft (54));
    toolbar.removeFromLeft (4);
    loopButton.setBounds (toolbar.removeFromLeft (48));
    toolbar.removeFromLeft (6);
    bpmLabel.setBounds (toolbar.removeFromLeft (56));
    toolbar.removeFromLeft (6);
    positionLabel.setBounds (toolbar.removeFromLeft (150));
    toolbar.removeFromLeft (6);
    perfButton.setBounds (toolbar.removeFromRight (50));
    toolbar.removeFromRight (6);
    statusLabel.setBounds (toolbar);

    keyboard.setBounds (area.removeFromBottom (keyboardHeight));

    if (perfPanel.isVisible())
        perfPanel.setBounds (area.removeFromBottom (160));

    trackViewport.setBounds (area.reduced (4));
    layoutTracks();
}
