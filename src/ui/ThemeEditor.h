#pragma once

#include "../AudioEngine.h"
#include "Theme.h"

// Settings > Theming: theme picker, save/delete, one setting per themable item
// (color area + hex field, or a slider for modifiers) and a live preview.
// Edits apply to the whole app at once (Manager::sendChangeMessage); unsaved
// edits live in the working-copy cache (see Theme.h).
class ThemeEditor final : public juce::Component,
                          private juce::ChangeListener
{
public:
    explicit ThemeEditor (AudioEngine& e) : engine (e)
    {
        auto& manager = theme::Manager::get();

        themeBox.setWantsKeyboardFocus (false);
        themeBox.onChange = [this] { onThemePicked(); };
        addAndMakeVisible (themeBox);

        saveButton.onClick = [this] { save(); };
        saveAsButton.onClick = [this] { askSaveAs(); };
        deleteButton.onClick = [this] { askDelete(); };
        revertButton.onClick = [this] { theme::Manager::get().revert(); };

        for (auto* b : { &saveButton, &saveAsButton, &deleteButton, &revertButton })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }

        status.setColour (juce::Label::textColourId, juce::Colours::grey);
        status.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (status);

        addAndMakeVisible (preview);

        const char* lastGroup = nullptr;

        for (int i = 0; i < theme::tokenCount; ++i)
        {
            const auto& d = theme::defs()[(size_t) i];

            if (lastGroup == nullptr || juce::String (lastGroup) != d.group)
            {
                groupStarts.push_back ({ (int) rows.size(), d.group });
                lastGroup = d.group;
            }

            rows.push_back (std::make_unique<Row> ((theme::Token) i));
            addAndMakeVisible (*rows.back());
        }

        manager.addChangeListener (this);
        refresh();
    }

    ~ThemeEditor() override   { theme::Manager::get().removeChangeListener (this); }

    // Lays the children out for `width` and returns the height needed (the
    // settings page scrolls, so this component does not scroll itself).
    int layout (int width)
    {
        int y = 0;

        themeBox.setBounds (0, y, 200, 26);
        saveButton.setBounds (208, y, 60, 26);
        saveAsButton.setBounds (272, y, 84, 26);
        deleteButton.setBounds (360, y, 64, 26);
        revertButton.setBounds (428, y, 64, 26);
        y += 30;
        status.setBounds (0, y, width, 18);
        y += 24;

        preview.setBounds (0, y, juce::jmin (width, 700), 178);
        y += 190;

        groupHeaders.clear();
        size_t nextGroup = 0;

        for (int i = 0; i < (int) rows.size(); ++i)
        {
            if (nextGroup < groupStarts.size() && groupStarts[nextGroup].first == i)
            {
                y += 8;
                groupHeaders.push_back ({ y, groupStarts[nextGroup].second });
                y += 24;
                ++nextGroup;
            }

            rows[(size_t) i]->setBounds (0, y, juce::jmin (width, 640), 26);
            y += 28;
        }

        setSize (width, y + 8);
        return y + 8;
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));

        for (auto& [y, name] : groupHeaders)
        {
            g.drawText (name, 0, y, getWidth(), 20, juce::Justification::centredLeft);
            g.setColour (juce::Colours::white.withAlpha (0.15f));
            g.fillRect (0, y + 21, juce::jmin (getWidth(), 640), 1);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
        }
    }

private:
    //==========================================================================
    // Color area + hex field (copy/paste), shown in a callout from a swatch
    struct ColourPicker final : juce::Component,
                                private juce::ChangeListener
    {
        ColourPicker (juce::Colour initial, bool hasAlpha, std::function<void (juce::Colour)> onPickToUse)
            : alpha (hasAlpha), onPick (std::move (onPickToUse)),
              selector (juce::ColourSelector::showColourspace | (hasAlpha ? juce::ColourSelector::showAlphaChannel : 0),
                        4, 0)
        {
            selector.setCurrentColour (initial, juce::dontSendNotification);
            selector.addChangeListener (this);
            addAndMakeVisible (selector);

            hex.setText (theme::hexOf (initial, alpha), false);
            hex.setSelectAllWhenFocused (true);
            hex.setFont (juce::FontOptions (14.0f));
            hex.setTooltip (alpha ? "#aarrggbb" : "#rrggbb");
            hex.onTextChange = [this] { applyHex (hex.getText()); };
            addAndMakeVisible (hex);

            copy.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (hex.getText()); };
            paste.onClick = [this]
            {
                const auto text = juce::SystemClipboard::getTextFromClipboard();

                if (applyHex (text))
                    hex.setText (text.trim(), false);
            };

            for (auto* b : { &copy, &paste })
            {
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (b);
            }

            setSize (280, alpha ? 340 : 316);
        }

        ~ColourPicker() override   { selector.removeChangeListener (this); }

        void resized() override
        {
            auto area = getLocalBounds().reduced (6);
            auto bottom = area.removeFromBottom (28);
            copy.setBounds (bottom.removeFromRight (52));
            bottom.removeFromRight (4);
            paste.setBounds (bottom.removeFromRight (52));
            bottom.removeFromRight (4);
            hex.setBounds (bottom);
            area.removeFromBottom (6);
            selector.setBounds (area);
        }

    private:
        bool applyHex (const juce::String& text)
        {
            juce::Colour c;

            if (! theme::parseHex (text, c))
                return false;

            selector.setCurrentColour (c, juce::dontSendNotification);
            onPick (c);
            return true;
        }

        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            const auto c = selector.getCurrentColour();
            hex.setText (theme::hexOf (c, alpha), false);   // false: no feedback into applyHex
            onPick (c);
        }

        bool alpha;
        std::function<void (juce::Colour)> onPick;
        juce::ColourSelector selector;
        juce::TextEditor hex;
        juce::TextButton copy { "Copy" }, paste { "Paste" };
    };

    //==========================================================================
    // One themable item
    struct Row final : juce::Component
    {
        explicit Row (theme::Token tokenToUse) : token (tokenToUse)
        {
            const auto& d = theme::def (token);

            label.setText (d.label, juce::dontSendNotification);
            label.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (label);

            if (d.isNumber)
            {
                slider.setSliderStyle (juce::Slider::LinearHorizontal);
                slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 22);
                slider.setRange (d.min, d.max, 0.01);
                slider.setWantsKeyboardFocus (false);
                slider.onValueChange = [this]
                {
                    if (! updating)
                        theme::Manager::get().setNumber (token, (float) slider.getValue());
                };
                addAndMakeVisible (slider);
            }
            else
            {
                swatch.setWantsKeyboardFocus (false);
                swatch.setTooltip ("Click to pick a color");
                swatch.onClick = [this] { openPicker(); };
                addAndMakeVisible (swatch);
            }

            reset.setWantsKeyboardFocus (false);
            reset.setTooltip (d.isNumber || d.parent < 0 ? "Back to the default"
                                                         : "Back to following '" + juce::String (theme::defs()[(size_t) d.parent].label) + "'");
            reset.onClick = [this] { theme::Manager::get().reset (token); };
            addAndMakeVisible (reset);
        }

        void refresh()
        {
            auto& manager = theme::Manager::get();
            const auto& d = theme::def (token);
            const auto overridden = manager.isOverridden (token);

            label.setColour (juce::Label::textColourId,
                             juce::Colours::white.withAlpha (overridden ? 0.95f : 0.6f));
            reset.setVisible (overridden);

            if (d.isNumber)
            {
                updating = true;
                slider.setValue (manager.number (token), juce::dontSendNotification);
                updating = false;
            }
            else
            {
                swatch.colour = manager.colour (token);
                swatch.inherited = ! overridden && d.parent >= 0;
                swatch.repaint();
            }
        }

        void resized() override
        {
            auto area = getLocalBounds();
            label.setBounds (area.removeFromLeft (230));
            reset.setBounds (area.removeFromRight (56).reduced (0, 2));
            area.removeFromRight (6);

            if (theme::def (token).isNumber)
                slider.setBounds (area);
            else
                swatch.setBounds (area.removeFromLeft (110).reduced (0, 2));
        }

    private:
        struct Swatch final : juce::Button
        {
            Swatch() : juce::Button ({}) {}

            void paintButton (juce::Graphics& g, bool over, bool) override
            {
                auto r = getLocalBounds().toFloat().reduced (0.5f);

                // Checkerboard-ish backing so translucent colors are visible
                g.setColour (juce::Colour (0xff555555));
                g.fillRoundedRectangle (r, theme::corner);
                g.setColour (juce::Colour (0xff888888));
                g.fillRect (r.withWidth (r.getWidth() / 2).withHeight (r.getHeight() / 2));

                g.setColour (colour);
                g.fillRoundedRectangle (r, theme::corner);

                g.setColour (juce::Colours::white.withAlpha (over ? 0.7f : 0.35f));
                g.drawRoundedRectangle (r, theme::corner, 1.0f);

                g.setColour (colour.getPerceivedBrightness() > 0.55f && colour.getAlpha() > 128
                                 ? juce::Colours::black.withAlpha (0.75f) : juce::Colours::white.withAlpha (0.85f));
                g.setFont (juce::FontOptions (12.0f));
                g.drawText (theme::hexOf (colour, colour.getAlpha() != 255), getLocalBounds(), juce::Justification::centred);
            }

            juce::Colour colour;
            bool inherited = false;
        };

        void openPicker()
        {
            const auto t = token;
            const auto& d = theme::def (t);

            auto picker = std::make_unique<ColourPicker> (theme::Manager::get().colour (t), d.hasAlpha,
                                                          [t] (juce::Colour c) { theme::Manager::get().setColour (t, c); });

            juce::CallOutBox::launchAsynchronously (std::move (picker),
                                                    getScreenBounds().removeFromLeft (240 + 110), nullptr);
        }

        theme::Token token;
        juce::Label label;
        Swatch swatch;
        juce::Slider slider;
        juce::TextButton reset { "Reset" };
        bool updating = false;
    };

    //==========================================================================
    // A miniature arrange view + sidebar drawn with the real tokens
    struct Preview final : juce::Component
    {
        static constexpr int buttonStrip = 38;

        Preview()
        {
            struct Mock { const char* text; const char* role; bool on; int width; };

            for (auto m : { Mock { "Menu", "topbar", false, 56 }, Mock { "Midi", "topbar", true, 50 },
                            Mock { "|<", "rtz", false, 36 }, Mock { "Play", "play", false, 50 },
                            Mock { "Rec", "record", false, 46 }, Mock { "Loop", "loop", true, 50 },
                            Mock { "R", "arm", true, 30 }, Mock { "S", "solo", false, 30 }, Mock { "M", "mute", true, 30 },
                            Mock { "Snap", "accent", true, 50 }, Mock { "Button", "", false, 64 } })
            {
                auto button = std::make_unique<juce::TextButton> (m.text);
                button->setToggleState (m.on, juce::dontSendNotification);

                if (juce::String (m.role).isNotEmpty())
                    theme::setButtonRole (*button, m.role);

                button->setInterceptsMouseClicks (false, false);   // a picture of the buttons, not controls
                button->setWantsKeyboardFocus (false);
                widths.push_back (m.width);
                addAndMakeVisible (*button);
                buttons.push_back (std::move (button));
            }
        }

        void resized() override
        {
            int x = 0;

            for (size_t i = 0; i < buttons.size(); ++i)
            {
                buttons[i]->setBounds (x, getHeight() - buttonStrip + 8, widths[i], 24);
                x += widths[i] + 6;
            }
        }

        std::vector<std::unique_ptr<juce::TextButton>> buttons;
        std::vector<int> widths;

        void paint (juce::Graphics& g) override
        {
            using T = theme::Token;
            auto area = getLocalBounds().withTrimmedBottom (buttonStrip);
            const auto sidebar = area.removeFromLeft (190);

            g.setColour (theme::colour (T::surfacePanel));
            g.fillRect (sidebar);

            const int rowH = 36;
            const juce::Colour strings = juce::Colour (0xff56b58c), brass = juce::Colour (0xffdd2c40);

            // --- sidebar rows ---
            auto rowArea = sidebar.reduced (2, 0);
            struct RowDef { const char* name; bool folder, selected; juce::Colour colour; };

            for (auto def : { RowDef { "Orchestra", true, false, juce::Colour (0xff6d7178) },
                              RowDef { "Strings", false, false, strings },
                              RowDef { "Brass", false, true, brass } })
            {
                auto bounds = rowArea.removeFromTop (rowH).toFloat().reduced (2.0f, 1.5f);
                theme::paintTrackBox (g, bounds, def.folder ? theme::Token::folderBg : theme::Token::trackMidiBg, def.selected);
                g.setColour (def.colour);
                g.fillRect (bounds.getX() + 1.0f, bounds.getY() + 2.0f, 4.0f, bounds.getHeight() - 4.0f);
                g.setColour (juce::Colours::white.withAlpha (0.8f));
                g.setFont (juce::FontOptions (13.0f));
                g.drawText (def.name, bounds.reduced (14.0f, 0.0f), juce::Justification::centredLeft);
            }

            // --- arrange area ---
            g.setColour (theme::colour (T::arrangeBg));
            g.fillRect (area);

            const int gutter = 54;
            int y = area.getY();

            const T lanes[] = { T::arrangeLaneFolder, T::arrangeLaneEven, T::arrangeLaneOdd };

            for (auto lane : lanes)
            {
                g.setColour (theme::colour (lane));
                g.fillRect (area.getX(), y, area.getWidth(), rowH);
                y += rowH;
            }

            const auto top = area.getY(), height = rowH * 3 + 20;

            for (int x = area.getX() + gutter; x < area.getRight(); x += 60)
            {
                g.setColour (theme::colour (T::arrangeBarline));
                g.fillRect (x, top, 1, height);
            }

            g.setColour (theme::colour (T::arrangeMarkerLine));
            g.fillRect (area.getX() + gutter + 120, top, 1, height);

            auto drawRegion = [&] (juce::Rectangle<int> r, juce::Colour trackColour, bool emphasised)
            {
                const auto style = theme::regionStyle (trackColour, emphasised);
                g.setColour (style.fill);
                g.fillRoundedRectangle (r.toFloat(), theme::corner);
                g.setColour (style.border);
                g.drawRoundedRectangle (r.toFloat(), theme::corner, 1.8f);
            };

            drawRegion ({ area.getX() + gutter + 8, top + rowH + 5, 100, rowH - 10 }, strings, false);
            drawRegion ({ area.getX() + gutter + 130, top + rowH + 5, 80, rowH - 10 }, strings, false);
            drawRegion ({ area.getX() + gutter + 40, top + rowH * 2 + 5, 150, rowH - 10 }, brass, true);

            g.setColour (theme::colour (T::arrangeGutterBg));
            g.fillRect (area.getX(), top, gutter, height);
            g.setColour (theme::colour (T::arrangeGutterBorder));
            g.fillRect (area.getX() + gutter - 1, top, 1, height);
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText ("Strings", area.getX() + 4, top + rowH, gutter - 8, rowH, juce::Justification::centredLeft);
            g.drawText ("Brass", area.getX() + 4, top + rowH * 2, gutter - 8, rowH, juce::Justification::centredLeft);

            g.setColour (theme::colour (T::transportLine));
            g.fillRect (area.getX() + gutter + 90, top, 1, height);
        }
    };

    //==========================================================================
    void refresh()
    {
        auto& manager = theme::Manager::get();
        const auto names = manager.listThemes();
        const auto active = manager.getActiveName();

        themeBox.clear (juce::dontSendNotification);

        for (int i = 0; i < names.size(); ++i)
            themeBox.addItem (names[i] + (names[i] == active && manager.isModified() ? " (modified)" : ""), i + 1);

        themeBox.setSelectedId (names.indexOf (active) + 1, juce::dontSendNotification);

        saveButton.setEnabled (manager.isModified() && ! manager.isBuiltIn (active));
        deleteButton.setEnabled (! manager.isBuiltIn (active));
        revertButton.setEnabled (manager.isModified());

        status.setText (manager.isModified()
                            ? "Unsaved changes (kept between sessions). Save, or Save as... to keep them under a name."
                            : (manager.isBuiltIn (active) ? "The built-in theme can't be changed or deleted: edit it, then Save as..."
                                                          : "Saved theme"),
                        juce::dontSendNotification);

        for (auto& row : rows)
            row->refresh();

        preview.repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        refresh();

        // Every view paints from the theme; one nudge repaints them all.
        engine.requestRepaint();

        if (auto* top = getTopLevelComponent())
            top->repaint();
    }

    void onThemePicked()
    {
        auto& manager = theme::Manager::get();
        const auto names = manager.listThemes();
        const auto wanted = names[themeBox.getSelectedId() - 1];

        if (wanted == manager.getActiveName())
            return;

        if (! manager.isModified())
        {
            manager.use (wanted);
            return;
        }

        refresh();   // put the combo back until the user decides

        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Unsaved theme changes",
                                            "Discard the unsaved changes to '" + manager.getActiveName() + "'?",
                                            "Discard", "Keep editing", this,
                                            juce::ModalCallbackFunction::create ([wanted] (int result)
                                            {
                                                if (result == 1)
                                                    theme::Manager::get().use (wanted);
                                            }));
    }

    void save()
    {
        const auto error = theme::Manager::get().save();

        if (error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save theme", error);
    }

    void askSaveAs()
    {
        auto* window = new juce::AlertWindow ("Save theme as", "Name of the theme:", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor ("name", theme::Manager::get().isBuiltIn (theme::Manager::get().getActiveName())
                                           ? juce::String() : theme::Manager::get().getActiveName());
        window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        window->enterModalState (true, juce::ModalCallbackFunction::create ([window] (int result)
        {
            if (result != 1)
                return;

            auto& manager = theme::Manager::get();
            const auto name = window->getTextEditorContents ("name").trim();

            if (name != manager.getActiveName() && manager.listThemes().contains (name))
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save theme",
                                                        "A theme named '" + name + "' already exists. Pick another name.");
                return;
            }

            const auto error = manager.saveAs (name);

            if (error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save theme", error);
        }), true);
    }

    void askDelete()
    {
        const auto name = theme::Manager::get().getActiveName();

        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Delete theme",
                                            "Delete the theme '" + name + "'? The built-in theme is used instead.",
                                            "Delete", "Cancel", this,
                                            juce::ModalCallbackFunction::create ([name] (int result)
                                            {
                                                if (result == 1)
                                                    theme::Manager::get().remove (name);
                                            }));
    }

    AudioEngine& engine;
    juce::ComboBox themeBox;
    juce::TextButton saveButton { "Save" }, saveAsButton { "Save as..." },
                     deleteButton { "Delete" }, revertButton { "Revert" };
    juce::Label status;
    Preview preview;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<std::pair<int, juce::String>> groupStarts;    // (first row index, group)
    std::vector<std::pair<int, juce::String>> groupHeaders;   // (y, group) after layout

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemeEditor)
};
