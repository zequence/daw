#include "../src/ui/WindowPlacement.h"
#include <juce_events/juce_events.h>

class WindowPlacementTests final : public juce::UnitTest
{
public:
    WindowPlacementTests() : UnitTest ("Window placement") {}

    void runTest() override
    {
        const juce::Rectangle<int> screen (0, 0, 1920, 1040);                 // a display minus the taskbar
        const juce::BorderSize<int> frame (31, 8, 8, 8);                      // title bar on top, thin borders

        beginTest ("a window that fits is left where it is");
        {
            expect (windowPlacement::keepOnScreen (screen, { 300, 200, 800, 600 }, frame) == juce::Point<int> (300, 200));
        }

        beginTest ("a title bar above the top of the screen is pulled down so it can be grabbed");
        {
            // The client area starts at y = -20: the title bar (31 px above it) would be 51 px off the top
            const auto p = windowPlacement::keepOnScreen (screen, { 300, -20, 800, 600 }, frame);
            expectEquals (p.y, 31);   // the frame's top edge is on the screen's top edge
            expectEquals (p.x, 300);
        }

        beginTest ("a window past the bottom or the sides is pulled in");
        {
            auto p = windowPlacement::keepOnScreen (screen, { 300, 900, 800, 600 }, frame);   // bottom at 1500
            expectEquals (p.y, screen.getBottom() - 600 - frame.getBottom());

            p = windowPlacement::keepOnScreen (screen, { -100, 200, 800, 600 }, frame);       // left of the screen
            expectEquals (p.x, frame.getLeft());

            p = windowPlacement::keepOnScreen (screen, { 1800, 200, 800, 600 }, frame);       // right of the screen
            expectEquals (p.x, screen.getRight() - 800 - frame.getRight());
        }

        beginTest ("a window taller than the screen keeps its title bar visible (the far edge is cut off)");
        {
            // A big editor, 1300 px tall on a 1040 px work area, centred by the old code
            const auto p = windowPlacement::keepOnScreen (screen, { 400, -150, 1100, 1300 }, frame);
            expectEquals (p.y, frame.getTop());   // the title bar sits at the very top: it can be moved and closed
        }

        beginTest ("a second display, and a taskbar at the top");
        {
            const juce::Rectangle<int> second (1920, 0, 2560, 1400);
            auto p = windowPlacement::keepOnScreen (second, { 1900, 100, 800, 600 }, frame);   // straddles the border
            expectEquals (p.x, second.getX() + frame.getLeft());

            const juce::Rectangle<int> belowTaskbar (0, 48, 1920, 992);                        // taskbar along the top
            p = windowPlacement::keepOnScreen (belowTaskbar, { 300, 10, 800, 600 }, frame);
            expectEquals (p.y, 48 + frame.getTop());
        }
    }
};

static WindowPlacementTests windowPlacementTests;
