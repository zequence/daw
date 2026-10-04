#pragma once

#include "../AudioEngine.h"

// The loading overlay (ISSUES/chat request): while a long operation runs (VE Pro
// sync, project load) it dims the window, blocks clicks and says what's going
// on, with a progress bar when the operation knows its progress. Follows the
// engine's BusyStatus; paints IMMEDIATELY on every change, because the message
// thread is often about to block on heavy work.
class BusyOverlay final : public juce::Component
{
public:
    explicit BusyOverlay (AudioEngine::BusyStatus& s) : status (s)
    {
        setInterceptsMouseClicks (true, true);   // swallow clicks while busy
        setAlwaysOnTop (true);
    }

    // Call from BusyStatus::onChanged
    void statusChanged()
    {
        if (status.active != isVisible())
        {
            setVisible (status.active);

            if (status.active)
                toFront (false);
        }

        if (! status.active)
            return;

        repaint();

        // Paint now: queued repaints don't get through while the message thread
        // is busy with the very work we're announcing.
        if (auto* peer = getPeer())
            peer->performAnyPendingRepaintsNow();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.55f));

        auto card = getLocalBounds().withSizeKeepingCentre (juce::jmin (460, getWidth() - 32), 130);

        g.setColour (juce::Colour (0xff23262b));
        g.fillRoundedRectangle (card.toFloat(), 8.0f);
        g.setColour (juce::Colour (0xff43464d));
        g.drawRoundedRectangle (card.toFloat(), 8.0f, 1.0f);

        auto area = card.reduced (22, 18);

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        g.drawText (status.title, area.removeFromTop (26), juce::Justification::centredLeft);

        g.setColour (juce::Colours::lightgrey);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (status.detail, area.removeFromTop (24), juce::Justification::centredLeft, true);

        area.removeFromTop (12);
        auto bar = area.removeFromTop (8).toFloat();

        g.setColour (juce::Colour (0xff15171a));
        g.fillRoundedRectangle (bar, 4.0f);

        g.setColour (juce::Colours::steelblue);

        if (status.progress >= 0.0)
        {
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) juce::jlimit (0.0, 1.0, status.progress)), 4.0f);
        }
        else
        {
            // Unknown progress: a moving segment (advances with each repaint)
            sweep = std::fmod (sweep + 0.07f, 1.0f);
            const auto width = bar.getWidth() * 0.25f;
            g.fillRoundedRectangle (bar.withWidth (width).withX (bar.getX() + (bar.getWidth() - width) * sweep), 4.0f);
        }
    }

private:
    AudioEngine::BusyStatus& status;
    float sweep = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BusyOverlay)
};
