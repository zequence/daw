#pragma once

#include "../model/ArticulationMenu.h"
#include "ThemedLookAndFeel.h"

// The MIDI editor's articulation chooser: one column per group, side by side in map order
// (Color | Main | Legato | Release | Tempo), each showing what the menu rules offer for the
// current choice (model/ArticulationMenu.h). Choosing doesn't close it: the columns follow the
// new choice - items appear, disappear, get ticked. Shown in a CallOutBox; closes on a click
// outside it.
class ArticulationPanel final : public juce::Component,
                                private juce::Timer
{
public:
    using Items = std::vector<articulations::MenuItem>;

    ArticulationPanel (std::function<Items()> itemsToUse, std::function<void (const juce::String&, const juce::String&)> chooseToUse)
        : items (std::move (itemsToUse)), choose (std::move (chooseToUse))
    {
        rebuild();
        startTimerHz (10);   // follows changes made elsewhere too (a prompt's answer, undo)
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour (theme::Token::surfacePanel));
    }

private:
    static constexpr int columnWidth = 160, rowHeight = 24, headerHeight = 22, gap = 6;

    // What the panel shows, as text: rebuilt only when it changes
    static juce::String signatureOf (const Items& list)
    {
        juce::String text;

        for (auto& item : list)
            text << item.text << (item.enabled ? "+" : "-") << (item.ticked ? "x" : "o") << "|";

        return text;
    }

    void rebuild()
    {
        const auto list = items();
        signature = signatureOf (list);
        buttons.clear();
        labels.clear();

        int column = -1, row = 0, maxRows = 0;

        for (auto& item : list)
        {
            if (item.kind == articulations::MenuItem::Kind::header)
            {
                ++column;
                row = 0;

                auto label = std::make_unique<juce::Label> (juce::String(), item.text);
                label->setFont (juce::FontOptions (12.0f, juce::Font::bold));
                label->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.65f));
                label->setBounds (gap + column * (columnWidth + gap), gap, columnWidth, headerHeight);
                addAndMakeVisible (*label);
                labels.push_back (std::move (label));
                continue;
            }

            auto button = std::make_unique<juce::TextButton> (item.text);
            button->setClickingTogglesState (false);
            button->setToggleState (item.ticked, juce::dontSendNotification);
            button->setEnabled (item.enabled);
            button->setTooltip (item.description);
            button->setWantsKeyboardFocus (false);
            button->setBounds (gap + column * (columnWidth + gap), gap + headerHeight + row * (rowHeight + 2), columnWidth, rowHeight);
            button->onClick = [this, group = item.group, name = item.name]
            {
                choose (group, name);
                // after the click has finished (the choice may have rebuilt everything)
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ArticulationPanel> (this)]
                                                 { if (safe != nullptr) safe->refresh(); });
            };
            addAndMakeVisible (*button);
            buttons.push_back (std::move (button));
            maxRows = juce::jmax (maxRows, ++row);
        }

        const auto columns = juce::jmax (1, column + 1);
        setSize (gap + columns * (columnWidth + gap), gap + headerHeight + juce::jmax (1, maxRows) * (rowHeight + 2) + gap);
    }

    void refresh()
    {
        if (signatureOf (items()) != signature)
            rebuild();
    }

    void timerCallback() override    { refresh(); }

    std::function<Items()> items;
    std::function<void (const juce::String&, const juce::String&)> choose;
    juce::String signature;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    std::vector<std::unique_ptr<juce::Label>> labels;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArticulationPanel)
};
