#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "AnalogStrip.h"

// A stereo audio channel strip: the console processing (filters, EQ, dynamics, drive - AnalogStrip.h),
// then fader (gain), pan, mute, solo-silencing and metering (peak and RMS).
// Sits between an input (an instrument, later a device input) and the master bus; the master bus
// is one too, between the channels and the device output.
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

    // Pan: -1 (left) .. 0 .. +1 (right), constant power (-3 dB in the middle)
    void setPan (float newPan) noexcept      { pan.store (juce::jlimit (-1.0f, 1.0f, newPan)); }
    float getPan() const noexcept            { return pan.load(); }

    // Silenced because other channels are soloed (the engine sets it)
    void setSoloSilenced (bool silenced) noexcept { soloSilenced.store (silenced); }
    bool isSoloSilenced() const noexcept          { return soloSilenced.load(); }

    // The most recent block's peak and RMS (0..1+, after the fader); any number of readers may poll these.
    float getLastPeak() const noexcept       { return peak.load(); }
    float getLastRms() const noexcept        { return rms.load(); }

    // The filters, EQ, dynamics and drive (before the fader). They run only with the analog console
    // mixer (Settings); the default mixer has no built-in processing, so every channel skips them
    static inline std::atomic<bool> consoleProcessing { false };
    AnalogStrip& getStrip() noexcept               { return strip; }
    const AnalogStrip& getStrip() const noexcept   { return strip; }

    //==============================================================================
    void prepareToPlay (double sampleRate, int) override
    {
        strip.prepare (sampleRate);
        const auto [left, right] = targetGains();
        lastLeft = left;
        lastRight = right;
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        midi.clear();

        if (consoleProcessing.load())
            strip.process (buffer);
        const auto [left, right] = targetGains();
        const auto samples = buffer.getNumSamples();

        // Ramped when anything changed, so faders and pans don't click
        if (buffer.getNumChannels() > 0) buffer.applyGainRamp (0, 0, samples, lastLeft, left);
        if (buffer.getNumChannels() > 1) buffer.applyGainRamp (1, 0, samples, lastRight, right);

        lastLeft = left;
        lastRight = right;

        float blockPeak = 0.0f, sumSquares = 0.0f;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            blockPeak = juce::jmax (blockPeak, buffer.getMagnitude (ch, 0, samples));
            const auto r = buffer.getRMSLevel (ch, 0, samples);
            sumSquares += r * r;
        }

        peak.store (blockPeak);
        rms.store (std::sqrt (sumSquares / (float) juce::jmax (1, buffer.getNumChannels())));
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
    // The left and right gains: the fader, the pan (constant power, normalised to unity in the
    // middle so a centred channel is as loud as before pan existed), mute and solo
    std::pair<float, float> targetGains() const noexcept
    {
        if (muted.load() || soloSilenced.load())
            return { 0.0f, 0.0f };

        const auto angle = (pan.load() + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        const auto g = gain.load() * juce::MathConstants<float>::sqrt2;
        return { g * std::cos (angle), g * std::sin (angle) };
    }

    std::atomic<float> gain { 1.0f }, pan { 0.0f }, peak { 0.0f }, rms { 0.0f };
    std::atomic<bool> muted { false }, soloSilenced { false };
    float lastLeft = 1.0f, lastRight = 1.0f;
    AnalogStrip strip;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioChannelProcessor)
};

// A bus's input: what is routed to it is summed here, then goes through its inserts to its strip
class PassThroughProcessor final : public juce::AudioProcessor
{
public:
    PassThroughProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)) {}

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& midi) override   { midi.clear(); }

    const juce::String getName() const override             { return "Bus Input"; }
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PassThroughProcessor)
};
