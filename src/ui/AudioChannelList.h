#pragma once

#include "../AudioEngine.h"
#include "../engine/AudioChannelProcessor.h"
#include "ColorPalette.h"
#include "ThemedLookAndFeel.h"

// The Audio-domain sidebar: one row per audio channel showing its input, a meter,
// mute and volume, grouped by folders (Cubase-style, like the track list): folder
// rows are half a channel row tall, nest arbitrarily, and the indented guide area
// on the left shows what belongs to which folder.
//
// Re-ordering (ISSUES.md "Sidebar"): drag rows to re-order; Ctrl/Shift-click
// selects multiple channels and dragging any selected row moves the group. The
// drag starts once the mouse leaves the pressed row and completes on drop:
// between rows inserts there, onto a folder row's middle drops into that folder.
// Call refresh() from a UI timer.
class AudioChannelList final : public juce::Component
{
public:
    explicit AudioChannelList (AudioEngine& e) : engine (e)
    {
        // Adding folders lives in the right-click menu (ISSUES.md: header button removed)
        viewport.setViewedComponent (&rowContainer, false);
        viewport.setScrollBarsShown (false, false, true, false);   // no scrollbar; the wheel still scrolls
        addAndMakeVisible (viewport);
    }

    // The current channel: the one last clicked (the mixer highlights it, and clicking a strip there
    // selects it here); 0 = none
    AudioEngine::AudioChannelId getCurrentChannel() const   { return multiSelection.count (current) > 0 ? current : 0; }

    void selectChannel (AudioEngine::AudioChannelId id)
    {
        multiSelection = { id };
        current = shiftAnchor = id;
        refresh();
    }

    void refresh()
    {
        auto freshItems = engine.getSidebarItems (false, true);

        if (freshItems != items || fixedRows.empty())
        {
            items = std::move (freshItems);
            rebuildRows();
        }

        // Drop selections that no longer exist
        const auto channelIds = engine.getAudioChannelIds();
        std::erase_if (multiSelection, [&channelIds] (auto id)
                       { return std::find (channelIds.begin(), channelIds.end(), id) == channelIds.end(); });

        for (auto& row : rowComponents)
        {
            if (auto* channelRow = dynamic_cast<Row*> (row.get()))
                channelRow->refresh (multiSelection.count (channelRow->channelId) > 0);
            else if (auto* folderRow = dynamic_cast<FolderRow*> (row.get()))
                folderRow->refresh();
        }

        for (auto& row : fixedRows)
            if (auto* busRow = dynamic_cast<BusRow*> (row.get()))
                busRow->refresh();
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        layoutRows();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232529));

    }

private:
    static constexpr int rowHeight = 64;
    static constexpr int folderRowHeight = 32;   // about half a channel row
    static constexpr int indentPerLevel = 10;

    struct RowContainer final : juce::Component
    {
        explicit RowContainer (AudioChannelList& o) : owner (o) {}

        void mouseDown (const juce::MouseEvent& event) override   // background context menu
        {
            if (event.mods.isPopupMenu())
                owner.showBackgroundMenu();
        }

        void paintOverChildren (juce::Graphics& g) override   // drop indicator
        {
            auto& dragState = owner.drag;

            if (! dragState.active || ! dragState.valid)
                return;

            g.setColour (juce::Colours::gold.withAlpha (0.9f));

            if (dragState.intoFolder)
                g.drawRoundedRectangle (dragState.folderHighlight.toFloat().reduced (2.0f, 1.5f), theme::corner, 2.0f);
            else if (dragState.indicatorY >= 0)
                g.fillRect (0, juce::jlimit (0, juce::jmax (0, getHeight() - 2), dragState.indicatorY - 1),
                            getWidth(), 2);
        }

        AudioChannelList& owner;
    };

    static int heightOfItem (const AudioEngine::SidebarItem& item)
    {
        return item.folder != 0 ? folderRowHeight : rowHeight;
    }

    //==========================================================================
    struct Row final : juce::Component
    {
        Row (AudioChannelList& ownerToUse, AudioEngine::AudioChannelId id, int depthToUse)
            : owner (ownerToUse), engine (ownerToUse.engine), channelId (id), depth (depthToUse)
        {
            muteButton.setClickingTogglesState (true);
            theme::setButtonRole (muteButton, "mute");
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

        void refresh (bool isSelected)
        {
            selected = isSelected;

            if (auto* processor = engine.getAudioChannel (channelId))
            {
                muteButton.setToggleState (processor->isMuted(), juce::dontSendNotification);
                meterLevel = juce::jmax (processor->getLastPeak(), meterLevel * 0.85f);
            }

            repaint();
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (event.mods.isPopupMenu())
                return;   // channels move by drag; folders keep their menus

            owner.rowMouseDown (false, channelId, event);
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            if (! event.mods.isPopupMenu())
                owner.rowMouseDrag (this, false, channelId, event);
        }

        void mouseUp (const juce::MouseEvent& event) override
        {
            if (! event.mods.isPopupMenu())
                owner.finishRowDrag (channelId);
        }

        void paint (juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);
            theme::paintTrackBox (g, bounds, theme::Token::trackAudioBg, selected);

            auto area = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).reduced (8, 4);

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
            auto meter = getLocalBounds().withTrimmedLeft (depth * indentPerLevel)
                             .reduced (8, 4).removeFromBottom (5).toFloat();
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRect (meter);

            const auto db = juce::Decibels::gainToDecibels (meterLevel, -60.0f);
            const auto proportion = juce::jlimit (0.0f, 1.0f, juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f));
            g.setColour (meterLevel >= 1.0f ? juce::Colours::red : juce::Colours::limegreen);
            g.fillRect (meter.withWidth (meter.getWidth() * proportion));
        }

        void resized() override
        {
            auto area = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).reduced (8, 4);
            area.removeFromTop (34);
            area.removeFromBottom (7);
            muteButton.setBounds (area.removeFromLeft (24));
            area.removeFromLeft (4);
            volumeSlider.setBounds (area);
        }

        AudioChannelList& owner;
        AudioEngine& engine;
        const AudioEngine::AudioChannelId channelId;
        const int depth;
        juce::TextButton muteButton { "M" };
        juce::Slider volumeSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        float meterLevel = 0.0f;
        bool selected = false;
    };

    //==========================================================================
    // Under the channels (which can be mixed: instruments' outputs, later recording inputs): the
    // buses, the six Aux buses and the master - fixed, in that order (not moved, not in folders)
    struct SectionHeader final : juce::Component
    {
        SectionHeader (const juce::String& titleToUse, const juce::String& noteToUse) : title (titleToUse), note (noteToUse)
        {
            setInterceptsMouseClicks (false, false);
        }

        void paint (juce::Graphics& g) override
        {
            auto area = getLocalBounds().reduced (8, 0);
            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.fillRect (area.getX(), 3, area.getWidth(), 1);
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (title.toUpperCase(), area, juce::Justification::centredLeft);

            if (note.isNotEmpty())
            {
                g.setColour (juce::Colours::grey);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (note, area.withTrimmedLeft (juce::roundToInt (juce::GlyphArrangement::getStringWidth (
                                juce::Font (juce::FontOptions (11.0f, juce::Font::bold)), title.toUpperCase())) + 8),
                            juce::Justification::centredLeft);
            }
        }

        juce::String title, note;
    };

    // An Aux bus (a placeholder until the buses work: MILESTONES.md "Audio mixer", phase 3) or the master
    struct BusRow final : juce::Component
    {
        BusRow (AudioEngine& e, int auxNumberToUse) : engine (e), auxNumber (auxNumberToUse)
        {
            muteButton.setClickingTogglesState (true);
            theme::setButtonRole (muteButton, "mute");
            muteButton.setWantsKeyboardFocus (false);
            muteButton.onClick = [this]
            {
                if (auto* p = processor())
                    p->setMuted (muteButton.getToggleState());
            };
            addAndMakeVisible (muteButton);

            volumeSlider.setRange (-60.0, 6.0, 0.1);
            volumeSlider.setValue (0.0, juce::dontSendNotification);
            volumeSlider.setDoubleClickReturnValue (true, 0.0);
            volumeSlider.setSkewFactorFromMidPoint (-12.0);
            volumeSlider.setWantsKeyboardFocus (false);
            volumeSlider.onValueChange = [this]
            {
                if (auto* p = processor())
                    p->setGain (juce::Decibels::decibelsToGain ((float) volumeSlider.getValue(), -60.0f));
            };
            addAndMakeVisible (volumeSlider);

            muteButton.setEnabled (auxNumber == 0);   // the Aux buses: not working yet
            volumeSlider.setEnabled (auxNumber == 0);
        }

        AudioChannelProcessor* processor() const   { return auxNumber == 0 ? engine.getMasterChannel() : nullptr; }

        void refresh()
        {
            if (auto* p = processor())
            {
                muteButton.setToggleState (p->isMuted(), juce::dontSendNotification);

                if (! volumeSlider.isMouseButtonDown())
                    volumeSlider.setValue (juce::Decibels::gainToDecibels (p->getGain(), -60.0f), juce::dontSendNotification);

                meterLevel = juce::jmax (p->getLastPeak(), meterLevel * 0.85f);
                repaint();
            }
        }

        void paint (juce::Graphics& g) override
        {
            theme::paintTrackBox (g, getLocalBounds().toFloat().reduced (2.0f, 1.5f), theme::Token::trackAudioBg, false);
            auto area = getLocalBounds().reduced (8, 4);

            g.setColour (juce::Colours::white.withAlpha (auxNumber == 0 ? 0.9f : 0.5f));
            g.setFont (juce::FontOptions (14.0f));
            g.drawText (auxNumber == 0 ? juce::String ("Master") : "Aux " + juce::String (auxNumber), area.removeFromTop (18), juce::Justification::centredLeft);

            g.setColour (juce::Colours::grey);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (auxNumber == 0 ? juce::String::fromUTF8 ("all channels \xe2\x86\x92 the audio device")
                                       : juce::String ("coming: sends from the channels"),
                        area.removeFromTop (14), juce::Justification::centredLeft);

            auto meter = getLocalBounds().reduced (8, 4).removeFromBottom (5).toFloat();
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRect (meter);

            const auto db = juce::Decibels::gainToDecibels (meterLevel, -60.0f);
            g.setColour (meterLevel >= 1.0f ? juce::Colours::red : juce::Colours::limegreen);
            g.fillRect (meter.withWidth (meter.getWidth() * juce::jlimit (0.0f, 1.0f, juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f))));
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
        const int auxNumber;   // 0: the master
        juce::TextButton muteButton { "M" };
        juce::Slider volumeSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        float meterLevel = 0.0f;
    };

    std::vector<std::unique_ptr<juce::Component>> fixedRows;   // the headers, the Aux buses and the master
    static constexpr int headerHeight = 24;

    //==========================================================================
    struct FolderRow final : juce::Component
    {
        FolderRow (AudioChannelList& ownerToUse, AudioEngine::FolderId id, int depthToUse)
            : owner (ownerToUse), engine (ownerToUse.engine), folderId (id), depth (depthToUse)
        {
            nameLabel.setText (engine.getFolderName (folderId), juce::dontSendNotification);
            nameLabel.setEditable (false, true);
            nameLabel.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
            nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            nameLabel.onTextChange = [this]
            {
                engine.setFolderName (folderId, nameLabel.getText());
                nameLabel.setText (engine.getFolderName (folderId), juce::dontSendNotification);
            };
            nameLabel.setInterceptsMouseClicks (false, false);
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

            owner.rowMouseDown (true, folderId, event);
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

            // A click (no drag happened) toggles collapse
            if (! owner.finishRowDrag())
                engine.setFolderCollapsed (folderId, ! engine.isFolderCollapsed (folderId));
                // finishRowDrag already scheduled the refresh (deferred: it deletes this row)
        }

        void mouseDoubleClick (const juce::MouseEvent& event) override
        {
            if (nameLabel.getBounds().contains (event.getPosition()))
                nameLabel.showEditor();
        }

        void paint (juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);
            theme::paintTrackBox (g, bounds, theme::Token::folderBg, false);

            g.setColour (AudioEngine::colourFromHex (engine.getFolderColour (folderId), juce::Colour (0xff6d7178)));
            g.fillRect (bounds.getX() + 1.0f, bounds.getY() + 2.0f, 4.0f, bounds.getHeight() - 4.0f);

            const auto collapsed = engine.isFolderCollapsed (folderId);
            juce::Path triangle;
            const auto cx = bounds.getX() + 13.0f, cy = bounds.getCentreY();

            if (collapsed)
                triangle.addTriangle (cx - 1.5f, cy - 3.0f, cx - 1.5f, cy + 3.0f, cx + 3.0f, cy);
            else
                triangle.addTriangle (cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 3.0f);

            g.setColour (juce::Colours::white.withAlpha (0.7f));
            g.fillPath (triangle);
        }

        void resized() override
        {
            nameLabel.setBounds (getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 24).reduced (0, 2));
        }

        AudioChannelList& owner;
        AudioEngine& engine;
        const AudioEngine::FolderId folderId;
        const int depth;
        juce::Label nameLabel;
    };

    //==========================================================================
    void rebuildRows()
    {
        rowComponents.clear();
        fixedRows.clear();

        for (auto& item : items)
        {
            std::unique_ptr<juce::Component> row;

            if (item.folder != 0)
                row = std::make_unique<FolderRow> (*this, item.folder, item.depth);
            else
                row = std::make_unique<Row> (*this, item.member, item.depth);

            rowContainer.addAndMakeVisible (*row);
            rowComponents.push_back (std::move (row));
        }

        if (items.empty())
            fixedRows.push_back (std::make_unique<SectionHeader> ("Channels", "none yet - add an instrument"));

        fixedRows.push_back (std::make_unique<SectionHeader> ("Buses", "none yet"));
        fixedRows.push_back (std::make_unique<SectionHeader> ("Aux", juce::String()));

        for (int aux = 1; aux <= 6; ++aux)
            fixedRows.push_back (std::make_unique<BusRow> (engine, aux));

        fixedRows.push_back (std::make_unique<SectionHeader> ("Master", juce::String()));
        fixedRows.push_back (std::make_unique<BusRow> (engine, 0));

        for (auto& row : fixedRows)
            rowContainer.addAndMakeVisible (*row);

        layoutRows();
        repaint();
    }

    void layoutRows()
    {
        const auto width = juce::jmax (1, viewport.getMaximumVisibleWidth());
        int y = 0;

        for (size_t i = 0; i < rowComponents.size(); ++i)
        {
            rowComponents[i]->setBounds (0, y, width, heightOfItem (items[i]));
            y += heightOfItem (items[i]);
        }

        for (auto& row : fixedRows)
        {
            const auto height = dynamic_cast<SectionHeader*> (row.get()) != nullptr ? headerHeight : rowHeight;
            row->setBounds (0, y, width, height);
            y += height;
        }

        // At least viewport height, so right-clicking the empty area reaches the container
        rowContainer.setSize (width, juce::jmax (1, y, viewport.getHeight()));
    }

    void refreshSoon()   // deferred refresh, safe from row callbacks
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<AudioChannelList> (this)]
                                         { if (safe != nullptr) safe->refresh(); });
    }

    //==========================================================================
    // Selection + drag (mirrors TrackList)

    void rowMouseDown (bool isFolder, int id, const juce::MouseEvent& event)
    {
        drag = {};
        clearSelectionOnMouseUp = false;

        if (isFolder)
            return;

        const auto channelId = (AudioEngine::AudioChannelId) id;
        current = channelId;

        if (event.mods.isCtrlDown())
        {
            if (multiSelection.count (channelId))
                multiSelection.erase (channelId);
            else
                multiSelection.insert (channelId);

            shiftAnchor = channelId;
        }
        else if (event.mods.isShiftDown() && shiftAnchor != 0)
        {
            multiSelection.clear();
            bool inRange = false;

            for (auto& item : items)
            {
                if (item.member == 0)
                    continue;

                const auto isEdge = item.member == shiftAnchor || item.member == channelId;

                if (isEdge || inRange)
                    multiSelection.insert (item.member);

                if (isEdge)
                {
                    if (inRange || shiftAnchor == channelId)
                        break;

                    inRange = true;
                }
            }
        }
        else
        {
            // Clicking an already-selected row must not re-select it (that would
            // steal the group drag); it resolves on mouse-up if no drag happened.
            if (multiSelection.count (channelId))
                clearSelectionOnMouseUp = true;
            else
                multiSelection = { channelId };

            shiftAnchor = channelId;
        }

        refresh();
    }

    void rowMouseDrag (juce::Component* row, bool isFolder, int id, const juce::MouseEvent& event)
    {
        if (! drag.active && row->getLocalBounds().contains (event.getPosition()))
            return;

        if (! drag.active)
        {
            drag.active = true;
            drag.sourceIsFolder = isFolder;
            drag.sourceId = id;

            if (! isFolder)
            {
                drag.draggedChannels.clear();

                if (multiSelection.count (id))
                {
                    for (auto& item : items)
                        if (item.member != 0 && multiSelection.count (item.member))
                            drag.draggedChannels.push_back (item.member);
                }
                else
                {
                    drag.draggedChannels.push_back (id);
                }
            }
        }

        computeDropTarget (event.getEventRelativeTo (&rowContainer).getPosition().y);
        rowContainer.repaint();
    }

    void computeDropTarget (int y)
    {
        drag.valid = false;
        drag.intoFolder = false;
        drag.indicatorY = -1;
        drag.folderHighlight = {};

        const auto isDragged = [this] (const AudioEngine::SidebarItem& item)
        {
            if (drag.sourceIsFolder)
                return item.folder == drag.sourceId;

            return item.member != 0 && std::find (drag.draggedChannels.begin(), drag.draggedChannels.end(),
                                                  item.member) != drag.draggedChannels.end();
        };

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
            drag.valid = true;
            drag.parent = 0;
            drag.index = childIndexAt (0, items.size());
            drag.indicatorY = rowY;
            return;
        }

        const auto& item = items[i];
        const auto height = heightOfItem (item);

        if (item.folder != 0 && y >= rowY + height / 4 && y <= rowY + 3 * height / 4)
        {
            if (drag.sourceIsFolder && item.folder == drag.sourceId)
                return;

            drag.valid = true;
            drag.intoFolder = true;
            drag.parent = item.folder;
            drag.index = std::numeric_limits<int>::max();
            drag.folderHighlight = rowComponents[i]->getBounds();
            return;
        }

        const auto before = y < rowY + height / 2;
        drag.valid = true;
        drag.parent = item.parent;
        drag.index = childIndexAt (item.parent, i) + (before ? 0 : 1);
        drag.indicatorY = before ? rowY : rowY + height;
    }

    bool finishRowDrag (int id = 0)
    {
        const auto wasDragging = drag.active;

        if (drag.active && drag.valid)
        {
            std::vector<AudioEngine::FolderId> folderIds;
            std::vector<int> memberIds;

            if (drag.sourceIsFolder)
                folderIds.push_back (drag.sourceId);
            else
                memberIds = drag.draggedChannels;

            engine.moveSidebarItems (false, folderIds, memberIds, drag.parent, drag.index);
        }
        else if (! wasDragging && clearSelectionOnMouseUp)
        {
            // The deferred plain click on a selected row: it becomes the selection
            multiSelection.clear();

            if (id != 0)
                multiSelection.insert (id);
        }

        clearSelectionOnMouseUp = false;
        drag = {};
        rowContainer.repaint();
        refreshSoon();   // rebuilding rows would delete the row we're called from
        return wasDragging;
    }

    //==========================================================================
    void showBackgroundMenu()
    {
        const auto safe = juce::Component::SafePointer<AudioChannelList> (this);
        juce::PopupMenu menu;

        // Channels themselves come from instruments (Instruments view), so the
        // background menu only offers folders here.
        menu.addItem ("Add folder", [safe]
        {
            if (safe != nullptr)
            {
                safe->engine.addFolder (false);
                safe->refresh();
            }
        });

        menu.showMenuAsync (juce::PopupMenu::Options());
    }

    void showFolderMenu (AudioEngine::FolderId folderId)
    {
        const auto safe = juce::Component::SafePointer<AudioChannelList> (this);
        juce::PopupMenu menu;

        menu.addItem ("New subfolder", [safe, folderId]
        {
            if (safe != nullptr)
            {
                safe->engine.addFolder (false, {}, folderId);
                safe->engine.setFolderCollapsed (folderId, false);
                safe->refresh();
            }
        });

        juce::PopupMenu moveTo;
        moveTo.addItem ("Top level", true, engine.getFolderParent (folderId) == 0, [safe, folderId]
        {
            if (safe != nullptr) { safe->engine.setFolderParent (folderId, 0); safe->refresh(); }
        });

        for (auto id : engine.getFolderIds (false))
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

    //==========================================================================
    AudioEngine& engine;
    juce::Viewport viewport;
    RowContainer rowContainer { *this };

    std::vector<AudioEngine::SidebarItem> items;
    std::vector<std::unique_ptr<juce::Component>> rowComponents;

    std::set<AudioEngine::AudioChannelId> multiSelection;
    AudioEngine::AudioChannelId current = 0;
    AudioEngine::AudioChannelId shiftAnchor = 0;
    bool clearSelectionOnMouseUp = false;

    struct DragState
    {
        bool active = false;
        bool sourceIsFolder = false;
        int sourceId = 0;
        std::vector<AudioEngine::AudioChannelId> draggedChannels;

        bool valid = false, intoFolder = false;
        AudioEngine::FolderId parent = 0;
        int index = 0;
        int indicatorY = -1;
        juce::Rectangle<int> folderHighlight;
    } drag;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioChannelList)
};
