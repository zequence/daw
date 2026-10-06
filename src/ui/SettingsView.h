#pragma once

#include "../AudioEngine.h"
#include "../integrations/VeproState.h"
#include "../integrations/VeproServer.h"
#include "ColorPalette.h"
#include "EditorSettings.h"
#include "../model/NoteNames.h"
#include "ThemeEditor.h"
#include "KeyCommandsEditor.h"
#include "ControllerLanesEditor.h"

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

        categories.names = { "Audio & MIDI", "Plugins", "Tracks", "Editor", "Controller lanes", "Agents (MCP)", "Integrations",
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

        // MIDI controllers: MIDI inputs used as control surfaces
        controllersHeading.setText ("MIDI controllers", juce::dontSendNotification);
        controllersHeading.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        controllersHeading.setColour (juce::Label::textColourId, juce::Colours::white);
        page.addAndMakeVisible (controllersHeading);

        controllersHint.setText ("A MIDI input added as a controller is a control surface: its notes and controllers never reach tracks "
                                 "or recording. Its controls are assigned to functions - for now articulations (an expression map's MIDI triggers).",
                                 juce::dontSendNotification);
        controllersHint.setColour (juce::Label::textColourId, juce::Colours::grey);
        controllersHint.setFont (juce::FontOptions (12.0f));
        controllersHint.setJustificationType (juce::Justification::topLeft);
        page.addAndMakeVisible (controllersHint);
        rebuildControllerToggles();

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

        // --- Editor > Midi ---
        editorMidiHeading.setText ("Midi", juce::dontSendNotification);
        editorMidiHeading.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        editorMidiHeading.setColour (juce::Label::textColourId, juce::Colours::white);
        page.addAndMakeVisible (editorMidiHeading);

        dropLabel.setText ("When choosing another root articulation would drop modifiers that no longer apply:",
                           juce::dontSendNotification);
        dropLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
        page.addAndMakeVisible (dropLabel);

        dropBox.addItem ("Ask first", 1);
        dropBox.addItem ("Drop automatically", 2);
        dropBox.setSelectedId (editorSettings::askBeforeDropping (settings) ? 1 : 2, juce::dontSendNotification);
        dropBox.setWantsKeyboardFocus (false);
        dropBox.onChange = [this]
        {
            engine.getSettingsFile().setValue (editorSettings::askBeforeDroppingKey, dropBox.getSelectedId() == 1);
            engine.getSettingsFile().saveIfNeeded();
        };
        page.addAndMakeVisible (dropBox);

        defaultRootToggle.setToggleState (editorSettings::firstRootIsDefault (settings), juce::dontSendNotification);
        defaultRootToggle.setTooltip ("A note with no root articulation then behaves as if the expression map's first root "
                                      "articulation were chosen. Nothing is written to the note.");
        defaultRootToggle.onClick = [this]
        {
            engine.getSettingsFile().setValue (editorSettings::firstRootIsDefaultKey, defaultRootToggle.getToggleState());
            engine.getSettingsFile().saveIfNeeded();
            engine.refreshAllPlayback();   // notes with no articulation now play as the first root, or no longer
        };

        cutOverlapsToggle.setToggleState (editorSettings::cutOverlappedNotes (settings), juce::dontSendNotification);
        cutOverlapsToggle.setTooltip ("Where regions overlap, a note that starts over another on the same key stops the earlier one "
                                      "there, and lasts at least until the earlier one would have ended. Playback only - the notes "
                                      "stay as written until the regions are glued.");
        cutOverlapsToggle.onClick = [this]
        {
            engine.getSettingsFile().setValue (editorSettings::cutOverlappedNotesKey, cutOverlapsToggle.getToggleState());
            engine.getSettingsFile().saveIfNeeded();
            engine.refreshAllPlayback();
        };

        moveModeLabel.setText ("Moving regions in the arrangement", juce::dontSendNotification);
        moveModeLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
        page.addAndMakeVisible (moveModeLabel);

        moveModeBox.addItem ("Select first, then move", 1);
        moveModeBox.addItem ("Select and move in one go", 2);
        moveModeBox.setTooltip ("Select first: a click selects, and a selected region drags. In one go: pressing any region "
                                "drags it straight away (a selection rectangle then starts on empty space). Alt swaps the two "
                                "for one drag: a direct move, or a selection rectangle starting on a region.");
        moveModeBox.setSelectedId (editorSettings::moveRegionsDirectly (settings) ? 2 : 1, juce::dontSendNotification);
        moveModeBox.setWantsKeyboardFocus (false);
        moveModeBox.onChange = [this]
        {
            engine.getSettingsFile().setValue (editorSettings::moveRegionsDirectlyKey, moveModeBox.getSelectedId() == 2);
            engine.getSettingsFile().saveIfNeeded();
        };
        page.addAndMakeVisible (moveModeBox);

        middleCLabel.setText ("Middle C (MIDI note 60) is called", juce::dontSendNotification);
        middleCLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
        page.addAndMakeVisible (middleCLabel);

        for (int octave = 3; octave <= 5; ++octave)
            middleCBox.addItem ("C" + juce::String (octave) + (octave == 3 ? "  (VSL, Cubase)" : juce::String()), octave);

        middleCBox.setSelectedId (editorSettings::middleCOctave (settings), juce::dontSendNotification);
        middleCBox.setWantsKeyboardFocus (false);
        middleCBox.onChange = [this]
        {
            engine.getSettingsFile().setValue (editorSettings::middleCOctaveKey, middleCBox.getSelectedId());
            engine.getSettingsFile().saveIfNeeded();
            noteNames::middleCOctave() = middleCBox.getSelectedId();

            if (auto* top = getTopLevelComponent())
                top->repaint();   // the piano keys show the new names
        };
        page.addAndMakeVisible (middleCBox);

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
        page.addChildComponent (themeEditor);

        // --- Key commands ---
        page.addChildComponent (keyCommandsEditor);

        // --- Controller lanes (the MIDI editor's lower pane) ---
        page.addChildComponent (controllerLanesEditor);

        for (auto* c : std::initializer_list<juce::Component*> { &scanButton, &retryButton, &rescanButton,
                                                                 &onTopToggle, &autoRecordToggle, &mcpToggle, &defaultRootToggle,
                                                                 &cutOverlapsToggle })
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
    enum Category { audioMidi = 0, plugins, tracks, editor, controllerLanes, agents, integrations, theming, keyCommands };

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
        controllersHeading.setVisible (category == audioMidi);
        controllersHint.setVisible (category == audioMidi);

        for (auto& toggle : controllerToggles)
            toggle->setVisible (category == audioMidi);
        pluginList->setVisible (category == plugins);

        for (auto* c : std::initializer_list<juce::Component*> { &scanButton, &retryButton, &rescanButton, &onTopToggle })
            c->setVisible (category == plugins);

        autoRecordToggle.setVisible (category == tracks);

        for (auto* c : std::initializer_list<juce::Component*> { &editorMidiHeading, &dropLabel, &dropBox, &defaultRootToggle,
                                                                 &cutOverlapsToggle, &middleCLabel, &middleCBox,
                                                                 &moveModeLabel, &moveModeBox })
            c->setVisible (category == editor);

        for (auto* c : std::initializer_list<juce::Component*> { &mcpToggle, &mcpStatus, &mcpRegisterHint })
            c->setVisible (category == agents);

        for (auto* c : std::initializer_list<juce::Component*> { &veproVersionLabel, &veproVersionBox, &veproVersionHint,
                                                                 &veproServerLabel, &veproHostEditor, &veproPortEditor,
                                                                 &veproServerHint })
            c->setVisible (category == integrations);

        themeEditor.setVisible (category == theming);
        keyCommandsEditor.setVisible (category == keyCommands);
        controllerLanesEditor.setVisible (category == controllerLanes);

        switch (category)
        {
            case audioMidi:
                deviceSelector->setBounds (4, y, juce::jmin (width - 8, 560), 460);
                y += 470;
                controllersHeading.setBounds (4, y, 300, 24);
                y += 28;
                controllersHint.setBounds (4, y, juce::jmin (width - 8, 640), 34);
                y += 38;

                for (auto& toggle : controllerToggles)
                {
                    toggle->setBounds (4, y, juce::jmin (width - 8, 520), 24);
                    y += 26;
                }

                y += 8;
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

            case editor:
                editorMidiHeading.setBounds (4, y, 200, 24);
                y += 30;
                dropLabel.setBounds (4, y, juce::jmin (width - 8, 640), 22);
                y += 26;
                dropBox.setBounds (4, y, 200, 24);
                y += 36;
                defaultRootToggle.setBounds (4, y, juce::jmin (width - 8, 640), 24);
                y += 36;
                cutOverlapsToggle.setBounds (4, y, juce::jmin (width - 8, 640), 24);
                y += 36;
                middleCLabel.setBounds (4, y, 230, 24);
                middleCBox.setBounds (238, y, 170, 24);
                y += 32;
                moveModeLabel.setBounds (4, y, 230, 24);
                moveModeBox.setBounds (238, y, 220, 24);
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
                themeEditor.setTopLeftPosition (4, y);
                y += themeEditor.layout (width - 8) + 8;
                break;

            case controllerLanes:
                controllerLanesEditor.setTopLeftPosition (4, y);
                y += controllerLanesEditor.layout (width - 8) + 8;
                break;

            case keyCommands:
                keyCommandsEditor.setTopLeftPosition (4, y);
                y += keyCommandsEditor.layout (width - 8) + 8;
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
                    g.setColour (theme::colour (theme::Token::selectionBg));
                    g.fillRoundedRectangle (row.toFloat().reduced (3.0f, 2.0f), theme::corner);
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

        SettingsView& owner;
    };

    AudioEngine& engine;

    juce::Label titleLabel;
    juce::TextButton closeButton { "X" };
    CategoryList categories;
    juce::Viewport viewport;
    Page page { *this };

    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    juce::Label controllersHeading, controllersHint;
    std::vector<std::unique_ptr<juce::ToggleButton>> controllerToggles;

    // One toggle per MIDI input device ("use as a controller")
    void rebuildControllerToggles()
    {
        controllerToggles.clear();
        const auto chosen = engine.getControllers();
        const auto devices = juce::MidiInput::getAvailableDevices();

        for (auto& device : devices)
        {
            auto toggle = std::make_unique<juce::ToggleButton> (device.name + ": use as a controller");
            toggle->setToggleState (chosen.contains (device.identifier), juce::dontSendNotification);
            toggle->onClick = [this, id = device.identifier, t = toggle.get()]
            {
                auto list = engine.getControllers();
                list.removeString (id);

                if (t->getToggleState())
                    list.add (id);

                engine.setControllers (list);
            };
            page.addAndMakeVisible (*toggle);
            controllerToggles.push_back (std::move (toggle));
        }

        if (devices.isEmpty())
        {
            auto none = std::make_unique<juce::ToggleButton> ("(no MIDI inputs found)");
            none->setEnabled (false);
            page.addAndMakeVisible (*none);
            controllerToggles.push_back (std::move (none));
        }
    }
    std::unique_ptr<juce::PluginListComponent> pluginList;
    juce::TextButton scanButton { "Scan for new plugins" }, retryButton { "Retry failed plugins" },
                     rescanButton { "Rescan everything" };
    juce::ToggleButton onTopToggle { "Plugin windows stay on top" };
    juce::ToggleButton autoRecordToggle { "Arm track on select (auto-record)" };
    juce::Label editorMidiHeading, dropLabel;
    juce::ComboBox dropBox, middleCBox;
    juce::Label middleCLabel, moveModeLabel;
    juce::ComboBox moveModeBox;
    juce::ToggleButton defaultRootToggle { "Use the first root articulation as the default (notes with no articulation behave as if it were chosen)" };
    juce::ToggleButton cutOverlapsToggle { "Overlapping regions: a note started over another on the same key cuts it (playback; glue makes it permanent)" };
    juce::ToggleButton mcpToggle { "Run the MCP server for AI agents (starts with the app)" };
    juce::Label mcpStatus;
    juce::TextEditor mcpRegisterHint;
    juce::Label veproVersionLabel, veproVersionHint, veproServerLabel, veproServerHint;
    juce::ComboBox veproVersionBox;
    juce::TextEditor veproHostEditor, veproPortEditor;

    ThemeEditor themeEditor { engine };

    KeyCommandsEditor keyCommandsEditor;
    ControllerLanesEditor controllerLanesEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsView)
};
