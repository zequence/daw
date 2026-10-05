#include "../src/ui/Smufl.h"

// The bundled Bravura font draws every curated SMuFL glyph (not an empty box or nothing).
// ORCHESTRAL_DAW_SMUFL_PREVIEW=<file.png> also writes what they look like, for a visual check.
class SmuflTests final : public juce::UnitTest
{
public:
    SmuflTests() : juce::UnitTest ("SMuFL symbols", "UI") {}

    void runTest() override
    {
        beginTest ("Bravura is bundled and draws every curated glyph");
        expect (smufl::bravura() != nullptr);

        const auto& glyphs = smufl::glyphs();
        const int cell = 40, columns = 9;
        juce::Image sheet (juce::Image::ARGB, columns * cell * 4, ((int) glyphs.size() / columns + 1) * cell, true);
        auto sheetContext = std::make_unique<juce::Graphics> (sheet);   // closed before the sheet is saved (drawing lands then)
        auto& sheetGraphics = *sheetContext;
        sheetGraphics.fillAll (juce::Colour (0xff23262b));

        for (size_t i = 0; i < glyphs.size(); ++i)
        {
            juce::Image image (juce::Image::ARGB, 48, 32, true);

            {
                juce::Graphics g (image);
                g.setColour (juce::Colours::white);
                smufl::draw (g, glyphs[i].name, { 4, 0, 40, 32 }, 13.0f);
            }

            int ink = 0;

            for (int y = 0; y < image.getHeight(); ++y)
                for (int x = 0; x < image.getWidth(); ++x)
                    if (image.getPixelAt (x, y).getAlpha() > 64)
                        ++ink;

            expect (ink > 4, juce::String (glyphs[i].name) + " draws nothing");
            expect (smufl::width (glyphs[i].name, 13.0f) > 0);

            const auto col = (int) i % columns, row = (int) i / columns;
            sheetGraphics.setColour (juce::Colours::white);
            smufl::draw (sheetGraphics, glyphs[i].name, { col * cell * 4 + 4, row * cell, cell, cell }, 13.0f);
            sheetGraphics.setFont (juce::FontOptions (9.0f));
            sheetGraphics.setColour (juce::Colours::grey);
            sheetGraphics.drawText (glyphs[i].name, col * cell * 4 + cell, row * cell, cell * 3 - 4, cell, juce::Justification::centredLeft);
        }

        beginTest ("text symbols draw as text; unknown names too");
        expect (smufl::find ("pizz.") == nullptr && smufl::width ("pizz.", 13.0f) > 0);

        sheetContext.reset();

        if (const auto preview = juce::SystemStats::getEnvironmentVariable ("ORCHESTRAL_DAW_SMUFL_PREVIEW", {}); preview.isNotEmpty())
        {
            juce::File file (preview);
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat().writeImageToStream (sheet, out);
        }
    }
};

static SmuflTests smuflTests;
