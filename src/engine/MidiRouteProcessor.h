#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <bitset>

// One MIDI track output: sits between a track's MIDI source (plus live input when the
// track is armed) and an instrument, rewriting every channel message to the output's
// MIDI channel.
//
// Also the track's MIDI mute/solo gate: when disabled, events are dropped and any
// sounding notes receive note-offs, so muting mid-phrase releases cleanly. The same
// happens when the target channel changes while notes are held.
class MidiRouteProcessor final : public juce::AudioProcessor
{
public:
    explicit MidiRouteProcessor (int initialChannel)
    {
        targetChannel.store (juce::jlimit (1, 16, initialChannel));
    }

    void setTargetChannel (int channel)      { targetChannel.store (juce::jlimit (1, 16, channel)); }
    int getTargetChannel() const             { return targetChannel.load(); }

    void setRouteEnabled (bool shouldPass)   { routeEnabled.store (shouldPass); }
    bool isRouteEnabled() const              { return routeEnabled.load(); }

    // Release whatever is sounding on the next block (e.g. the track just lost
    // its live-input arming while keys were held).
    void killHeldNotes()                     { killRequest.store (true); }

    //==============================================================================
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& midi) override
    {
        const auto channel = targetChannel.load();
        const auto enabled = routeEnabled.load();
        const auto kill = killRequest.exchange (false);

        scratch.clear();

        // Release held notes when the gate closes, the destination channel changes,
        // or a kill was requested (the track lost its arming mid-note).
        if ((! enabled || channel != lastChannel || kill) && heldKeys.any())
        {
            for (int key = 0; key < 128; ++key)
                if (heldKeys[(size_t) key])
                    scratch.addEvent (juce::MidiMessage::noteOff (lastChannel, key), 0);

            heldKeys.reset();
        }

        if (enabled)
        {
            for (const auto metadata : midi)
            {
                auto message = metadata.getMessage();

                if (message.getChannel() > 0)
                    message.setChannel (channel);

                if (message.isNoteOn())
                    heldKeys.set ((size_t) message.getNoteNumber());
                else if (message.isNoteOff())
                    heldKeys.reset ((size_t) message.getNoteNumber());

                scratch.addEvent (message, metadata.samplePosition);
            }
        }

        lastChannel = channel;
        midi.swapWith (scratch);
    }

    //==============================================================================
    const juce::String getName() const override              { return "MIDI Route"; }
    bool acceptsMidi() const override                        { return true; }
    bool producesMidi() const override                       { return true; }
    void prepareToPlay (double, int) override                {}
    void releaseResources() override                         {}
    double getTailLengthSeconds() const override             { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override      { return nullptr; }
    bool hasEditor() const override                          { return false; }
    int getNumPrograms() override                            { return 1; }
    int getCurrentProgram() override                         { return 0; }
    void setCurrentProgram (int) override                    {}
    const juce::String getProgramName (int) override         { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override   {}
    void setStateInformation (const void*, int) override     {}

private:
    std::atomic<int> targetChannel { 1 };
    std::atomic<bool> routeEnabled { true };
    std::atomic<bool> killRequest { false };

    // Audio-thread state
    std::bitset<128> heldKeys;
    int lastChannel = 1;
    juce::MidiBuffer scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiRouteProcessor)
};
