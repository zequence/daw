#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// A content view that isn't built yet: a title and a few lines of context.
class PlaceholderView final : public juce::Component
{
public:
    explicit PlaceholderView (const juce::String& titleToUse) : title (titleToUse) {}

    void setDetails (const juce::StringArray& newLines)
    {
        if (lines != newLines)
        {
            lines = newLines;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1d1f23));

        auto area = getLocalBounds().reduced (24);

        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        g.drawText (title, area.removeFromTop (34), juce::Justification::topLeft);

        area.removeFromTop (6);
        g.setColour (juce::Colours::lightgrey);
        g.setFont (juce::FontOptions (14.0f));

        for (auto& line : lines)
            g.drawText (line, area.removeFromTop (22), juce::Justification::topLeft);
    }

private:
    juce::String title;
    juce::StringArray lines;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaceholderView)
};
