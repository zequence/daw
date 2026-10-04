#include "TrackList.h"
#include "ColorPalette.h"
#include "Theme.h"

namespace
{
    using sidebar::indentPerLevel;
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

        owner.rowMouseDown (this, false, trackId, event);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.rowMouseDrag (this, false, trackId, event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.finishRowDrag (trackId);
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (nameLabel.getBounds().contains (event.getPosition()))
            nameLabel.showEditor();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);

        theme::paintRowBox (g, bounds, false, selected);

        // The track color shows as a left border only; uncolored = grey (ISSUES.md)
        g.setColour (AudioEngine::colourFromHex (engine.getTrackColour (trackId), juce::Colour (0xff6d7178)));
        g.fillRect (bounds.getX() + 1.0f, bounds.getY() + 2.0f, 4.0f, bounds.getHeight() - 4.0f);

        if (selected)
        {
            g.setColour (juce::Colour (0xff6c87b5));
            g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
        }
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
        // Same text color as the tracks; the bold smaller font sets folders apart (ISSUES.md)
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

        owner.rowMouseDown (this, true, folderId, event);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.rowMouseDrag (this, true, folderId, event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            return;

        // A click (no drag happened): the arrow toggles collapse; the rest of
        // the row selects the folder (ISSUES.md) - which selects every track
        // inside it, ready for multi-channel work.
        if (! owner.finishRowDrag (folderId))
        {
            if (event.x < depth * indentPerLevel + 26)
                engine.setFolderCollapsed (folderId, ! engine.isFolderCollapsed (folderId));
            else
                owner.selectFolder (folderId);
        }
        // finishRowDrag already scheduled the refresh (deferred: it may delete this row)
    }

    void setSelected (bool shouldBeSelected)
    {
        if (selected != shouldBeSelected)
        {
            selected = shouldBeSelected;
            repaint();
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (nameLabel.getBounds().contains (event.getPosition()))
            nameLabel.showEditor();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);
        theme::paintRowBox (g, bounds, true, selected);

        // Folder color as a left border; uncolored = grey (like tracks)
        g.setColour (AudioEngine::colourFromHex (engine.getFolderColour (folderId), juce::Colour (0xff6d7178)));
        g.fillRect (bounds.getX() + 1.0f, bounds.getY() + 2.0f, 4.0f, bounds.getHeight() - 4.0f);

        // Collapse triangle
        const auto collapsed = engine.isFolderCollapsed (folderId);
        juce::Path triangle;
        const auto cx = bounds.getX() + 13.0f, cy = bounds.getCentreY();

        if (collapsed)
            triangle.addTriangle (cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f, cx + 5.0f, cy);
        else
            triangle.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 5.0f);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillPath (triangle);
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
    bool selected = false;

public:
    AudioEngine::FolderId getFolderId() const noexcept { return folderId; }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FolderRow)
};

//==============================================================================
TrackList::TrackList (AudioEngine& e, sidebar::VerticalScroll& v) : engine (e), vscroll (v)
{
    // Adding tracks/folders lives in the right-click menus (ISSUES.md: header
    // buttons removed)
    viewport.setViewedComponent (&rowContainer, false);
    viewport.setScrollBarsShown (false, false, true, false);   // no scrollbar; the wheel still scrolls (ISSUES.md)
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

    // Two-way sync with the shared vertical scroll (the arrangement is on the
    // same Y axis): follow it when someone else moved it, push our own scrolling.
    if (vscroll.revision != lastScrollRevision)
        viewport.setViewPosition (viewport.getViewPositionX(), vscroll.y);
    else if (viewport.getViewPositionY() != vscroll.y)
        vscroll.set (viewport.getViewPositionY());

    vscroll.set (viewport.getViewPositionY());   // viewport clamping wins
    lastScrollRevision = vscroll.revision;

    realizeVisibleRows();   // scrolling brings new rows into existence

    // Drop selections that no longer exist
    if (! multiSelection.empty())
    {
        const auto trackIds = engine.getTrackIds();
        const std::set<AudioEngine::TrackId> existing (trackIds.begin(), trackIds.end());
        std::erase_if (multiSelection, [&existing] (auto id) { return existing.count (id) == 0; });
    }

    // Only live (near-visible) rows refresh. A selected folder takes the
    // selection highlight away from tracks (ISSUES.md).
    for (auto& [key, component] : liveRows)
    {
        if (auto* trackRow = dynamic_cast<Row*> (component.get()))
            trackRow->refresh (selectedFolder == 0
                                 && (multiSelection.empty() ? trackRow->getTrackId() == selectedTrack
                                                            : multiSelection.count (trackRow->getTrackId()) > 0),
                               engine.isTrackArmed (trackRow->getTrackId()));
        else if (auto* folderRow = dynamic_cast<FolderRow*> (component.get()))
        {
            folderRow->setSelected (folderRow->getFolderId() == selectedFolder);
            folderRow->refresh();
        }
    }
}

void TrackList::selectFolder (AudioEngine::FolderId folderId)
{
    // Only the folder itself is selected (ISSUES.md: no auto-selecting its
    // tracks, and any track selection goes away)
    selectedFolder = folderId;
    multiSelection.clear();

    if (onSelectionChanged)
        onSelectionChanged (multiSelection);

    refreshSoon();
}

void TrackList::setSubtreeCollapsed (AudioEngine::FolderId folderId, bool collapsed)
{
    // The folder and every folder below it
    const auto all = engine.getSidebarItems (true, false);
    int folderDepth = -1;

    for (auto& item : all)
    {
        if (folderDepth < 0)
        {
            if (item.folder == folderId)
            {
                folderDepth = item.depth;
                engine.setFolderCollapsed (folderId, collapsed);
            }

            continue;
        }

        if (item.depth <= folderDepth)
            break;   // left the subtree

        if (item.folder != 0)
            engine.setFolderCollapsed (item.folder, collapsed);
    }

    refreshSoon();
}

int TrackList::heightOfItem (const AudioEngine::SidebarItem& item)
{
    return sidebar::heightOf (item);
}

// The list is VIRTUALIZED: only rows near the visible area own components
// (big projects have 1000+ tracks; building them all made collapse/expand
// and every UI tick slow). Rows are keyed by (folder, member, depth), so a
// row survives as long as its item does and scrolling only creates the rows
// that come into view.
void TrackList::rebuildRows()
{
    rowTops.clear();
    int y = 0;

    for (auto& item : items)
    {
        rowTops.push_back (y);
        y += heightOfItem (item);
    }

    totalHeight = y;
    layoutRows();
}

void TrackList::layoutRows()
{
    // At least viewport height, so right-clicking the empty area reaches the container
    rowContainer.setSize (juce::jmax (1, viewport.getMaximumVisibleWidth()),
                          juce::jmax (1, totalHeight, viewport.getHeight()));
    realizeVisibleRows();
}

void TrackList::realizeVisibleRows()
{
    const auto width = juce::jmax (1, viewport.getMaximumVisibleWidth());
    const auto margin = juce::jmax (200, viewport.getHeight());   // a screen of slack each way
    const auto top = viewport.getViewPositionY() - margin;
    const auto bottom = viewport.getViewPositionY() + viewport.getHeight() + margin;

    std::set<RowKey> wanted;

    for (size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];
        const auto height = heightOfItem (item);

        if (rowTops[i] + height < top || rowTops[i] > bottom)
            continue;

        const RowKey key { item.folder, item.member, item.depth };
        wanted.insert (key);

        auto& row = liveRows[key];

        if (row == nullptr)
        {
            if (item.folder != 0)
                row = std::make_unique<FolderRow> (*this, engine, item.folder, item.depth);
            else
                row = std::make_unique<Row> (*this, engine, item.member, item.depth);

            rowContainer.addAndMakeVisible (*row);
        }

        row->setBounds (0, rowTops[i], width, height);
    }

    // Retire rows that left the window - but never while a mouse button is down:
    // the row under the cursor may be mid-gesture (drag/click).
    if (juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        return;

    for (auto it = liveRows.begin(); it != liveRows.end();)
    {
        if (wanted.count (it->first) == 0)
            it = liveRows.erase (it);
        else
            ++it;
    }
}

//==============================================================================
// Selection + drag

void TrackList::rowMouseDown (juce::Component*, bool isFolder, int id, const juce::MouseEvent& event)
{
    drag = {};
    clearSelectionOnMouseUp = false;

    if (isFolder)
        return;   // folders don't join the multi-selection; a plain drag moves just the folder

    selectedFolder = 0;   // clicking a track ends a folder selection
    const auto trackId = (AudioEngine::TrackId) id;

    if (event.mods.isCtrlDown())
    {
        // The current single selection becomes the first member of the group
        if (multiSelection.empty() && selectedTrack != 0)
            multiSelection.insert (selectedTrack);

        if (multiSelection.count (trackId))
            multiSelection.erase (trackId);
        else
            multiSelection.insert (trackId);

        shiftAnchor = trackId;
    }
    else if (event.mods.isShiftDown() && shiftAnchor != 0)
    {
        // Range over the visual order of track rows, from the anchor to the click
        multiSelection.clear();
        bool inRange = false;

        for (auto& item : items)
        {
            if (item.member == 0)
                continue;

            const auto isEdge = item.member == shiftAnchor || item.member == trackId;

            if (isEdge || inRange)
                multiSelection.insert (item.member);

            if (isEdge)
            {
                if (inRange || shiftAnchor == trackId)
                    break;          // closing edge (or a one-row range)

                inRange = true;     // opening edge
            }
        }
    }
    else
    {
        // Clicking an already-selected row must not re-select it (that would steal
        // the group drag); it resolves on mouse-up if no drag happened.
        if (multiSelection.count (trackId))
        {
            clearSelectionOnMouseUp = true;
        }
        else
        {
            multiSelection.clear();

            if (onSelect)
                onSelect (trackId);
        }

        shiftAnchor = trackId;
    }

    // Ctrl/Shift changed the multi-selection: the shell arms it (auto-record)
    if ((event.mods.isCtrlDown() || event.mods.isShiftDown()) && onSelectionChanged)
        onSelectionChanged (multiSelection);

    refresh();
}

void TrackList::rowMouseDrag (juce::Component* row, bool isFolder, int id, const juce::MouseEvent& event)
{
    // "Moving only happens when the mouse moves outside of the channel being dragged"
    if (! drag.active && row->getLocalBounds().contains (event.getPosition()))
        return;

    if (! drag.active)
    {
        drag.active = true;
        drag.sourceIsFolder = isFolder;
        drag.sourceId = id;

        if (! isFolder)
            drag.draggedTracks = multiSelection.count (id) ? selectionInVisualOrder()
                                                           : std::vector<AudioEngine::TrackId> { id };
    }

    computeDropTarget (event.getEventRelativeTo (&rowContainer).getPosition().y);
    rowContainer.repaint();
}

void TrackList::computeDropTarget (int y)
{
    drag.valid = false;
    drag.intoFolder = false;
    drag.indicatorY = -1;
    drag.folderHighlight = {};

    const auto isDragged = [this] (const AudioEngine::SidebarItem& item)
    {
        if (drag.sourceIsFolder)
            return item.folder == drag.sourceId;

        return item.member != 0 && std::find (drag.draggedTracks.begin(), drag.draggedTracks.end(),
                                              item.member) != drag.draggedTracks.end();
    };

    // Count the siblings of 'parent' that appear before item index i (dragged ones
    // don't count: the engine removes them from the list before inserting).
    const auto childIndexAt = [&] (AudioEngine::FolderId parent, size_t i)
    {
        int index = 0;

        for (size_t j = 0; j < i; ++j)
            if (items[j].parent == parent && ! isDragged (items[j]))
                ++index;

        return index;
    };

    int rowY = 0;
    size_t i = 0;

    for (; i < items.size(); ++i)
    {
        const auto height = heightOfItem (items[i]);

        if (y < rowY + height)
            break;

        rowY += height;
    }

    if (i >= items.size())
    {
        // Below every row: append at the top level
        drag.valid = true;
        drag.parent = 0;
        drag.index = childIndexAt (0, items.size());
        drag.indicatorY = rowY;
        return;
    }

    const auto& item = items[i];
    const auto height = heightOfItem (item);

    // A folder row's middle drops INTO the folder (its edges insert around it)
    if (item.folder != 0 && y >= rowY + height / 4 && y <= rowY + 3 * height / 4)
    {
        if (drag.sourceIsFolder && item.folder == drag.sourceId)
            return;   // not into itself (subtrees are rejected by the engine on drop)

        drag.valid = true;
        drag.intoFolder = true;
        drag.parent = item.folder;
        drag.index = std::numeric_limits<int>::max();   // append
        drag.folderHighlight = { 0, rowTops[i], rowContainer.getWidth(), height };
        return;
    }

    const auto before = y < rowY + height / 2;
    drag.valid = true;
    drag.parent = item.parent;
    drag.index = childIndexAt (item.parent, i) + (before ? 0 : 1);
    drag.indicatorY = before ? rowY : rowY + height;
}

bool TrackList::finishRowDrag (int id)
{
    const auto wasDragging = drag.active;

    if (drag.active && drag.valid)
    {
        std::vector<AudioEngine::FolderId> folderIds;
        std::vector<int> memberIds;

        if (drag.sourceIsFolder)
            folderIds.push_back (drag.sourceId);
        else
            memberIds.assign (drag.draggedTracks.begin(), drag.draggedTracks.end());

        engine.moveSidebarItems (true, folderIds, memberIds, drag.parent, drag.index);
    }
    else if (! wasDragging && clearSelectionOnMouseUp)
    {
        // The deferred plain click on a selected row: now it becomes the selection
        multiSelection.clear();

        if (onSelect)
            onSelect (id);
    }

    clearSelectionOnMouseUp = false;
    drag = {};
    rowContainer.repaint();
    refreshSoon();   // rebuilding rows would delete the row we're called from
    return wasDragging;
}

void TrackList::refreshSoon()
{
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TrackList> (this)]
                                     { if (safe != nullptr) safe->refresh(); });
}

std::vector<AudioEngine::TrackId> TrackList::selectionInVisualOrder() const
{
    std::vector<AudioEngine::TrackId> ordered;

    for (auto& item : items)
        if (item.member != 0 && multiSelection.count (item.member))
            ordered.push_back (item.member);

    return ordered;
}

void TrackList::RowContainer::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
        owner.showBackgroundMenu();
}

void TrackList::RowContainer::paintOverChildren (juce::Graphics& g)
{
    auto& dragState = owner.drag;

    if (! dragState.active || ! dragState.valid)
        return;

    g.setColour (juce::Colours::gold.withAlpha (0.9f));

    if (dragState.intoFolder)
        g.drawRoundedRectangle (dragState.folderHighlight.toFloat().reduced (2.0f, 1.5f), 4.0f, 2.0f);
    else if (dragState.indicatorY >= 0)
        g.fillRect (0, juce::jlimit (0, juce::jmax (0, getHeight() - 2), dragState.indicatorY - 1), getWidth(), 2);
}

//==============================================================================
void TrackList::showBackgroundMenu()
{
    const auto safe = juce::Component::SafePointer<TrackList> (this);
    juce::PopupMenu menu;

    menu.addItem ("Add track", [safe] { if (safe != nullptr && safe->onAddTrack) safe->onAddTrack(); });
    menu.addItem ("Add folder", [safe]
    {
        if (safe != nullptr)
        {
            safe->engine.addFolder (true);
            safe->refresh();
        }
    });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

void TrackList::showFolderMenu (AudioEngine::FolderId folderId)
{
    const auto safe = juce::Component::SafePointer<TrackList> (this);
    juce::PopupMenu menu;

    menu.addItem ("New track inside", [safe, folderId]
    {
        if (safe != nullptr && safe->onAddTrackInFolder)
            safe->onAddTrackInFolder (folderId);
    });

    menu.addItem ("New subfolder", [safe, folderId]
    {
        if (safe != nullptr)
        {
            safe->engine.addFolder (true, {}, folderId);
            safe->engine.setFolderCollapsed (folderId, false);
            safe->refresh();
        }
    });

    menu.addSeparator();
    menu.addItem ("Collapse all (with subfolders)",
                  [safe, folderId] { if (safe != nullptr) safe->setSubtreeCollapsed (folderId, true); });
    menu.addItem ("Expand all (with subfolders)",
                  [safe, folderId] { if (safe != nullptr) safe->setSubtreeCollapsed (folderId, false); });
    menu.addSeparator();

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
    menu.addSubMenu ("Color", colours::buildMenu (engine.getFolderColour (folderId),
                                                  [safe, folderId] (juce::String hex)
                                                  {
                                                      if (safe != nullptr)
                                                      {
                                                          safe->engine.setFolderColour (folderId, hex);
                                                          safe->refresh();
                                                      }
                                                  }));
    menu.addSeparator();
    menu.addItem ("Remove folder (contents move up)", [safe, folderId]
    {
        if (safe != nullptr) { safe->engine.removeFolder (folderId); safe->refresh(); }
    });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

void TrackList::resized()
{
    viewport.setBounds (getLocalBounds());
    layoutRows();
}

void TrackList::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232529));
}
