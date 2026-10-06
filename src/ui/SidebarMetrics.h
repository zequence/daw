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

    constexpr int folderRowHeight = 28;      // about half a channel row
    constexpr int indentPerLevel = 10;

    inline int heightOf (const AudioEngine::SidebarItem& item)
    {
        return item.folder != 0 ? folderRowHeight : trackRowHeight();
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
