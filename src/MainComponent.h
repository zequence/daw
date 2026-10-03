#pragma once

#include "AudioEngine.h"
#include "TrackRow.h"
#include "PluginScanProcess.h"
#include "diagnostics/PerformancePanel.h"

class MainComponent final : public juce::Component,
                            private juce::MidiKeyboardState::Listener,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    explicit MainComponent (AudioEngine&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void addTrack();
    void removeTrack (AudioEngine::TrackId);
    void armTrack (AudioEngine::TrackId);
    void showAudioSettings();
    void showPluginsMenu();
    void showPluginManager();
    void startPluginScan (juce::StringArray args);
    void layoutTracks();
    void togglePerfPanel();

    void handleNoteOn (juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void handleNoteOff (juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    AudioEngine& engine;

    juce::TextButton audioButton { "Audio Settings..." }, pluginsButton { "Plugins..." }, addTrackButton { "+ Track" },
                     perfButton { "Perf" };

    PerformanceTracker perfTracker { engine };
    PerformancePanel perfPanel { perfTracker };
    juce::Label statusLabel;

    std::unique_ptr<PluginScanProcess> pluginScan;
    juce::String scanStatus;
    bool reloadingPluginCache = false;

    juce::Viewport trackViewport;
    juce::Component trackContainer;
    std::vector<std::unique_ptr<TrackRow>> trackRows;
    int trackCounter = 0;

    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard { keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard };

    juce::Component::SafePointer<juce::DialogWindow> audioDialog, pluginDialog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
