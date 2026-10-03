#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// A stereo audio channel strip: gain, mute and peak metering.
// Sits between an input (an instrument, later a device input) and the master output.
class AudioChannelProcessor final : public juce::AudioProcessor
{
public:
    AudioChannelProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    void setGain (float newGain) noexcept    { gain.store (newGain); }
    float getGain() const noexcept           { return gain.load(); }
    void setMuted (bool shouldMute) noexcept { muted.store (shouldMute); }
    bool isMuted() const noexcept            { return muted.load(); }

    // Peak of the most recent block; any number of readers may poll this.
    float getLastPeak() const noexcept       { return peak.load(); }

    //==============================================================================
    void prepareToPlay (double, int) override { lastGain = muted.load() ? 0.0f : gain.load(); }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        midi.clear();

        const auto target = muted.load() ? 0.0f : gain.load();

        if (std::abs (target - lastGain) < 1.0e-6f)
            buffer.applyGain (target);
        else
            buffer.applyGainRamp (0, buffer.getNumSamples(), lastGain, target);

        lastGain = target;

        float blockPeak = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            blockPeak = juce::jmax (blockPeak, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));

        peak.store (blockPeak);
    }

    //==============================================================================
    const juce::String getName() const override             { return "Audio Channel"; }
    bool acceptsMidi() const override                       { return false; }
    bool producesMidi() const override                      { return false; }
    double getTailLengthSeconds() const override            { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override     { return nullptr; }
    bool hasEditor() const override                         { return false; }
    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override  {}
    void setStateInformation (const void*, int) override    {}

private:
    std::atomic<float> gain { 1.0f }, peak { 0.0f };
    std::atomic<bool> muted { false };
    float lastGain = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioChannelProcessor)
};
