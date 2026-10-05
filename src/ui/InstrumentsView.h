#pragma once

#include "../AudioEngine.h"
#include "Theme.h"

// Content view: the instrument rack. Instruments used by the focused track sort to the
// top of the list. Call refresh() from a UI timer.
class InstrumentsView final : public juce::Component
{
public:
    explicit InstrumentsView (AudioEngine& e) : engine (e)
    {
        syncButton.setWantsKeyboardFocus (false);
        syncButton.setTooltip ("Create a connected VE Pro instrument, named MIDI channels and a track "
                               "per player for every instance on the Vienna Ensemble Pro server "
                               "(server address in Settings > Integrations)");
        syncButton.onClick = [this] { if (onVeproSync) onVeproSync(); };
        addAndMakeVisible (syncButton);

        viewport.setViewedComponent (&rowContainer, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }

    std::function<void (AudioEngine::InstrumentId)> onOpenPluginGui, onEditInstrument, onRemoveInstrument;
    std::function<void()> onAddInstrument, onVeproSync;

    // Instruments fed by this track float to the top and get a highlight.
    void focusTrack (AudioEngine::TrackId trackId)
    {
        focusedInstruments.clear();

        for (auto& output : engine.getTrackOutputs (trackId))
            focusedInstruments.push_back (output.instrument);

        lastOrder.clear();   // force a rebuild in the new order
        refresh();
        viewport.setViewPosition (0, 0);
    }

    void refresh()
    {
        auto order = sortedIds();

        if (order != lastOrder)
        {
            lastOrder = order;
            rebuildRows();
        }

        for (auto& row : rows)
            row->refresh();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        auto header = area.removeFromTop (30);
        syncButton.setBounds (header.removeFromLeft (170).reduced (0, 2));
        area.removeFromTop (6);
        viewport.setBounds (area);
        layoutRows();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1d1f23));

        if (rows.empty())
        {
            g.setColour (juce::Colours::grey);
            g.setFont (juce::FontOptions (14.0f));
            g.drawFittedText ("No instruments loaded.\n\nAdd one here, or pick \"New instrument...\" "
                              "when choosing a track's output.",
                              getLocalBounds().reduced (24), juce::Justification::centred, 6);
        }
    }

private:
    static constexpr int rowHeight = 58;

    struct Row final : juce::Component
    {
        Row (InstrumentsView& ownerToUse, AudioEngine::InstrumentId id) : owner (ownerToUse), instrumentId (id)
        {
            guiButton.setWantsKeyboardFocus (false);
            guiButton.onClick = [this] { if (owner.onOpenPluginGui) owner.onOpenPluginGui (instrumentId); };
            addAndMakeVisible (guiButton);

            editButton.setWantsKeyboardFocus (false);
            editButton.onClick = [this] { if (owner.onEditInstrument) owner.onEditInstrument (instrumentId); };
            addAndMakeVisible (editButton);

            removeButton.setWantsKeyboardFocus (false);
            removeButton.setTooltip ("Remove this instrument (asks what to do with the tracks playing it)");
            removeButton.onClick = [this] { if (owner.onRemoveInstrument) owner.onRemoveInstrument (instrumentId); };
            addAndMakeVisible (removeButton);
        }

        void refresh() { repaint(); }

        void paint (juce::Graphics& g) override
        {
            const auto focused = std::find (owner.focusedInstruments.begin(), owner.focusedInstruments.end(),
                                            instrumentId) != owner.focusedInstruments.end();

            auto bounds = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (juce::Colour (0xff2b2e33));
            g.fillRoundedRectangle (bounds, 4.0f);

            if (focused)
            {
                g.setColour (theme::colour (theme::Token::selectionBorder));
                g.drawRoundedRectangle (bounds, 4.0f, 1.2f);
            }

            auto area = getLocalBounds().reduced (12, 8);

            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.setFont (juce::FontOptions (16.0f));
            g.drawText (owner.engine.getInstrumentName (instrumentId), area.removeFromTop (22),
                        juce::Justification::centredLeft);

            // Which tracks feed this instrument?
            juce::StringArray feeders;

            for (auto trackId : owner.engine.getTrackIds())
                for (auto& output : owner.engine.getTrackOutputs (trackId))
                    if (output.instrument == instrumentId)
                        feeders.addIfNotAlreadyThere (owner.engine.getTrackName (trackId)
                                                      + (output.midiPort > 1
                                                             ? " (p" + juce::String (output.midiPort) + "."
                                                                 + juce::String (output.midiChannel) + ")"
                                                             : " (ch " + juce::String (output.midiChannel) + ")"));

            g.setColour (juce::Colours::grey);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (feeders.isEmpty() ? "no tracks routed here" : feeders.joinIntoString (", "),
                        area.removeFromTop (18), juce::Justification::centredLeft);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (12, 14);
            removeButton.setBounds (area.removeFromRight (70));
            area.removeFromRight (6);
            editButton.setBounds (area.removeFromRight (54));
            area.removeFromRight (6);
            guiButton.setBounds (area.removeFromRight (54));
        }

        InstrumentsView& owner;
        const AudioEngine::InstrumentId instrumentId;
        juce::TextButton guiButton { "GUI" }, editButton { "Edit" }, removeButton { "Remove" };
    };

    std::vector<AudioEngine::InstrumentId> sortedIds() const
    {
        std::vector<AudioEngine::InstrumentId> ids;

        for (auto& [id, name] : engine.getInstruments())
            ids.push_back (id);

        std::stable_partition (ids.begin(), ids.end(), [this] (auto id)
        {
            return std::find (focusedInstruments.begin(), focusedInstruments.end(), id) != focusedInstruments.end();
        });

        return ids;
    }

    void rebuildRows()
    {
        rows.clear();

        for (auto id : lastOrder)
        {
            auto row = std::make_unique<Row> (*this, id);
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
    juce::TextButton syncButton { "Sync to VE Pro Server" };
    juce::Viewport viewport;
    juce::Component rowContainer;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<AudioEngine::InstrumentId> focusedInstruments, lastOrder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentsView)
};
