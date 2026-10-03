#pragma once

#include "../AudioEngine.h"
#include "../integrations/VeproState.h"
#include "../integrations/VeproServer.h"
#include "ColorPalette.h"

// Full-window settings page (replaces the whole UI; close with X or ESC).
// Tab system (ISSUES.md "Settings Window"): a category column on the left
// (Audio & MIDI, Plugins, Tracks, Agents, Key commands), the selected
// category's actual settings on the right. The selection persists.
class SettingsView final : public juce::Component
{
public:
    static constexpr auto autoRecordOnSelectKey = "autoRecordOnSelect";
    static constexpr auto pluginWindowsOnTopKey = "pluginWindowsOnTop";
    static constexpr auto mcpEnabledKey = "mcpEnabled";
    static constexpr auto mcpPortKey = "mcpPort";
    static constexpr auto categoryKey = "settingsCategory";

    explicit SettingsView (AudioEngine& e) : engine (e)
    {
        titleLabel.setText ("Settings", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        addAndMakeVisible (titleLabel);

        closeButton.setWantsKeyboardFocus (false);
        closeButton.setTooltip ("Close (Esc)");
        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeButton);

        categories.names = { "Audio & MIDI", "Plugins", "Tracks", "Agents (MCP)", "Integrations",
                             "Theming", "Key commands" };
        categories.onSelect = [this] (int index) { setCategory (index); };
        addAndMakeVisible (categories);

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

        // --- Integrations ---
        veproVersionLabel.setText ("Vienna Ensemble Pro Server version", juce::dontSendNotification);
        veproVersionLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
        page.addAndMakeVisible (veproVersionLabel);

        veproVersionHint.setText ("Used by instrument.connectVepro: the connection-state format differs "
                                  "between Pro Server releases.", juce::dontSendNotification);
        veproVersionHint.setColour (juce::Label::textColourId, juce::Colours::grey);
        veproVersionHint.setFont (juce::FontOptions (12.0f));
        page.addAndMakeVisible (veproVersionHint);

        const auto versions = vepro::supportedVersions();

        for (int i = 0; i < versions.size(); ++i)
            veproVersionBox.addItem (versions[i], i + 1);

        const auto currentVersion = settings.getValue (vepro::versionSettingsKey, vepro::defaultVersion());
        veproVersionBox.setSelectedItemIndex (juce::jmax (0, versions.indexOf (currentVersion)),
                                              juce::dontSendNotification);
        veproVersionBox.setWantsKeyboardFocus (false);
        veproVersionBox.onChange = [this]
        {
            engine.getSettingsFile().setValue (vepro::versionSettingsKey, veproVersionBox.getText());
            engine.getSettingsFile().saveIfNeeded();
        };
        page.addAndMakeVisible (veproVersionBox);

        veproServerLabel.setText ("VE Pro Server address", juce::dontSendNotification);
        veproServerLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
        page.addAndMakeVisible (veproServerLabel);

        veproHostEditor.setText (settings.getValue (vepro::serverHostKey, vepro::defaultServerHost()));
        veproHostEditor.onTextChange = [this]
        {
            engine.getSettingsFile().setValue (vepro::serverHostKey, veproHostEditor.getText().trim());
            engine.getSettingsFile().saveIfNeeded();
        };
        page.addAndMakeVisible (veproHostEditor);

        veproPortEditor.setInputRestrictions (5, "0123456789");
        veproPortEditor.setText (juce::String (settings.getIntValue (vepro::serverPortKey, vepro::defaultServerPort)));
        veproPortEditor.onTextChange = [this]
        {
            engine.getSettingsFile().setValue (vepro::serverPortKey, veproPortEditor.getText().getIntValue());
            engine.getSettingsFile().saveIfNeeded();
        };
        page.addAndMakeVisible (veproPortEditor);

        veproServerHint.setText ("\"auto\" discovers the server on the network (it announces itself); "
                                 "set an address only to pick a specific server. Used by "
                                 "\"Sync to VE Pro Server\" and vepro.sync, via VSL's CLI (VE Pro 8.1+).",
                                 juce::dontSendNotification);
        veproServerHint.setColour (juce::Label::textColourId, juce::Colours::grey);
        veproServerHint.setFont (juce::FontOptions (12.0f));
        page.addAndMakeVisible (veproServerHint);

        // --- Theming ---
        opacityLabel.setText ("Track color opacity", juce::dontSendNotification);
        opacityLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
        page.addAndMakeVisible (opacityLabel);

        opacitySlider.setRange (0.2, 1.0, 0.01);
        opacitySlider.setValue (settings.getDoubleValue (colours::opacitySettingsKey, 1.0),
                                juce::dontSendNotification);
        opacitySlider.setWantsKeyboardFocus (false);
        opacitySlider.onValueChange = [this]
        {
            engine.getSettingsFile().setValue (colours::opacitySettingsKey, opacitySlider.getValue());
            engine.getSettingsFile().saveIfNeeded();
            page.repaint();   // the examples follow live
        };
        page.addAndMakeVisible (opacitySlider);

        for (auto* c : std::initializer_list<juce::Component*> { &scanButton, &retryButton, &rescanButton,
                                                                 &onTopToggle, &autoRecordToggle, &mcpToggle })
        {
            c->setWantsKeyboardFocus (false);
            page.addAndMakeVisible (c);
        }

        categories.selected = juce::jlimit (0, categories.names.size() - 1,
                                            settings.getIntValue (categoryKey, 0));
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

        area.removeFromTop (8);
        categories.setBounds (area.removeFromLeft (180));
        area.removeFromLeft (14);
        viewport.setBounds (area);

        layoutPage();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1d1f23));
    }

private:
    enum Category { audioMidi = 0, plugins, tracks, agents, integrations, theming, keyCommands };

    void setCategory (int index)
    {
        engine.getSettingsFile().setValue (categoryKey, index);
        engine.getSettingsFile().saveIfNeeded();
        viewport.setViewPosition (0, 0);
        layoutPage();
    }

    // Lays out (and shows/hides) the controls of the selected category only.
    void layoutPage()
    {
        const auto category = categories.selected;
        const auto width = juce::jmax (320, viewport.getMaximumVisibleWidth() - 4);
        const auto visibleHeight = juce::jmax (200, viewport.getMaximumVisibleHeight());
        int y = 8;

        deviceSelector->setVisible (category == audioMidi);
        pluginList->setVisible (category == plugins);

        for (auto* c : std::initializer_list<juce::Component*> { &scanButton, &retryButton, &rescanButton, &onTopToggle })
            c->setVisible (category == plugins);

        autoRecordToggle.setVisible (category == tracks);

        for (auto* c : std::initializer_list<juce::Component*> { &mcpToggle, &mcpStatus, &mcpRegisterHint })
            c->setVisible (category == agents);

        for (auto* c : std::initializer_list<juce::Component*> { &veproVersionLabel, &veproVersionBox, &veproVersionHint,
                                                                 &veproServerLabel, &veproHostEditor, &veproPortEditor,
                                                                 &veproServerHint })
            c->setVisible (category == integrations);

        for (auto* c : std::initializer_list<juce::Component*> { &opacityLabel, &opacitySlider })
            c->setVisible (category == theming);

        switch (category)
        {
            case audioMidi:
                deviceSelector->setBounds (4, y, juce::jmin (width - 8, 560), 460);
                y += 470;
                break;

            case plugins:
                scanButton.setBounds (4, y, 150, 26);
                retryButton.setBounds (160, y, 150, 26);
                rescanButton.setBounds (316, y, 150, 26);
                y += 34;
                onTopToggle.setBounds (4, y, 320, 24);
                y += 30;
                pluginList->setBounds (4, y, width - 8, juce::jmax (260, visibleHeight - y - 10));
                y += pluginList->getHeight() + 10;
                break;

            case tracks:
                autoRecordToggle.setBounds (4, y, 360, 24);
                y += 32;
                break;

            case agents:
                mcpToggle.setBounds (4, y, 460, 24);
                y += 28;
                mcpStatus.setBounds (4, y, width - 8, 18);
                y += 22;
                mcpRegisterHint.setBounds (4, y, juce::jmin (620, width - 8), 24);
                y += 32;
                break;

            case integrations:
                veproVersionLabel.setBounds (4, y, 280, 22);
                veproVersionBox.setBounds (288, y, 120, 24);
                y += 30;
                veproVersionHint.setBounds (4, y, width - 8, 18);
                y += 28;
                veproServerLabel.setBounds (4, y, 280, 22);
                veproHostEditor.setBounds (288, y, 160, 24);
                veproPortEditor.setBounds (452, y, 60, 24);
                y += 30;
                veproServerHint.setBounds (4, y, width - 8, 18);
                y += 26;
                break;

            case theming:
                opacityLabel.setBounds (4, y, 180, 22);
                opacitySlider.setBounds (188, y, juce::jmin (320, width - 196), 24);
                y += 32;
                themingExamples = { 4, y, juce::jmin (520, width - 8), 96 };
                y += 102;
                break;

            case keyCommands:
                keyCommandsBounds = { 4, y, width - 8, 120 };
                y += 126;
                break;
        }

        page.setSize (width, juce::jmax (y, visibleHeight));
        page.repaint();
    }

    //==========================================================================
    // The left category column
    struct CategoryList final : juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff232529));

            for (int i = 0; i < names.size(); ++i)
            {
                const auto row = juce::Rectangle<int> (0, i * rowHeight, getWidth(), rowHeight);

                if (i == selected)
                {
                    g.setColour (juce::Colour (0xff39404d));
                    g.fillRoundedRectangle (row.toFloat().reduced (3.0f, 2.0f), 4.0f);
                }

                g.setColour (i == selected ? juce::Colours::white : juce::Colours::white.withAlpha (0.6f));
                g.setFont (juce::FontOptions (14.0f, i == selected ? juce::Font::bold : juce::Font::plain));
                g.drawText (names[i], row.reduced (12, 0), juce::Justification::centredLeft);
            }
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            const auto index = event.y / rowHeight;

            if (index >= 0 && index < names.size() && index != selected)
            {
                selected = index;
                repaint();

                if (onSelect)
                    onSelect (index);
            }
        }

        static constexpr int rowHeight = 36;
        juce::StringArray names;
        int selected = 0;
        std::function<void (int)> onSelect;
    };

    //==========================================================================
    struct Page final : juce::Component
    {
        explicit Page (SettingsView& ownerToUse) : owner (ownerToUse) {}

        void paint (juce::Graphics& g) override
        {
            if (owner.categories.selected == theming)
            {
                // Visible examples next to the opacity slider (ISSUES.md): a mock
                // track row with its left color border, and a mock region border.
                const auto alpha = (float) owner.opacitySlider.getValue();
                auto area = owner.themingExamples.toFloat();

                auto rowArea = area.removeFromTop (40.0f).reduced (0.0f, 4.0f);
                g.setColour (juce::Colour (0xff2b2e33));
                g.fillRoundedRectangle (rowArea, 4.0f);
                g.setColour (AudioEngine::colourFromHex ("#dd2c40").withAlpha (alpha));
                g.fillRect (rowArea.getX() + 1.0f, rowArea.getY() + 2.0f, 4.0f, rowArea.getHeight() - 4.0f);
                g.setColour (juce::Colours::white.withAlpha (0.7f));
                g.setFont (juce::FontOptions (13.0f));
                g.drawText ("Track row", rowArea.reduced (14.0f, 0.0f), juce::Justification::centredLeft);

                auto regionArea = area.removeFromTop (44.0f).reduced (0.0f, 6.0f).withTrimmedRight (area.getWidth() * 0.4f);
                g.setColour (juce::Colour (0x995d8fc4));
                g.fillRoundedRectangle (regionArea, 4.0f);
                g.setColour (AudioEngine::colourFromHex ("#56b58c").withAlpha (alpha));
                g.drawRoundedRectangle (regionArea, 4.0f, 1.8f);
                g.setColour (juce::Colours::white.withAlpha (0.7f));
                g.drawText ("Region", regionArea.reduced (10.0f, 0.0f), juce::Justification::centredLeft);
                return;
            }

            if (owner.categories.selected != keyCommands)
                return;

            g.setColour (juce::Colours::lightgrey);
            g.setFont (juce::FontOptions (13.0f));
            g.drawFittedText ("Space: start/stop playback\n"
                              "Home: back to the beginning\n"
                              "F12: performance monitor\n"
                              "Esc: close this page / go back\n"
                              "Editor - S/D: select/draw mode, Ctrl+Z/Y: undo/redo,\n"
                              "arrows: nudge selected notes",
                              owner.keyCommandsBounds, juce::Justification::topLeft, 8);
        }

        SettingsView& owner;
    };

    AudioEngine& engine;

    juce::Label titleLabel;
    juce::TextButton closeButton { "X" };
    CategoryList categories;
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
    juce::Label veproVersionLabel, veproVersionHint, veproServerLabel, veproServerHint;
    juce::ComboBox veproVersionBox;
    juce::TextEditor veproHostEditor, veproPortEditor;

    juce::Label opacityLabel;
    juce::Slider opacitySlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::Rectangle<int> keyCommandsBounds, themingExamples;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsView)
};
