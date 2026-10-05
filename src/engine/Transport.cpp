#include "Transport.h"

void Transport::prepare (double newRate)
{
    if (newRate <= 0)
        return;

    // Keep the musical position stable across sample-rate changes.
    if (sampleRate > 0 && position > 0)
    {
        const auto m = map.load();
        position = m->ticksToSamples (m->samplesToTicks (position, sampleRate), newRate);
        startPosition = m->ticksToSamples (m->samplesToTicks (startPosition, sampleRate), newRate);
    }

    sampleRate = newRate;
    rateShared.store (newRate);
    positionShared.store (position);
}

void Transport::beginBlock (int numSamples)
{
    const auto m = map.load();

    block = {};
    block.map = m;
    block.sampleRate = sampleRate;
    block.length = numSamples;

    const auto cmd = command.exchange (cmdNone);
    const auto located = locateTarget.exchange (-1);
    const auto preRollSamples = (juce::int64) std::llround (preRollMs.load() * 0.001 * sampleRate);

    // Playback (re)starting at 'position': begin the pre-roll before it
    const auto beginPreRoll = [this, preRollSamples]
    {
        startPosition = position;
        preRolling = preRollSamples > 0;

        if (preRolling)
            position -= preRollSamples;
    };

    if (located >= 0)
    {
        position = m->ticksToSamples (located, sampleRate);
        startPosition = position;
        preRolling = false;

        if (playing)
        {
            block.killAtStart = true;
            block.chaseAtStart = true;
            beginPreRoll();   // a locate while playing restarts, so events due before the new position still sound
        }
    }

    if (cmd == cmdPlay && ! playing)
    {
        playing = true;
        block.chaseAtStart = true;
        beginPreRoll();
    }
    else if (cmd == cmdStop && playing)
    {
        playing = false;
        block.killAtStart = true;

        if (preRolling)   // stopped before the pre-roll ended: the position is where playback was going to start
        {
            position = startPosition;
            preRolling = false;
        }
    }

    if (playing && numSamples > 0)
    {
        const auto loopStartT = loopStartTick.load();
        const auto loopEndT = loopEndTick.load();
        const auto loopStartS = m->ticksToSamples (loopStartT, sampleRate);
        const auto loopEndS = m->ticksToSamples (loopEndT, sampleRate);
        const bool loopValid = looping.load() && loopEndS > loopStartS;

        // The previous block may have landed exactly on the loop end, or the playhead
        // may sit beyond the loop (loop enabled late): wrap to the loop start either way.
        if (loopValid && position >= loopEndS)
        {
            position = loopStartS;
            startPosition = position;
            preRolling = false;
            block.wrappedAtStart = true;
            block.killAtStart = true;
            block.chaseAtStart = true;
        }

        // The pre-roll ends once the position reaches where playback really begins
        if (preRolling && position >= startPosition)
            preRolling = false;

        block.inPreRoll = preRolling;
        block.regionEndTick = loopValid ? loopEndT : std::numeric_limits<juce::int64>::max();
        const auto startTickOfPlayback = m->samplesToTicks (startPosition, sampleRate);

        const auto end = position + numSamples;

        if (loopValid && position < loopEndS && end > loopEndS)
        {
            const auto firstLength = (int) (loopEndS - position);

            block.segments[0] = { m->samplesToTicks (position, sampleRate), loopEndT,
                                  position, 0, firstLength, 0 };

            const auto remainder = numSamples - firstLength;
            block.segments[1] = { loopStartT, m->samplesToTicks (loopStartS + remainder, sampleRate),
                                  loopStartS, firstLength, remainder, loopStartT };

            block.numSegments = 2;
            block.looped = true;
            position = loopStartS + remainder;
        }
        else
        {
            block.segments[0] = { m->samplesToTicks (position, sampleRate), m->samplesToTicks (end, sampleRate),
                                  position, 0, numSamples, 0 };
            block.numSegments = 1;
            position = end;
        }

        // Events scheduled before the gate play only if they were written at or after it. In a
        // pre-roll block the gate is where playback really begins; otherwise nothing in the
        // segment lies before its own start, so the gate changes nothing.
        block.segments[0].gateTick = block.inPreRoll ? startTickOfPlayback : block.segments[0].startTick;

        // Loop look-ahead: the last pre-roll's worth of a lap also plays the NEXT lap's early events.
        // They are scheduled just before the loop start; the same samples, one loop length earlier.
        if (loopValid && preRollSamples > 0)
        {
            const auto loopLengthS = loopEndS - loopStartS;
            const auto windowStart = loopEndS - juce::jmin (preRollSamples, loopLengthS);
            const auto& s0 = block.segments[0];
            const auto s0End = s0.startSample + s0.numSamples;
            const auto a = juce::jmax (s0.startSample, windowStart);
            const auto b = juce::jmin (s0End, loopEndS);

            if (b > a)
            {
                block.hasAhead = true;
                block.ahead = { m->samplesToTicks (a - loopLengthS, sampleRate), m->samplesToTicks (b - loopLengthS, sampleRate),
                                a - loopLengthS, s0.offset + (int) (a - s0.startSample), (int) (b - a), loopStartT };
            }
        }

        if (preRolling && position >= startPosition)
            preRolling = false;
    }

    block.playing = playing;
    positionShared.store (preRolling && position < startPosition ? startPosition : position);   // held at the start during a pre-roll
    playingShared.store (playing);
}
