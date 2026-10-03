#pragma once

#include <atomic>
#include "../model/TempoMap.h"

// Owns the playhead.
//
// The message thread posts commands (play, stop, locate, loop); the audio thread consumes
// them in beginBlock(), advances the position, and publishes a Block describing the tick
// range(s) covered by the current audio buffer. The per-track MIDI sources read that Block
// later in the same audio callback, so every track sees an identical timeline.
//
// A Block has two segments when the loop wraps inside the buffer; otherwise one (or none
// when stopped). Lock-free apart from the tempo map shared_ptr reference count.
class Transport
{
public:
    Transport() { setTempoMap (TempoMap::create()); }

    //==============================================================================
    // Any thread
    void setTempoMap (TempoMap::Ptr m)      { map.store (std::move (m)); }
    TempoMap::Ptr getTempoMap() const       { return map.load(); }

    //==============================================================================
    // Message-thread commands (consumed by the next audio block)
    void play()                             { command.store (cmdPlay); }
    void stop()                             { command.store (cmdStop); }
    void togglePlayStop()                   { isPlaying() ? stop() : play(); }
    void locate (juce::int64 tick)
    {
        tick = juce::jmax ((juce::int64) 0, tick);
        locateTarget.store (tick);

        // Reflect the move in the UI-visible position right away; the audio thread
        // applies the same value authoritatively in its next block.
        positionShared.store (map.load()->ticksToSamples (tick, rateShared.load()));
    }
    void returnToZero()                     { locate (0); }

    void setLoopRegion (juce::int64 startTick, juce::int64 endTick)
    {
        loopStartTick.store (juce::jmax ((juce::int64) 0, startTick));
        loopEndTick.store (endTick);
    }

    void setLooping (bool shouldLoop)       { looping.store (shouldLoop); }
    bool isLooping() const                  { return looping.load(); }
    juce::int64 getLoopStart() const        { return loopStartTick.load(); }
    juce::int64 getLoopEnd() const          { return loopEndTick.load(); }

    bool isPlaying() const                  { return playingShared.load(); }

    juce::int64 getPositionTicks() const
    {
        return map.load()->samplesToTicks (positionShared.load(), rateShared.load());
    }

    double getPositionSeconds() const
    {
        const auto rate = rateShared.load();
        return rate > 0 ? (double) positionShared.load() / rate : 0.0;
    }

    //==============================================================================
    // Audio thread
    struct Segment
    {
        juce::int64 startTick = 0, endTick = 0;   // [startTick, endTick)
        juce::int64 startSample = 0;              // timeline sample position of startTick
        int offset = 0, numSamples = 0;           // where this segment sits in the buffer
    };

    struct Block
    {
        bool playing = false;
        bool chaseAtStart = false;    // playback (re)started or relocated: chase controllers
        bool killAtStart = false;     // stop/locate/loop-landing: send note-offs at offset 0
        bool looped = false;          // segments[1] starts at the loop point
        int numSegments = 0;
        Segment segments[2];
        double sampleRate = 48000.0;
        int length = 0;
        TempoMap::Ptr map;

        // Buffer offset for an event at 'tick' inside the given segment, clamped into range.
        int offsetFor (juce::int64 tick, int segmentIndex) const noexcept
        {
            const auto& s = segments[segmentIndex];
            const auto rel = map->ticksToSamples (tick, sampleRate) - s.startSample;
            return s.offset + (int) juce::jlimit ((juce::int64) 0, (juce::int64) s.numSamples - 1, rel);
        }
    };

    void prepare (double sampleRate);
    void beginBlock (int numSamples);
    const Block& getBlock() const noexcept  { return block; }

private:
    enum Command { cmdNone = 0, cmdPlay, cmdStop };

    std::atomic<TempoMap::Ptr> map;
    std::atomic<int> command { cmdNone };
    std::atomic<juce::int64> locateTarget { -1 };
    std::atomic<juce::int64> loopStartTick { 0 }, loopEndTick { 0 };
    std::atomic<bool> looping { false };

    // Published for the UI
    std::atomic<juce::int64> positionShared { 0 };
    std::atomic<bool> playingShared { false };
    std::atomic<double> rateShared { 48000.0 };

    // Audio-thread state
    Block block;
    juce::int64 position = 0;         // timeline position in samples
    bool playing = false;
    double sampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE (Transport)
};
