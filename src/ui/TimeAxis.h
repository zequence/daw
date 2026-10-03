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
