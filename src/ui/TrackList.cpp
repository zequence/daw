#include "TrackList.h"

namespace
{
    constexpr int rowHeight = 56;
    constexpr int folderRowHeight = 28;      // about half a channel row
    constexpr int indentPerLevel = 10;

    // The slight area on the left that shows what belongs to which folder.
    void paintIndentGuides (juce::Graphics& g, int depth, int height)
    {
        for (int level = 1; level <= depth; ++level)
        {
            g.setColour (juce::Colours::gold.withAlpha (0.18f + 0.04f * (float) level));
            g.fillRect (level * indentPerLevel - 6, 0, 2, height);
        }
    }
}

//==============================================================================
class TrackList::Row final : public juce::Component
{
public:
    Row (TrackList& ownerToUse, AudioEngine& engineToUse, AudioEngine::TrackId id, int depthToUse)
        : owner (ownerToUse), engine (engineToUse), trackId (id), depth (depthToUse)
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
        auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);

        g.setColour (selected ? juce::Colour (0xff39404d) : juce::Colour (0xff2b2e33));
        g.fillRoundedRectangle (bounds, 4.0f);

        if (selected)
        {
            g.setColour (juce::Colour (0xff6c87b5));
            g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
        }

        paintIndentGuides (g, depth, getHeight());
    }

    void resized() override
    {
        auto area = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).reduced (8, 4);
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
    const int depth;

    juce::Label nameLabel;
    juce::TextButton armButton { "R" }, editorButton { "E" }, soloButton { "S" },
                     muteButton { "M" }, instrumentButton { "I" }, recordModeButton { "Add" };
    bool selected = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Row)
};

//==============================================================================
class TrackList::FolderRow final : public juce::Component
{
public:
    FolderRow (TrackList& ownerToUse, AudioEngine& engineToUse, AudioEngine::FolderId id, int depthToUse)
        : owner (ownerToUse), engine (engineToUse), folderId (id), depth (depthToUse)
    {
        nameLabel.setText (engine.getFolderName (folderId), juce::dontSendNotification);
        nameLabel.setEditable (false, true);
        nameLabel.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        nameLabel.setColour (juce::Label::textColourId, juce::Colours::gold.withAlpha (0.85f));
        nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        nameLabel.onTextChange = [this]
        {
            engine.setFolderName (folderId, nameLabel.getText());
            nameLabel.setText (engine.getFolderName (folderId), juce::dontSendNotification);
        };
        nameLabel.setInterceptsMouseClicks (false, false);   // single click toggles; double click edits
        addAndMakeVisible (nameLabel);
    }

    void refresh()
    {
        if (! nameLabel.isBeingEdited())
            nameLabel.setText (engine.getFolderName (folderId), juce::dontSendNotification);

        repaint();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            owner.showFolderMenu (folderId);
            return;
        }

        engine.setFolderCollapsed (folderId, ! engine.isFolderCollapsed (folderId));
        owner.refresh();
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (nameLabel.getBounds().contains (event.getPosition()))
            nameLabel.showEditor();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);
        g.setColour (juce::Colour (0xff2e3038));
        g.fillRoundedRectangle (bounds, 4.0f);

        // Collapse triangle
        const auto collapsed = engine.isFolderCollapsed (folderId);
        juce::Path triangle;
        const auto cx = bounds.getX() + 13.0f, cy = bounds.getCentreY();

        if (collapsed)
            triangle.addTriangle (cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f, cx + 5.0f, cy);
        else
            triangle.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 5.0f);

        g.setColour (juce::Colours::gold.withAlpha (0.8f));
        g.fillPath (triangle);

        paintIndentGuides (g, depth, getHeight());
    }

    void resized() override
    {
        nameLabel.setBounds (getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 24).reduced (0, 2));
    }

private:
    TrackList& owner;
    AudioEngine& engine;
    const AudioEngine::FolderId folderId;
    const int depth;
    juce::Label nameLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FolderRow)
};

//==============================================================================
TrackList::TrackList (AudioEngine& e) : engine (e)
{
    addButton.setWantsKeyboardFocus (false);
    addButton.onClick = [this] { if (onAddTrack) onAddTrack(); };
    addAndMakeVisible (addButton);

    addFolderButton.setWantsKeyboardFocus (false);
    addFolderButton.setTooltip ("Add a folder for grouping tracks (right-click tracks and folders to move them)");
    addFolderButton.onClick = [this]
    {
        engine.addFolder (true);
        refresh();
    };
    addAndMakeVisible (addFolderButton);

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
    auto freshItems = engine.getSidebarItems (true, true);

    if (freshItems != items)
    {
        items = std::move (freshItems);
        rebuildRows();
    }

    for (size_t i = 0; i < rowComponents.size(); ++i)
    {
        if (auto* trackRow = dynamic_cast<Row*> (rowComponents[i].get()))
            trackRow->refresh (trackRow->getTrackId() == selectedTrack,
                               trackRow->getTrackId() == engine.getArmedTrack());
        else if (auto* folderRow = dynamic_cast<FolderRow*> (rowComponents[i].get()))
            folderRow->refresh();
    }
}

void TrackList::rebuildRows()
{
    rowComponents.clear();

    for (auto& item : items)
    {
        std::unique_ptr<juce::Component> row;

        if (item.folder != 0)
            row = std::make_unique<FolderRow> (*this, engine, item.folder, item.depth);
        else
            row = std::make_unique<Row> (*this, engine, item.member, item.depth);

        rowContainer.addAndMakeVisible (*row);
        rowComponents.push_back (std::move (row));
    }

    layoutRows();
}

void TrackList::layoutRows()
{
    const auto width = juce::jmax (1, viewport.getMaximumVisibleWidth());
    int y = 0;

    for (size_t i = 0; i < rowComponents.size(); ++i)
    {
        const auto height = items[i].folder != 0 ? folderRowHeight : rowHeight;
        rowComponents[i]->setBounds (0, y, width, height);
        y += height;
    }

    rowContainer.setSize (width, juce::jmax (1, y));
}

void TrackList::showFolderMenu (AudioEngine::FolderId folderId)
{
    const auto safe = juce::Component::SafePointer<TrackList> (this);
    juce::PopupMenu menu;

    menu.addItem ("New subfolder", [safe, folderId]
    {
        if (safe != nullptr)
        {
            safe->engine.addFolder (true, {}, folderId);
            safe->engine.setFolderCollapsed (folderId, false);
            safe->refresh();
        }
    });

    juce::PopupMenu moveTo;
    moveTo.addItem ("Top level", true, engine.getFolderParent (folderId) == 0, [safe, folderId]
    {
        if (safe != nullptr) { safe->engine.setFolderParent (folderId, 0); safe->refresh(); }
    });

    for (auto id : engine.getFolderIds (true))
    {
        if (id == folderId)
            continue;

        moveTo.addItem (engine.getFolderName (id), true, engine.getFolderParent (folderId) == id, [safe, folderId, id]
        {
            if (safe != nullptr) { safe->engine.setFolderParent (folderId, id); safe->refresh(); }
        });
    }

    menu.addSubMenu ("Move to folder", moveTo);
    menu.addSeparator();
    menu.addItem ("Remove folder (contents move up)", [safe, folderId]
    {
        if (safe != nullptr) { safe->engine.removeFolder (folderId); safe->refresh(); }
    });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

void TrackList::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (30).reduced (6, 3);
    addFolderButton.setBounds (header.removeFromRight (62));
    header.removeFromRight (4);
    addButton.setBounds (header);
    viewport.setBounds (area);
    layoutRows();
}

void TrackList::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232529));
}
