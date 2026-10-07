#pragma once

#include "../AudioEngine.h"

// Shared vertical metrics for the midi domain (ISSUES.md "Arrange view"):
// the track list rows and the arrangement lanes sit on the SAME Y axis - same
// row heights, same order (folders included), same scroll offset. The sidebar
// and the arrangement both read/write this, so scrolling one scrolls the other.
namespace sidebar
{
    // The zoom (Ctrl+Shift+wheel, + / -) sets the instrument folders' rows; MIDI tracks and audio rows
    // keep one height (midiRowHeight)
    constexpr int minTrackRowHeight = 36, maxTrackRowHeight = 160;
    constexpr int midiRowHeight = 26;
    inline int& trackRowHeightSetting()   { static int height = minTrackRowHeight; return height; }
    inline int trackRowHeight()           { return trackRowHeightSetting(); }

    constexpr int folderRowHeight = 28;
    constexpr int indentPerLevel = 10;

    // The rows' names (folders, MIDI tracks, audio): Segoe UI - lighter and narrower than the default
    // sans, orderly - regular for tracks and audio, semibold for folders (instruments are on tape)
    inline const juce::Colour rowTextColour { 0xffd4d6da };   // the rows' names: a very light grey, not white

    inline juce::Font rowFont (float height, bool folder)
    {
        static const auto hasSegoe = juce::Font::findAllTypefaceNames().contains ("Segoe UI");

        if (! hasSegoe)
            return juce::Font (juce::FontOptions (height, folder ? juce::Font::bold : juce::Font::plain));

        return juce::Font (juce::FontOptions ("Segoe UI", height, juce::Font::plain).withStyle (folder ? "Semibold" : "Regular"));
    }

    inline int heightOf (const AudioEngine::SidebarItem& item)
    {
        return item.instrument != 0 ? trackRowHeight()
             : item.folder != 0     ? folderRowHeight
             : item.channel != 0    ? 2 * midiRowHeight   // audio: its name, and its meter under it
                                    : midiRowHeight;
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
