#pragma once

#include "ExpressionMap.h"

// What the MIDI editor shows and does with articulations, as plain logic (the
// views only draw it): the menu's contents, the effective choice with the
// "first root articulation as default" setting, labels and symbols.
// MILESTONES.md "Articulation / expression maps" > "The editor".
namespace articulations
{
    using Selection = ExpressionMap::Selection;

    // The choice as the editor treats it. With the default-root setting on, a
    // note with no root counts as having the map's first root articulation; it
    // is implicit - nothing is written to the note.
    inline Selection effective (const ExpressionMap& map, const Selection& stored, bool useFirstRootAsDefault)
    {
        if (! useFirstRootAsDefault || stored.root.trim().isNotEmpty())
            return stored;

        if (const auto* first = map.firstRoot())
        {
            Selection result;
            result.root = first->name;
            return result;   // modifiers can't exist without a root, so none are carried over
        }

        return stored;
    }

    // Does everything in the choice exist in the map and fit together? (Cheap:
    // the editor asks for every visible note; problemsOf() says what is wrong.)
    inline bool resolves (const ExpressionMap& map, const Selection& selection)
    {
        if (selection.isEmpty())
            return true;

        const auto* root = selection.root.trim().isEmpty() || map.groups.empty()
                             ? nullptr : ExpressionMap::findArticulation (map.groups.front(), selection.root);

        if (root == nullptr)
            return false;   // modifiers without a root, or a root that isn't in the map

        for (size_t i = 0; i < selection.modifiers.size(); ++i)
        {
            const auto* group = map.findGroup (selection.modifiers[i].first);

            if (group == nullptr || group == map.rootGroup())
                return false;

            const auto* articulation = ExpressionMap::findArticulation (*group, selection.modifiers[i].second);

            if (articulation == nullptr || ! ExpressionMap::appliesToRoot (*articulation, root->name))
                return false;

            for (size_t j = 0; j < i; ++j)
                if (ExpressionMap::sameName (selection.modifiers[j].first, group->name))
                    return false;   // two from one group
        }

        return true;
    }

    // "Legato + Short"; the names as written in the map when they resolve
    inline juce::String label (const Selection& selection)
    {
        juce::StringArray parts;

        if (selection.root.trim().isNotEmpty())
            parts.add (selection.root);

        for (auto& modifier : selection.modifiers)
            parts.add (modifier.second);

        return parts.joinIntoString (" + ");
    }

    // The glyphs for a note: the root's symbol, then the modifiers'. An
    // articulation with no symbol is shown by the start of its name. Names the
    // map doesn't have are shown as written.
    inline juce::String symbols (const ExpressionMap& map, const Selection& selection)
    {
        const auto symbolOf = [&map] (const ExpressionMap::Group* group, const juce::String& name)
        {
            if (group != nullptr)
                if (const auto* articulation = ExpressionMap::findArticulation (*group, name))
                    if (articulation->symbol.trim().isNotEmpty())
                        return articulation->symbol.trim();

            return name.trim().substring (0, 3);
        };

        juce::String text;

        if (selection.root.trim().isNotEmpty())
            text = symbolOf (map.rootGroup(), selection.root);

        for (auto& [groupName, name] : selection.modifiers)
            text += symbolOf (map.findGroup (groupName), name);

        return text;
    }

    // The symbols of a choice one by one (the root's, then the modifiers'), only those that have
    // one - each a SMuFL glyph name or a text (ui/Smufl.h draws both)
    inline juce::StringArray symbolParts (const ExpressionMap& map, const Selection& selection)
    {
        juce::StringArray parts;

        const auto add = [&parts] (const ExpressionMap::Group* group, const juce::String& name)
        {
            if (group != nullptr)
                if (const auto* articulation = ExpressionMap::findArticulation (*group, name))
                    if (articulation->symbol.trim().isNotEmpty())
                        parts.add (articulation->symbol.trim());
        };

        if (selection.root.trim().isNotEmpty())
            add (map.rootGroup(), selection.root);

        for (auto& [groupName, name] : selection.modifiers)
            add (map.findGroup (groupName), name);

        return parts;
    }

    //==========================================================================
    // The menu. Roots are always all there and always available; a modifier
    // group shows only the modifiers that apply to the root, and an item is
    // greyed out when another item of its group is chosen. Every item toggles.
    // With sound slots a group shows what some slot has with the choices of the
    // groups before it, and another item of a chosen group replaces it.
    struct MenuItem
    {
        enum class Kind { header, item };

        Kind kind = Kind::item;
        juce::String text;           // symbol + name (a header: the group's name)
        juce::String symbol;         // the articulation's own symbol (may be empty)
        juce::String description;
        juce::String group, name;    // what choosing it passes to choose()
        bool isRoot = false;
        bool enabled = true;
        bool ticked = false;         // chosen on every target (a partial choice is not ticked)
        bool sameDepthAsPrevious = false;   // a header: an alternative to the group shown before it (stack them)
    };

    // 'targets': the choice of each note being edited (or the one for new notes),
    // already effective(). With no root on a target, no modifiers are offered.
    inline std::vector<MenuItem> buildMenu (const ExpressionMap& map, const std::vector<Selection>& targets)
    {
        std::vector<MenuItem> menu;

        if (map.groups.empty())
            return menu;

        const auto symbolled = [] (const ExpressionMap::Articulation& a)
        {
            return a.symbol.trim().isNotEmpty() ? a.symbol.trim() + "  " + a.name : a.name;
        };

        const auto allTargets = [&targets] (auto&& predicate)
        {
            return ! targets.empty() && std::all_of (targets.begin(), targets.end(), predicate);
        };

        // Roots
        {
            MenuItem header;
            header.kind = MenuItem::Kind::header;
            header.text = map.groups.front().name;
            menu.push_back (header);

            for (auto& root : map.groups.front().articulations)
            {
                if (map.hasSlots() && ! map.rootHasSlots (root.name))
                    continue;   // no sound slot uses it

                MenuItem item;
                item.text = symbolled (root);
                item.description = root.description;
                item.symbol = root.symbol.trim();
                item.group = map.groups.front().name;
                item.name = root.name;
                item.isRoot = true;
                item.ticked = allTargets ([&] (const Selection& s) { return ExpressionMap::sameName (s.root, root.name); });
                menu.push_back (item);
            }
        }

        // Modifiers need a root on every target
        const auto everyTargetHasARoot = allTargets ([&map] (const Selection& s)
        {
            return s.root.trim().isNotEmpty() && ExpressionMap::findArticulation (map.groups.front(), s.root) != nullptr;
        });

        if (! everyTargetHasARoot)
            return menu;

        size_t lastShown = 0;   // the modifier group shown last (0: none yet)

        for (size_t g = 1; g < map.groups.size(); ++g)
        {
            std::vector<MenuItem> items;

            for (auto& modifier : map.groups[g].articulations)
            {
                // Only what every target can have: with sound slots, what some slot has
                // together with the target's choices in the groups before this one
                if (! allTargets ([&] (const Selection& s)
                {
                    if (! map.hasSlots())
                        return ExpressionMap::appliesToRoot (modifier, s.root);

                    return map.offers (s, g, modifier.name);
                }))
                    continue;

                MenuItem item;
                item.text = symbolled (modifier);
                item.description = modifier.description;
                item.symbol = modifier.symbol.trim();
                item.group = map.groups[g].name;
                item.name = modifier.name;
                item.ticked = allTargets ([&] (const Selection& s)
                {
                    return std::any_of (s.modifiers.begin(), s.modifiers.end(), [&] (const auto& m)
                    {
                        return ExpressionMap::sameName (m.first, map.groups[g].name) && ExpressionMap::sameName (m.second, modifier.name);
                    });
                });
                item.enabled = allTargets ([&] (const Selection& s) { return map.choose (s, item.group, item.name).ok(); });
                items.push_back (item);
            }

            if (items.empty())
                continue;   // nothing in this group applies: the group is not offered

            MenuItem header;
            header.kind = MenuItem::Kind::header;
            header.text = map.groups[g].name;
            header.sameDepthAsPrevious = map.hasSlots() && lastShown > 0
                                           && allTargets ([&] (const Selection& s) { return map.sameDepth (s, lastShown, g); });
            menu.push_back (header);
            menu.insert (menu.end(), items.begin(), items.end());
            lastShown = g;
        }

        return menu;
    }

    //==========================================================================
    // Choosing one item for every target: the result per target, and the
    // modifiers the change would drop (so the caller can ask first).
    struct Applied
    {
        std::vector<Selection> results;
        std::vector<std::pair<juce::String, juce::String>> dropped;   // (group, articulation), without repeats
        juce::String error;                                           // empty = ok

        bool ok() const noexcept    { return error.isEmpty(); }
    };

    inline Applied apply (const ExpressionMap& map, const std::vector<Selection>& targets,
                          const juce::String& group, const juce::String& name)
    {
        Applied applied;

        for (size_t i = 0; i < targets.size(); ++i)
        {
            const auto choice = map.choose (targets[i], group, name);

            if (! choice.ok())
            {
                applied.error = targets.size() > 1 ? "note " + juce::String ((int) i + 1) + ": " + choice.error : choice.error;
                applied.results.clear();
                applied.dropped.clear();
                return applied;
            }

            applied.results.push_back (choice.selection);

            for (auto& dropped : choice.dropped)
                if (std::none_of (applied.dropped.begin(), applied.dropped.end(), [&] (const auto& d)
                                  { return ExpressionMap::sameName (d.first, dropped.first) && ExpressionMap::sameName (d.second, dropped.second); }))
                    applied.dropped.push_back (dropped);
        }

        return applied;
    }

    // Does every target already show the same choice? ("Mixed" otherwise)
    inline bool allEqual (const std::vector<Selection>& targets)
    {
        return std::all_of (targets.begin(), targets.end(), [&] (const Selection& s) { return s == targets.front(); });
    }
}
