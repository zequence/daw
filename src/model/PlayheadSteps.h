#pragma once

#include "MidiSequence.h"

// Where Left/Right moves the transport line in the MIDI editor. Right: to the nearest
// note end ahead; left: to the nearest note start behind (with parallel notes, the
// closest one). No note that way: to the next grid line.
inline juce::int64 nextPlayheadStop (juce::int64 position, bool forward, const MidiSequence* sequence, juce::int64 step)
{
    juce::int64 best = -1;

    if (sequence != nullptr)
        for (auto& note : sequence->getNotes())
        {
            if (forward)
            {
                const auto end = note.startTick + note.lengthTicks;

                if (end > position && (best < 0 || end < best))
                    best = end;
            }
            else if (note.startTick < position && note.startTick > best)
            {
                best = note.startTick;
            }
        }

    if (best >= 0)
        return best;

    step = juce::jmax ((juce::int64) 1, step);

    if (forward)
        return (position / step + 1) * step;

    return juce::jmax ((juce::int64) 0, ((position + step - 1) / step - 1) * step);
}
