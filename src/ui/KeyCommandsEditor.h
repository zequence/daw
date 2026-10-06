#pragma once

#include "KeyCommands.h"
#include "Theme.h"

// Settings > Key commands: every command (KeyCommands.h) with its keys. Click the keys
// to press new ones (replacing them), "+" to add another key, the arrow to go back to
// the default. A key another command in the same context already uses shows in red.
class KeyCommandsEditor final : public juce::Component
{
public:
    KeyCommandsEditor()
    {
        hint.setText ("Click a command's keys, then press the new key (click elsewhere to cancel). \"+\" adds another key; "
                      "the arrow restores the default. Articulation keys are set per expression map.",
                      juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, juce::Colours::grey);
        hint.setFont (juce::FontOptions (12.0f));
        hint.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (hint);

        resetAllButton.setWantsKeyboardFocus (false);
        resetAllButton.onClick = [this]
        {
            keys::Bindings::get().resetAll();
            refresh();
        };
        addAndMakeVisible (resetAllButton);

        juce::String context;

        for (auto& command : keys::commands())
        {
            if (context != command.context)
            {
                context = command.context;
                auto heading = std::make_unique<juce::Label> (juce::String(), context);
                heading->setFont (juce::FontOptions (15.0f, juce::Font::bold));
                heading->setColour (juce::Label::textColourId, juce::Colours::white);
                addAndMakeVisible (*heading);
                headings.push_back (std::move (heading));
            }

            auto row = std::make_unique<Row> (*this, command);
            addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }

        refresh();
    }

    // Lays out at this width; returns the height used
    int layout (int width)
    {
        int y = 0;
        hint.setBounds (0, y, juce::jmin (width, 640), 34);
        resetAllButton.setBounds (juce::jmin (width, 640) + 8, y, 120, 26);
        y += 42;

        size_t heading = 0;
        juce::String context;

        for (auto& row : rows)
        {
            if (context != row->command.context)
            {
                context = row->command.context;
                y += heading == 0 ? 0 : 10;
                headings[heading++]->setBounds (0, y, 300, 24);
                y += 28;
            }

            row->setBounds (0, y, juce::jmin (width, 760), 26);
            y += 28;
        }

        setSize (width, y);
        return y;
    }

    void refresh()
    {
        for (auto& row : rows)
            row->refresh();
    }

private:
    // Takes the next key pressed while it has the focus
    struct Capture final : juce::Component
    {
        bool keyPressed (const juce::KeyPress& press) override
        {
            if (onKey)
                onKey (press);

            return true;
        }

        void focusLost (FocusChangeType) override
        {
            if (onCancel)
                onCancel();
        }

        std::function<void (const juce::KeyPress&)> onKey;
        std::function<void()> onCancel;
    };

    struct Row final : juce::Component
    {
        Row (KeyCommandsEditor& ownerToUse, const keys::Command& commandToUse) : owner (ownerToUse), command (commandToUse)
        {
            label.setText (command.label, juce::dontSendNotification);
            label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
            addAndMakeVisible (label);

            for (auto* b : { &keysButton, &addButton, &resetButton })
            {
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (b);
            }

            keysButton.onClick = [this] { startCapture (false); };
            addButton.setTooltip ("Add another key");
            addButton.onClick = [this] { startCapture (true); };
            resetButton.setTooltip ("Back to the default");
            resetButton.onClick = [this]
            {
                keys::Bindings::get().reset (command.id);
                owner.refresh();
            };

            capture.setWantsKeyboardFocus (true);
            capture.onKey = [this] (const juce::KeyPress& press)
            {
                auto list = adding ? keys::Bindings::get().keysFor (command.id) : std::vector<juce::KeyPress>();

                if (std::find (list.begin(), list.end(), press) == list.end())
                    list.push_back (press);

                capturing = false;
                keys::Bindings::get().set (command.id, list);
                owner.refresh();
                capture.giveAwayKeyboardFocus();
            };
            capture.onCancel = [this]
            {
                if (capturing)
                {
                    capturing = false;
                    refresh();
                }
            };
            addAndMakeVisible (capture);
        }

        void startCapture (bool add)
        {
            adding = add;
            capturing = true;
            keysButton.setButtonText ("Press a key...");
            capture.grabKeyboardFocus();
        }

        void refresh()
        {
            auto& bindings = keys::Bindings::get();
            const auto list = bindings.keysFor (command.id);
            juce::StringArray conflicts;

            for (auto& press : list)
                if (const auto other = bindings.conflictFor (command.id, press); other.isNotEmpty())
                    conflicts.add (press.getTextDescriptionWithIcons() + " is also: " + other);

            keysButton.setButtonText (keys::Bindings::describe (list));
            keysButton.setColour (juce::TextButton::textColourOffId, conflicts.isEmpty() ? juce::Colours::white
                                                                                        : juce::Colour (0xffff6b6b));
            keysButton.setTooltip (conflicts.isEmpty() ? juce::String ("Click, then press the new key")
                                                       : conflicts.joinIntoString ("\n"));
            resetButton.setEnabled (! bindings.isDefault (command.id));
        }

        void resized() override
        {
            auto area = getLocalBounds();
            label.setBounds (area.removeFromLeft (360));
            keysButton.setBounds (area.removeFromLeft (230).reduced (0, 1));
            area.removeFromLeft (4);
            addButton.setBounds (area.removeFromLeft (26).reduced (0, 1));
            area.removeFromLeft (4);
            resetButton.setBounds (area.removeFromLeft (26).reduced (0, 1));
            capture.setBounds (0, 0, 1, 1);   // invisible: it only takes the key
        }

        KeyCommandsEditor& owner;
        const keys::Command& command;
        juce::Label label;
        juce::TextButton keysButton, addButton { "+" }, resetButton { juce::String::fromUTF8 ("\xe2\x86\xba") };
        Capture capture;
        bool capturing = false, adding = false;
    };

    juce::Label hint;
    juce::TextButton resetAllButton { "Reset all" };
    std::vector<std::unique_ptr<juce::Label>> headings;
    std::vector<std::unique_ptr<Row>> rows;
};
