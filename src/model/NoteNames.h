#pragma once

#include <juce_core/juce_core.h>
#include <atomic>

// Note names. MIDI note 60 (middle C) is called C3 by default, as VSL and Cubase write
// it (the lowest note, 0, is then C-2); Settings > Editor can make it C4 or C5.
namespace noteNames
{
    inline std::atomic<int>& middleCOctave()
    {
        static std::atomic<int> octave { 3 };   // the octave name of MIDI note 60
        return octave;
    }

    inline int octaveOf (int key)      { return key / 12 - (5 - middleCOctave().load()); }

    inline juce::String name (int key)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[((key % 12) + 12) % 12]) + juce::String (octaveOf (key));
    }
}
