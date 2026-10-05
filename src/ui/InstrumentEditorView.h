#pragma once

#include "../AudioEngine.h"
#include "../api/CommandDispatcher.h"
#include "ThemedLookAndFeel.h"

// Content view: one instrument's MIDI channels, 16 per port (a multiport plugin such as VE Pro has
// up to 16 ports; the port selector at the top picks which). Per channel: its name, its expression
// map (MILESTONES.md "MIDI track and instrument configuration views") and which tracks play it.
// Names and bindings of channels synced from a VE Pro server are immutable; their map is not.
// Back button or ESC returns to the rack.
class InstrumentEditorView final : public juce::Component
{
public:
    InstrumentEditorView (AudioEngine& e, CommandDispatcher& d) : engine (e), dispatcher (d)
    {
        backButton.setWantsKeyboardFocus (false);
        backButton.onClick = [this] { if (onBack) onBack(); };
        addAndMakeVisible (backButton);

        titleLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        addAndMakeVisible (titleLabel);

        guiButton.setWantsKeyboardFocus (false);
        guiButton.onClick = [this] { if (onOpenPluginGui) onOpenPluginGui (instrumentId); };
        addAndMakeVisible (guiButton);

        mapsButton.setWantsKeyboardFocus (false);
        mapsButton.setTooltip ("Create and edit the project's expression maps");
        mapsButton.onClick = [this] { if (onEditMap) onEditMap ({}); };
        addAndMakeVisible (mapsButton);

        portLabel.setText ("MIDI port", juce::dontSendNotification);
        portLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
        addAndMakeVisible (portLabel);

        portBox.setTooltip ("The plugin's own MIDI ports (VE Pro mirrors its server's port setting)");
        portBox.setWantsKeyboardFocus (false);
        portBox.onChange = [this]
        {
            port = juce::jmax (1, portBox.getSelectedId());
            refresh();
        };
        addAndMakeVisible (portBox);

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
    std::function<void (const juce::String&)> onEditMap;   // open the expression map editor (on that map, or the first)

    void setInstrument (AudioEngine::InstrumentId id)
    {
        instrumentId = id;
        port = 1;
        titleLabel.setText (engine.getInstrumentName (id), juce::dontSendNotification);

        portBox.clear (juce::dontSendNotification);

        for (int p = 1; p <= engine.getInstrumentMidiPortCount (id); ++p)
            portBox.addItem ("Port " + juce::String (p), p);

        portBox.setSelectedId (1, juce::dontSendNotification);
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
        header.removeFromRight (8);
        mapsButton.setBounds (header.removeFromRight (150));
        titleLabel.setBounds (header);

        area.removeFromTop (6);
        auto portRow = area.removeFromTop (26);
        portLabel.setBounds (portRow.removeFromLeft (70));
        portBox.setBounds (portRow.removeFromLeft (120));

        area.removeFromTop (8);
        viewport.setBounds (area);

        const auto width = juce::jmax (1, viewport.getMaximumVisibleWidth());
        rowContainer.setSize (width, 16 * rowHeight);

        for (size_t i = 0; i < channelRows.size(); ++i)
            channelRows[i]->setBounds (0, (int) i * rowHeight, width, rowHeight);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour (theme::Token::surfaceWindow));
    }

private:
    static constexpr int rowHeight = 34;
    static constexpr int noneId = 1, firstMapId = 2, missingId = 100000;

    struct ChannelRow final : juce::Component
    {
        ChannelRow (InstrumentEditorView& ownerToUse, int channelToUse) : owner (ownerToUse), channel (channelToUse)
        {
            nameLabel.setEditable (false, true);
            nameLabel.setColour (juce::Label::outlineColourId, juce::Colour (0xff3a3d44));
            nameLabel.onTextChange = [this]
            {
                owner.engine.setInstrumentChannelName (owner.instrumentId, channel, nameLabel.getText(), owner.port);
            };
            addAndMakeVisible (nameLabel);

            mapBox.setWantsKeyboardFocus (false);
            mapBox.setTooltip ("The expression map of this channel (shared by every track that plays it)");
            mapBox.onChange = [this] { chooseMap(); };
            addAndMakeVisible (mapBox);

            editButton.setWantsKeyboardFocus (false);
            editButton.setTooltip ("Edit this channel's expression map");
            editButton.onClick = [this] { if (owner.onEditMap) owner.onEditMap (currentMap); };
            addAndMakeVisible (editButton);
        }

        void refresh()
        {
            const auto channels = owner.engine.getInstrumentMidiChannels (owner.instrumentId);
            juce::String name;
            auto synced = false;
            currentMap = {};

            for (auto& info : channels)
                if (info.midiPort == owner.port && info.midiChannel == channel)
                {
                    name = info.name;
                    synced = info.synced;
                    currentMap = info.expressionMap;
                }

            // Synced channels: the server owns the name and the binding
            nameLabel.setEditable (false, ! synced);
            nameLabel.setColour (juce::Label::textColourId, synced ? juce::Colours::grey : juce::Colours::white);
            nameLabel.setTooltip (synced ? "Synced from the VE Pro server: the name can't be changed here" : "Double-click to name this channel");

            if (! nameLabel.isBeingEdited())
                nameLabel.setText (name, juce::dontSendNotification);

            updateMapBox();
            editButton.setEnabled (currentMap.isNotEmpty() && owner.engine.getExpressionMap (currentMap).has_value());
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto area = getLocalBounds().reduced (4, 2);

            g.setColour (juce::Colours::lightgrey);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Ch " + juce::String (channel), area.removeFromLeft (44), juce::Justification::centredLeft);

            // Assignments: tracks whose outputs target this instrument+port+channel
            juce::StringArray assigned;

            for (auto trackId : owner.engine.getTrackIds())
                for (auto& output : owner.engine.getTrackOutputs (trackId))
                    if (output.instrument == owner.instrumentId && output.midiChannel == channel && output.midiPort == owner.port)
                        assigned.addIfNotAlreadyThere (owner.engine.getTrackName (trackId));

            area.removeFromLeft (176 + 8 + 190 + 8 + 56 + 8);   // name, map, edit
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
            area.removeFromLeft (8);
            mapBox.setBounds (area.removeFromLeft (190));
            area.removeFromLeft (8);
            editButton.setBounds (area.removeFromLeft (56));
        }

    private:
        // The combo box lists "None", the project's maps, and - when the channel names a map the project
        // doesn't have - that name as missing (kept, so it can be fixed by creating the map)
        void updateMapBox()
        {
            juce::StringArray names;

            for (auto& map : owner.engine.getExpressionMaps())
                names.add (map.name);

            const auto key = names.joinIntoString ("\n") + "\x1f" + currentMap;

            if (key == lastKey)
                return;

            lastKey = key;
            mapBox.clear (juce::dontSendNotification);
            mapBox.addItem ("None", noneId);

            for (int i = 0; i < names.size(); ++i)
                mapBox.addItem (names[i], firstMapId + i);

            if (currentMap.isEmpty())
            {
                mapBox.setSelectedId (noneId, juce::dontSendNotification);
                return;
            }

            const auto index = names.indexOf (currentMap, true);

            if (index >= 0)
            {
                mapBox.setSelectedId (firstMapId + index, juce::dontSendNotification);
            }
            else
            {
                mapBox.addItem (currentMap + "  (missing)", missingId);
                mapBox.setSelectedId (missingId, juce::dontSendNotification);
            }
        }

        void chooseMap()
        {
            const auto id = mapBox.getSelectedId();

            if (id == missingId)
                return;   // not a choice: it only shows what is there

            const auto name = id == noneId ? juce::String() : mapBox.getText();

            auto params = new juce::DynamicObject();
            params->setProperty ("instrumentId", owner.instrumentId);
            params->setProperty ("channel", channel);
            params->setProperty ("port", owner.port);
            params->setProperty ("map", name);
            const auto reply = owner.dispatcher.run ("instrument.setChannelMap", juce::var (params));

            if (! (bool) reply["ok"])
                juce::Logger::writeToLog ("Instrument view: setChannelMap failed: " + reply["error"].toString());

            lastKey = {};
            refresh();
        }

        InstrumentEditorView& owner;
        const int channel;
        juce::Label nameLabel;
        juce::ComboBox mapBox;
        juce::TextButton editButton { "Edit" };
        juce::String currentMap, lastKey;
    };

    AudioEngine& engine;
    CommandDispatcher& dispatcher;
    AudioEngine::InstrumentId instrumentId = 0;
    int port = 1;

    juce::TextButton backButton { juce::String::fromUTF8 ("← Back") }, guiButton { "GUI" }, mapsButton { "Expression maps..." };
    juce::Label titleLabel, portLabel;
    juce::ComboBox portBox;
    juce::Viewport viewport;
    juce::Component rowContainer;
    std::vector<std::unique_ptr<ChannelRow>> channelRows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentEditorView)
};
