#pragma once

#include "../AudioEngine.h"

// Content view: one instrument's 16 MIDI channels - editable names plus which tracks
// are assigned to each. Back button or ESC returns to the rack.
class InstrumentEditorView final : public juce::Component
{
public:
    explicit InstrumentEditorView (AudioEngine& e) : engine (e)
    {
        backButton.setWantsKeyboardFocus (false);
        backButton.onClick = [this] { if (onBack) onBack(); };
        addAndMakeVisible (backButton);

        titleLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        addAndMakeVisible (titleLabel);

        guiButton.setWantsKeyboardFocus (false);
        guiButton.onClick = [this] { if (onOpenPluginGui) onOpenPluginGui (instrumentId); };
        addAndMakeVisible (guiButton);

        viewport.setViewedComponent (&rowContainer, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        for (int ch = 1; ch <= 16; ++ch)
        {
            auto row = std::make_unique<ChannelRow> (*this, ch);
            rowContainer.addAndMakeVisible (*row);
            channelRows.push_back (std::move (row));
        }
    }

    std::function<void()> onBack;
    std::function<void (AudioEngine::InstrumentId)> onOpenPluginGui;

    void setInstrument (AudioEngine::InstrumentId id)
    {
        instrumentId = id;
        titleLabel.setText (engine.getInstrumentName (id), juce::dontSendNotification);
        refresh();
    }

    AudioEngine::InstrumentId getInstrument() const noexcept { return instrumentId; }

    void refresh()
    {
        for (auto& row : channelRows)
            row->refresh();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        auto header = area.removeFromTop (34);
        backButton.setBounds (header.removeFromLeft (70));
        header.removeFromLeft (10);
        guiButton.setBounds (header.removeFromRight (60));
        titleLabel.setBounds (header);

        area.removeFromTop (8);
        viewport.setBounds (area);

        const auto width = juce::jmax (1, viewport.getMaximumVisibleWidth());
        rowContainer.setSize (width, 16 * rowHeight);

        for (size_t i = 0; i < channelRows.size(); ++i)
            channelRows[i]->setBounds (0, (int) i * rowHeight, width, rowHeight);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1d1f23));
    }

private:
    static constexpr int rowHeight = 34;

    struct ChannelRow final : juce::Component
    {
        ChannelRow (InstrumentEditorView& ownerToUse, int channelToUse) : owner (ownerToUse), channel (channelToUse)
        {
            nameLabel.setEditable (false, true);
            nameLabel.setColour (juce::Label::outlineColourId, juce::Colour (0xff3a3d44));
            nameLabel.onTextChange = [this]
            {
                owner.engine.setInstrumentChannelName (owner.instrumentId, channel, nameLabel.getText());
            };
            addAndMakeVisible (nameLabel);
        }

        void refresh()
        {
            if (! nameLabel.isBeingEdited())
                nameLabel.setText (owner.engine.getInstrumentChannelName (owner.instrumentId, channel),
                                   juce::dontSendNotification);

            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto area = getLocalBounds().reduced (4, 2);

            g.setColour (juce::Colours::lightgrey);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Ch " + juce::String (channel), area.removeFromLeft (44), juce::Justification::centredLeft);

            // Assignments: tracks whose outputs target this instrument+channel
            juce::StringArray assigned;

            for (auto trackId : owner.engine.getTrackIds())
                for (auto& output : owner.engine.getTrackOutputs (trackId))
                    if (output.instrument == owner.instrumentId && output.midiChannel == channel)
                        assigned.addIfNotAlreadyThere (owner.engine.getTrackName (trackId));

            area.removeFromLeft (184);   // name editor space
            g.setColour (assigned.isEmpty() ? juce::Colours::darkgrey : juce::Colours::skyblue.withAlpha (0.8f));
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (assigned.isEmpty() ? "-" : assigned.joinIntoString (", "),
                        area.reduced (6, 0), juce::Justification::centredLeft);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (4, 4);
            area.removeFromLeft (44);
            nameLabel.setBounds (area.removeFromLeft (176));
        }

        InstrumentEditorView& owner;
        const int channel;
        juce::Label nameLabel;
    };

    AudioEngine& engine;
    AudioEngine::InstrumentId instrumentId = 0;

    juce::TextButton backButton { juce::String::fromUTF8 ("← Back") }, guiButton { "GUI" };
    juce::Label titleLabel;
    juce::Viewport viewport;
    juce::Component rowContainer;
    std::vector<std::unique_ptr<ChannelRow>> channelRows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentEditorView)
};
