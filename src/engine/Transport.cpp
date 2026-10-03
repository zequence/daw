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

    if (located >= 0)
    {
        position = m->ticksToSamples (located, sampleRate);

        if (playing)
        {
            block.killAtStart = true;
            block.chaseAtStart = true;
        }
    }

    if (cmd == cmdPlay && ! playing)
    {
        playing = true;
        block.chaseAtStart = true;
    }
    else if (cmd == cmdStop && playing)
    {
        playing = false;
        block.killAtStart = true;
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
            block.killAtStart = true;
            block.chaseAtStart = true;
        }

        const auto end = position + numSamples;

        if (loopValid && position < loopEndS && end > loopEndS)
        {
            const auto firstLength = (int) (loopEndS - position);

            block.segments[0] = { m->samplesToTicks (position, sampleRate), loopEndT,
                                  position, 0, firstLength };

            const auto remainder = numSamples - firstLength;
            block.segments[1] = { loopStartT, m->samplesToTicks (loopStartS + remainder, sampleRate),
                                  loopStartS, firstLength, remainder };

            block.numSegments = 2;
            block.looped = true;
            position = loopStartS + remainder;
        }
        else
        {
            block.segments[0] = { m->samplesToTicks (position, sampleRate), m->samplesToTicks (end, sampleRate),
                                  position, 0, numSamples };
            block.numSegments = 1;
            position = end;
        }
    }

    block.playing = playing;
    positionShared.store (position);
    playingShared.store (playing);
}
