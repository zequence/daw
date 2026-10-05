#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <array>

// A MIDI monitor: what the app hands each instrument, exactly as it leaves the track routes
// (after channel rewriting, before the plugin) - notes, program changes, controllers, with the
// block's wall-clock time and the sample position inside the block, so their order and spacing
// are exact. Off unless started (midi.monitor). The audio thread only writes into a lock-free
// FIFO (dropping entries when it is full, never waiting); the message thread drains it.
class MidiMonitor
{
public:
    struct Event
    {
        double blockMs = 0.0;          // juce::Time::getMillisecondCounterHiRes() at the block
        double sampleRate = 48000.0;
        int offset = 0;                // sample position in the block
        juce::int64 tick = 0;          // the transport's position at the block
        int trackId = 0, instrumentId = 0, port = 1;
        juce::uint8 bytes[3] {};
        int size = 0;

        double timeMs() const noexcept    { return blockMs + 1000.0 * offset / sampleRate; }
    };

    void setEnabled (bool shouldRecord)       { enabled.store (shouldRecord); }
    bool isEnabled() const noexcept           { return enabled.load (std::memory_order_relaxed); }

    // Audio thread
    void push (const Event& event) noexcept
    {
        const auto scope = fifo.write (1);

        if (scope.blockSize1 > 0)
            entries[(size_t) scope.startIndex1] = event;
        else if (scope.blockSize2 > 0)
            entries[(size_t) scope.startIndex2] = event;
        else
            dropped.fetch_add (1, std::memory_order_relaxed);
    }

    // Message thread: everything recorded since the last drain
    std::vector<Event> drain()
    {
        std::vector<Event> result;
        const auto scope = fifo.read (fifo.getNumReady());

        for (int i = 0; i < scope.blockSize1; ++i)
            result.push_back (entries[(size_t) (scope.startIndex1 + i)]);

        for (int i = 0; i < scope.blockSize2; ++i)
            result.push_back (entries[(size_t) (scope.startIndex2 + i)]);

        return result;
    }

    int takeDropped() noexcept    { return dropped.exchange (0); }

private:
    static constexpr int capacity = 8192;
    std::atomic<bool> enabled { false };
    std::atomic<int> dropped { 0 };
    juce::AbstractFifo fifo { capacity };
    std::array<Event, capacity> entries;
};
