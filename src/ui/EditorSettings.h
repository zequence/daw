#pragma once

#include <juce_data_structures/juce_data_structures.h>

// Settings > Editor > Midi. The settings page writes them, the MIDI editor reads them.
namespace editorSettings
{
    // When changing the root articulation would drop modifiers that no longer
    // apply: ask first (true, the default) or drop them straight away
    constexpr auto askBeforeDroppingKey = "editorAskBeforeDroppingModifiers";

    // A note with no root articulation behaves as if the map's first root
    // articulation were chosen (implicit; nothing is written to the note). Off by default.
    constexpr auto firstRootIsDefaultKey = "editorFirstRootIsDefault";

    // What colours the notes in the MIDI editor: their velocity (the default) or their
    // articulation's sound slot (the expression map's slot colour)
    constexpr auto noteColoursKey = "editorNoteColours";
    inline bool coloursBySlot (juce::PropertiesFile& settings)   { return settings.getValue (noteColoursKey, "velocity") == "slot"; }

    // The name of MIDI note 60: C3 (VSL, Cubase; the default), C4 or C5
    constexpr auto middleCOctaveKey = "editorMiddleCOctave";

    inline int middleCOctave (juce::PropertiesFile& settings)
    {
        const auto octave = settings.getIntValue (middleCOctaveKey, 3);
        return octave >= 3 && octave <= 5 ? octave : 3;
    }

    inline bool askBeforeDropping (juce::PropertiesFile& settings)    { return settings.getBoolValue (askBeforeDroppingKey, true); }
    inline bool firstRootIsDefault (juce::PropertiesFile& settings)   { return settings.getBoolValue (firstRootIsDefaultKey, false); }
}
