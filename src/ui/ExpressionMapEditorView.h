#pragma once

#include "../AudioEngine.h"
#include "../api/CommandDispatcher.h"
#include "ThemedLookAndFeel.h"
#include "../model/NoteNames.h"
#include "ColorPalette.h"

// Content view: create and edit expression maps (MILESTONES.md "Articulation / expression maps").
//
// Left: the project's maps. Middle: the selected map's groups (the first is the root group) and
// the selected group's articulations. Right: the details of whatever is selected - the map (name,
// description, key names), a group, or an articulation (name, symbol, description, timing offset,
// key range, which roots a modifier applies to, and its outputs, sent in series).
//
// Sound slots (MILESTONES.md "Sound slots"): a fourth column lists the map's slots - every valid
// combination of articulations with its own outputs, key range and timing offset - with a filter.
// In a map with slots the articulations only have names, symbols, descriptions and defaults; a map
// whose articulations carry outputs can be turned into slots from the map's details.
//
// Every change goes through the same commands as scripts and agents use (expressionmap.set for
// content, the rename commands for names so the notes that use a name follow), so it is undoable
// in the global history. The editor keeps a working copy; an edit that would make the map invalid
// is not saved and the reason is shown, so the project never holds an invalid map.
class ExpressionMapEditorView final : public juce::Component
{
public:
    ExpressionMapEditorView (AudioEngine& e, CommandDispatcher& d) : engine (e), dispatcher (d)
    {
        backButton.setWantsKeyboardFocus (false);
        backButton.onClick = [this] { if (onBack) onBack(); };
        addAndMakeVisible (backButton);

        titleLabel.setText ("Expression maps", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        addAndMakeVisible (titleLabel);

        status.setFont (juce::FontOptions (12.0f));
        status.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (status);

        setUpList (mapList, mapModel, "Maps");
        setUpList (groupList, groupModel, "Groups (the first is the root group)");
        setUpList (articulationList, articulationModel, "Articulations");
        setUpList (slotList, slotModel, "Sound slots");

        slotModel.count = [this] { return (int) visibleSlots.size(); };
        slotModel.text = [this] (int row)
        {
            const auto& slot = working->slots[(size_t) visibleSlots[(size_t) row]];
            return ExpressionMap::labelOf (slot.selection) + "   " + summary (slot.outputs);
        };
        slotModel.selected = [this] (int row)
        {
            if (row < 0 || programmatic || ! working.has_value() || row >= (int) visibleSlots.size())
                return;

            selectedSlot = visibleSlots[(size_t) row];
            focus = Focus::slot;
            rebuildDetailsSoon();
        };

        slotFilter.setTextToShowWhenEmpty ("Filter: Ponticello, Rep. ...", juce::Colours::grey);
        slotFilter.setFont (juce::FontOptions (13.0f));
        slotFilter.onTextChange = [this] { refreshSlots(); };
        addAndMakeVisible (slotFilter);

        mapModel.count = [this] { return (int) mapNames.size(); };
        mapModel.text = [this] (int row) { return mapNames[(size_t) row]; };
        mapModel.selected = [this] (int row) { if (row >= 0 && ! programmatic) selectMap (mapNames[(size_t) row], false); };

        groupModel.count = [this] { return working.has_value() ? (int) working->groups.size() : 0; };
        groupModel.text = [this] (int row) { return working->groups[(size_t) row].name + (row == 0 ? "  (root)" : ""); };
        groupModel.selected = [this] (int row)
        {
            if (row < 0 || programmatic || ! working.has_value())
                return;

            selectedGroup = row;
            selectedArticulation = -1;
            focus = Focus::group;
            articulationList.updateContent();
            selectQuietly (articulationList, -1);
            rebuildDetailsSoon();
        };

        articulationModel.count = [this] { return currentGroup() != nullptr ? (int) currentGroup()->articulations.size() : 0; };
        articulationModel.text = [this] (int row)
        {
            auto& a = currentGroup()->articulations[(size_t) row];
            return a.symbol.isNotEmpty() ? a.symbol + "  " + a.name : a.name;
        };
        articulationModel.selected = [this] (int row)
        {
            if (row < 0 || programmatic || currentGroup() == nullptr)
                return;

            selectedArticulation = row;
            focus = Focus::articulation;
            rebuildDetailsSoon();
        };

        for (auto* b : { &newMap, &duplicateMap, &deleteMap, &addGroup, &removeGroup, &groupUp, &groupDown,
                         &addArticulation, &removeArticulation, &articulationUp, &articulationDown,
                         &addSlot, &duplicateSlot, &removeSlot })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }

        libraryButton.setWantsKeyboardFocus (false);
        libraryButton.setTooltip ("Maps kept in files: add one to the project, save this one for other projects, import and export");
        libraryButton.onClick = [this] { showLibraryMenu(); };
        addAndMakeVisible (libraryButton);

        newMap.onClick = [this] { createMap(); };
        duplicateMap.onClick = [this] { duplicateCurrentMap(); };
        deleteMap.onClick = [this] { deleteCurrentMap(); };
        addGroup.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return addGroupTo (m); }); };
        removeGroup.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return removeGroupFrom (m); }); };
        groupUp.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return moveGroup (m, -1); }); };
        groupDown.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return moveGroup (m, +1); }); };
        addArticulation.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return addArticulationTo (m); }); };
        removeArticulation.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return removeArticulationFrom (m); }); };
        articulationUp.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return moveArticulation (m, -1); }); };
        articulationDown.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return moveArticulation (m, +1); }); };
        addSlot.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return addSlotTo (m, false); }); };
        duplicateSlot.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return addSlotTo (m, true); }); };
        removeSlot.onClick = [this] { editStructure ([this] (ExpressionMap& m) { return removeSlotFrom (m); }); };
        addSlot.setTooltip ("A new slot for the first combination that has none yet; set its combination in the details");
        duplicateSlot.setTooltip ("A copy of the selected slot (its outputs too); change its combination in the details");

        detailsViewport.setViewedComponent (&details, false);
        detailsViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (detailsViewport);

        refresh();
    }

    std::function<void()> onBack;

    // Open the editor on a map (empty = keep the current one)
    void select (const juce::String& mapName)
    {
        refresh();

        if (mapName.isNotEmpty())
            selectMap (mapName, true);
    }

    // Called regularly while the view is showing: follows changes made elsewhere (undo, API)
    void refresh()
    {
        std::vector<juce::String> names;

        for (auto& map : engine.getExpressionMaps())
            names.push_back (map.name);

        if (names != mapNames)
        {
            mapNames = names;
            mapList.updateContent();
        }

        if (currentMap.isEmpty() && ! mapNames.empty())
            selectMap (mapNames.front(), true);

        if (currentMap.isNotEmpty())
        {
            const auto stored = engine.getExpressionMap (currentMap);

            if (! stored.has_value())
            {
                clearSelection();
            }
            else if (stored->toXml()->toString() != syncedXml)   // changed elsewhere: take it over
            {
                loadWorking (*stored);
            }
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        auto header = area.removeFromTop (34);
        backButton.setBounds (header.removeFromLeft (70));
        header.removeFromLeft (10);
        libraryButton.setBounds (header.removeFromRight (90));
        titleLabel.setBounds (header);

        area.removeFromTop (6);
        status.setBounds (area.removeFromBottom (44));
        area.removeFromBottom (6);

        auto left = area.removeFromLeft (200);
        area.removeFromLeft (10);
        auto middle = area.removeFromLeft (270);
        area.removeFromLeft (10);

        layoutList (left, mapList, { &newMap, &duplicateMap, &deleteMap });

        const auto groupsHeight = juce::jmin (middle.getHeight() / 2, 230);
        layoutList (middle.removeFromTop (groupsHeight), groupList, { &addGroup, &removeGroup, &groupUp, &groupDown });
        middle.removeFromTop (8);
        layoutList (middle, articulationList, { &addArticulation, &removeArticulation, &articulationUp, &articulationDown });

        auto slotsArea = area.removeFromLeft (juce::jmin (330, area.getWidth() / 2));
        area.removeFromLeft (10);
        listHeadings[&slotList]->setBounds (slotsArea.removeFromTop (18));
        slotFilter.setBounds (slotsArea.removeFromTop (26).reduced (0, 1));
        slotsArea.removeFromTop (4);
        layoutList (slotsArea, slotList, { &addSlot, &duplicateSlot, &removeSlot });
        listHeadings[&slotList]->setVisible (true);

        detailsViewport.setBounds (area);
        layoutDetails();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour (theme::Token::surfaceWindow));
    }

private:
    enum class Focus { map, group, articulation, slot };
    using Output = ExpressionMap::Output;

    //==========================================================================
    // List plumbing
    struct ListModel final : juce::ListBoxModel
    {
        std::function<int()> count;
        std::function<juce::String (int)> text;
        std::function<void (int)> selected;

        int getNumRows() override    { return count ? count() : 0; }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool isSelected) override
        {
            if (row < 0 || row >= getNumRows())
                return;

            if (isSelected)
            {
                g.setColour (theme::colour (theme::Token::selectionBg));
                g.fillRect (0, 0, width, height);
            }

            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (text (row), 8, 0, width - 12, height, juce::Justification::centredLeft, true);
        }

        void selectedRowsChanged (int lastRow) override    { if (selected) selected (lastRow); }
    };

    void setUpList (juce::ListBox& list, ListModel& model, const juce::String& title)
    {
        list.setModel (&model);
        list.setRowHeight (24);
        list.setColour (juce::ListBox::backgroundColourId, theme::colour (theme::Token::surfacePanel));
        list.setColour (juce::ListBox::outlineColourId, theme::colour (theme::Token::borderSubtle));
        list.setOutlineThickness (1);
        list.setWantsKeyboardFocus (false);
        list.setTitle (title);
        addAndMakeVisible (list);

        auto heading = std::make_unique<juce::Label> (juce::String(), title);
        heading->setFont (juce::FontOptions (12.0f, juce::Font::bold));
        heading->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
        addAndMakeVisible (*heading);
        headings.push_back (std::move (heading));
        listHeadings[&list] = headings.back().get();
    }

    void layoutList (juce::Rectangle<int> area, juce::ListBox& list, std::initializer_list<juce::Button*> buttons)
    {
        if (&list != &slotList)   // the slot list's heading sits above its filter (resized)
            listHeadings[&list]->setBounds (area.removeFromTop (18));
        auto row = area.removeFromBottom (26);
        const auto width = row.getWidth() / (int) buttons.size();

        for (auto* b : buttons)
            b->setBounds (row.removeFromLeft (width).reduced (2, 0));

        list.setBounds (area);
    }

    //==========================================================================
    // Selection and the working copy
    // Selecting rows from code must not behave like a click (which changes what the details show)
    void selectQuietly (juce::ListBox& list, int row)
    {
        const juce::ScopedValueSetter<bool> guard (programmatic, true);

        if (row >= 0)
            list.selectRow (row, true, true);
        else
            list.deselectAllRows();
    }

    const ExpressionMap::Group* currentGroup() const
    {
        return working.has_value() && selectedGroup >= 0 && selectedGroup < (int) working->groups.size()
                 ? &working->groups[(size_t) selectedGroup] : nullptr;
    }

    ExpressionMap::Group* currentGroupMutable()
    {
        return working.has_value() && selectedGroup >= 0 && selectedGroup < (int) working->groups.size()
                 ? &working->groups[(size_t) selectedGroup] : nullptr;
    }

    ExpressionMap::Articulation* currentArticulation()
    {
        auto* group = currentGroupMutable();
        return group != nullptr && selectedArticulation >= 0 && selectedArticulation < (int) group->articulations.size()
                 ? &group->articulations[(size_t) selectedArticulation] : nullptr;
    }

    ExpressionMap::Slot* currentSlot()
    {
        return working.has_value() && selectedSlot >= 0 && selectedSlot < (int) working->slots.size()
                 ? &working->slots[(size_t) selectedSlot] : nullptr;
    }

    // The slots the filter lets through (every word must appear in the slot's label)
    void refreshSlots()
    {
        visibleSlots.clear();

        if (working.has_value())
        {
            const auto words = juce::StringArray::fromTokens (slotFilter.getText(), true);

            for (size_t i = 0; i < working->slots.size(); ++i)
            {
                const auto label = ExpressionMap::labelOf (working->slots[i].selection);

                if (std::all_of (words.begin(), words.end(), [&label] (const juce::String& w) { return label.containsIgnoreCase (w); }))
                    visibleSlots.push_back ((int) i);
            }
        }

        slotList.updateContent();
        const auto it = std::find (visibleSlots.begin(), visibleSlots.end(), selectedSlot);
        selectQuietly (slotList, it == visibleSlots.end() ? -1 : (int) (it - visibleSlots.begin()));
        slotList.repaint();
    }

    // "PC 112, 0 · KS C1"
    static juce::String summary (const std::vector<Output>& outputs)
    {
        juce::StringArray parts;

        for (auto& o : outputs)
            parts.add (o.type == Output::Type::programChange ? "PC " + juce::String (o.number)
                       : o.type == Output::Type::controller ? "CC" + juce::String (o.number) + "=" + juce::String (o.value)
                                                            : "KS " + juce::String (o.number));

        return parts.joinIntoString (", ");
    }

    void selectMap (const juce::String& name, bool updateList)
    {
        if (name == currentMap && working.has_value())
            return;

        const auto stored = engine.getExpressionMap (name);

        if (! stored.has_value())
            return;

        currentMap = stored->name;
        selectedGroup = 0;
        selectedArticulation = -1;
        selectedSlot = -1;
        focus = Focus::map;
        loadWorking (*stored);

        if (updateList)
        {
            const auto it = std::find (mapNames.begin(), mapNames.end(), currentMap);

            if (it != mapNames.end())
                selectQuietly (mapList, (int) (it - mapNames.begin()));
        }
    }

    void clearSelection()
    {
        currentMap = {};
        working.reset();
        syncedXml = {};
        selectedGroup = 0;
        selectedArticulation = -1;
        selectedSlot = -1;
        problems.clear();
        groupList.updateContent();
        articulationList.updateContent();
        refreshSlots();
        rebuildDetailsSoon();
        updateStatus();
    }

    void loadWorking (const ExpressionMap& map)
    {
        working = map;
        syncedXml = map.toXml()->toString();
        problems.clear();

        if (selectedGroup >= (int) working->groups.size())
            selectedGroup = juce::jmax (0, (int) working->groups.size() - 1);

        if (const auto* group = currentGroup(); group == nullptr || selectedArticulation >= (int) group->articulations.size())
            selectedArticulation = -1;

        if (selectedArticulation < 0 && focus == Focus::articulation)
            focus = Focus::group;

        if (selectedSlot >= (int) working->slots.size())
            selectedSlot = -1;

        if (selectedSlot < 0 && focus == Focus::slot)
            focus = Focus::map;

        refreshSlots();

        groupList.updateContent();
        selectQuietly (groupList, selectedGroup);
        articulationList.updateContent();
        selectQuietly (articulationList, selectedArticulation);

        rebuildDetailsSoon();
        updateStatus();
    }

    // Save the working copy if it is valid; otherwise say why not
    void commit()
    {
        if (! working.has_value())
            return;

        problems = working->validate();

        if (problems.isEmpty())
        {
            working->name = currentMap;
            auto params = new juce::DynamicObject();
            params->setProperty ("map", working->toVar());
            const auto reply = dispatcher.run ("expressionmap.set", juce::var (params));

            if (! (bool) reply["ok"])
                problems.add (reply["error"].toString());
            else
                syncedXml = working->toXml()->toString();
        }

        groupList.updateContent();
        articulationList.updateContent();
        refreshSlots();
        updateStatus();
    }

    void updateStatus()
    {
        if (! working.has_value())
        {
            status.setColour (juce::Label::textColourId, juce::Colours::grey);
            status.setText (mapNames.empty() ? "No expression maps yet. Create one with New." : "Select a map.",
                            juce::dontSendNotification);
            return;
        }

        if (problems.isEmpty())
        {
            status.setColour (juce::Label::textColourId, juce::Colours::grey);
            status.setText ("'" + currentMap + "' is saved with the project. Changes apply at once; names are renamed everywhere they are used.",
                            juce::dontSendNotification);
        }
        else
        {
            status.setColour (juce::Label::textColourId, juce::Colour (0xffe06c5c));
            status.setText ("Not saved - " + problems.joinIntoString ("; "), juce::dontSendNotification);
        }
    }

    //==========================================================================
    // Structure edits (add/remove/move). 'edit' changes the working copy and returns an error
    // sentence (empty = done); the result is saved when valid.
    void editStructure (std::function<juce::String (ExpressionMap&)> edit)
    {
        if (! working.has_value())
            return;

        auto copy = *working;

        if (const auto error = edit (copy); error.isNotEmpty())
        {
            problems = { error };
            updateStatus();
            return;
        }

        working = copy;
        commit();
        groupList.updateContent();
        selectQuietly (groupList, selectedGroup);
        articulationList.updateContent();
        selectQuietly (articulationList, selectedArticulation);
        refreshSlots();

        rebuildDetailsSoon();
    }

    static juce::String uniqueName (const juce::String& base, const std::function<bool (const juce::String&)>& exists)
    {
        if (! exists (base))
            return base;

        for (int i = 2;; ++i)
            if (const auto candidate = base + " " + juce::String (i); ! exists (candidate))
                return candidate;
    }

    juce::String addGroupTo (ExpressionMap& map)
    {
        ExpressionMap::Group group;
        group.name = uniqueName ("New group", [&map] (const juce::String& n) { return map.findGroup (n) != nullptr; });
        map.groups.push_back (group);
        selectedGroup = (int) map.groups.size() - 1;
        selectedArticulation = -1;
        focus = Focus::group;
        return {};
    }

    juce::String removeGroupFrom (ExpressionMap& map)
    {
        if (selectedGroup <= 0)
            return "The root group can't be removed.";

        map.groups.erase (map.groups.begin() + selectedGroup);
        selectedGroup = juce::jmin (selectedGroup, (int) map.groups.size() - 1);
        selectedArticulation = -1;
        focus = Focus::group;
        return {};
    }

    juce::String moveGroup (ExpressionMap& map, int direction)
    {
        const auto target = selectedGroup + direction;

        if (selectedGroup <= 0 || target < 1 || target >= (int) map.groups.size())
            return selectedGroup <= 0 ? "The root group stays first." : juce::String();

        std::swap (map.groups[(size_t) selectedGroup], map.groups[(size_t) target]);
        selectedGroup = target;
        return {};
    }

    juce::String addArticulationTo (ExpressionMap& map)
    {
        if (selectedGroup < 0 || selectedGroup >= (int) map.groups.size())
            return {};

        auto& group = map.groups[(size_t) selectedGroup];
        ExpressionMap::Articulation articulation;
        articulation.name = uniqueName ("New articulation", [&group] (const juce::String& n) { return ExpressionMap::findArticulation (group, n) != nullptr; });
        group.articulations.push_back (articulation);
        selectedArticulation = (int) group.articulations.size() - 1;
        focus = Focus::articulation;
        return {};
    }

    juce::String removeArticulationFrom (ExpressionMap& map)
    {
        if (selectedGroup < 0 || selectedArticulation < 0 || selectedGroup >= (int) map.groups.size())
            return {};

        auto& group = map.groups[(size_t) selectedGroup];

        if (selectedArticulation >= (int) group.articulations.size())
            return {};

        const auto name = group.articulations[(size_t) selectedArticulation].name;

        const auto usedBy = std::count_if (map.slots.begin(), map.slots.end(), [&] (const ExpressionMap::Slot& slot)
        {
            if (selectedGroup == 0)
                return ExpressionMap::sameName (slot.selection.root, name);

            return std::any_of (slot.selection.modifiers.begin(), slot.selection.modifiers.end(), [&] (const auto& m)
                                { return ExpressionMap::sameName (m.first, group.name) && ExpressionMap::sameName (m.second, name); });
        });

        if (usedBy > 0)
            return "'" + name + "' is in " + juce::String ((int) usedBy) + " sound slots; remove those first (filter the slots by its name)";

        // A root that a modifier applies to exclusively: removing it would silently widen that
        // modifier to every root (an empty list means all), so ask for that to be changed first
        if (selectedGroup == 0)
            for (size_t g = 1; g < map.groups.size(); ++g)
                for (auto& modifier : map.groups[g].articulations)
                    if (modifier.appliesTo.size() == 1 && ExpressionMap::sameName (modifier.appliesTo[0], name))
                        return "'" + modifier.name + "' (" + map.groups[g].name + ") applies only to '" + name
                               + "'. Change what it applies to first.";

        group.articulations.erase (group.articulations.begin() + selectedArticulation);

        if (selectedGroup == 0)   // the other modifiers that listed it forget it
            for (size_t g = 1; g < map.groups.size(); ++g)
                for (auto& modifier : map.groups[g].articulations)
                    for (int i = modifier.appliesTo.size(); --i >= 0;)
                        if (ExpressionMap::sameName (modifier.appliesTo[i], name))
                            modifier.appliesTo.remove (i);

        selectedArticulation = juce::jmin (selectedArticulation, (int) group.articulations.size() - 1);
        focus = selectedArticulation >= 0 ? Focus::articulation : Focus::group;
        return {};
    }

    juce::String moveArticulation (ExpressionMap& map, int direction)
    {
        if (selectedGroup < 0 || selectedGroup >= (int) map.groups.size())
            return {};

        auto& group = map.groups[(size_t) selectedGroup];
        const auto target = selectedArticulation + direction;

        if (selectedArticulation < 0 || target < 0 || target >= (int) group.articulations.size())
            return {};

        std::swap (group.articulations[(size_t) selectedArticulation], group.articulations[(size_t) target]);
        selectedArticulation = target;
        return {};
    }

    // A new slot: a copy of the selected one, or the first combination (a root, then a root with one
    // modifier) that has no slot yet. Its combination is then set in the details.
    juce::String addSlotTo (ExpressionMap& map, bool copySelected)
    {
        if (map.groups.empty() || map.groups.front().articulations.empty())
            return "the map needs a root articulation first";

        ExpressionMap::Slot slot;

        if (copySelected)
        {
            if (selectedSlot < 0 || selectedSlot >= (int) map.slots.size())
                return "select the slot to duplicate";

            slot = map.slots[(size_t) selectedSlot];
        }

        std::vector<ExpressionMap::Selection> candidates;

        for (auto& root : map.groups.front().articulations)
        {
            ExpressionMap::Selection alone;
            alone.root = root.name;
            candidates.push_back (alone);
        }

        for (auto& root : map.groups.front().articulations)
            for (size_t g = 1; g < map.groups.size(); ++g)
                for (auto& modifier : map.groups[g].articulations)
                {
                    ExpressionMap::Selection with;
                    with.root = root.name;
                    with.modifiers.emplace_back (map.groups[g].name, modifier.name);
                    candidates.push_back (with);
                }

        const auto free = std::find_if (candidates.begin(), candidates.end(),
                                        [&map] (const ExpressionMap::Selection& c) { return map.findSlot (c) == nullptr; });

        if (free == candidates.end())
            return "every root, and every root with one modifier, already has a slot: duplicate one and change its combination";

        slot.selection = *free;

        if (! map.hasSlots() && map.convertToSlots().isEmpty())   // the first slot: the map moves over to slots
            if (map.findSlot (slot.selection) != nullptr)
            {
                selectedSlot = (int) (map.findSlot (slot.selection) - map.slots.data());
                focus = Focus::slot;
                return {};
            }

        map.slots.push_back (slot);
        selectedSlot = (int) map.slots.size() - 1;
        focus = Focus::slot;
        return {};
    }

    juce::String removeSlotFrom (ExpressionMap& map)
    {
        if (selectedSlot < 0 || selectedSlot >= (int) map.slots.size())
            return "select the slot to remove";

        map.slots.erase (map.slots.begin() + selectedSlot);
        selectedSlot = juce::jmin (selectedSlot, (int) map.slots.size() - 1);
        focus = selectedSlot >= 0 ? Focus::slot : Focus::map;
        return {};
    }

    //==========================================================================
    // Maps: new, duplicate, delete
    void createMap()
    {
        ExpressionMap map;
        map.name = uniqueName ("New map", [this] (const juce::String& n) { return engine.getExpressionMap (n).has_value(); });
        ExpressionMap::Group root;
        root.name = "Articulation";
        ExpressionMap::Articulation normal;
        normal.name = "Normal";
        root.articulations.push_back (normal);
        map.groups.push_back (root);
        saveNewMap (map);
    }

    // Runs a library command; its error, if any, goes to the status line. True when it worked.
    bool runLibraryCommand (const juce::String& command, std::initializer_list<std::pair<const char*, juce::var>> values)
    {
        auto params = new juce::DynamicObject();

        for (auto& [key, value] : values)
            params->setProperty (juce::Identifier (key), value);

        const auto reply = dispatcher.run (command, juce::var (params));

        if (! (bool) reply["ok"])
        {
            problems = { reply["error"].toString() };
            updateStatus();
            return false;
        }

        return true;
    }

    void showLibraryMenu()
    {
        juce::PopupMenu menu, add;
        const auto library = dispatcher.run ("expressionmap.libraryList", {})["result"];
        juce::StringArray names;

        for (int i = 0; i < library.size(); ++i)
        {
            const auto name = library[i]["name"].toString();
            names.add (name);
            add.addItem (1000 + i, (bool) library[i]["template"] ? name + "  (template)" : name);
        }

        if (names.isEmpty())
            add.addItem (-1, "(the library is empty)", false, false);

        menu.addSubMenu ("Add to the project", add);
        menu.addItem (1, "Save this map to the library", currentMap.isNotEmpty());
        menu.addItem (2, "Delete from the library...", names.size() > 0);
        menu.addSeparator();
        menu.addItem (3, "Import from a file...");
        menu.addItem (4, "Export this map to a file...", currentMap.isNotEmpty());

        juce::Component::SafePointer<ExpressionMapEditorView> safe (this);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&libraryButton), [safe, names] (int result)
        {
            if (safe != nullptr && result != 0)
                safe->libraryChoice (result, names);
        });
    }

    void libraryChoice (int result, const juce::StringArray& names)
    {
        juce::Component::SafePointer<ExpressionMapEditorView> safe (this);

        if (result >= 1000)
        {
            const auto name = names[result - 1000];

            if (runLibraryCommand ("expressionmap.addFromLibrary", { { "name", name } }))
            {
                refresh();
                selectMap (name, true);
            }
        }
        else if (result == 1)
        {
            if (runLibraryCommand ("expressionmap.saveToLibrary", { { "name", currentMap } }))
            {
                problems = {};
                updateStatus();
            }
        }
        else if (result == 2)
        {
            juce::PopupMenu choose;

            for (int i = 0; i < names.size(); ++i)
                choose.addItem (1 + i, names[i]);

            choose.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&libraryButton), [safe, names] (int picked)
            {
                if (safe == nullptr || picked == 0)
                    return;

                const auto name = names[picked - 1];

                juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Delete from the library",
                                                    "Delete '" + name + "' from the library? Projects keep their own copies.",
                                                    "Delete", "Cancel", safe.getComponent(),
                                                    juce::ModalCallbackFunction::create ([safe, name] (int ok)
                {
                    if (safe != nullptr && ok == 1)
                        safe->runLibraryCommand ("expressionmap.deleteFromLibrary", { { "name", name } });
                }));
            });
        }
        else if (result == 3)
        {
            fileChooser = std::make_shared<juce::FileChooser> ("Import an expression map", juce::File(), "*.xml");

            fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                      [safe] (const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();

                if (safe != nullptr && file != juce::File() && safe->runLibraryCommand ("expressionmap.import", { { "path", file.getFullPathName() } }))
                    safe->refresh();
            });
        }
        else if (result == 4)
        {
            const auto name = currentMap;
            fileChooser = std::make_shared<juce::FileChooser> ("Export the expression map",
                                                               juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                                   .getChildFile (juce::File::createLegalFileName (name) + ".xml"),
                                                               "*.xml");

            fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                          | juce::FileBrowserComponent::warnAboutOverwriting,
                                      [safe, name] (const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();

                if (safe != nullptr && file != juce::File())
                    safe->runLibraryCommand ("expressionmap.export", { { "name", name }, { "path", file.getFullPathName() } });
            });
        }
    }

    void duplicateCurrentMap()
    {
        if (! working.has_value())
            return;

        auto copy = *working;
        copy.name = uniqueName (currentMap + " copy", [this] (const juce::String& n) { return engine.getExpressionMap (n).has_value(); });
        saveNewMap (copy);
    }

    void saveNewMap (const ExpressionMap& map)
    {
        auto params = new juce::DynamicObject();
        params->setProperty ("map", map.toVar());
        const auto reply = dispatcher.run ("expressionmap.set", juce::var (params));

        if (! (bool) reply["ok"])
        {
            problems = { reply["error"].toString() };
            updateStatus();
            return;
        }

        refresh();
        selectMap (map.name, true);
    }

    void deleteCurrentMap()
    {
        if (currentMap.isEmpty())
            return;

        const auto name = currentMap;
        auto usedBy = 0;

        for (auto& [instrumentId, instrumentName] : engine.getInstruments())
            for (auto& channel : engine.getInstrumentMidiChannels (instrumentId))
                if (ExpressionMap::sameName (channel.expressionMap, name))
                    ++usedBy;

        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Delete expression map",
                                            "Delete '" + name + "'?" + (usedBy > 0 ? " " + juce::String (usedBy)
                                                                          + (usedBy == 1 ? " instrument channel uses it and will show a missing map." : " instrument channels use it and will show a missing map.")
                                                                        : juce::String()),
                                            "Delete", "Cancel", this,
                                            juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<ExpressionMapEditorView> (this), name] (int result)
                                            {
                                                if (result != 1 || safe == nullptr)
                                                    return;

                                                auto params = new juce::DynamicObject();
                                                params->setProperty ("name", name);
                                                safe->dispatcher.run ("expressionmap.remove", juce::var (params));
                                                safe->clearSelection();
                                                safe->refresh();
                                            }));
    }

    //==========================================================================
    // Renames go through the commands, so the notes that use a name follow it
    bool renameViaCommand (const juce::String& command, std::initializer_list<std::pair<const char*, juce::String>> arguments)
    {
        if (! problems.isEmpty())
        {
            problems.insert (0, "Fix this first (it is not saved): ");   // names are renamed in the saved map
            updateStatus();
            return false;
        }

        auto params = new juce::DynamicObject();

        for (auto& [key, value] : arguments)
            params->setProperty (juce::Identifier (key), value);

        const auto reply = dispatcher.run (command, juce::var (params));

        if (! (bool) reply["ok"])
        {
            problems = { reply["error"].toString() };
            updateStatus();
            return false;
        }

        // Take the renamed map over (the name and every reference to it changed)
        if (const auto stored = engine.getExpressionMap (command == "expressionmap.rename" ? arguments.begin()[1].second : currentMap))
        {
            currentMap = stored->name;
            loadWorking (*stored);
        }

        refresh();
        return true;
    }

    //==========================================================================
    // The details panel: rebuilt when the selection changes, never from inside one of its own callbacks
    void rebuildDetailsSoon()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ExpressionMapEditorView> (this)]
                                         { if (safe != nullptr) safe->rebuildDetails(); });
    }

    struct Row
    {
        juce::Component* label = nullptr;     // may be null
        juce::Component* control = nullptr;
        int height = 26;
        int controlWidth = 0;                 // 0 = fill
    };

    template <typename T, typename... Args>
    T* own (Args&&... args)
    {
        auto component = std::make_unique<T> (std::forward<Args> (args)...);
        auto* raw = component.get();
        details.addAndMakeVisible (raw);
        detailComponents.push_back (std::move (component));
        return raw;
    }

    void addHeading (const juce::String& text)
    {
        auto* label = own<juce::Label> (juce::String(), text);
        label->setFont (juce::FontOptions (14.0f, juce::Font::bold));
        label->setColour (juce::Label::textColourId, juce::Colours::white);
        rows.push_back ({ nullptr, label, 28 });
    }

    void addHint (const juce::String& text, int height = 30)
    {
        auto* label = own<juce::Label> (juce::String(), text);
        label->setFont (juce::FontOptions (12.0f));
        label->setColour (juce::Label::textColourId, juce::Colours::grey);
        label->setMinimumHorizontalScale (1.0f);
        rows.push_back ({ nullptr, label, height });
    }

    juce::Label* makeLabel (const juce::String& text)
    {
        auto* label = own<juce::Label> (juce::String(), text);
        label->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
        return label;
    }

    // A text field that commits on Return and when it loses focus. 'commitText' returns false to
    // refuse the text (the field then shows the old value again).
    juce::TextEditor* addText (const juce::String& label, const juce::String& value,
                               std::function<bool (const juce::String&)> commitText, int height = 26, bool multiline = false)
    {
        auto* editor = own<juce::TextEditor>();
        editor->setMultiLine (multiline, true);
        editor->setReturnKeyStartsNewLine (multiline);
        editor->setText (value, false);
        editor->setFont (juce::FontOptions (13.0f));

        auto commitNow = [editor, value, commitText = std::move (commitText), original = std::make_shared<juce::String> (value)]
        {
            const auto text = editor->getText();

            if (text == *original)
                return;

            if (commitText (text))
                *original = text;
            else
                editor->setText (*original, false);
        };

        editor->onReturnKey = commitNow;
        editor->onFocusLost = commitNow;
        rows.push_back ({ makeLabel (label), editor, height });
        return editor;
    }

    // An integer field: empty = "unset" when allowEmpty, otherwise clamped to [lo, hi]
    juce::TextEditor* addNumber (const juce::String& label, std::optional<int> value, int lo, int hi, bool allowEmpty,
                                 std::function<void (std::optional<int>)> commitNumber, const juce::String& hint = {})
    {
        auto* editor = addText (label, value.has_value() ? juce::String (*value) : juce::String(),
                                [lo, hi, allowEmpty, commitNumber = std::move (commitNumber)] (const juce::String& text)
                                {
                                    if (text.trim().isEmpty())
                                    {
                                        if (! allowEmpty)
                                            return false;

                                        commitNumber (std::nullopt);
                                        return true;
                                    }

                                    commitNumber (juce::jlimit (lo, hi, text.getIntValue()));
                                    return true;
                                });

        editor->setInputRestrictions (6, "-0123456789");

        if (hint.isNotEmpty())
            editor->setTextToShowWhenEmpty (hint, juce::Colours::grey);

        return editor;
    }

    void rebuildDetails()
    {
        detailComponents.clear();
        rows.clear();

        if (! working.has_value())
        {
            layoutDetails();
            return;
        }

        const auto noteName = [] (int key) { return noteNames::name (key); };

        if (focus == Focus::slot && currentSlot() != nullptr)
            buildSlotDetails (noteName);
        else if (focus == Focus::articulation && currentArticulation() != nullptr)
            buildArticulationDetails (noteName);
        else if (focus == Focus::group && currentGroupMutable() != nullptr)
            buildGroupDetails();
        else
            buildMapDetails (noteName);

        layoutDetails();
    }

    void buildMapDetails (const std::function<juce::String (int)>& noteName)
    {
        addHeading ("Map");
        addText ("Name", working->name, [this] (const juce::String& text)
        {
            return renameViaCommand ("expressionmap.rename", { { "name", currentMap }, { "newName", text } });
        });
        addText ("Description", working->description, [this] (const juce::String& text)
        {
            working->description = text;
            commit();
            return true;
        }, 52, true);

        addHint ("Instrument channels refer to a map by name; renaming it here renames it there too.");

        addHeading ("Sound slots");

        if (working->hasSlots())
        {
            addHint (juce::String ((int) working->slots.size()) + " sound slots: every valid combination of articulations, with what it "
                     "sends. The articulation menu offers only combinations that have a slot; an articulation's defaults fill in the rest.", 44);
        }
        else
        {
            addHint ("This map adds up outputs set on its articulations. Sound slots give every combination its own outputs, key range "
                     "and timing instead, and the menu then offers only the combinations that exist.", 44);
            auto* convert = own<juce::TextButton> ("Make sound slots from the articulations");
            convert->setTooltip ("One slot per combination the applies-to lists allow, sending what it sends now");
            convert->onClick = [this] { editStructure ([] (ExpressionMap& m) { return m.convertToSlots(); }); };
            rows.push_back ({ nullptr, convert, 26, 290 });
        }

        addHeading ("Key names");
        addHint ("Name keys the way a library labels its keyboard (\"C0: Legato\"). The editor writes the name on the piano key and shows the "
                 "instruction as a tooltip. A keyswitch articulation's key is named automatically.", 44);

        for (size_t i = 0; i < working->keyNames.size(); ++i)
        {
            auto& keyName = working->keyNames[i];
            const auto index = i;

            addNumber ("Key " + noteName (keyName.key), keyName.key, 0, 127, false, [this, index] (std::optional<int> value)
            {
                if (index < working->keyNames.size() && value.has_value())
                {
                    working->keyNames[index].key = *value;
                    commit();
                    rebuildDetailsSoon();
                }
            });
            addText ("   name", keyName.name, [this, index] (const juce::String& text)
            {
                if (index < working->keyNames.size())
                {
                    working->keyNames[index].name = text;
                    commit();
                }

                return true;
            });
            addText ("   instruction", keyName.instruction, [this, index] (const juce::String& text)
            {
                if (index < working->keyNames.size())
                {
                    working->keyNames[index].instruction = text;
                    commit();
                }

                return true;
            }, 40, true);

            auto* remove = own<juce::TextButton> ("Remove this key name");
            remove->onClick = [this, index]
            {
                if (index < working->keyNames.size())
                {
                    working->keyNames.erase (working->keyNames.begin() + (std::ptrdiff_t) index);
                    commit();
                    rebuildDetailsSoon();
                }
            };
            rows.push_back ({ nullptr, remove, 26, 170 });
        }

        auto* add = own<juce::TextButton> ("Add a key name");
        add->onClick = [this]
        {
            int key = 24;

            while (std::any_of (working->keyNames.begin(), working->keyNames.end(), [key] (const auto& k) { return k.key == key; }) && key < 127)
                ++key;

            working->keyNames.push_back ({ key, "Name", {} });
            commit();
            rebuildDetailsSoon();
        };
        rows.push_back ({ nullptr, add, 26, 170 });
    }

    void buildGroupDetails()
    {
        auto* group = currentGroupMutable();
        const auto isRoot = selectedGroup == 0;

        addHeading (isRoot ? "Root group" : "Modifier group");
        addHint (isRoot ? "The main articulations (Staccato, Legato...). Every map has this group, first, and a note has at most one of them."
                        : "Modifiers (Release, Mute...) that go with a root articulation. A note has at most one from each group. "
                          "What a modifier applies to is set on the modifier itself.", 44);

        const auto oldName = group->name;
        addText ("Name", group->name, [this, oldName] (const juce::String& text)
        {
            return renameViaCommand ("expressionmap.renameGroup", { { "name", currentMap }, { "group", oldName }, { "newName", text } });
        });
        addText ("Description", group->description, [this] (const juce::String& text)
        {
            if (auto* g = currentGroupMutable())
            {
                g->description = text;
                commit();
            }

            return true;
        }, 52, true);
    }

    void buildArticulationDetails (const std::function<juce::String (int)>& noteName)
    {
        auto* articulation = currentArticulation();
        const auto* group = currentGroup();
        const auto isRoot = selectedGroup == 0;

        addHeading (juce::String (isRoot ? "Root articulation" : "Modifier") + " in '" + group->name + "'");

        const auto groupName = group->name, oldName = articulation->name;
        addText ("Name", articulation->name, [this, groupName, oldName] (const juce::String& text)
        {
            return renameViaCommand ("expressionmap.renameArticulation",
                                     { { "name", currentMap }, { "group", groupName }, { "articulation", oldName }, { "newName", text } });
        });
        addText ("Symbol", articulation->symbol, [this] (const juce::String& text)
        {
            if (auto* a = currentArticulation()) { a->symbol = text; commit(); }
            return true;
        });
        addText ("Description", articulation->description, [this] (const juce::String& text)
        {
            if (auto* a = currentArticulation()) { a->description = text; commit(); }
            return true;
        }, 52, true);

        if (working->hasSlots())
        {
            buildDefaults (*articulation);
            return;
        }

        // Timing offset: when the note is triggered; the switch still goes out just before it
        {
            auto* editor = addText ("Timing offset (ms)", juce::String (articulation->timingOffsetMs, 1), [this] (const juce::String& text)
            {
                if (auto* a = currentArticulation())
                {
                    a->timingOffsetMs = juce::jlimit (-5000.0, 5000.0, text.getDoubleValue());
                    commit();
                }

                return true;
            });
            editor->setInputRestrictions (8, "-.0123456789");
            addHint ("Negative = the note is triggered earlier (a legato that sounds late, e.g. -70). The keyswitch still goes out just before the note.", 30);
        }

        addNumber ("Playable keys from", articulation->keyLow >= 0 ? std::optional<int> (articulation->keyLow) : std::nullopt, 0, 127, true,
                   [this] (std::optional<int> value)
                   {
                       if (auto* a = currentArticulation())
                       {
                           a->keyLow = value.value_or (-1);
                           if (! value.has_value()) a->keyHigh = -1;
                           commit();
                       }
                   }, "any");
        addNumber ("   to", articulation->keyHigh >= 0 ? std::optional<int> (articulation->keyHigh) : std::nullopt, 0, 127, true,
                   [this] (std::optional<int> value)
                   {
                       if (auto* a = currentArticulation())
                       {
                           a->keyHigh = value.value_or (-1);
                           if (! value.has_value()) a->keyLow = -1;
                           commit();
                       }
                   }, "any");

        // Which roots a modifier works with
        if (! isRoot)
        {
            addHeading ("Applies to");
            auto* all = own<juce::ToggleButton> ("All root articulations");
            all->setToggleState (articulation->appliesTo.isEmpty(), juce::dontSendNotification);
            all->onClick = [this, all]
            {
                if (auto* a = currentArticulation())
                {
                    a->appliesTo.clear();

                    if (! all->getToggleState())   // "not all": start from every root, then take some away
                        for (auto& root : working->groups.front().articulations)
                            a->appliesTo.add (root.name);

                    commit();
                    rebuildDetailsSoon();
                }
            };
            rows.push_back ({ nullptr, all, 26 });

            if (! articulation->appliesTo.isEmpty())
                for (auto& root : working->groups.front().articulations)
                {
                    auto* toggle = own<juce::ToggleButton> ("   " + root.name);
                    toggle->setToggleState (articulation->appliesTo.contains (root.name, true), juce::dontSendNotification);
                    toggle->onClick = [this, toggle, rootName = root.name]
                    {
                        if (auto* a = currentArticulation())
                        {
                            for (int i = a->appliesTo.size(); --i >= 0;)
                                if (ExpressionMap::sameName (a->appliesTo[i], rootName))
                                    a->appliesTo.remove (i);

                            if (toggle->getToggleState())
                                a->appliesTo.add (rootName);

                            commit();
                            rebuildDetailsSoon();   // the last one off turns "All" back on
                        }
                    };
                    rows.push_back ({ nullptr, toggle, 24 });
                }
        }

        // Outputs, sent in series when the articulation becomes active
        addHeading ("Output (sent in this order)");
        addHint ("What reaches the instrument when this articulation becomes active. Nothing stops two articulations from using the same key or CC.", 30);

        buildOutputs ([this]() -> std::vector<Output>* { auto* a = currentArticulation(); return a != nullptr ? &a->outputs : nullptr; },
                      noteName);
    }

    // The output rows of a list (an articulation's or a slot's) and an Add button
    using OutputsOf = std::function<std::vector<Output>*()>;

    void buildOutputs (OutputsOf target, const std::function<juce::String (int)>& noteName)
    {
        for (size_t i = 0; i < target()->size(); ++i)
            buildOutputRow (i, noteName, target);

        auto* add = own<juce::TextButton> ("Add an output");
        add->onClick = [this, target]
        {
            if (auto* outputs = target())
            {
                Output output;
                output.type = working->hasSlots() ? Output::Type::programChange : Output::Type::keyswitch;
                output.number = working->hasSlots() ? 0 : 24;
                output.value = working->hasSlots() ? 0 : 100;
                outputs->push_back (output);
                commit();
                rebuildDetailsSoon();
            }
        };
        rows.push_back ({ nullptr, add, 26, 140 });
    }

    // Defaults (maps with slots): per other modifier group, what is chosen along with this articulation
    void buildDefaults (const ExpressionMap::Articulation& articulation)
    {
        addHeading ("Defaults");
        addHint ("Chosen along with this articulation when their group has nothing chosen yet (Rep. -> Tempo 120; a colour -> Long). "
                 "Choosing it then always gives a whole sound slot.", 44);

        for (size_t g = 1; g < working->groups.size(); ++g)
        {
            if ((int) g == selectedGroup)
                continue;

            const auto groupName = working->groups[g].name;
            auto* box = own<juce::ComboBox>();
            box->addItem (juce::String::fromUTF8 ("â"), 1);
            int selected = 1, id = 2;

            for (auto& candidate : working->groups[g].articulations)
            {
                box->addItem (candidate.name, id);

                for (auto& [dg, dn] : articulation.defaults)
                    if (ExpressionMap::sameName (dg, groupName) && ExpressionMap::sameName (dn, candidate.name))
                        selected = id;

                ++id;
            }

            box->setSelectedId (selected, juce::dontSendNotification);
            box->onChange = [this, box, groupName]
            {
                if (auto* a = currentArticulation())
                {
                    std::erase_if (a->defaults, [&] (const auto& d) { return ExpressionMap::sameName (d.first, groupName); });

                    if (box->getSelectedId() > 1)
                        a->defaults.emplace_back (groupName, box->getText());

                    commit();
                }
            };
            rows.push_back ({ makeLabel (groupName), box, 26, 220 });
        }
    }

    // A sound slot: its combination (one choice per group), timing offset, key range and outputs
    void buildSlotDetails (const std::function<juce::String (int)>& noteName)
    {
        auto* slot = currentSlot();
        addHeading ("Sound slot");
        addHint ("One combination of articulations and what it sends. The articulation menu offers only combinations that have a slot.", 30);

        for (size_t g = 0; g < working->groups.size(); ++g)
        {
            const auto& group = working->groups[g];
            auto* box = own<juce::ComboBox>();
            int selected = 0, id = 2;

            if (g > 0)
            {
                box->addItem (juce::String::fromUTF8 ("â"), 1);
                selected = 1;
            }

            for (auto& articulation : group.articulations)
            {
                box->addItem (articulation.name, id);

                if (g == 0 ? ExpressionMap::sameName (slot->selection.root, articulation.name)
                           : std::any_of (slot->selection.modifiers.begin(), slot->selection.modifiers.end(), [&] (const auto& m)
                                          { return ExpressionMap::sameName (m.first, group.name) && ExpressionMap::sameName (m.second, articulation.name); }))
                    selected = id;

                ++id;
            }

            box->setSelectedId (selected, juce::dontSendNotification);
            box->onChange = [this, box, g]
            {
                auto* s = currentSlot();

                if (s == nullptr || g >= working->groups.size())
                    return;

                const auto groupName = working->groups[g].name;

                if (g == 0)
                {
                    s->selection.root = box->getText();
                }
                else
                {
                    std::erase_if (s->selection.modifiers, [&] (const auto& m) { return ExpressionMap::sameName (m.first, groupName); });

                    if (box->getSelectedId() > 1)
                        s->selection.modifiers.emplace_back (groupName, box->getText());

                    s->selection = working->canonical (s->selection);
                }

                commit();
                rebuildDetailsSoon();
            };
            rows.push_back ({ makeLabel (group.name + (g == 0 ? " (root)" : "")), box, 26, 220 });
        }

        {
            auto* editor = addText ("Timing offset (ms)", juce::String (slot->timingOffsetMs, 1), [this] (const juce::String& text)
            {
                if (auto* s = currentSlot())
                {
                    s->timingOffsetMs = juce::jlimit (-5000.0, 5000.0, text.getDoubleValue());
                    commit();
                }

                return true;
            });
            editor->setInputRestrictions (8, "-.0123456789");
        }

        addNumber ("Playable keys from", slot->keyLow >= 0 ? std::optional<int> (slot->keyLow) : std::nullopt, 0, 127, true,
                   [this] (std::optional<int> value)
                   {
                       if (auto* s = currentSlot())
                       {
                           s->keyLow = value.value_or (-1);
                           if (! value.has_value()) s->keyHigh = -1;
                           commit();
                       }
                   }, "any");
        addNumber ("   to", slot->keyHigh >= 0 ? std::optional<int> (slot->keyHigh) : std::nullopt, 0, 127, true,
                   [this] (std::optional<int> value)
                   {
                       if (auto* s = currentSlot())
                       {
                           s->keyHigh = value.value_or (-1);
                           if (! value.has_value()) s->keyLow = -1;
                           commit();
                       }
                   }, "any");

        // Its notes' colour in the MIDI editor (top bar: "Colour: sound slot")
        {
            auto* box = own<juce::ComboBox>();
            box->addItem ("None", 1);
            int id = 2, selected = 1;

            for (auto& entry : colours::palette())
            {
                box->addItem (entry.name, id);

                if (slot->colour.equalsIgnoreCase (entry.hex))
                    selected = id;

                ++id;
            }

            if (selected == 1 && slot->colour.isNotEmpty())   // a colour outside the palette (a generated map)
            {
                box->addItem (slot->colour, id);
                selected = id;
            }

            box->setSelectedId (selected, juce::dontSendNotification);
            box->onChange = [this, box]
            {
                if (auto* s = currentSlot())
                {
                    const auto index = box->getSelectedId() - 2;
                    const auto& palette = colours::palette();

                    if (box->getSelectedId() == 1)
                        s->colour = {};
                    else if (index >= 0 && index < (int) palette.size())
                        s->colour = palette[(size_t) index].hex;

                    commit();
                    refreshSlots();
                }
            };
            rows.push_back ({ makeLabel ("Colour"), box, 26, 220 });
        }

        addHeading ("Output (sent in this order)");
        buildOutputs ([this]() -> std::vector<Output>* { auto* s = currentSlot(); return s != nullptr ? &s->outputs : nullptr; }, noteName);
    }

    void buildOutputRow (size_t index, const std::function<juce::String (int)>& noteName, OutputsOf target)
    {
        const auto output = (*target())[index];

        auto* type = own<juce::ComboBox>();
        type->addItem ("Keyswitch", 1);
        type->addItem ("CC", 2);
        type->addItem ("Program change", 3);
        type->setSelectedId (output.type == Output::Type::keyswitch ? 1 : output.type == Output::Type::controller ? 2 : 3,
                             juce::dontSendNotification);
        type->onChange = [this, index, type, target]
        {
            if (auto* outputs = target(); outputs != nullptr && index < outputs->size())
            {
                auto& o = (*outputs)[index];
                o.type = type->getSelectedId() == 1 ? Output::Type::keyswitch
                       : type->getSelectedId() == 2 ? Output::Type::controller : Output::Type::programChange;
                o.value = o.type == Output::Type::controller ? 0 : 100;
                o.held = false;
                o.bank = -1;
                commit();
                rebuildDetailsSoon();
            }
        };
        rows.push_back ({ makeLabel ("Output " + juce::String ((int) index + 1)), type, 26, 170 });

        const auto setOutput = [this, index, target] (const std::function<void (Output&)>& change)
        {
            if (auto* outputs = target(); outputs != nullptr && index < outputs->size())
            {
                change ((*outputs)[index]);
                commit();
            }
        };

        switch (output.type)
        {
            case Output::Type::keyswitch:
                addNumber ("   key" + juce::String (" (" + noteName (output.number) + ")"), output.number, 0, 127, false,
                           [setOutput, this] (std::optional<int> v) { setOutput ([v] (Output& o) { o.number = *v; }); rebuildDetailsSoon(); });
                addNumber ("   velocity", output.value, 1, 127, false, [setOutput] (std::optional<int> v) { setOutput ([v] (Output& o) { o.value = *v; }); });
                {
                    auto* held = own<juce::ToggleButton> ("   Held until the articulation changes (otherwise tapped)");
                    held->setToggleState (output.held, juce::dontSendNotification);
                    held->onClick = [setOutput, held] { setOutput ([held] (Output& o) { o.held = held->getToggleState(); }); };
                    rows.push_back ({ nullptr, held, 24 });
                }
                break;

            case Output::Type::controller:
                addNumber ("   CC number", output.number, 0, 127, false, [setOutput] (std::optional<int> v) { setOutput ([v] (Output& o) { o.number = *v; }); });
                addNumber ("   value", output.value, 0, 127, false, [setOutput] (std::optional<int> v) { setOutput ([v] (Output& o) { o.value = *v; }); });
                break;

            case Output::Type::programChange:
                addNumber ("   program", output.number, 0, 127, false, [setOutput] (std::optional<int> v) { setOutput ([v] (Output& o) { o.number = *v; }); });
                addNumber ("   bank", output.bank >= 0 ? std::optional<int> (output.bank) : std::nullopt, 0, 16383, true,
                           [setOutput] (std::optional<int> v) { setOutput ([v] (Output& o) { o.bank = v.value_or (-1); }); }, "none");
                break;
        }

        auto* remove = own<juce::TextButton> ("Remove output " + juce::String ((int) index + 1));
        remove->onClick = [this, index, target]
        {
            if (auto* outputs = target(); outputs != nullptr && index < outputs->size())
            {
                outputs->erase (outputs->begin() + (std::ptrdiff_t) index);
                commit();
                rebuildDetailsSoon();
            }
        };
        rows.push_back ({ nullptr, remove, 26, 140 });
    }

    void layoutDetails()
    {
        const auto width = juce::jmax (260, detailsViewport.getMaximumVisibleWidth() - 4);
        constexpr int labelWidth = 150, gap = 6;
        int y = 4;

        for (auto& row : rows)
        {
            const auto controlX = row.label != nullptr ? labelWidth + gap : 0;
            const auto controlWidth = row.controlWidth > 0 ? juce::jmin (row.controlWidth, width - controlX) : width - controlX - 4;

            if (row.label != nullptr)
                row.label->setBounds (0, y, labelWidth, row.height);

            row.control->setBounds (controlX, y, controlWidth, row.height);
            y += row.height + 4;
        }

        details.setSize (width, juce::jmax (y + 8, detailsViewport.getHeight()));
    }

    //==========================================================================
    AudioEngine& engine;
    CommandDispatcher& dispatcher;

    std::vector<juce::String> mapNames;
    juce::String currentMap, syncedXml;
    std::optional<ExpressionMap> working;
    juce::StringArray problems;
    int selectedGroup = 0, selectedArticulation = -1, selectedSlot = -1;
    std::vector<int> visibleSlots;   // indices into working->slots that pass the filter
    Focus focus = Focus::map;
    bool programmatic = false;

    juce::TextButton backButton { juce::String::fromUTF8 ("← Back") }, libraryButton { "Library..." };
    std::shared_ptr<juce::FileChooser> fileChooser;
    juce::Label titleLabel, status;
    ListModel mapModel, groupModel, articulationModel, slotModel;
    juce::ListBox mapList, groupList, articulationList, slotList;
    juce::TextEditor slotFilter;
    std::vector<std::unique_ptr<juce::Label>> headings;
    std::map<juce::ListBox*, juce::Label*> listHeadings;
    juce::TextButton newMap { "New" }, duplicateMap { "Duplicate" }, deleteMap { "Delete" },
                     addGroup { "Add" }, removeGroup { "Remove" }, groupUp { "Up" }, groupDown { "Down" },
                     addArticulation { "Add" }, removeArticulation { "Remove" }, articulationUp { "Up" }, articulationDown { "Down" },
                     addSlot { "Add" }, duplicateSlot { "Duplicate" }, removeSlot { "Remove" };

    juce::Viewport detailsViewport;
    juce::Component details;
    std::vector<std::unique_ptr<juce::Component>> detailComponents;
    std::vector<Row> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExpressionMapEditorView)
};
