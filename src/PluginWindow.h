#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "ui/SystemTitleBar.h"

// Floating window showing a plugin's own editor.
// The owner is responsible for deleting it; onClose is called when the user closes it.
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

    std::function<void()> onClose;

private:
    SystemTitleBar titleBar { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};
