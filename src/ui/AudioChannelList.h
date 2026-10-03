#pragma once

#include "../AudioEngine.h"
#include "../engine/AudioChannelProcessor.h"

// The Audio-domain sidebar: one row per audio channel showing its input, a meter,
// mute and volume. Call refresh() from a UI timer.
class AudioChannelList final : public juce::Component
{
public:
    explicit AudioChannelList (AudioEngine& e) : engine (e)
    {
        viewport.setViewedComponent (&rowContainer, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }

    void refresh()
    {
        const auto ids = engine.getAudioChannelIds();

        const auto needsRebuild = ids.size() != rows.size()
            || ! std::equal (ids.begin(), ids.end(), rows.begin(),
                             [] (auto id, const auto& row) { return row->channelId == id; });

        if (needsRebuild)
            rebuildRows();

        for (auto& row : rows)
            row->refresh();
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        layoutRows();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232529));

        if (rows.empty())
        {
            g.setColour (juce::Colours::grey);
            g.setFont (juce::FontOptions (13.0f));
            g.drawFittedText ("No audio channels yet.\nAdd an instrument and its channel appears here.",
                              getLocalBounds().reduced (12), juce::Justification::centredTop, 4);
        }
    }

private:
    static constexpr int rowHeight = 64;

    struct Row final : juce::Component
    {
        Row (AudioEngine& engineToUse, AudioEngine::AudioChannelId id) : engine (engineToUse), channelId (id)
        {
            muteButton.setClickingTogglesState (true);
            muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orange.darker (0.3f));
            muteButton.setWantsKeyboardFocus (false);
            muteButton.onClick = [this]
            {
                if (auto* processor = engine.getAudioChannel (channelId))
                    processor->setMuted (muteButton.getToggleState());
            };
            addAndMakeVisible (muteButton);

            volumeSlider.setRange (-60.0, 6.0, 0.1);
            volumeSlider.setValue (0.0, juce::dontSendNotification);
            volumeSlider.setDoubleClickReturnValue (true, 0.0);
            volumeSlider.setSkewFactorFromMidPoint (-12.0);
            volumeSlider.setWantsKeyboardFocus (false);
            volumeSlider.onValueChange = [this]
            {
                if (auto* processor = engine.getAudioChannel (channelId))
                    processor->setGain (juce::Decibels::decibelsToGain ((float) volumeSlider.getValue(), -60.0f));
            };
            addAndMakeVisible (volumeSlider);
        }

        void refresh()
        {
            if (auto* processor = engine.getAudioChannel (channelId))
            {
                muteButton.setToggleState (processor->isMuted(), juce::dontSendNotification);
                meterLevel = juce::jmax (processor->getLastPeak(), meterLevel * 0.85f);
            }

            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat().reduced (2.0f, 1.5f);
            g.setColour (juce::Colour (0xff2b2e33));
            g.fillRoundedRectangle (bounds, 4.0f);

            auto area = getLocalBounds().reduced (8, 4);

            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.setFont (juce::FontOptions (14.0f));
            g.drawText (engine.getAudioChannelName (channelId), area.removeFromTop (18), juce::Justification::centredLeft);

            const auto input = engine.getAudioChannelInput (channelId);
            g.setColour (juce::Colours::grey);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (input != 0 ? juce::String::fromUTF8 ("← ") + engine.getInstrumentName (input)
                                   : juce::String ("no input"),
                        area.removeFromTop (14), juce::Justification::centredLeft);

            // Horizontal meter under the controls
            auto meter = getLocalBounds().reduced (8, 4).removeFromBottom (5).toFloat();
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRect (meter);

            const auto db = juce::Decibels::gainToDecibels (meterLevel, -60.0f);
            const auto proportion = juce::jlimit (0.0f, 1.0f, juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f));
            g.setColour (meterLevel >= 1.0f ? juce::Colours::red : juce::Colours::limegreen);
            g.fillRect (meter.withWidth (meter.getWidth() * proportion));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (8, 4);
            area.removeFromTop (34);
            area.removeFromBottom (7);
            muteButton.setBounds (area.removeFromLeft (24));
            area.removeFromLeft (4);
            volumeSlider.setBounds (area);
        }

        AudioEngine& engine;
        const AudioEngine::AudioChannelId channelId;
        juce::TextButton muteButton { "M" };
        juce::Slider volumeSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        float meterLevel = 0.0f;
    };

    void rebuildRows()
    {
        rows.clear();

        for (auto id : engine.getAudioChannelIds())
        {
            auto row = std::make_unique<Row> (engine, id);
            rowContainer.addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }

        layoutRows();
    }

    void layoutRows()
    {
        const auto width = juce::jmax (1, viewport.getMaximumVisibleWidth());
        rowContainer.setSize (width, juce::jmax (1, (int) rows.size() * rowHeight));

        for (size_t i = 0; i < rows.size(); ++i)
            rows[i]->setBounds (0, (int) i * rowHeight, width, rowHeight);
    }

    AudioEngine& engine;
    juce::Viewport viewport;
    juce::Component rowContainer;
    std::vector<std::unique_ptr<Row>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioChannelList)
};
