#pragma once

#include "AudioEngine.h"

// One MIDI track in the track list: arm, name, output picker, instrument editor,
// demo/clear clip, MIDI solo/mute, and (interim, until the Audio view exists) the
// volume and meter of the first output's audio channel.
class TrackRow final : public juce::Component
{
public:
    TrackRow (AudioEngine&, AudioEngine::TrackId, const juce::String& name);

    AudioEngine::TrackId getTrackId() const noexcept { return trackId; }

    void setArmed (bool);
    void refresh();   // meter, output label, button states; call from a UI timer

    std::function<void (AudioEngine::TrackId)> onArmClicked, onRemoveClicked, onSetDemo, onClearClip,
                                               onChooseOutput, onOpenInstrument;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AudioChannelProcessor* getFirstOutputChannel() const;

    AudioEngine& engine;
    const AudioEngine::TrackId trackId;

    juce::TextButton armButton { "R" }, outputButton { "(no output)" }, editButton { "Edit" },
                     demoButton { "Demo" }, clearButton { "Clear" },
                     soloButton { "S" }, muteButton { "M" }, removeButton { "X" };
    juce::Label nameLabel;
    juce::Slider volumeSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    float meterLevel = 0.0f;
    bool armed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackRow)
};
