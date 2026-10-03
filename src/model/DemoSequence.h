#pragma once

#include "MidiSequence.h"

// Two bars of test material until real clips exist: a quarter-note arpeggio with a
// modwheel (CC1) swell - enough to hear note timing, looping and CC chasing.
inline MidiSequence::Ptr makeDemoSequence()
{
    constexpr auto Q = Ticks::perQuarterNote;

    std::vector<MidiSequence::Note> notes;
    const int keys[] = { 60, 62, 64, 67, 72, 67, 64, 62 };

    for (int i = 0; i < 8; ++i)
        notes.push_back ({ i * Q, Q * 9 / 10, 1, keys[i], 96 });

    std::vector<MidiSequence::Control> controls;
    controls.push_back ({ 0, MidiSequence::ControlType::controller, 1, 11, 100 });   // expression

    for (int i = 0; i < 32; ++i)   // CC1 swell up and back over two bars
    {
        const auto phase = i < 16 ? i : 31 - i;
        controls.push_back ({ i * Q / 4, MidiSequence::ControlType::controller, 1, 1, 25 + phase * 6 });
    }

    return MidiSequence::create (std::move (notes), std::move (controls));
}
