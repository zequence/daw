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

    // Pre-roll (MILESTONES.md "Timing offset"): playback starts this long BEFORE the start
    // position, while the displayed position stays at the start until it is reached. It lets
    // events that a negative timing offset schedules earlier than the part being played sound
    // on time. 0 = none (the default: nothing changes).
    void setPreRollMs (double ms)           { preRollMs.store (juce::jmax (0.0, ms)); }
    double getPreRollMs() const             { return preRollMs.load(); }

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

        // Events scheduled before this tick play only if they were WRITTEN at or after it: during a
        // pre-roll (or a loop look-ahead) the time just before the part being played holds events of
        // that part (shifted early) and ordinary events that lie before it (which must stay silent).
        juce::int64 gateTick = 0;
    };

    struct Block
    {
        bool playing = false;
        bool chaseAtStart = false;    // playback (re)started or relocated: chase controllers
        bool killAtStart = false;     // stop/locate/loop-landing: send note-offs at offset 0
        bool looped = false;          // segments[1] starts at the loop point
        bool wrappedAtStart = false;  // killAtStart/chaseAtStart because the previous block landed on the loop end
        bool inPreRoll = false;       // the block starts before the position where playback really begins
        juce::int64 regionEndTick = std::numeric_limits<juce::int64>::max();   // the loop end: events WRITTEN at or after it don't play

        // Loop look-ahead: in the last pre-roll's worth of a lap, the NEXT lap's early events are due.
        // 'ahead' covers their ticks (just before the loop start) over the same buffer samples as the
        // tail of the current lap; only events written inside the loop play from it.
        bool hasAhead = false;
        Segment ahead;
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

        int offsetFor (juce::int64 tick, const Segment& s) const noexcept
        {
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
    std::atomic<double> preRollMs { 0.0 };

    // Published for the UI
    std::atomic<juce::int64> positionShared { 0 };
    std::atomic<bool> playingShared { false };
    std::atomic<double> rateShared { 48000.0 };

    // Audio-thread state
    Block block;
    juce::int64 position = 0;         // timeline position in samples
    bool playing = false;
    double sampleRate = 48000.0;
    juce::int64 startPosition = 0;     // where playback really begins (the first sample after the pre-roll)
    bool preRolling = false;

    JUCE_DECLARE_NON_COPYABLE (Transport)
};
