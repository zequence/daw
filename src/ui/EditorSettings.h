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

    inline bool askBeforeDropping (juce::PropertiesFile& settings)    { return settings.getBoolValue (askBeforeDroppingKey, true); }
    inline bool firstRootIsDefault (juce::PropertiesFile& settings)   { return settings.getBoolValue (firstRootIsDefaultKey, false); }
}
