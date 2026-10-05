#pragma once

#include "../model/ArticulationMenu.h"
#include "ThemedLookAndFeel.h"
#include "Smufl.h"

// The MIDI editor's articulation chooser: one column per group, side by side in map order
// (Color | Main | Legato | Release | Tempo), each showing what the menu rules offer for the
// current choice (model/ArticulationMenu.h). Inside a column every articulation is a row of
// sub-columns - its symbol (a SMuFL glyph or text, ui/Smufl.h), name and description - each as wide
// as its widest content. Choosing doesn't close it: the columns follow the new choice - items appear,
// disappear, get ticked. Shown in a CallOutBox; closes on a click outside it.
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
    static constexpr int rowHeight = 24, headerHeight = 22, gap = 8, cellPad = 6, subGap = 10, minSymbolWidth = 18;

    static juce::Font nameFont()          { return juce::FontOptions (13.0f); }
    static juce::Font descriptionFont()   { return juce::FontOptions (12.0f); }

    static int widthOf (const juce::String& text, const juce::Font& font)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, text));
    }

    // One articulation: symbol | name | description, at the column's sub-column positions
    struct Row final : juce::Button
    {
        Row (const articulations::MenuItem& item, int symbolWidthToUse, int nameWidthToUse)
            : juce::Button (item.name), symbol (item.symbol), name (item.name), description (item.description),
              symbolWidth (symbolWidthToUse), nameWidth (nameWidthToUse)
        {
            setToggleState (item.ticked, juce::dontSendNotification);
            setEnabled (item.enabled);
            setTooltip (item.description);
            setWantsKeyboardFocus (false);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            auto bounds = getLocalBounds().toFloat();

            if (getToggleState())
                g.setColour (theme::colour (theme::Token::selectionBg));
            else
                g.setColour (juce::Colours::white.withAlpha (down ? 0.14f : highlighted ? 0.08f : 0.0f));

            g.fillRoundedRectangle (bounds, theme::corner);

            if (getToggleState())
            {
                g.setColour (theme::colour (theme::Token::selectionBorder));
                g.drawRoundedRectangle (bounds.reduced (0.5f), theme::corner, 1.0f);
            }

            const auto alpha = isEnabled() ? 1.0f : 0.35f;
            auto x = cellPad;

            g.setColour (juce::Colours::white.withAlpha (0.9f * alpha));
            smufl::draw (g, symbol, { x, 0, symbolWidth, getHeight() }, 13.0f, juce::Justification::centred);
            x += symbolWidth + subGap;

            g.setColour (juce::Colours::white.withAlpha (0.92f * alpha));
            g.setFont (nameFont());
            g.drawText (name, x, 0, nameWidth, getHeight(), juce::Justification::centredLeft, false);
            x += nameWidth + subGap;

            if (description.isNotEmpty() && description != name)
            {
                g.setColour (juce::Colours::white.withAlpha (0.5f * alpha));
                g.setFont (descriptionFont());
                g.drawText (description, x, 0, getWidth() - x - cellPad, getHeight(), juce::Justification::centredLeft, false);
            }
        }

        juce::String symbol, name, description;
        int symbolWidth, nameWidth;
    };

    // What the panel shows, as text: rebuilt only when it changes
    static juce::String signatureOf (const Items& list)
    {
        juce::String text;

        for (auto& item : list)
            text << item.text << (item.enabled ? "+" : "-") << (item.ticked ? "x" : "o") << (item.sameDepthAsPrevious ? "^" : "") << "|";

        return text;
    }

    void rebuild()
    {
        const auto list = items();
        signature = signatureOf (list);
        rows.clear();
        labels.clear();

        // Columns of sections: a header starts a section, in a new column - or, when its group is at
        // the same depth as the one before (alternatives: repetitions' Legato or Release), below it
        struct Section { juce::String heading; std::vector<const articulations::MenuItem*> members; };
        std::vector<std::vector<Section>> columns;

        for (auto& item : list)
        {
            if (item.kind == articulations::MenuItem::Kind::header)
            {
                if (item.sameDepthAsPrevious && ! columns.empty())
                    columns.back().push_back ({ item.text, {} });
                else
                    columns.push_back ({ { item.text, {} } });
            }
            else if (! columns.empty())
            {
                columns.back().back().members.push_back (&item);
            }
        }

        int x = gap, height = 0;

        for (auto& sections : columns)
        {
            // one set of sub-column widths for the whole column
            int symbolWidth = minSymbolWidth, nameWidth = 0, descriptionWidth = 0;

            for (auto& section : sections)
            {
                nameWidth = juce::jmax (nameWidth, widthOf (section.heading, juce::FontOptions (12.0f, juce::Font::bold)));

                for (auto* item : section.members)
                {
                    symbolWidth = juce::jmax (symbolWidth, smufl::width (item->symbol, 13.0f));

                    nameWidth = juce::jmax (nameWidth, widthOf (item->name, nameFont()));

                    if (item->description.isNotEmpty() && item->description != item->name)
                        descriptionWidth = juce::jmax (descriptionWidth, widthOf (item->description, descriptionFont()));
                }
            }

            const auto columnWidth = cellPad + symbolWidth + subGap + nameWidth
                                     + (descriptionWidth > 0 ? subGap + descriptionWidth : 0) + cellPad;
            auto y = gap;

            for (auto& section : sections)
            {
                if (y > gap)
                    y += gap;   // space between stacked groups

                auto label = std::make_unique<juce::Label> (juce::String(), section.heading);
                label->setFont (juce::FontOptions (12.0f, juce::Font::bold));
                label->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.65f));
                label->setBorderSize ({ 0, cellPad, 0, 0 });
                label->setBounds (x, y, columnWidth, headerHeight);
                addAndMakeVisible (*label);
                labels.push_back (std::move (label));
                y += headerHeight;

                for (auto* item : section.members)
                {
                    auto button = std::make_unique<Row> (*item, symbolWidth, nameWidth);
                    button->setBounds (x, y, columnWidth, rowHeight);
                    button->onClick = [this, group = item->group, name = item->name]
                    {
                        choose (group, name);
                        // after the click has finished (the choice may have rebuilt everything)
                        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ArticulationPanel> (this)]
                                                         { if (safe != nullptr) safe->refresh(); });
                    };
                    addAndMakeVisible (*button);
                    rows.push_back (std::move (button));
                    y += rowHeight + 2;
                }
            }

            height = juce::jmax (height, y);
            x += columnWidth + gap;
        }

        setSize (juce::jmax (120, x), juce::jmax (height, gap + headerHeight + rowHeight) + gap);
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
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<std::unique_ptr<juce::Label>> labels;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArticulationPanel)
};
