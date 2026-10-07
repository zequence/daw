#pragma once

#include "../AudioEngine.h"

// Shared vertical metrics for the midi domain (ISSUES.md "Arrange view"):
// the track list rows and the arrangement lanes sit on the SAME Y axis - same
// row heights, same order (folders included), same scroll offset. The sidebar
// and the arrangement both read/write this, so scrolling one scrolls the other.
namespace sidebar
{
    // A track row's height: zoomable (Ctrl+Shift+wheel, + / -), from one line (the smallest) up
    constexpr int minTrackRowHeight = 26, maxTrackRowHeight = 120;
    inline int& trackRowHeightSetting()   { static int height = minTrackRowHeight; return height; }
    inline int trackRowHeight()           { return trackRowHeightSetting(); }

    constexpr int folderRowHeight = 28;
    constexpr int instrumentRowHeight = 36;   // room for its name on tape
    constexpr int indentPerLevel = 10;

    // The rows' names (folders, MIDI tracks, audio): Segoe UI - lighter and narrower than the default
    // sans, orderly - regular for tracks and audio, semibold for folders (instruments are on tape)
    inline juce::Font rowFont (float height, bool folder)
    {
        static const auto hasSegoe = juce::Font::findAllTypefaceNames().contains ("Segoe UI");

        if (! hasSegoe)
            return juce::Font (juce::FontOptions (height, folder ? juce::Font::bold : juce::Font::plain));

        return juce::Font (juce::FontOptions ("Segoe UI", height, juce::Font::plain).withStyle (folder ? "Semibold" : "Regular"));
    }

    inline int heightOf (const AudioEngine::SidebarItem& item)
    {
        return item.instrument != 0 ? instrumentRowHeight : (item.folder != 0 ? folderRowHeight : trackRowHeight());
    }

    // One shared scroll offset; views poll 'revision' from their timers.
    struct VerticalScroll
    {
        int y = 0;
        int revision = 0;

        void set (int newY)
        {
            newY = juce::jmax (0, newY);

            if (newY != y)
            {
                y = newY;
                ++revision;
            }
        }
    };
}
