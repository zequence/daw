#pragma once

#include "../AudioEngine.h"
#include <AppBinaryData.h>

// Shared vertical metrics for the midi domain (ISSUES.md "Arrange view"):
// the track list rows and the arrangement lanes sit on the SAME Y axis - same
// row heights, same order (folders included), same scroll offset. The sidebar
// and the arrangement both read/write this, so scrolling one scrolls the other.
namespace sidebar
{
    // The zoom (Ctrl+Shift+wheel, + / -) sets every row's height: folders, instruments, MIDI, audio
    constexpr int midiRowHeight = 26;
    constexpr int minTrackRowHeight = midiRowHeight, maxTrackRowHeight = 160;
    inline int& trackRowHeightSetting()   { static int height = minTrackRowHeight; return height; }
    inline int trackRowHeight()           { return trackRowHeightSetting(); }

    constexpr int indentPerLevel = 10;

    // The rows' names (folders, MIDI tracks, audio): Segoe UI - lighter and narrower than the default
    // sans, orderly - regular for tracks and audio, semibold for folders (instruments are on tape)
    // A folder's name: typed on a worn typewriter, like the label on an archive box (Special Elite, embedded)
    inline juce::Font folderFont (float height)
    {
        static const auto typeface = juce::Typeface::createSystemTypefaceFor (AppBinaryData::SpecialEliteRegular_ttf,
                                                                              AppBinaryData::SpecialEliteRegular_ttfSize);
        return juce::Font (juce::FontOptions (typeface).withHeight (height));
    }

    // How far to move a label down so its letters (cap height) sit in the middle of the row - a label
    // centres the font's whole box, and some fonts (Special Elite) carry more above the letters than below
    inline int visualCentreOffset (const juce::Font& font)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (font, "H", 0.0f, 0.0f);   // baseline at 0: the H spans -capHeight..0
        const auto capHeight = -glyphs.getBoundingBox (0, -1, true).getY();
        const auto visualCentreFromTop = font.getAscent() - capHeight * 0.5f;
        return juce::roundToInt (font.getHeight() * 0.5f - visualCentreFromTop);
    }

    inline const juce::Colour rowTextColour { 0xffd4d6da };   // the rows' names: a very light grey, not white

    inline juce::Font rowFont (float height, bool folder)
    {
        static const auto hasSegoe = juce::Font::findAllTypefaceNames().contains ("Segoe UI");

        if (! hasSegoe)
            return juce::Font (juce::FontOptions (height, folder ? juce::Font::bold : juce::Font::plain));

        return juce::Font (juce::FontOptions ("Segoe UI", height, juce::Font::plain).withStyle (folder ? "Semibold" : "Regular"));
    }

    inline int heightOf (const AudioEngine::SidebarItem&)   // every row alike (folders, instruments, MIDI, audio): the zoom
    {
        return trackRowHeight();
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
