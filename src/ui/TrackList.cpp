#include "TrackList.h"

namespace
{
    constexpr int rowHeight = 56;
}

//==============================================================================
class TrackList::Row final : public juce::Component
{
public:
    Row (TrackList& ownerToUse, AudioEngine& engineToUse, AudioEngine::TrackId id)
        : owner (ownerToUse), engine (engineToUse), trackId (id)
    {
        nameLabel.setText (engine.getTrackName (trackId), juce::dontSendNotification);
        nameLabel.setEditable (false, true);
        nameLabel.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        nameLabel.setFont (juce::FontOptions (14.0f));
        nameLabel.onTextChange = [this]
        {
            engine.setTrackName (trackId, nameLabel.getText());
            nameLabel.setText (engine.getTrackName (trackId), juce::dontSendNotification);
        };
        nameLabel.setInterceptsMouseClicks (false, false);   // single click selects; double click edits

        armButton.setTooltip ("Arm for live input and recording");
        armButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::red.darker (0.2f));
        armButton.onClick = [this] { if (owner.onArm) owner.onArm (trackId); };

        editorButton.setTooltip ("Open the MIDI editor for this track");
        editorButton.onClick = [this] { if (owner.onOpenEditor) owner.onOpenEditor (trackId); };

        soloButton.setTooltip ("Solo (MIDI)");
        soloButton.setClickingTogglesState (true);
        soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::goldenrod);
        soloButton.onClick = [this] { engine.setTrackSoloed (trackId, soloButton.getToggleState()); };

        muteButton.setTooltip ("Mute (MIDI)");
        muteButton.setClickingTogglesState (true);
        muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orange.darker (0.3f));
        muteButton.onClick = [this] { engine.setTrackMuted (trackId, muteButton.getToggleState()); };

        instrumentButton.setTooltip ("Open this track's instrument (plugin GUI and rack entry)");
        instrumentButton.onClick = [this] { if (owner.onOpenInstrument) owner.onOpenInstrument (trackId); };

        recordModeButton.setTooltip ("Recording mode - Add: merge new takes into the clip. "
                                     "Rpl: from your first played note, existing material is replaced until you stop.");
        recordModeButton.onClick = [this]
        {
            engine.setTrackRecordReplace (trackId, ! engine.isTrackRecordReplace (trackId));
        };

        for (auto* c : std::initializer_list<juce::Component*> { &armButton, &editorButton, &soloButton,
                                                                 &muteButton, &instrumentButton, &recordModeButton })
        {
            c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }

        addAndMakeVisible (nameLabel);
    }

    AudioEngine::TrackId getTrackId() const noexcept { return trackId; }

    void refresh (bool isSelected, bool isArmed)
    {
        selected = isSelected;
        armButton.setToggleState (isArmed, juce::dontSendNotification);
        muteButton.setToggleState (engine.isTrackMuted (trackId), juce::dontSendNotification);
        soloButton.setToggleState (engine.isTrackSoloed (trackId), juce::dontSendNotification);
        instrumentButton.setEnabled (! engine.getTrackOutputs (trackId).empty());

        const auto replace = engine.isTrackRecordReplace (trackId);
        recordModeButton.setButtonText (replace ? "Rpl" : "Add");
        recordModeButton.setColour (juce::TextButton::buttonColourId,
                                    replace ? juce::Colours::darkred.darker (0.3f) : juce::Colour (0xff333842));

        if (! nameLabel.isBeingEdited())
            nameLabel.setText (engine.getTrackName (trackId), juce::dontSendNotification);

        repaint();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            if (owner.onShowContextMenu)
                owner.onShowContextMenu (trackId);
            return;
        }

        if (owner.onSelect)
            owner.onSelect (trackId);
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (nameLabel.getBounds().contains (event.getPosition()))
            nameLabel.showEditor();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (2.0f, 1.5f);

        g.setColour (selected ? juce::Colour (0xff39404d) : juce::Colour (0xff2b2e33));
        g.fillRoundedRectangle (bounds, 4.0f);

        if (selected)
        {
            g.setColour (juce::Colour (0xff6c87b5));
            g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 4);
        nameLabel.setBounds (area.removeFromTop (22));
        area.removeFromTop (2);

        auto buttons = area.removeFromTop (22);

        for (auto* b : std::initializer_list<juce::TextButton*> { &armButton, &editorButton, &soloButton,
                                                                  &muteButton, &instrumentButton })
        {
            b->setBounds (buttons.removeFromLeft (26));
            buttons.removeFromLeft (3);
        }

        recordModeButton.setBounds (buttons.removeFromLeft (34));
    }

private:
    TrackList& owner;
    AudioEngine& engine;
    const AudioEngine::TrackId trackId;

    juce::Label nameLabel;
    juce::TextButton armButton { "R" }, editorButton { "E" }, soloButton { "S" },
                     muteButton { "M" }, instrumentButton { "I" }, recordModeButton { "Add" };
    bool selected = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Row)
};

//==============================================================================
TrackList::TrackList (AudioEngine& e) : engine (e)
{
    addButton.setWantsKeyboardFocus (false);
    addButton.onClick = [this] { if (onAddTrack) onAddTrack(); };
    addAndMakeVisible (addButton);

    viewport.setViewedComponent (&rowContainer, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
}

TrackList::~TrackList() = default;

void TrackList::setSelectedTrack (AudioEngine::TrackId id)
{
    selectedTrack = id;
    refresh();
}

void TrackList::refresh()
{
    const auto ids = engine.getTrackIds();

    const auto needsRebuild = ids.size() != rows.size()
        || ! std::equal (ids.begin(), ids.end(), rows.begin(),
                         [] (auto id, const auto& row) { return row->getTrackId() == id; });

    if (needsRebuild)
        rebuildRows();

    for (auto& row : rows)
        row->refresh (row->getTrackId() == selectedTrack, row->getTrackId() == engine.getArmedTrack());
}

void TrackList::rebuildRows()
{
    rows.clear();

    for (auto id : engine.getTrackIds())
    {
        auto row = std::make_unique<Row> (*this, engine, id);
        rowContainer.addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }

    layoutRows();
}

void TrackList::layoutRows()
{
    const auto width = juce::jmax (1, viewport.getMaximumVisibleWidth());
    rowContainer.setSize (width, juce::jmax (1, (int) rows.size() * rowHeight));

    for (size_t i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, (int) i * rowHeight, width, rowHeight);
}

void TrackList::resized()
{
    auto area = getLocalBounds();
    addButton.setBounds (area.removeFromTop (30).reduced (6, 3));
    viewport.setBounds (area);
    layoutRows();
}

void TrackList::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232529));
}
