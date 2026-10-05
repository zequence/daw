#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Where a window may sit so it can always be moved and closed.
namespace windowPlacement
{
    // The top-left (of the client area) that puts the whole window frame - above all
    // its title bar - inside 'usableArea' (a display minus the taskbar). A window
    // larger than the area is aligned to the area's top-left: the title bar stays
    // reachable and only the far edge is cut off. A window that already fits is
    // left where it is.
    inline juce::Point<int> keepOnScreen (juce::Rectangle<int> usableArea, juce::Rectangle<int> clientBounds,
                                          juce::BorderSize<int> frame)
    {
        const auto fullWidth = clientBounds.getWidth() + frame.getLeftAndRight();
        const auto fullHeight = clientBounds.getHeight() + frame.getTopAndBottom();

        const auto left = clientBounds.getX() - frame.getLeft();   // the frame's own top-left
        const auto top = clientBounds.getY() - frame.getTop();

        return { juce::jlimit (usableArea.getX(), juce::jmax (usableArea.getX(), usableArea.getRight() - fullWidth), left) + frame.getLeft(),
                 juce::jlimit (usableArea.getY(), juce::jmax (usableArea.getY(), usableArea.getBottom() - fullHeight), top) + frame.getTop() };
    }
}
