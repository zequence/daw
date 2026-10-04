#include "SystemTitleBar.h"

#if JUCE_WINDOWS

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>

#pragma comment (lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
 #define DWMWA_USE_IMMERSIVE_DARK_MODE 20   // 19 before Windows 10 20H1
#endif

SystemTitleBar::SystemTitleBar (juce::Component& windowToStyle) : window (windowToStyle)
{
    juce::Desktop::getInstance().addDarkModeSettingListener (this);
}

SystemTitleBar::~SystemTitleBar()
{
    juce::Desktop::getInstance().removeDarkModeSettingListener (this);
}

void SystemTitleBar::apply()
{
    auto* peer = window.getPeer();

    if (peer == nullptr)
        return;

    auto* hwnd = (HWND) peer->getNativeHandle();
    const BOOL dark = juce::Desktop::getInstance().isDarkModeActive() ? TRUE : FALSE;

    // The attribute is 20 on current Windows 10/11 and 19 on early Windows 10
    // builds; setting the unsupported one just fails harmlessly.
    if (FAILED (DwmSetWindowAttribute (hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof (dark))))
        DwmSetWindowAttribute (hwnd, 19, &dark, sizeof (dark));

    // Make the frame repaint now (otherwise it may only change on the next activation)
    SetWindowPos (hwnd, nullptr, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

#else

SystemTitleBar::SystemTitleBar (juce::Component& windowToStyle) : window (windowToStyle) {}
SystemTitleBar::~SystemTitleBar() = default;
void SystemTitleBar::apply() {}

#endif
