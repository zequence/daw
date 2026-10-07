#include "../src/ui/Theme.h"

// Registry and pure helpers only: the Manager's save/restore paths write to the
// user's data folder, so they are not exercised here.
class ThemeTests final : public juce::UnitTest
{
public:
    ThemeTests() : UnitTest ("Theme") {}

    void runTest() override
    {
        beginTest ("registry: unique ids, parents come first, numbers within range");
        {
            juce::StringArray seen;

            for (int i = 0; i < theme::tokenCount; ++i)
            {
                const auto& d = theme::defs()[(size_t) i];
                expect (! seen.contains (d.id), juce::String ("duplicate token id ") + d.id);
                seen.add (d.id);

                if (d.isNumber)
                    expect (d.number >= d.min && d.number <= d.max, juce::String ("default out of range: ") + d.id);
                else
                    expect (d.parent < i, juce::String ("parent must be listed before ") + d.id);
            }
        }

        beginTest ("hex parsing: #rrggbb, rrggbb, #aarrggbb; junk is refused");
        {
            juce::Colour c;
            expect (theme::parseHex ("#1a2b3c", c) && c == juce::Colour (0xff1a2b3c));
            expect (theme::parseHex ("  1A2B3C ", c) && c == juce::Colour (0xff1a2b3c));
            expect (theme::parseHex ("#801a2b3c", c) && c == juce::Colour (0x801a2b3c));
            expect (! theme::parseHex ("#12345", c));
            expect (! theme::parseHex ("#gg0000", c));
            expect (theme::parseHex (theme::hexOf (juce::Colour (0x80112233), true), c) && c == juce::Colour (0x80112233));
        }

        beginTest ("the built-in theme resolves to the colors the app always had");
        {
            auto& m = theme::Manager::get();
            expect (m.colour (theme::Token::arrangeBg) == juce::Colour (0xff1a1c1f));
            expect (m.colour (theme::Token::arrangeBarline) == juce::Colour (0xff2e3136));
            expect (theme::rowColour (juce::Colour (0xff000000), true, false) == juce::Colour (0xff000000).interpolatedWith (juce::Colours::white, 0.16f));   // selected: brighter
            expect (m.colour (theme::Token::trackInstrumentBg) == m.colour (theme::Token::folderBg));   // follows the folder background

            const auto track = juce::Colour (0xff56b58c);
            const auto normal = theme::regionStyle (track, false);
            expect (normal.fill == track.withMultipliedSaturation (0.45f).withMultipliedBrightness (1.2f).withAlpha (0.6f));
            expect (normal.border == track);

            const auto selected = theme::regionStyle (track, true);
            expect (selected.fill == track.withMultipliedSaturation (0.45f).withMultipliedBrightness (1.2f).withAlpha (0.95f));
            expect (selected.border == track.withMultipliedBrightness (1.6f));
        }
    }
};

static ThemeTests themeTests;
