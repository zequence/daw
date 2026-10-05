#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Transport.h"

// Graph node that taps the live MIDI input while the transport plays and recording is
// active. Each event is stamped with its musical position (ticks) on the audio thread and
// pushed into a lock-free FIFO; the message thread drains it (see MidiRecorder).
//
// When the loop wraps mid-buffer, a marker is pushed between the pre- and post-wrap
// events so held notes can be closed at the loop end.
class MidiRecorderProcessor final : public juce::AudioProcessor
{
public:
    explicit MidiRecorderProcessor (const Transport& t) : transport (t), buffer (fifoCapacity)
    {
    }

    void setActive (bool shouldRecord)   { active.store (shouldRecord); }
    bool isActive() const                { return active.load(); }

    struct TimedEvent
    {
        juce::int64 tick = 0;
        int size = 0;
        juce::uint8 data[3] = {};
        bool wrapMarker = false;
    };

    // Message thread: hand every queued event to the callback, in order.
    template <typename Callback>
    void drain (Callback&& handle)
    {
        const auto ready = fifo.getNumReady();
        int start1, size1, start2, size2;
        fifo.prepareToRead (ready, start1, size1, start2, size2);

        for (int i = 0; i < size1; ++i) handle (buffer[(size_t) (start1 + i)]);
        for (int i = 0; i < size2; ++i) handle (buffer[(size_t) (start2 + i)]);

        fifo.finishedRead (size1 + size2);
    }

    //==============================================================================
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& midi) override
    {
        const auto& b = transport.getBlock();

        if (active.load() && b.playing && b.numSegments > 0)
        {
            const bool wrapped = b.numSegments == 2;
            bool markerPushed = ! wrapped;

            for (const auto metadata : midi)
            {
                const auto offset = metadata.samplePosition;
                const int segment = (wrapped && offset >= b.segments[1].offset) ? 1 : 0;

                if (segment == 1 && ! markerPushed)
                {
                    pushMarker (b.segments[0].endTick);
                    markerPushed = true;
                }

                if (metadata.numBytes <= 3)   // channel voice messages only; no sysex
                {
                    const auto& seg = b.segments[segment];

                    TimedEvent event;
                    event.tick = b.map->samplesToTicks (seg.startSample + (offset - seg.offset), b.sampleRate);

                    // During a pre-roll the transport runs before the position it shows: what is played then
                    // is recorded at the start of the take, never before it
                    if (b.inPreRoll && segment == 0)
                        event.tick = juce::jmax (event.tick, seg.gateTick);
                    event.size = metadata.numBytes;
                    std::memcpy (event.data, metadata.data, (size_t) metadata.numBytes);
                    push (event);
                }
            }

            if (! markerPushed)
                pushMarker (b.segments[0].endTick);
        }

        midi.clear();   // the recorder is a sink
    }

    //==============================================================================
    const juce::String getName() const override              { return "MIDI Recorder"; }
    bool acceptsMidi() const override                        { return true; }
    bool producesMidi() const override                       { return false; }
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
    void push (const TimedEvent& event)
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 > 0)
        {
            buffer[(size_t) start1] = event;
            fifo.finishedWrite (1);
        }
        // else: FIFO full, event dropped - with 4096 slots and a 30 Hz drain this
        // would take a sustained flood of controller data to hit
    }

    void pushMarker (juce::int64 tick)
    {
        TimedEvent marker;
        marker.tick = tick;
        marker.wrapMarker = true;
        push (marker);
    }

    static constexpr int fifoCapacity = 4096;

    const Transport& transport;
    std::atomic<bool> active { false };
    juce::AbstractFifo fifo { fifoCapacity };
    std::vector<TimedEvent> buffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiRecorderProcessor)
};
