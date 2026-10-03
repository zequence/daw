#pragma once

#include "AudioEngine.h"
#include "PluginWindow.h"

// One track in the track list: arm, name, instrument picker, editor, mute, volume, meter.
class TrackRow final : public juce::Component
{
public:
    TrackRow (AudioEngine&, AudioEngine::TrackId, const juce::String& name);
    ~TrackRow() override;

    AudioEngine::TrackId getTrackId() const noexcept { return trackId; }

    void setArmed (bool);
    void updateMeter();

    std::function<void (AudioEngine::TrackId)> onArmClicked, onRemoveClicked;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void showInstrumentMenu();
    void loadInstrument (const juce::PluginDescription&);
    void clearInstrument();
    void openEditor();
    void closeEditor();

    AudioEngine& engine;
    const AudioEngine::TrackId trackId;

    juce::TextButton armButton { "R" }, instrumentButton { "(no instrument)" }, editButton { "Edit" },
                     muteButton { "M" }, removeButton { "X" };
    juce::Label nameLabel;
    juce::Slider volumeSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    float meterLevel = 0.0f;
    bool armed = false;

    std::unique_ptr<PluginWindow> pluginWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackRow)
};
