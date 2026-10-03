#pragma once

#include "../AudioEngine.h"

// Full-window settings page (replaces the whole UI; close with X or ESC).
// Sections: Audio & MIDI (device selector), Plugins, Tracks, Key commands.
class SettingsView final : public juce::Component
{
public:
    static constexpr auto autoRecordOnSelectKey = "autoRecordOnSelect";
    static constexpr auto pluginWindowsOnTopKey = "pluginWindowsOnTop";
    static constexpr auto mcpEnabledKey = "mcpEnabled";
    static constexpr auto mcpPortKey = "mcpPort";

    explicit SettingsView (AudioEngine& e) : engine (e)
    {
        titleLabel.setText ("Settings", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        addAndMakeVisible (titleLabel);

        closeButton.setWantsKeyboardFocus (false);
        closeButton.setTooltip ("Close (Esc)");
        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeButton);

        viewport.setViewedComponent (&page, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        // --- Audio & MIDI ---
        deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent> (
            engine.getDeviceManager(), 0, 0, 2, 64, true, false, true, false);
        page.addAndMakeVisible (*deviceSelector);

        // --- Plugins ---
        pluginList = std::make_unique<juce::PluginListComponent> (
            engine.getFormatManager(), engine.getKnownPlugins(), engine.getDeadMansPedalFile(), nullptr, true);
        page.addAndMakeVisible (*pluginList);

        scanButton.onClick = [this] { if (onStartScan) onStartScan ({}); };
        retryButton.onClick = [this] { if (onStartScan) onStartScan ({ "--retry-failed" }); };
        rescanButton.onClick = [this] { if (onStartScan) onStartScan ({ "--rescan-all" }); };

        auto& settings = engine.getSettingsFile();

        onTopToggle.setToggleState (settings.getBoolValue (pluginWindowsOnTopKey, true), juce::dontSendNotification);
        onTopToggle.onClick = [this]
        {
            engine.getSettingsFile().setValue (pluginWindowsOnTopKey, onTopToggle.getToggleState());
            engine.getSettingsFile().saveIfNeeded();

            if (onPluginOnTopChanged)
                onPluginOnTopChanged (onTopToggle.getToggleState());
        };

        // --- Tracks ---
        autoRecordToggle.setToggleState (settings.getBoolValue (autoRecordOnSelectKey, true), juce::dontSendNotification);
        autoRecordToggle.onClick = [this]
        {
            engine.getSettingsFile().setValue (autoRecordOnSelectKey, autoRecordToggle.getToggleState());
            engine.getSettingsFile().saveIfNeeded();
        };

        // --- Agents (MCP) ---
        mcpToggle.setToggleState (settings.getBoolValue (mcpEnabledKey, false), juce::dontSendNotification);
        mcpToggle.onClick = [this]
        {
            engine.getSettingsFile().setValue (mcpEnabledKey, mcpToggle.getToggleState());
            engine.getSettingsFile().saveIfNeeded();

            if (onMcpToggled)
                onMcpToggled (mcpToggle.getToggleState());
        };

        mcpStatus.setColour (juce::Label::textColourId, juce::Colours::grey);
        mcpStatus.setFont (juce::FontOptions (12.0f));
        page.addAndMakeVisible (mcpStatus);

        mcpRegisterHint.setReadOnly (true);
        mcpRegisterHint.setMultiLine (false);
        mcpRegisterHint.setScrollbarsShown (false);
        mcpRegisterHint.setCaretVisible (false);
        mcpRegisterHint.setText ("claude mcp add --transport http orchestral-daw http://127.0.0.1:"
                                 + juce::String (settings.getIntValue (mcpPortKey, 53218)) + "/mcp");
        page.addAndMakeVisible (mcpRegisterHint);

        for (auto* c : std::initializer_list<juce::Component*> { &scanButton, &retryButton, &rescanButton,
                                                                 &onTopToggle, &autoRecordToggle, &mcpToggle })
        {
            c->setWantsKeyboardFocus (false);
            page.addAndMakeVisible (c);
        }
    }

    std::function<void()> onClose;
    std::function<void (juce::StringArray)> onStartScan;
    std::function<void (bool)> onPluginOnTopChanged;
    std::function<void (bool)> onMcpToggled;

    void setMcpStatus (const juce::String& status)
    {
        mcpStatus.setText (status, juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 10);

        auto header = area.removeFromTop (36);
        closeButton.setBounds (header.removeFromRight (30).reduced (0, 3));
        titleLabel.setBounds (header);

        area.removeFromTop (4);
        viewport.setBounds (area);

        // Lay out the page content
        const auto width = juce::jmax (300, viewport.getMaximumVisibleWidth() - 4);
        int y = 0;

        sectionBounds.clear();

        // Audio & MIDI
        sectionBounds.push_back ({ "Audio & MIDI", { 0, y, width, sectionHeaderHeight } });
        y += sectionHeaderHeight;
        deviceSelector->setBounds (12, y, juce::jmin (width - 24, 560), 420);
        y += 430;

        // Plugins
        sectionBounds.push_back ({ "Plugins", { 0, y, width, sectionHeaderHeight } });
        y += sectionHeaderHeight;
        scanButton.setBounds (12, y, 150, 26);
        retryButton.setBounds (168, y, 150, 26);
        rescanButton.setBounds (324, y, 150, 26);
        y += 34;
        onTopToggle.setBounds (12, y, 320, 24);
        y += 30;
        pluginList->setBounds (12, y, width - 24, 300);
        y += 310;

        // Tracks
        sectionBounds.push_back ({ "Tracks", { 0, y, width, sectionHeaderHeight } });
        y += sectionHeaderHeight;
        autoRecordToggle.setBounds (12, y, 320, 24);
        y += 32;

        // Agents (MCP)
        sectionBounds.push_back ({ "Agents (MCP)", { 0, y, width, sectionHeaderHeight } });
        y += sectionHeaderHeight;
        mcpToggle.setBounds (12, y, 420, 24);
        y += 28;
        mcpStatus.setBounds (12, y, width - 24, 18);
        y += 22;
        mcpRegisterHint.setBounds (12, y, juce::jmin (620, width - 24), 24);
        y += 32;

        // Key commands
        sectionBounds.push_back ({ "Key commands", { 0, y, width, sectionHeaderHeight } });
        y += sectionHeaderHeight;
        keyCommandsBounds = { 12, y, width - 24, 90 };
        y += 96;

        page.setSize (width, y);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1d1f23));
    }

private:
    static constexpr int sectionHeaderHeight = 40;

    struct Page final : juce::Component
    {
        explicit Page (SettingsView& ownerToUse) : owner (ownerToUse) {}

        void paint (juce::Graphics& g) override
        {
            for (auto& [name, bounds] : owner.sectionBounds)
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
                g.drawText (name, bounds.withTrimmedTop (10), juce::Justification::bottomLeft);

                g.setColour (juce::Colour (0xff3a3d44));
                g.drawHorizontalLine (bounds.getBottom() - 1, (float) bounds.getX(), (float) bounds.getRight());
            }

            g.setColour (juce::Colours::lightgrey);
            g.setFont (juce::FontOptions (13.0f));
            g.drawFittedText ("Space: start/stop playback\n"
                              "Home: back to the beginning\n"
                              "F12: performance monitor\n"
                              "Esc: close this page / go back",
                              owner.keyCommandsBounds, juce::Justification::topLeft, 6);
        }

        SettingsView& owner;
    };

    AudioEngine& engine;

    juce::Label titleLabel;
    juce::TextButton closeButton { "X" };
    juce::Viewport viewport;
    Page page { *this };

    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    std::unique_ptr<juce::PluginListComponent> pluginList;
    juce::TextButton scanButton { "Scan for new plugins" }, retryButton { "Retry failed plugins" },
                     rescanButton { "Rescan everything" };
    juce::ToggleButton onTopToggle { "Plugin windows stay on top" };
    juce::ToggleButton autoRecordToggle { "Arm track on select (auto-record)" };
    juce::ToggleButton mcpToggle { "Run the MCP server for AI agents (starts with the app)" };
    juce::Label mcpStatus;
    juce::TextEditor mcpRegisterHint;

    std::vector<std::pair<juce::String, juce::Rectangle<int>>> sectionBounds;
    juce::Rectangle<int> keyCommandsBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsView)
};
