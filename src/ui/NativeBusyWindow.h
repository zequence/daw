#pragma once

#include <juce_graphics/juce_graphics.h>
#include <memory>

// The animated busy card, drawn by a native window on its OWN thread (Windows).
//
// JUCE draws only on the message thread, and long operations keep that thread
// busy (a VE Pro plugin load alone blocks it ~110 ms; the graph rebuild ~1.8 s),
// so anything JUCE-drawn stutters or freezes. This window has its own thread and
// message loop and keeps animating at 60 fps regardless. It is deliberately NOT
// owned by the app window: cross-thread ownership in Windows attaches the two
// threads' input queues, which would let a blocked UI thread freeze it too.
//
// Thread-safe: show/update/hide may be called from the message thread at any
// time; they only store state and post messages, never wait.
class NativeBusyWindow
{
public:
    NativeBusyWindow();
    ~NativeBusyWindow();

    // Screen rectangle (physical pixels) to centre the card on, and the UI scale.
    void show (juce::Rectangle<int> screenArea, float scale);
    void update (const juce::String& title, const juce::String& detail, double progress);
    void hide();

    bool isAvailable() const noexcept;   // false on other platforms or if creation failed

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (NativeBusyWindow)
};
