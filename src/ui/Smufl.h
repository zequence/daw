#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <AppBinaryData.h>
#include <map>

// Articulation symbols. A symbol is either the name of a SMuFL glyph (the Standard Music Font
// Layout: "articStaccatoAbove", "ornamentTrill" ...), drawn with Bravura - the SMuFL reference
// font, bundled under the SIL Open Font License (resources/fonts/Bravura-OFL.txt) - or plain text
// ("pizz.", "c.l.") for techniques that have no glyph.
//
// The glyphs offered are a curated set (names and code points as SMuFL defines them); a glyph
// name not in it is drawn as text.
namespace smufl
{
    struct Glyph
    {
        const char* name;
        juce::juce_wchar codepoint;
        const char* category;
        const char* description;
    };

    inline const std::vector<Glyph>& glyphs()
    {
        static const std::vector<Glyph> list {
            { "articStaccatoAbove",            0xE4A2, "Articulations", "Staccato" },
            { "articStaccatissimoAbove",       0xE4A6, "Articulations", "Staccatissimo" },
            { "articStaccatissimoWedgeAbove",  0xE4A8, "Articulations", "Staccatissimo wedge" },
            { "articTenutoAbove",              0xE4A4, "Articulations", "Tenuto" },
            { "articTenutoStaccatoAbove",      0xE4B2, "Articulations", "Loure (tenuto-staccato, portato)" },
            { "articAccentAbove",              0xE4A0, "Articulations", "Accent" },
            { "articAccentStaccatoAbove",      0xE4B0, "Articulations", "Accent-staccato" },
            { "articMarcatoAbove",             0xE4AC, "Articulations", "Marcato" },
            { "articMarcatoStaccatoAbove",     0xE4AE, "Articulations", "Marcato-staccato" },
            { "articTenutoAccentAbove",        0xE4B4, "Articulations", "Tenuto-accent" },
            { "articStressAbove",              0xE4B6, "Articulations", "Stress" },
            { "articSoftAccentAbove",          0xED40, "Articulations", "Soft accent" },
            { "articLaissezVibrerAbove",       0xE4BA, "Articulations", "Laissez vibrer" },

            { "dynamicPiano",                  0xE520, "Dynamics",      "Piano" },
            { "dynamicMP",                     0xE52C, "Dynamics",      "Mezzo-piano" },
            { "dynamicMF",                     0xE52D, "Dynamics",      "Mezzo-forte" },
            { "dynamicForte",                  0xE522, "Dynamics",      "Forte" },
            { "dynamicFortePiano",             0xE534, "Dynamics",      "Forte-piano" },
            { "dynamicSforzato",               0xE539, "Dynamics",      "Sforzato" },
            { "dynamicSforzando1",             0xE536, "Dynamics",      "Sforzando" },
            { "dynamicCrescendoHairpin",       0xE53E, "Dynamics",      "Crescendo" },
            { "dynamicDiminuendoHairpin",      0xE53F, "Dynamics",      "Diminuendo" },
            { "dynamicMessaDiVoce",            0xE540, "Dynamics",      "Messa di voce (swell)" },

            { "ornamentTrill",                 0xE566, "Ornaments",     "Trill" },
            { "ornamentShortTrill",            0xE56C, "Ornaments",     "Short trill" },
            { "ornamentMordent",               0xE56D, "Ornaments",     "Mordent" },
            { "ornamentTurn",                  0xE567, "Ornaments",     "Turn" },
            { "ornamentTremblement",           0xE56E, "Ornaments",     "Tremblement" },

            { "tremolo1",                      0xE220, "Tremolo",       "Tremolo, 1 stroke" },
            { "tremolo2",                      0xE221, "Tremolo",       "Tremolo, 2 strokes" },
            { "tremolo3",                      0xE222, "Tremolo",       "Tremolo, 3 strokes" },
            { "tremolo4",                      0xE223, "Tremolo",       "Tremolo, 4 strokes" },
            { "buzzRoll",                      0xE22A, "Tremolo",       "Buzz roll" },
            { "unmeasuredTremolo",             0xE22C, "Tremolo",       "Unmeasured tremolo" },

            { "stringsUpBow",                  0xE612, "Strings",       "Up bow" },
            { "stringsDownBow",                0xE610, "Strings",       "Down bow" },
            { "stringsHarmonic",               0xE614, "Strings",       "Harmonic" },
            { "stringsMuteOn",                 0xE616, "Strings",       "Mute on (con sordino)" },
            { "stringsMuteOff",                0xE617, "Strings",       "Mute off" },
            { "stringsBowBehindBridge",        0xE618, "Strings",       "Bow behind bridge (sul ponticello)" },
            { "stringsJeteAbove",              0xE620, "Strings",       "Jete (ricochet)" },
            { "stringsOverpressureDownBow",    0xE61B, "Strings",       "Overpressure" },
            { "pluckedSnapPizzicatoAbove",     0xE631, "Strings",       "Snap pizzicato" },
            { "pluckedLeftHandPizzicato",      0xE633, "Strings",       "Left-hand pizzicato" },
            { "stringsVibratoPulse",           0xE623, "Strings",       "Vibrato pulse" },

            { "fermataAbove",                  0xE4C0, "Other",         "Fermata" },
            { "breathMarkComma",               0xE4CE, "Other",         "Breath mark" },
            { "caesura",                       0xE4D1, "Other",         "Caesura" },
            { "repeat1Bar",                    0xE500, "Other",         "Repeat (bar)" },
            { "glissandoUp",                   0xE585, "Other",         "Glissando (portamento)" },
            { "noteWhole",                     0xE1D2, "Other",         "Whole note (long)" },
            { "noteHalfUp",                    0xE1D3, "Other",         "Half note" },
            { "noteQuarterUp",                 0xE1D5, "Other",         "Quarter note" },
        };

        return list;
    }

    inline const Glyph* find (const juce::String& name)
    {
        for (auto& glyph : glyphs())
            if (name == glyph.name)
                return &glyph;

        return nullptr;
    }

    inline juce::Typeface::Ptr bravura()
    {
        static auto typeface = juce::Typeface::createSystemTypefaceFor (AppBinaryData::Bravura_otf, AppBinaryData::Bravura_otfSize);
        return typeface;
    }

    // A glyph's outline at a reference size (SMuFL: 1 em = 4 staff spaces), cached
    inline const juce::Path& outline (const Glyph& glyph)
    {
        static std::map<juce::juce_wchar, juce::Path> cache;
        auto [it, inserted] = cache.try_emplace (glyph.codepoint);

        if (inserted)
        {
            juce::GlyphArrangement arrangement;
            arrangement.addLineOfText (juce::Font (juce::FontOptions (bravura()).withHeight (100.0f)),
                                       juce::String::charToString (glyph.codepoint), 0.0f, 0.0f);
            arrangement.createPath (it->second);
        }

        return it->second;
    }

    // The scale a glyph is drawn at next to text of 'textHeight': fitted into a box about as tall
    // as the text and 2.6 times as wide, but enlarged at most to 1.6 staff spaces per text height,
    // so the small marks (a staccato dot, a tenuto line) are legible without becoming blobs
    inline juce::AffineTransform placement (const juce::Path& path, float textHeight, juce::Point<float> centre)
    {
        const auto bounds = path.getBounds();
        const auto staffSpace = 100.0f / 4.0f;
        auto scale = 1.6f * textHeight / staffSpace;

        if (! bounds.isEmpty())
        {
            // ... and at least half a text height big (a lone staccato dot)
            const auto biggest = juce::jmax (1.0f, juce::jmax (bounds.getHeight(), bounds.getWidth()));
            scale = juce::jmax (scale, textHeight * 0.45f / biggest);
            scale = juce::jmin (scale, textHeight * 1.2f / juce::jmax (1.0f, bounds.getHeight()),
                                textHeight * 2.6f / juce::jmax (1.0f, bounds.getWidth()));
        }

        return juce::AffineTransform::translation (-bounds.getCentreX(), -bounds.getCentreY()).scaled (scale).translated (centre);
    }

    // How wide draw() makes a symbol
    inline int width (const juce::String& symbol, float textHeight)
    {
        if (symbol.isEmpty())
            return 0;

        if (const auto* glyph = find (symbol))
        {
            const auto& path = outline (*glyph);
            return (int) std::ceil (path.getBoundsTransformed (placement (path, textHeight, {})).getWidth()) + 2;
        }

        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (textHeight)), symbol));
    }

    // Draws one symbol (a glyph, or text) in 'area', vertically centred; left-aligned or centred
    // as 'justification' says. Uses the current colour. Returns the width it took.
    inline int draw (juce::Graphics& g, const juce::String& symbol, juce::Rectangle<int> area, float textHeight,
                     juce::Justification justification = juce::Justification::centredLeft)
    {
        if (symbol.isEmpty())
            return 0;

        if (const auto* glyph = find (symbol))
        {
            const auto& path = outline (*glyph);
            const auto w = width (symbol, textHeight);
            const auto centreX = justification.testFlags (juce::Justification::horizontallyCentred)
                                   ? (float) area.getCentreX() : (float) area.getX() + w * 0.5f;
            g.fillPath (path, placement (path, textHeight, { centreX, (float) area.getCentreY() }));
            return w;
        }

        const auto font = juce::Font (juce::FontOptions (textHeight));
        g.setFont (font);
        g.drawText (symbol, area, justification, false);
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, symbol));
    }
}
