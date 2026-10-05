#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <bitset>
#include "MidiMonitor.h"

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
    explicit MidiRouteProcessor (int initialChannel, int initialPort = 1)
    {
        targetChannel.store (juce::jlimit (1, 16, initialChannel));
        targetPort.store (juce::jlimit (1, 16, initialPort));
    }

    // MIDI port (VST3 event bus) of the destination plugin. Port 1 passes plain
    // messages; ports >= 2 are wrapped as  F0 7D 50 <bus> <st hi> <st lo> <data..> F7,
    // which our patched JUCE VST3 host unwraps onto event bus 'port - 1'
    // (patches/juce-vst3-event-bus.patch).
    void setTargetPort (int port)            { targetPort.store (juce::jlimit (1, 16, port)); }
    int getTargetPort() const                { return targetPort.load(); }

    static juce::MidiMessage wrapForPort (const juce::MidiMessage& message, int port)
    {
        const auto* raw = message.getRawData();
        const auto size = message.getRawDataSize();

        if (port <= 1 || size < 1 || size > 3 || raw[0] < 0x80 || raw[0] >= 0xf0)
            return message;   // port 1, or not a channel message: pass unchanged

        juce::uint8 bytes[9] = { 0xf0, 0x7d, 0x50, (juce::uint8) (port - 1),
                                 (juce::uint8) (raw[0] >> 4), (juce::uint8) (raw[0] & 0x0f) };
        int length = 6;

        for (int i = 1; i < size; ++i)
            bytes[length++] = raw[i];

        bytes[length++] = 0xf7;
        return juce::MidiMessage (bytes, length, message.getTimeStamp());
    }

    void setTargetChannel (int channel)      { targetChannel.store (juce::jlimit (1, 16, channel)); }
    int getTargetChannel() const             { return targetChannel.load(); }

    void setRouteEnabled (bool shouldPass)   { routeEnabled.store (shouldPass); }
    bool isRouteEnabled() const              { return routeEnabled.load(); }

    // Release whatever is sounding on the next block (e.g. the track just lost
    // its live-input arming while keys were held).
    void killHeldNotes()                     { killRequest.store (true); }

    // The MIDI monitor records what this route hands the instrument (set once, at creation)
    void setMonitor (MidiMonitor* monitorToUse, std::function<juce::int64()> transportTick, int trackId, int instrumentId)
    {
        monitor = monitorToUse;
        tickNow = std::move (transportTick);
        monitorTrack = trackId;
        monitorInstrument = instrumentId;
    }

    //==============================================================================
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& midi) override
    {
        const auto channel = targetChannel.load();
        const auto port = targetPort.load();
        const auto enabled = routeEnabled.load();
        const auto kill = killRequest.exchange (false);

        scratch.clear();

        // Release held notes when the gate closes, the destination channel changes,
        // or a kill was requested (the track lost its arming mid-note).
        if ((! enabled || channel != lastChannel || kill) && heldKeys.any())
        {
            for (int key = 0; key < 128; ++key)
                if (heldKeys[(size_t) key])
                    scratch.addEvent (wrapForPort (juce::MidiMessage::noteOff (lastChannel, key), port), 0);

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

                scratch.addEvent (wrapForPort (message, port), metadata.samplePosition);
            }
        }

        lastChannel = channel;
        midi.swapWith (scratch);

        if (monitor != nullptr && monitor->isEnabled() && ! midi.isEmpty())
        {
            MidiMonitor::Event event;
            event.blockMs = juce::Time::getMillisecondCounterHiRes();
            event.sampleRate = sampleRate;
            event.tick = tickNow ? tickNow() : 0;
            event.trackId = monitorTrack;
            event.instrumentId = monitorInstrument;
            event.port = port;

            for (const auto metadata : midi)
            {
                // record the plain message (port >= 2 traffic is wrapped for the VST3 host)
                const auto* raw = metadata.data;
                const auto wrapped = metadata.numBytes >= 7 && raw[0] == 0xf0 && raw[1] == 0x7d && raw[2] == 0x50;
                event.offset = metadata.samplePosition;
                event.size = 0;

                if (wrapped)
                {
                    event.bytes[0] = (juce::uint8) ((raw[4] << 4) | (raw[5] & 0x0f));
                    event.size = 1;

                    for (int i = 6; i < metadata.numBytes - 1 && event.size < 3; ++i)
                        event.bytes[event.size++] = raw[i];
                }
                else
                {
                    for (int i = 0; i < metadata.numBytes && event.size < 3; ++i)
                        event.bytes[event.size++] = raw[i];
                }

                monitor->push (event);
            }
        }
    }

    //==============================================================================
    const juce::String getName() const override              { return "MIDI Route"; }
    bool acceptsMidi() const override                        { return true; }
    bool producesMidi() const override                       { return true; }
    void prepareToPlay (double rate, int) override           { sampleRate = rate; }
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
    std::atomic<int> targetPort { 1 };
    std::atomic<bool> routeEnabled { true };
    std::atomic<bool> killRequest { false };

    MidiMonitor* monitor = nullptr;
    std::function<juce::int64()> tickNow;
    int monitorTrack = 0, monitorInstrument = 0;
    double sampleRate = 48000.0;

    // Audio-thread state
    std::bitset<128> heldKeys;
    int lastChannel = 1;
    juce::MidiBuffer scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiRouteProcessor)
};
