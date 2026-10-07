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

    // A row's kind, as a small line symbol at its left (after Cubase): a folder, an instrument (a
    // keyboard), a MIDI track (the 5-pin MIDI plug), an audio track (a waveform)
    enum class TrackKind { folder, instrument, midi, audio, bus };
    constexpr int iconWidth = 14;

    inline void drawTrackIcon (juce::Graphics& g, juce::Rectangle<float> box, TrackKind kind, juce::Colour colour)
    {
        g.setColour (colour);
        const auto c = box.getCentre();

        if (kind == TrackKind::folder)   // a folder: its tab, its body
        {
            juce::Path folder;
            const auto body = juce::Rectangle<float> (13.0f, 9.5f).withCentre (c.translated (0.0f, 0.75f));
            folder.startNewSubPath (body.getX(), body.getY() - 1.5f);
            folder.lineTo (body.getX() + 4.5f, body.getY() - 1.5f);
            folder.lineTo (body.getX() + 6.0f, body.getY());
            folder.lineTo (body.getRight(), body.getY());
            folder.lineTo (body.getRight(), body.getBottom());
            folder.lineTo (body.getX(), body.getBottom());
            folder.closeSubPath();
            g.strokePath (folder.createPathWithRoundedCorners (1.2f), juce::PathStrokeType (1.1f));
        }
        else if (kind == TrackKind::instrument)   // a keyboard: three white keys, two black ones between them
        {
            const auto keys = juce::Rectangle<float> (13.0f, 9.0f).withCentre (c);
            g.drawRoundedRectangle (keys, 1.2f, 1.1f);

            for (auto i : { 1, 2 })
                g.fillRect (keys.getX() + keys.getWidth() * (float) i / 3.0f - 0.5f, keys.getY() + 4.5f, 1.0f, keys.getHeight() - 4.5f);

            for (auto i : { 1, 2 })
                g.fillRect (keys.getX() + keys.getWidth() * (float) i / 3.0f - 1.5f, keys.getY(), 3.0f, 5.0f);
        }
        else if (kind == TrackKind::midi)   // the MIDI plug: a ring, five pins in an arc, the key at the top
        {
            const auto radius = 5.5f;
            g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c), 1.1f);
            g.fillRect (c.x - 1.0f, c.y - radius - 0.5f, 2.0f, 2.0f);

            for (auto degrees : { 180.0f, 135.0f, 90.0f, 45.0f, 0.0f })   // the lower half's arc
            {
                const auto a = juce::degreesToRadians (degrees);
                const auto pin = c + juce::Point<float> (std::cos (a), std::sin (a)) * (radius * 0.55f);
                g.fillEllipse (juce::Rectangle<float> (1.7f, 1.7f).withCentre (pin));
            }
        }
        else if (kind == TrackKind::bus)   // a bus: three lines gathered into one
        {
            juce::Path bus;

            for (auto dy : { -4.0f, 0.0f, 4.0f })
            {
                bus.startNewSubPath (c.x - 6.0f, c.y + dy);
                bus.quadraticTo (c.x - 1.5f, c.y + dy * 0.5f, c.x + 1.0f, c.y);
            }

            bus.startNewSubPath (c.x + 1.0f, c.y);
            bus.lineTo (c.x + 6.0f, c.y);
            g.strokePath (bus, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else   // audio: a short waveform
        {
            juce::Path wave;
            const float heights[] { 0.15f, 0.55f, 1.0f, 0.4f, 0.8f, 0.3f, 0.6f, 0.15f };
            const auto step = 12.0f / 7.0f, left = c.x - 6.0f;

            for (int i = 0; i < 8; ++i)
            {
                const auto x = left + (float) i * step, h = heights[i] * 5.0f;
                wave.startNewSubPath (x, c.y - h);
                wave.lineTo (x, c.y + h);
            }

            g.strokePath (wave, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }


    // The rows' type (Inter, embedded): MIDI and audio names in Regular; folders in SemiBold capitals,
    // spaced, in their own colour - headings in the same family (the instrument's tape is the one
    // decorative touch)
    inline juce::Font interFont (float height, bool semiBold)
    {
        static const auto regular = juce::Typeface::createSystemTypefaceFor (AppBinaryData::InterRegular_ttf, AppBinaryData::InterRegular_ttfSize);
        static const auto semi = juce::Typeface::createSystemTypefaceFor (AppBinaryData::InterSemiBold_ttf, AppBinaryData::InterSemiBold_ttfSize);
        return juce::Font (juce::FontOptions (semiBold ? semi : regular).withHeight (height));
    }

    inline juce::Font trackNameFont()    { return interFont (13.0f, false); }
    inline juce::Font folderNameFont()   { return interFont (12.0f, true).withExtraKerningFactor (0.08f); }

    inline const juce::Colour rowTextColour { 0xffd4d6da };   // the rows' names: a very light grey, not white

    inline juce::Font rowFont (float height, bool folder)
    {
        static const auto hasSegoe = juce::Font::findAllTypefaceNames().contains ("Segoe UI");

        if (! hasSegoe)
            return juce::Font (juce::FontOptions (height, folder ? juce::Font::bold : juce::Font::plain));

        return juce::Font (juce::FontOptions ("Segoe UI", height, juce::Font::plain).withStyle (folder ? "Semibold" : "Regular"));
    }

    // Every row follows the zoom; the rows with a name tag (audio, buses, grouped folders and instruments) are taller
    constexpr int instrumentExtraHeight = 10;

    inline int heightOf (const AudioEngine::SidebarItem& item)
    {
        return trackRowHeight() + (item.tagged ? instrumentExtraHeight : 0);   // a name tag: a taller row
    }

    // Automation mode (F2): only what can be automated shows - the MIDI tracks step out of the
    // sidebar and the arrangement (folders, instruments and their audio stay); any other view puts
    // them back
    inline bool& automationModeSetting()   { static bool on = false; return on; }

    // The rows the sidebar and the arrangement show (the same list: one Y axis)
    inline std::vector<AudioEngine::SidebarItem> visibleItems (const AudioEngine& engine)
    {
        auto items = engine.getSidebarItems (true, true);

        if (automationModeSetting())
            std::erase_if (items, [] (const AudioEngine::SidebarItem& item) { return item.member != 0; });

        // Audio a group sums: hidden altogether (the group stands for it)
        std::erase_if (items, [&engine] (const AudioEngine::SidebarItem& item)
                       { return item.channel != 0 && engine.isGroupBus (engine.getAudioChannelOutput (item.channel)); });

        // An instrument with one output is that channel: no audio row of its own
        std::erase_if (items, [&engine] (const AudioEngine::SidebarItem& item)
                       {
                           const auto instrument = item.channel != 0 ? engine.getAudioChannelInput (item.channel) : 0;
                           return instrument != 0 && engine.isSingleOutputInstrument (instrument);
                       });

        return items;
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
