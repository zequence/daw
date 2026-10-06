#pragma once

#include "Theme.h"

// The X at the right end of a view's top bar: closes a view that sits over the
// arrangement (MIDI editor, instruments, history...). A drawn cross, brighter on hover.
class CloseButton final : public juce::Button
{
public:
    CloseButton() : juce::Button ("Close")
    {
        setTooltip ("Close (Esc)");
        setWantsKeyboardFocus (false);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (0.5f);

        if (highlighted || down)
        {
            g.setColour (theme::colour (theme::Token::buttonBg).brighter (down ? 0.1f : 0.3f));
            g.fillRoundedRectangle (bounds, theme::corner);
        }

        const auto size = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.32f;
        const auto c = bounds.getCentre();
        juce::Path cross;
        cross.startNewSubPath (c.x - size, c.y - size);
        cross.lineTo (c.x + size, c.y + size);
        cross.startNewSubPath (c.x + size, c.y - size);
        cross.lineTo (c.x - size, c.y + size);

        g.setColour (theme::colour (theme::Token::buttonText).withAlpha (highlighted ? 1.0f : 0.7f));
        g.strokePath (cross, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};
