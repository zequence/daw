#include "TrackRow.h"
#include "TrackChannelProcessor.h"

namespace
{
    constexpr int noInstrumentMenuId = 1;
    constexpr float minDb = -60.0f;
}

TrackRow::TrackRow (AudioEngine& e, AudioEngine::TrackId id, const juce::String& name)
    : engine (e), trackId (id)
{
    armButton.setTooltip ("Arm: route live MIDI to this track");
    armButton.setClickingTogglesState (false);
    armButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::red.darker (0.2f));
    armButton.onClick = [this] { if (onArmClicked) onArmClicked (trackId); };

    nameLabel.setText (name, juce::dontSendNotification);
    nameLabel.setEditable (false, true);
    nameLabel.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);

    instrumentButton.setTooltip ("Choose an instrument plugin");
    instrumentButton.onClick = [this] { showInstrumentMenu(); };

    editButton.setTooltip ("Open the instrument's editor");
    editButton.setEnabled (false);
    editButton.onClick = [this] { openEditor(); };

    demoButton.setTooltip ("Replace this track's clip with a two-bar demo clip");
    demoButton.onClick = [this] { if (onSetDemo) onSetDemo (trackId); };

    clearButton.setTooltip ("Delete this track's clip");
    clearButton.setEnabled (false);
    clearButton.onClick = [this] { if (onClearClip) onClearClip (trackId); };

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orange.darker (0.3f));
    muteButton.onClick = [this]
    {
        if (auto* channel = engine.getChannel (trackId))
            channel->setMuted (muteButton.getToggleState());
    };

    volumeSlider.setRange (minDb, 6.0, 0.1);
    volumeSlider.setValue (0.0, juce::dontSendNotification);
    volumeSlider.setDoubleClickReturnValue (true, 0.0);
    volumeSlider.setSkewFactorFromMidPoint (-12.0);
    volumeSlider.setTextValueSuffix (" dB");
    volumeSlider.setPopupDisplayEnabled (true, true, this);
    volumeSlider.onValueChange = [this]
    {
        if (auto* channel = engine.getChannel (trackId))
            channel->setGain (juce::Decibels::decibelsToGain ((float) volumeSlider.getValue(), minDb));
    };

    removeButton.setTooltip ("Remove track");
    removeButton.onClick = [this] { if (onRemoveClicked) onRemoveClicked (trackId); };

    for (auto* c : std::initializer_list<juce::Component*> { &armButton, &nameLabel, &instrumentButton, &editButton,
                                                             &demoButton, &clearButton, &muteButton, &volumeSlider,
                                                             &removeButton })
    {
        c->setWantsKeyboardFocus (false);
        addAndMakeVisible (c);
    }
}

TrackRow::~TrackRow()
{
    closeEditor();
}

void TrackRow::setArmed (bool shouldBeArmed)
{
    armed = shouldBeArmed;
    armButton.setToggleState (armed, juce::dontSendNotification);
    repaint();
}

void TrackRow::updateMeter()
{
    clearButton.setEnabled (engine.getTrackSequence (trackId) != nullptr);

    if (auto* channel = engine.getChannel (trackId))
    {
        const auto peak = channel->takePeak();
        const auto newLevel = juce::jmax (peak, meterLevel * 0.85f);

        if (std::abs (newLevel - meterLevel) > 0.001f)
        {
            meterLevel = newLevel;
            repaint();
        }
    }
}

//==============================================================================
void TrackRow::showInstrumentMenu()
{
    const auto types = engine.getInstrumentTypes();

    juce::PopupMenu menu;
    menu.addItem (noInstrumentMenuId, "None", true, engine.getInstrument (trackId) == nullptr);
    menu.addSeparator();

    if (types.isEmpty())
        menu.addItem (-1, "No instruments found - use Plugins... to scan", false, false);
    else
        juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortByManufacturer);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (instrumentButton),
                        [safe = juce::Component::SafePointer<TrackRow> (this), types] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;

                            if (result == noInstrumentMenuId)
                            {
                                safe->clearInstrument();
                                return;
                            }

                            const auto index = juce::KnownPluginList::getIndexChosenByMenu (types, result);

                            if (juce::isPositiveAndBelow (index, types.size()))
                                safe->loadInstrument (types.getReference (index));
                        });
}

void TrackRow::loadInstrument (const juce::PluginDescription& description)
{
    closeEditor();
    instrumentButton.setButtonText ("Loading " + description.name + "...");
    instrumentButton.setEnabled (false);
    editButton.setEnabled (false);

    engine.loadInstrument (trackId, description,
        [safe = juce::Component::SafePointer<TrackRow> (this), description] (bool ok, const juce::String& error)
        {
            if (safe == nullptr)
                return;

            safe->instrumentButton.setEnabled (true);

            if (! ok)
            {
                safe->instrumentButton.setButtonText ("(no instrument)");
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                        "Couldn't load plugin",
                                                        description.name + "\n\n" + error);
                return;
            }

            safe->instrumentButton.setButtonText (description.name);
            safe->editButton.setEnabled (true);

            if (safe->nameLabel.getText().startsWith ("Track "))
                safe->nameLabel.setText (description.name, juce::dontSendNotification);

            safe->openEditor();
        });
}

void TrackRow::clearInstrument()
{
    closeEditor();
    engine.clearInstrument (trackId);
    instrumentButton.setButtonText ("(no instrument)");
    instrumentButton.setEnabled (true);
    editButton.setEnabled (false);
}

void TrackRow::openEditor()
{
    if (pluginWindow != nullptr)
    {
        pluginWindow->setVisible (true);
        pluginWindow->toFront (true);
        return;
    }

    if (auto* plugin = engine.getInstrument (trackId))
    {
        pluginWindow = std::make_unique<PluginWindow> (*plugin, nameLabel.getText() + " - " + plugin->getName());
        pluginWindow->onClose = [safe = juce::Component::SafePointer<TrackRow> (this)]
        {
            // Defer deletion: we're inside the window's own callback.
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->closeEditor(); });
        };
    }
}

void TrackRow::closeEditor()
{
    pluginWindow.reset();
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
    nameLabel.setBounds (area.removeFromLeft (160));
    area.removeFromLeft (6);
    removeButton.setBounds (area.removeFromRight (28));
    area.removeFromRight (6);
    volumeSlider.setBounds (area.removeFromRight (140));
    area.removeFromRight (6);
    muteButton.setBounds (area.removeFromRight (28));
    area.removeFromRight (6);
    clearButton.setBounds (area.removeFromRight (50));
    area.removeFromRight (6);
    demoButton.setBounds (area.removeFromRight (52));
    area.removeFromRight (6);
    editButton.setBounds (area.removeFromRight (50));
    area.removeFromRight (6);
    instrumentButton.setBounds (area);
}
