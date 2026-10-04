#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Makes a native title bar follow the Windows light/dark setting (Settings >
// Personalization > Colors), including changes made while the app runs.
// Owned by a DocumentWindow with a native title bar; call apply() once the
// window is visible (the native window must exist). No-op on other platforms.
class SystemTitleBar final : private juce::DarkModeSettingListener
{
public:
    explicit SystemTitleBar (juce::Component& windowToStyle);
    ~SystemTitleBar() override;

    void apply();

private:
    void darkModeSettingChanged() override   { apply(); }

    juce::Component& window;

    JUCE_DECLARE_NON_COPYABLE (SystemTitleBar)
};
