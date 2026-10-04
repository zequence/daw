#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../api/CommandDispatcher.h"
#include "Theme.h"

// Content view: the global action history. Purely command-driven - it reads
// history.list and clicks become history.travel, the same surface agents use.
// Entries after the current position (a travelled-back state) render greyed.
class HistoryView final : public juce::Component,
                          private juce::ListBoxModel,
                          private juce::Timer
{
public:
    explicit HistoryView (CommandDispatcher& d) : dispatcher (d)
    {
        titleLabel.setText ("History", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        addAndMakeVisible (titleLabel);

        filterBox.addItem ("All", 1);
        filterBox.addItem ("Tracks", 2);
        filterBox.addItem ("Clips", 3);
        filterBox.addItem ("Instruments", 4);
        filterBox.addItem ("Markers", 5);
        filterBox.addItem ("Tempo", 6);
        filterBox.addItem ("Recording", 7);
        filterBox.addItem ("Project", 8);
        filterBox.addItem ("Selected track", 9);
        filterBox.setSelectedId (1, juce::dontSendNotification);
        filterBox.setWantsKeyboardFocus (false);
        filterBox.onChange = [this] { refresh(); };
        addAndMakeVisible (filterBox);

        list.setModel (this);
        list.setRowHeight (26);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff1d1f23));
        addAndMakeVisible (list);

        startTimerHz (2);   // the list is cheap to rebuild
    }

    void setSelectedTrack (int trackId) { selectedTrack = trackId; }

    void refresh()
    {
        auto params = new juce::DynamicObject();

        switch (filterBox.getSelectedId())
        {
            case 2: params->setProperty ("category", "track"); break;
            case 3: params->setProperty ("category", "clip"); break;
            case 4: params->setProperty ("category", "instrument"); break;
            case 5: params->setProperty ("category", "marker"); break;
            case 6: params->setProperty ("category", "tempo"); break;
            case 7: params->setProperty ("category", "recording"); break;
            case 8: params->setProperty ("category", "project"); break;
            case 9: params->setProperty ("trackId", selectedTrack); break;
            default: break;
        }

        const auto reply = dispatcher.run ("history.list", juce::var (params));
        const auto result = reply.getProperty ("result", {});

        if (auto* array = result.getArray())
        {
            rows = *array;
            currentId = 0;

            for (auto& row : rows)
                if ((bool) row.getProperty ("current", false))
                    currentId = (int) row.getProperty ("id", 0);

            list.updateContent();
            list.repaint();

            // Keep the current entry visible
            for (int i = 0; i < rows.size(); ++i)
                if ((int) rows[i].getProperty ("id", 0) == currentId)
                    list.scrollToEnsureRowIsOnscreen (i);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        auto header = area.removeFromTop (32);
        filterBox.setBounds (header.removeFromRight (160).reduced (0, 3));
        titleLabel.setBounds (header);
        area.removeFromTop (6);
        list.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1d1f23));
    }

    void visibilityChanged() override
    {
        if (isVisible())
            refresh();
    }

private:
    //==============================================================================
    int getNumRows() override { return rows.size(); }

    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool) override
    {
        if (rowNumber >= rows.size())
            return;

        const auto& row = rows.getReference (rowNumber);
        const auto id = (int) row.getProperty ("id", 0);
        const auto isCurrent = id == currentId;
        const auto isFuture = id > currentId;

        if (isCurrent)
        {
            g.setColour (theme::colour (theme::Token::selectionBg));
            g.fillRect (0, 0, width, height);
            g.setColour (theme::colour (theme::Token::selectionBorder));
            g.drawRect (0, 0, width, height);
        }

        g.setColour (isFuture ? juce::Colours::grey.withAlpha (0.5f)
                              : (isCurrent ? juce::Colours::white : juce::Colours::lightgrey));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (row.getProperty ("time", {}).toString(), 8, 0, 64, height, juce::Justification::centredLeft);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (row.getProperty ("description", {}).toString(), 76, 0, width - 180, height,
                    juce::Justification::centredLeft);
        g.setColour (juce::Colours::grey.withAlpha (isFuture ? 0.4f : 0.8f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (row.getProperty ("category", {}).toString(), width - 96, 0, 88, height,
                    juce::Justification::centredRight);
    }

    void listBoxItemClicked (int rowNumber, const juce::MouseEvent&) override
    {
        if (rowNumber >= rows.size())
            return;

        auto params = new juce::DynamicObject();
        params->setProperty ("id", rows[rowNumber].getProperty ("id", 0));
        dispatcher.run ("history.travel", juce::var (params));
        refresh();
    }

    void timerCallback() override
    {
        if (isShowing())
            refresh();
    }

    CommandDispatcher& dispatcher;
    juce::Label titleLabel;
    juce::ComboBox filterBox;
    juce::ListBox list;
    juce::Array<juce::var> rows;
    int currentId = 0, selectedTrack = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HistoryView)
};
