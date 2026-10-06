#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "ui/SystemTitleBar.h"
#include "ui/WindowPlacement.h"

// Floating window showing a plugin's own editor.
// The owner is responsible for deleting it; onClose is called when the user closes it.
//
// The title bar must always be reachable. Editors are often taller than the screen
// (VSL Synchron) or resize themselves a moment after they open, and the window was
// only centred once, so its title bar could end up above the top edge with no way
// to move or close it. ensureOnScreen() pulls it back; it runs when the window
// opens, a moment after the plugin resizes it, and whenever it is shown again.
class PluginWindow final : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& plugin, const juce::String& title, bool onTop = true)
        : DocumentWindow (title,
                          juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                          juce::DocumentWindow::minimiseButton | juce::DocumentWindow::closeButton)
    {
        auto* editor = plugin.hasEditor() ? plugin.createEditorAndMakeActive() : nullptr;

        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (plugin);

        setUsingNativeTitleBar (true);
        setAlwaysOnTop (onTop);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        titleBar.apply();
        ensureOnScreen();

        // Plugins often size their editor after it has opened
        for (const auto delayMs : { 300, 1500 })
            juce::Timer::callAfterDelay (delayMs, [safe = juce::Component::SafePointer<PluginWindow> (this)]
                                                  { if (safe != nullptr) safe->ensureOnScreen(); });
    }

    ~PluginWindow() override
    {
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        setVisible (false);

        if (onClose)
            onClose();
    }

    // Moves the window, if needed, so its whole frame (the title bar first of all)
    // is inside the usable area of the display it is mostly on.
    void ensureOnScreen()
    {
        auto* peer = getPeer();

        if (peer == nullptr)
            return;

        const auto frame = peer->getFrameSize();
        const auto client = getBounds();
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* display = displays.getDisplayForRect (frame.addedTo (client));

        if (display == nullptr)
            display = displays.getPrimaryDisplay();

        if (display == nullptr)
            return;

        const auto target = windowPlacement::keepOnScreen (display->userBounds.toNearestInt(), client, frame);

        if (target != client.getPosition())
            setTopLeftPosition (target);
    }

    // The plugin resized the window: check again once it has settled
    void resized() override
    {
        juce::DocumentWindow::resized();

        if (placementCheckPending)
            return;

        placementCheckPending = true;
        juce::Timer::callAfterDelay (150, [safe = juce::Component::SafePointer<PluginWindow> (this)]
                                          {
                                              if (safe == nullptr)
                                                  return;

                                              safe->placementCheckPending = false;
                                              safe->ensureOnScreen();
                                          });
    }

    std::function<void()> onClose;

    // Keys the plugin's editor doesn't use go to the main window (G closes the GUI, Space plays...)
    // even while the plugin window has the focus. Some plugins catch every key themselves.
    std::function<bool (const juce::KeyPress&)> onKey;

    bool keyPressed (const juce::KeyPress& key) override
    {
        return onKey != nullptr && onKey (key);
    }

private:
    SystemTitleBar titleBar { *this };
    bool placementCheckPending = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};
