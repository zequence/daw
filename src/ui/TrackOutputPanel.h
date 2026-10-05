#pragma once

#include "../AudioEngine.h"
#include "../api/CommandDispatcher.h"

// MIDI track configuration (MILESTONES.md "MIDI track and instrument configuration views"): the port
// and the channel of a track's output, beyond port 1 (a multiport plugin such as VE Pro offers up to
// 16 ports of 16 channels). Shown in a callout from the track's context menu.
//
// The instrument is shown, not changed here ("Set output..." picks it). For a track that came from a
// VE Pro sync, the server decides instrument, port and channel: both choices are greyed out.
// Changes go through track.setOutput, like everything else.
class TrackOutputPanel final : public juce::Component
{
public:
    TrackOutputPanel (AudioEngine& e, CommandDispatcher& d, AudioEngine::TrackId id) : engine (e), dispatcher (d), trackId (id)
    {
        title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        title.setText (engine.getTrackName (trackId), juce::dontSendNotification);
        addAndMakeVisible (title);

        for (auto* l : { &instrumentLabel, &portLabel, &channelLabel })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
            addAndMakeVisible (l);
        }

        note.setFont (juce::FontOptions (12.0f));
        note.setColour (juce::Label::textColourId, juce::Colours::grey);
        note.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (note);

        portLabel.setText ("MIDI port", juce::dontSendNotification);
        channelLabel.setText ("MIDI channel", juce::dontSendNotification);

        for (auto* box : { &portBox, &channelBox })
        {
            box->setWantsKeyboardFocus (false);
            addAndMakeVisible (box);
        }

        portBox.onChange = [this] { apply(); };
        channelBox.onChange = [this] { apply(); };

        const auto outputs = engine.getTrackOutputs (trackId);

        if (outputs.empty())
        {
            instrumentLabel.setText ("No output yet. Use \"Set output...\" to choose an instrument.", juce::dontSendNotification);
            portBox.setEnabled (false);
            channelBox.setEnabled (false);
            note.setText ({}, juce::dontSendNotification);
            setSize (360, 150);
            return;
        }

        const auto output = outputs.front();
        instrumentId = output.instrument;
        instrumentLabel.setText ("Instrument: " + engine.getInstrumentName (instrumentId), juce::dontSendNotification);

        for (int p = 1; p <= engine.getInstrumentMidiPortCount (instrumentId); ++p)
            portBox.addItem ("Port " + juce::String (p), p);

        portBox.setSelectedId (output.midiPort, juce::dontSendNotification);
        fillChannels (output.midiPort);
        channelBox.setSelectedId (output.midiChannel, juce::dontSendNotification);

        // A synced track: the server owns the binding
        const auto info = engine.getTrackChannelInfo (trackId);
        immutable = info.has_value() && info->synced;

        if (immutable)
        {
            portBox.setEnabled (false);
            channelBox.setEnabled (false);
            note.setText ("This track comes from a VE Pro sync: its instrument, port and channel are set by the server.",
                          juce::dontSendNotification);
        }
        else
        {
            note.setText ("Port 1 is the plugin's own MIDI input; further ports are its extra MIDI event buses.",
                          juce::dontSendNotification);
        }

        setSize (360, 190);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        title.setBounds (area.removeFromTop (24));
        area.removeFromTop (4);
        instrumentLabel.setBounds (area.removeFromTop (22));
        area.removeFromTop (6);

        auto portRow = area.removeFromTop (26);
        portLabel.setBounds (portRow.removeFromLeft (110));
        portBox.setBounds (portRow.removeFromLeft (150));
        area.removeFromTop (6);

        auto channelRow = area.removeFromTop (26);
        channelLabel.setBounds (channelRow.removeFromLeft (110));
        channelBox.setBounds (channelRow.removeFromLeft (220));
        area.removeFromTop (8);

        note.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff23262b));
    }

private:
    // The channels of the chosen port, with the names they have been given
    void fillChannels (int port)
    {
        channelBox.clear (juce::dontSendNotification);

        for (int ch = 1; ch <= 16; ++ch)
        {
            const auto name = engine.getInstrumentChannelName (instrumentId, ch, port);
            channelBox.addItem ("Channel " + juce::String (ch) + (name.isNotEmpty() ? "  (" + name + ")" : juce::String()), ch);
        }
    }

    void apply()
    {
        if (immutable || instrumentId == 0)
            return;

        const auto port = juce::jmax (1, portBox.getSelectedId());

        // The channel names belong to a port: refill when the port changed, keeping the channel number
        if (port != lastPort)
        {
            const auto keep = juce::jmax (1, channelBox.getSelectedId());
            fillChannels (port);
            channelBox.setSelectedId (keep, juce::dontSendNotification);
            lastPort = port;
        }

        auto params = new juce::DynamicObject();
        params->setProperty ("trackId", trackId);
        params->setProperty ("instrumentId", instrumentId);
        params->setProperty ("channel", juce::jmax (1, channelBox.getSelectedId()));
        params->setProperty ("port", port);
        const auto reply = dispatcher.run ("track.setOutput", juce::var (params));

        if (! (bool) reply["ok"])
            juce::Logger::writeToLog ("Track output panel: track.setOutput failed: " + reply["error"].toString());
    }

    AudioEngine& engine;
    CommandDispatcher& dispatcher;
    const AudioEngine::TrackId trackId;
    AudioEngine::InstrumentId instrumentId = 0;
    bool immutable = false;
    int lastPort = 1;

    juce::Label title, instrumentLabel, portLabel, channelLabel, note;
    juce::ComboBox portBox, channelBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackOutputPanel)
};
