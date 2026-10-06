#pragma once

#include "ControllerLanes.h"
#include "Theme.h"

// Settings > Controller lanes: every lane (velocity, pitch bend, aftertouch, CC 0-127) with a
// toggle - available to add to a track's lanes in the MIDI editor - and its name (editable;
// emptying it goes back to the standard name).
class ControllerLanesEditor final : public juce::Component
{
public:
    ControllerLanesEditor()
    {
        hint.setText ("Available lanes can be added to a track in the MIDI editor: right-click the lane pane below the notes. "
                      "Names are yours to change; empty a name to get the standard one back.",
                      juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, juce::Colours::grey);
        hint.setFont (juce::FontOptions (12.0f));
        hint.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (hint);

        for (auto& id : lanes::allIds())
        {
            auto row = std::make_unique<Row> (id);
            addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
    }

    int layout (int width)
    {
        int y = 0;
        hint.setBounds (0, y, juce::jmin (width, 640), 34);
        y += 42;

        for (auto& row : rows)
        {
            row->setBounds (0, y, juce::jmin (width, 520), 24);
            y += 26;
        }

        setSize (width, y);
        return y;
    }

private:
    struct Row final : juce::Component
    {
        explicit Row (const juce::String& laneId) : id (laneId)
        {
            auto& settings = lanes::Settings::get();
            const auto lane = lanes::parse (id);

            available.setToggleState (settings.isAvailable (id), juce::dontSendNotification);
            available.setWantsKeyboardFocus (false);
            available.setTooltip ("Available: can be added to a track's lanes in the MIDI editor");
            available.onClick = [this] { lanes::Settings::get().setAvailable (id, available.getToggleState()); };
            addAndMakeVisible (available);

            number.setText (lane.kind == lanes::Kind::controller ? "CC" + juce::String (lane.cc) : juce::String(),
                            juce::dontSendNotification);
            number.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
            addAndMakeVisible (number);

            name.setText (settings.name (id), juce::dontSendNotification);
            name.setTextToShowWhenEmpty (lane.kind == lanes::Kind::controller ? "(no name)" : lanes::defaultName (id),
                                         juce::Colours::grey);
            name.onFocusLost = name.onReturnKey = [this]
            {
                lanes::Settings::get().setName (id, name.getText());
                name.setText (lanes::Settings::get().name (id), juce::dontSendNotification);
            };
            addAndMakeVisible (name);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            available.setBounds (area.removeFromLeft (28));
            number.setBounds (area.removeFromLeft (56));
            name.setBounds (area.reduced (0, 1));
        }

        juce::String id;
        juce::ToggleButton available;
        juce::Label number;
        juce::TextEditor name;
    };

    juce::Label hint;
    std::vector<std::unique_ptr<Row>> rows;
};
