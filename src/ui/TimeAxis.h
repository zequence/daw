#pragma once

#include "../model/TempoMap.h"

// The one shared time axis (MILESTONES.md): the timeline bar owns it, the
// arrangement and the piano roll align to it. X coordinates are component-local;
// every timeline component sits at the content area's left edge and reserves the
// same left gutter (the piano roll's keys column width), so tick <-> x mapping is
// identical everywhere.
struct TimeAxis
{
    static constexpr int gutter = 56;

    double ticksPerPixel = 32000.0;          // ~30 px per quarter note initially
    juce::int64 scrollTick = 0;
    int revision = 0;                        // bumped on every change; views repaint when it moves
    bool snap = true;                        // snap to grid (the transport's Snap button); off = free positions

    // The grid follows the zoom (no setting): zoomed out it is bars; zooming in, half notes,
    // quarters, eighths... down to 1/256 - the finest whose lines are far enough apart. A 16th or
    // shorter needs 24 px; longer values need more, rising evenly to three times that for a half
    // note (an eighth 40, a quarter 56, a half 72), so the long steps hold on longer.
    static constexpr double minGridPixels = 24.0;

    static double gridPixelsNeeded (juce::int64 unit)
    {
        const auto sixteenth = (double) (Ticks::perQuarterNote / 4);

        if ((double) unit <= sixteenth)
            return minGridPixels;

        return juce::jmin (3.0 * minGridPixels, minGridPixels + 16.0 * std::log2 ((double) unit / sixteenth));
    }

    juce::int64 gridStep (const TempoMap& map, juce::int64 atTick) const
    {
        auto step = map.getTicksPerBar (juce::jmax ((juce::int64) 0, atTick));   // the coarsest: a bar, whatever the meter

        for (auto unit = Ticks::perQuarterNote * 2; unit >= Ticks::perQuarterNote / 64; unit /= 2)
        {
            if ((double) unit / ticksPerPixel < gridPixelsNeeded (unit))
                break;

            step = juce::jmin (step, unit);
        }

        return step;
    }

    // The nearest grid line (counted from the bar's start); the tick itself with snap off
    juce::int64 snapToGrid (const TempoMap& map, juce::int64 tick) const
    {
        tick = juce::jmax ((juce::int64) 0, tick);

        if (! snap)
            return tick;

        const auto bar = map.getBarStart (tick);
        const auto barLength = map.getTicksPerBar (tick);
        const auto step = gridStep (map, tick);
        const auto lines = (tick - bar + step / 2) / step;
        const auto nextBar = map.getBarStart (bar + barLength);
        const auto line = juce::jmin (nextBar, bar + lines * step);

        // A step that doesn't divide the bar evenly (a half note in 3/4): the next bar line may be nearer
        return nextBar - tick < std::abs (tick - line) ? nextBar : line;
    }

    juce::int64 xToTick (int x) const noexcept
    {
        return scrollTick + (juce::int64) juce::jmax (0.0, (x - gutter) * ticksPerPixel);
    }

    int tickToX (juce::int64 tick) const noexcept
    {
        return gutter + (int) ((double) (tick - scrollTick) / ticksPerPixel);
    }

    void zoomAround (int x, double factor)
    {
        const auto anchor = xToTick (x);
        ticksPerPixel = juce::jlimit (400.0, 400000.0, ticksPerPixel * factor);
        scrollTick = juce::jmax ((juce::int64) 0, anchor - (juce::int64) ((x - gutter) * ticksPerPixel));
        ++revision;
    }

    void scrollByPixels (double deltaPixels)
    {
        scrollTick = juce::jmax ((juce::int64) 0, scrollTick + (juce::int64) (deltaPixels * ticksPerPixel));
        ++revision;
    }

    // Standard wheel handling shared by every timeline component.
    // Returns true when the event was consumed (zoom or horizontal scroll).
    bool handleWheel (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
    {
        if (event.mods.isCtrlDown())
        {
            zoomAround (event.x, wheel.deltaY > 0 ? 0.8 : 1.25);
            return true;
        }

        if (event.mods.isShiftDown())
        {
            scrollByPixels (-wheel.deltaY * 320.0);
            return true;
        }

        return false;
    }
};
