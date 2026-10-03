#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Per-track stereo channel strip: gain, mute and peak metering.
// Sits between a track's instrument plugin and the master output.
class TrackChannelProcessor final : public juce::AudioProcessor
{
public:
    TrackChannelProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    void setGain (float newGain) noexcept   { gain.store (newGain); }
    void setMuted (bool shouldMute) noexcept { muted.store (shouldMute); }

    // Returns the peak since the last call and resets it (call from the UI thread).
    float takePeak() noexcept               { return peak.exchange (0.0f); }

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

        auto previous = peak.load();
        while (blockPeak > previous && ! peak.compare_exchange_weak (previous, blockPeak)) {}
    }

    //==============================================================================
    const juce::String getName() const override             { return "Track Channel"; }
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackChannelProcessor)
};
