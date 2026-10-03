#include "TrackRow.h"
#include "engine/AudioChannelProcessor.h"

namespace
{
    constexpr float minDb = -60.0f;
}

TrackRow::TrackRow (AudioEngine& e, AudioEngine::TrackId id, const juce::String& name)
    : engine (e), trackId (id)
{
    armButton.setTooltip ("Arm: route live MIDI to this track's outputs and record onto it");
    armButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::red.darker (0.2f));
    armButton.onClick = [this] { if (onArmClicked) onArmClicked (trackId); };

    nameLabel.setText (name, juce::dontSendNotification);
    nameLabel.setEditable (false, true);
    nameLabel.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);

    outputButton.setTooltip ("Choose where this track sends MIDI");
    outputButton.onClick = [this] { if (onChooseOutput) onChooseOutput (trackId); };

    editButton.setTooltip ("Open the output instrument's editor");
    editButton.onClick = [this] { if (onOpenInstrument) onOpenInstrument (trackId); };

    demoButton.setTooltip ("Replace this track's clip with a two-bar demo clip");
    demoButton.onClick = [this] { if (onSetDemo) onSetDemo (trackId); };

    clearButton.setTooltip ("Delete this track's clip");
    clearButton.onClick = [this] { if (onClearClip) onClearClip (trackId); };

    soloButton.setTooltip ("Solo (MIDI): mute all other tracks");
    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::goldenrod);
    soloButton.onClick = [this] { engine.setTrackSoloed (trackId, soloButton.getToggleState()); };

    muteButton.setTooltip ("Mute (MIDI): stop this track's events; held notes are released");
    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orange.darker (0.3f));
    muteButton.onClick = [this] { engine.setTrackMuted (trackId, muteButton.getToggleState()); };

    volumeSlider.setTooltip ("Volume of the output instrument's audio channel");
    volumeSlider.setRange (minDb, 6.0, 0.1);
    volumeSlider.setValue (0.0, juce::dontSendNotification);
    volumeSlider.setDoubleClickReturnValue (true, 0.0);
    volumeSlider.setSkewFactorFromMidPoint (-12.0);
    volumeSlider.setTextValueSuffix (" dB");
    volumeSlider.setPopupDisplayEnabled (true, true, this);
    volumeSlider.onValueChange = [this]
    {
        if (auto* channel = getFirstOutputChannel())
            channel->setGain (juce::Decibels::decibelsToGain ((float) volumeSlider.getValue(), minDb));
    };

    removeButton.setTooltip ("Remove track");
    removeButton.onClick = [this] { if (onRemoveClicked) onRemoveClicked (trackId); };

    for (auto* c : std::initializer_list<juce::Component*> { &armButton, &nameLabel, &outputButton, &editButton,
                                                             &demoButton, &clearButton, &soloButton, &muteButton,
                                                             &volumeSlider, &removeButton })
    {
        c->setWantsKeyboardFocus (false);
        addAndMakeVisible (c);
    }
}

AudioChannelProcessor* TrackRow::getFirstOutputChannel() const
{
    const auto outputs = engine.getTrackOutputs (trackId);

    if (outputs.empty())
        return nullptr;

    return engine.getAudioChannel (engine.getAudioChannelForInstrument (outputs.front().instrument));
}

void TrackRow::setArmed (bool shouldBeArmed)
{
    armed = shouldBeArmed;
    armButton.setToggleState (armed, juce::dontSendNotification);
    repaint();
}

void TrackRow::refresh()
{
    const auto outputs = engine.getTrackOutputs (trackId);

    juce::String label = "(no output)";

    if (! outputs.empty())
    {
        label = engine.getInstrumentName (outputs.front().instrument) + " · ch " + juce::String (outputs.front().midiChannel);

        if (outputs.size() > 1)
            label << "  +" << juce::String ((int) outputs.size() - 1);
    }

    if (outputButton.getButtonText() != label)
        outputButton.setButtonText (label);

    editButton.setEnabled (! outputs.empty());
    clearButton.setEnabled (engine.getTrackSequence (trackId) != nullptr);
    muteButton.setToggleState (engine.isTrackMuted (trackId), juce::dontSendNotification);
    soloButton.setToggleState (engine.isTrackSoloed (trackId), juce::dontSendNotification);

    // Meter (interim: shows the first output's audio channel)
    float peak = 0.0f;

    if (auto* channel = getFirstOutputChannel())
        peak = channel->getLastPeak();

    const auto newLevel = juce::jmax (peak, meterLevel * 0.85f);

    if (std::abs (newLevel - meterLevel) > 0.001f)
    {
        meterLevel = newLevel;
        repaint();
    }
}

//==============================================================================
void TrackRow::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (2.0f);

    g.setColour (armed ? juce::Colour (0xff3a2f33) : juce::Colour (0xff2b2e33));
    g.fillRoundedRectangle (bounds, 4.0f);

    // Peak meter along the right edge.
    auto meter = getLocalBounds().removeFromRight (14).reduced (4, 6).toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (meter);

    const auto db = juce::Decibels::gainToDecibels (meterLevel, minDb);
    const auto proportion = juce::jlimit (0.0f, 1.0f, juce::jmap (db, minDb, 0.0f, 0.0f, 1.0f));
    g.setColour (meterLevel >= 1.0f ? juce::Colours::red : juce::Colours::limegreen);
    g.fillRect (meter.withTop (meter.getBottom() - meter.getHeight() * proportion));
}

void TrackRow::resized()
{
    auto area = getLocalBounds().reduced (8, 6);
    area.removeFromRight (14);

    armButton.setBounds (area.removeFromLeft (28));
    area.removeFromLeft (6);
    nameLabel.setBounds (area.removeFromLeft (140));
    area.removeFromLeft (6);
    removeButton.setBounds (area.removeFromRight (28));
    area.removeFromRight (6);
    volumeSlider.setBounds (area.removeFromRight (120));
    area.removeFromRight (6);
    muteButton.setBounds (area.removeFromRight (28));
    area.removeFromRight (4);
    soloButton.setBounds (area.removeFromRight (28));
    area.removeFromRight (6);
    clearButton.setBounds (area.removeFromRight (48));
    area.removeFromRight (4);
    demoButton.setBounds (area.removeFromRight (48));
    area.removeFromRight (6);
    editButton.setBounds (area.removeFromRight (44));
    area.removeFromRight (6);
    outputButton.setBounds (area);
}
