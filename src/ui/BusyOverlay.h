#pragma once

#include "../AudioEngine.h"

// The loading overlay: while a long operation runs (VE Pro sync, project load)
// it dims the window, blocks clicks and says what's going on, with a progress
// bar when the operation knows its progress.
//
// Animation: JUCE draws only on the message thread, which the announced work
// keeps busy in bursts. So the overlay (a) animates from its own timer whenever
// the thread is free - the work yields between steps - with time-based motion,
// so it doesn't stutter or crawl, and (b) paints IMMEDIATELY on every status
// change, so each step's text is visible even right before a blocking stretch.
class BusyOverlay final : public juce::Component,
                          private juce::Timer
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
            {
                toFront (false);
                startTimerHz (40);
                frames = 0;
                longestGapMs = 0.0;
                shownAt = lastFrameAt = juce::Time::getMillisecondCounterHiRes();
            }
            else
            {
                stopTimer();

                // Smoothness report: how often we managed to draw, and where it froze
                const auto nowMs = juce::Time::getMillisecondCounterHiRes();
                noteGap (nowMs - lastFrameAt, lastDetail + " (until done)");

                const auto seconds = (nowMs - shownAt) * 0.001;
                juce::Logger::writeToLog ("Busy overlay: " + juce::String (frames) + " frames in "
                                          + juce::String (seconds, 1) + " s ("
                                          + juce::String (frames / juce::jmax (0.001, seconds), 0)
                                          + " fps), longest freeze " + juce::String (juce::roundToInt (longestGapMs)) + " ms"
                                          + (freezes.isEmpty() ? juce::String()
                                                               : "; freezes > 100 ms: " + freezes.joinIntoString (" | ")));
                freezes.clear();
            }
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
        const auto nowMs = juce::Time::getMillisecondCounterHiRes();
        noteGap (nowMs - lastFrameAt, lastDetail);
        lastFrameAt = nowMs;
        lastDetail = status.detail;
        ++frames;

        const auto seconds = nowMs * 0.001;

        g.fillAll (juce::Colours::black.withAlpha (0.55f));

        auto card = getLocalBounds().withSizeKeepingCentre (juce::jmin (480, getWidth() - 32), 130);

        g.setColour (juce::Colour (0xff23262b));
        g.fillRoundedRectangle (card.toFloat(), 8.0f);
        g.setColour (juce::Colour (0xff43464d));
        g.drawRoundedRectangle (card.toFloat(), 8.0f, 1.0f);

        auto area = card.reduced (22, 18);

        // Spinner: a rotating arc next to the title
        auto titleRow = area.removeFromTop (26);
        const auto spinner = titleRow.removeFromRight (22).toFloat().withSizeKeepingCentre (18.0f, 18.0f);
        const auto angle = (float) std::fmod (seconds * juce::MathConstants<double>::twoPi * 1.2,
                                              juce::MathConstants<double>::twoPi);
        juce::Path arc;
        arc.addCentredArc (spinner.getCentreX(), spinner.getCentreY(), 8.0f, 8.0f, angle,
                           0.0f, juce::MathConstants<float>::pi * 1.4f, true);
        g.setColour (juce::Colours::steelblue);
        g.strokePath (arc, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        g.drawText (status.title, titleRow, juce::Justification::centredLeft);

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
            const auto filled = bar.withWidth (bar.getWidth() * (float) juce::jlimit (0.0, 1.0, status.progress));
            g.fillRoundedRectangle (filled, 4.0f);

            // A soft shimmer travelling along the filled part shows we're alive
            const auto shimmerX = filled.getX() + filled.getWidth() * (float) std::fmod (seconds * 0.8, 1.0);
            g.setColour (juce::Colours::white.withAlpha (0.25f));
            g.fillRect (juce::Rectangle<float> (shimmerX - 12.0f, bar.getY(), 24.0f, bar.getHeight())
                            .getIntersection (filled));
        }
        else
        {
            // Unknown progress: a segment sweeping back and forth (time-based)
            const auto phase = (float) (0.5 - 0.5 * std::cos (seconds * juce::MathConstants<double>::pi));
            const auto width = bar.getWidth() * 0.25f;
            g.fillRoundedRectangle (bar.withWidth (width).withX (bar.getX() + (bar.getWidth() - width) * phase), 4.0f);
        }
    }

private:
    void timerCallback() override
    {
        repaint();

        // Windows only sends WM_PAINT when the message queue is empty, which a
        // chain of work steps rarely leaves it - so paint directly.
        if (auto* peer = getPeer())
            peer->performAnyPendingRepaintsNow();
    }

    AudioEngine::BusyStatus& status;

    void noteGap (double gapMs, const juce::String& during)
    {
        longestGapMs = juce::jmax (longestGapMs, gapMs);

        if (gapMs > 100.0 && freezes.size() < 30)
            freezes.add (juce::String (juce::roundToInt (gapMs)) + " ms in '" + during + "'");
    }

    // Smoothness measurement (logged when the overlay closes)
    int frames = 0;
    double shownAt = 0.0, lastFrameAt = 0.0, longestGapMs = 0.0;
    juce::String lastDetail;
    juce::StringArray freezes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BusyOverlay)
};
