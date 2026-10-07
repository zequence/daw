#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "ThemedLookAndFeel.h"

// The Add Track dialog: what kind (a folder, a MIDI track, an instrument with its MIDI and audio, an
// audio track, a bus), how many (more than one: named automatically - the name and a number), the
// name, and for an instrument its plugin; for an audio track mono or stereo.
class AddTrackDialog final : public juce::Component
{
public:
    enum class Kind { folder, midi, instrument, audio, bus };

    struct Choice
    {
        Kind kind = Kind::midi;
        int count = 1;
        juce::String name;                       // "" = the default names
        juce::PluginDescription plugin;          // an instrument's
        bool stereo = true;                      // an audio track's
    };

    // Shows the dialog; 'done' gets the choice when Add is pressed (not on Cancel)
    static void show (juce::Component* parent, juce::Array<juce::PluginDescription> instrumentTypes,
                      std::function<void (const Choice&)> done)
    {
        auto* dialog = new AddTrackDialog (std::move (instrumentTypes), std::move (done));
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (dialog);
        options.dialogTitle = "Add track";
        options.componentToCentreAround = parent;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    }

    AddTrackDialog (juce::Array<juce::PluginDescription> types, std::function<void (const Choice&)> doneToUse)
        : instrumentTypes (std::move (types)), done (std::move (doneToUse))
    {
        const std::pair<Kind, const char*> kinds[] { { Kind::folder, "Folder" }, { Kind::midi, "MIDI" }, { Kind::instrument, "Instrument" },
                                                     { Kind::audio, "Audio" }, { Kind::bus, "Bus" } };

        for (auto [kind, label] : kinds)
        {
            auto button = std::make_unique<juce::TextButton> (label);
            button->setClickingTogglesState (true);
            button->setRadioGroupId (1);
            button->onClick = [this, kind = kind] { choice.kind = kind; update(); };
            addAndMakeVisible (*button);
            kindButtons.push_back ({ kind, std::move (button) });
        }

        kindButtons[1].second->setToggleState (true, juce::dontSendNotification);   // MIDI

        countLabel.setText ("How many", juce::dontSendNotification);
        addAndMakeVisible (countLabel);
        count.setSliderStyle (juce::Slider::IncDecButtons);
        count.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 24);
        count.setRange (1, 64, 1);
        count.setValue (1, juce::dontSendNotification);
        count.onValueChange = [this] { choice.count = (int) count.getValue(); update(); };
        addAndMakeVisible (count);

        nameLabel.setText ("Name", juce::dontSendNotification);
        addAndMakeVisible (nameLabel);
        name.setTextToShowWhenEmpty ("(the default names)", juce::Colours::grey);
        name.onTextChange = [this] { update(); };
        addAndMakeVisible (name);

        pluginButton.onClick = [this] { choosePlugin(); };
        addAndMakeVisible (pluginButton);

        stereo.setButtonText ("Stereo");
        mono.setButtonText ("Mono");

        for (auto* b : { &stereo, &mono })
        {
            b->setRadioGroupId (2);
            b->setClickingTogglesState (true);
            addAndMakeVisible (b);
        }

        stereo.setToggleState (true, juce::dontSendNotification);
        stereo.onClick = [this] { choice.stereo = true; };
        mono.onClick = [this] { choice.stereo = false; };

        note.setJustificationType (juce::Justification::topLeft);
        note.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (note);

        addButton.onClick = [this] { add(); };
        cancelButton.onClick = [this] { close(); };
        addAndMakeVisible (addButton);
        addAndMakeVisible (cancelButton);

        setSize (460, 280);
        update();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16);
        auto row = area.removeFromTop (28);
        const auto width = row.getWidth() / (int) kindButtons.size();

        for (auto& [kind, button] : kindButtons)
            button->setBounds (row.removeFromLeft (width).reduced (2, 0));

        area.removeFromTop (14);
        row = area.removeFromTop (26);
        countLabel.setBounds (row.removeFromLeft (90));
        count.setBounds (row.removeFromLeft (120));

        area.removeFromTop (8);
        row = area.removeFromTop (26);
        nameLabel.setBounds (row.removeFromLeft (90));
        name.setBounds (row);

        area.removeFromTop (8);
        row = area.removeFromTop (26);
        row.removeFromLeft (90);
        pluginButton.setBounds (row);
        stereo.setBounds (row.removeFromLeft (90));
        row.removeFromLeft (6);
        mono.setBounds (row.removeFromLeft (90));

        auto buttons = area.removeFromBottom (28);
        cancelButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        addButton.setBounds (buttons.removeFromRight (90));

        area.removeFromTop (8);
        note.setBounds (area.withTrimmedLeft (90));
    }

    void paint (juce::Graphics& g) override   { g.fillAll (theme::colour (theme::Token::surfacePanel)); }

private:
    void update()
    {
        const auto many = choice.count > 1;
        pluginButton.setVisible (choice.kind == Kind::instrument);
        pluginButton.setButtonText (choice.plugin.name.isNotEmpty() ? choice.plugin.name : juce::String ("Choose the instrument..."));
        stereo.setVisible (choice.kind == Kind::audio);
        mono.setVisible (choice.kind == Kind::audio);

        const auto typed = name.getText().trim();
        juce::String text;

        if (many)
            text = "Named automatically: " + (typed.isNotEmpty() ? typed + " 1, " + typed + " 2, ..." : juce::String ("the default names"));

        if (choice.kind == Kind::instrument)
            text << (text.isEmpty() ? "" : "\n") << "Each with its MIDI track and its audio.";
        else if (choice.kind == Kind::bus)
            text << (text.isEmpty() ? "" : "\n") << "Tracks reach a bus through their outputs; it goes to the master (or another bus).";
        else if (choice.kind == Kind::audio)
            text << (text.isEmpty() ? "" : "\n") << "Its input is heard and recorded once recording comes; wave files soon too.";

        note.setText (text, juce::dontSendNotification);
        addButton.setEnabled (choice.kind != Kind::instrument || choice.plugin.name.isNotEmpty());
    }

    void choosePlugin()
    {
        juce::PopupMenu menu;

        if (instrumentTypes.isEmpty())
            menu.addItem (1, "No instruments found - scan in Settings > Plugins first", false, false);
        else
            juce::KnownPluginList::addToMenu (menu, instrumentTypes, juce::KnownPluginList::sortByManufacturer);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pluginButton),
                            [safe = juce::Component::SafePointer<AddTrackDialog> (this)] (int result)
                            {
                                if (safe == nullptr || result == 0)
                                    return;

                                const auto index = juce::KnownPluginList::getIndexChosenByMenu (safe->instrumentTypes, result);

                                if (juce::isPositiveAndBelow (index, safe->instrumentTypes.size()))
                                {
                                    safe->choice.plugin = safe->instrumentTypes.getReference (index);
                                    safe->update();
                                }
                            });
    }

    void add()
    {
        choice.name = name.getText().trim();
        const auto chosen = choice;
        const auto callback = done;
        close();

        if (callback)
            callback (chosen);
    }

    void close()
    {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState (0);
    }

    juce::Array<juce::PluginDescription> instrumentTypes;
    std::function<void (const Choice&)> done;
    Choice choice;

    std::vector<std::pair<Kind, std::unique_ptr<juce::TextButton>>> kindButtons;
    juce::Label countLabel, nameLabel, note;
    juce::Slider count;
    juce::TextEditor name;
    juce::TextButton pluginButton, addButton { "Add" }, cancelButton { "Cancel" };
    juce::ToggleButton stereo, mono;
};
