#pragma once

#include <juce_core/juce_core.h>
#include <vector>

// Expression maps (MILESTONES.md "Articulation / expression maps").
//
// A map is a named, ordered list of groups. Group 0 is the ROOT group (the
// main articulations: Staccato, Legato...); every other group holds MODIFIERS
// (Release, Attack, Mute...). A modifier articulation says which root
// articulations it works with. A note's articulation is one root plus at most
// one modifier per group; modifiers only exist together with a root.
//
// Names are identifiers and compare case-insensitively ("Staccato" and
// "staccato" are the same name); the typed case is kept for display.
//
// A plain value type: edit a copy, share an immutable snapshot (like
// MidiSequence and TempoMap). Validation checks structure and value ranges and
// reports every problem as a sentence. It deliberately does NOT restrict how
// outputs combine: the same CC, key or program change in several articulations
// (even ones active together) is left to the user.
struct ExpressionMap
{
    // What an articulation sends to the instrument when it becomes active
    struct Output
    {
        enum class Type { keyswitch, controller, programChange };

        Type type = Type::keyswitch;
        int number = 0;          // keyswitch: key; controller: CC number; programChange: program (all 0..127)
        int value = 100;         // keyswitch: velocity (1..127); controller: value (0..127); unused for programChange
        bool held = false;       // keyswitch only: held while the note sounds, instead of tapped
        int bank = -1;           // programChange only: -1 = none, else 0..16383
    };

    struct Articulation
    {
        juce::String name;           // identifier, unique (ignoring case) within its group
        juce::String symbol;         // short glyph or text for notes and menus
        juce::String description;
        // Sent in this order, one after another, when the articulation becomes
        // active (e.g. a keyswitch, then a CC, then a program change). May be
        // empty: an articulation that needs nothing sent.
        std::vector<Output> outputs;
        double timingOffsetMs = 0.0; // when the note is triggered: < 0 earlier, > 0 later (0 = as written)
        int keyLow = -1, keyHigh = -1;   // optional playable key range (-1 = unspecified)

        // Modifier articulations only: the root articulations this works with
        // (names). Empty = every root articulation.
        juce::StringArray appliesTo;
    };

    struct Group
    {
        juce::String name;           // identifier, unique (ignoring case) within the map
        juce::String description;
        std::vector<Articulation> articulations;
    };

    // A note's articulation: one root articulation plus at most one modifier per
    // group, by name (names are identifiers, compared ignoring case). Stored as
    // chosen, never cleaned up: a name that isn't in the instrument's map is a
    // visible error, not a reason to erase the note's data. Empty = none.
    struct Selection
    {
        juce::String root;
        std::vector<std::pair<juce::String, juce::String>> modifiers;   // (group, articulation)

        bool isEmpty() const noexcept    { return root.trim().isEmpty() && modifiers.empty(); }

        bool operator== (const Selection& other) const
        {
            if (! sameName (root, other.root) || modifiers.size() != other.modifiers.size())
                return false;

            for (size_t i = 0; i < modifiers.size(); ++i)
                if (! sameName (modifiers[i].first, other.modifiers[i].first)
                     || ! sameName (modifiers[i].second, other.modifiers[i].second))
                    return false;

            return true;
        }

        bool operator!= (const Selection& other) const    { return ! (*this == other); }

        //   <ARTICULATION root="Legato"><MODIFIER group="Release" name="Short"/>...</ARTICULATION>
        std::unique_ptr<juce::XmlElement> toXml() const
        {
            auto xml = std::make_unique<juce::XmlElement> ("ARTICULATION");
            xml->setAttribute ("root", root);

            for (auto& [group, articulationName] : modifiers)
            {
                auto* m = xml->createNewChildElement ("MODIFIER");
                m->setAttribute ("group", group);
                m->setAttribute ("name", articulationName);
            }

            return xml;
        }

        static Selection fromXml (const juce::XmlElement& xml)
        {
            Selection selection;
            selection.root = xml.getStringAttribute ("root");

            for (auto* m : xml.getChildWithTagNameIterator ("MODIFIER"))
                selection.modifiers.emplace_back (m->getStringAttribute ("group"), m->getStringAttribute ("name"));

            return selection;
        }
    };

    juce::String name;               // identifier of the map
    juce::String description;
    std::vector<Group> groups;       // [0] = the root group

    //==========================================================================
    static bool sameName (const juce::String& a, const juce::String& b)
    {
        return a.trim().equalsIgnoreCase (b.trim());
    }

    const Group* rootGroup() const noexcept     { return groups.empty() ? nullptr : &groups.front(); }

    // The first root articulation (the default root, if the user turns that on)
    const Articulation* firstRoot() const noexcept
    {
        if (groups.empty() || groups.front().articulations.empty())
            return nullptr;

        return &groups.front().articulations.front();
    }

    const Group* findGroup (const juce::String& groupName) const
    {
        for (auto& group : groups)
            if (sameName (group.name, groupName))
                return &group;

        return nullptr;
    }

    static const Articulation* findArticulation (const Group& group, const juce::String& articulationName)
    {
        for (auto& articulation : group.articulations)
            if (sameName (articulation.name, articulationName))
                return &articulation;

        return nullptr;
    }

    const Articulation* findArticulation (const juce::String& groupName, const juce::String& articulationName) const
    {
        if (auto* group = findGroup (groupName))
            return findArticulation (*group, articulationName);

        return nullptr;
    }

    // Does this modifier work with the named root articulation?
    static bool appliesToRoot (const Articulation& modifier, const juce::String& rootName)
    {
        if (modifier.appliesTo.isEmpty())
            return true;

        for (auto& name : modifier.appliesTo)
            if (sameName (name, rootName))
                return true;

        return false;
    }

    //==========================================================================
    // Every problem with the map, each a sentence (empty = valid)
    juce::StringArray validate() const
    {
        juce::StringArray problems;
        const auto mapLabel = "map '" + name + "'";

        if (name.trim().isEmpty())
            problems.add ("a map needs a name");

        if (groups.empty())
        {
            problems.add (mapLabel + " has no groups; it needs a root group (the first group)");
            return problems;
        }

        for (size_t g = 0; g < groups.size(); ++g)
        {
            auto& group = groups[g];
            const auto groupLabel = (g == 0 ? "root group '" : "group '") + group.name + "'";

            if (group.name.trim().isEmpty())
                problems.add ("group " + juce::String ((int) g + 1) + " needs a name");

            for (size_t other = 0; other < g; ++other)
                if (! group.name.trim().isEmpty() && sameName (groups[other].name, group.name))
                    problems.add ("two groups are named '" + group.name + "' (names ignore case); "
                                  "group names must be unique within the map");

            for (size_t a = 0; a < group.articulations.size(); ++a)
            {
                auto& articulation = group.articulations[a];
                const auto label = "'" + articulation.name + "' in " + groupLabel;

                if (articulation.name.trim().isEmpty())
                    problems.add ("articulation " + juce::String ((int) a + 1) + " in " + groupLabel + " needs a name");

                for (size_t other = 0; other < a; ++other)
                    if (! articulation.name.trim().isEmpty() && sameName (group.articulations[other].name, articulation.name))
                        problems.add (groupLabel + " has two articulations named '" + articulation.name
                                      + "' (names ignore case); articulation names must be unique within a group");

                for (size_t o = 0; o < articulation.outputs.size(); ++o)
                    validateOutput (articulation.outputs[o],
                                    articulation.outputs.size() > 1 ? label + " (output " + juce::String ((int) o + 1) + ")" : label,
                                    problems);

                if (articulation.keyLow != -1 || articulation.keyHigh != -1)
                    if (articulation.keyLow < 0 || articulation.keyHigh > 127 || articulation.keyLow > articulation.keyHigh)
                        problems.add (label + " has an invalid key range " + juce::String (articulation.keyLow) + "-"
                                      + juce::String (articulation.keyHigh) + " (0-127, low <= high, or both unset)");

                if (std::abs (articulation.timingOffsetMs) > 5000.0)
                    problems.add (label + " has a timing offset of " + juce::String (articulation.timingOffsetMs)
                                  + " ms; the limit is 5000 ms either way");

                if (g == 0)
                {
                    if (! articulation.appliesTo.isEmpty())
                        problems.add (label + " is a root articulation and can't have an 'applies to' list "
                                      "(only modifiers do)");
                }
                else
                {
                    for (auto& rootName : articulation.appliesTo)
                        if (findArticulation (groups.front(), rootName) == nullptr)
                            problems.add (label + " applies to '" + rootName + "', which is not a root articulation "
                                          "(the root group has: " + names (groups.front()) + ")");
                }
            }
        }

        return problems;
    }

    bool isValid() const    { return validate().isEmpty(); }

    //==========================================================================
    // Choosing articulations. The editor's menu and the API share these rules:
    //  - every item toggles (choose it again to unselect it);
    //  - a modifier group is exclusive: once an item is chosen, the group's other
    //    items are unavailable until it is unselected;
    //  - the root group is not: choosing another root switches to it directly;
    //  - modifiers only exist together with a root, and only those that apply to
    //    it are offered;
    //  - when the root changes, modifiers that still apply are kept and the others
    //    would be dropped - reported in 'dropped', so the caller can ask the user
    //    first or just accept it (a setting).
    struct Choice
    {
        Selection selection;     // the result (the input, unchanged, when it failed)
        std::vector<std::pair<juce::String, juce::String>> dropped;   // (group, articulation) that the change removes
        juce::String error;      // empty = done

        bool ok() const noexcept    { return error.isEmpty(); }
    };

    Choice choose (const Selection& current, const juce::String& groupName, const juce::String& articulationName) const
    {
        Choice result;
        result.selection = current;

        // A refusal leaves the selection as it was
        const auto refuse = [&result] (const juce::String& message)
        {
            result.error = message;
            return result;
        };

        const auto* group = findGroup (groupName);

        if (group == nullptr)
            return refuse ("no group '" + groupName + "' in map '" + name + "' (groups: " + groupNames() + ")");

        const auto* articulation = findArticulation (*group, articulationName);

        if (articulation == nullptr)
            return refuse ("no articulation '" + articulationName + "' in group '" + group->name
                                                   + "' (it has: " + names (*group) + ")");


        if (group == &groups.front())
        {
            if (sameName (current.root, articulation->name))
            {
                // Unselecting the root: no root, so no modifiers either
                result.selection = {};
                result.dropped = current.modifiers;
                return result;
            }

            result.selection = {};
            result.selection.root = articulation->name;

            for (auto& modifier : current.modifiers)
            {
                const auto* existing = findArticulation (modifier.first, modifier.second);

                if (existing != nullptr && appliesToRoot (*existing, articulation->name))
                    result.selection.modifiers.push_back (modifier);
                else
                    result.dropped.push_back (modifier);
            }

            return result;
        }

        // A modifier
        if (current.root.trim().isEmpty())
            return refuse ("modifiers need a root articulation; choose one from '" + groups.front().name
                                                   + "' first (" + names (groups.front()) + ")");

        if (findArticulation (groups.front(), current.root) == nullptr)
            return refuse ("the root articulation '" + current.root + "' is not in map '" + name
                                                   + "'; choose a root from '" + groups.front().name + "' first");

        if (! appliesToRoot (*articulation, current.root))
            return refuse ("'" + articulation->name + "' (group '" + group->name
                                                   + "') doesn't apply to the root '" + current.root + "'; it applies to: "
                                                   + articulation->appliesTo.joinIntoString (", "));

        for (size_t i = 0; i < current.modifiers.size(); ++i)
        {
            if (! sameName (current.modifiers[i].first, group->name))
                continue;

            if (sameName (current.modifiers[i].second, articulation->name))
            {
                result.selection.modifiers.erase (result.selection.modifiers.begin() + (std::ptrdiff_t) i);   // toggled off
                return result;
            }

            return refuse ("group '" + group->name + "' already has '" + current.modifiers[i].second
                                                   + "' chosen; unselect it first (choosing it again toggles it off)");
        }

        result.selection.modifiers.emplace_back (group->name, articulation->name);

        // Canonical order: the groups' order in the map (so equal choices compare equal)
        std::stable_sort (result.selection.modifiers.begin(), result.selection.modifiers.end(),
                          [this] (const auto& a, const auto& b) { return groupIndex (a.first) < groupIndex (b.first); });
        return result;
    }

    // The modifiers the root allows, per group, for the menu. Groups with
    // nothing left are omitted.
    struct Available
    {
        const Group* group = nullptr;
        std::vector<const Articulation*> articulations;
    };

    std::vector<Available> availableModifiers (const juce::String& rootName) const
    {
        std::vector<Available> result;

        for (size_t g = 1; g < groups.size(); ++g)
        {
            Available available;
            available.group = &groups[g];

            for (auto& articulation : groups[g].articulations)
                if (appliesToRoot (articulation, rootName))
                    available.articulations.push_back (&articulation);

            if (! available.articulations.empty())
                result.push_back (std::move (available));
        }

        return result;
    }

    // Everything wrong with a note's choice against this map (empty = fine).
    // The editor marks these notes as errors; nothing is erased.
    juce::StringArray problemsOf (const Selection& selection) const
    {
        juce::StringArray problems;

        if (groups.empty())
        {
            if (! selection.isEmpty())
                problems.add ("map '" + name + "' has no groups");

            return problems;
        }

        const auto* root = selection.root.trim().isEmpty() ? nullptr : findArticulation (groups.front(), selection.root);

        if (selection.root.trim().isEmpty() && ! selection.modifiers.empty())
            problems.add ("modifiers need a root articulation");

        if (selection.root.trim().isNotEmpty() && root == nullptr)
            problems.add ("root articulation '" + selection.root + "' is not in map '" + name + "' (it has: "
                          + names (groups.front()) + ")");

        juce::StringArray seenGroups;

        for (auto& [groupName, articulationName] : selection.modifiers)
        {
            const auto* group = findGroup (groupName);

            if (group == nullptr || group == &groups.front())
            {
                problems.add (group == nullptr ? "group '" + groupName + "' is not in map '" + name + "'"
                                               : "'" + groupName + "' is the root group, not a modifier group");
                continue;
            }

            if (seenGroups.contains (group->name, true))
                problems.add ("two articulations are chosen from group '" + group->name + "' (only one is allowed)");

            seenGroups.add (group->name);
            const auto* articulation = findArticulation (*group, articulationName);

            if (articulation == nullptr)
                problems.add ("'" + articulationName + "' is not in group '" + group->name + "' (it has: " + names (*group) + ")");
            else if (root != nullptr && ! appliesToRoot (*articulation, root->name))
                problems.add ("'" + articulation->name + "' (group '" + group->name + "') doesn't apply to the root '"
                              + root->name + "'");
        }

        return problems;
    }

    //==========================================================================
    // Project / library files. Group order is preserved (the first is the root
    // group). Reading is forgiving: missing attributes take their defaults and
    // unknown output types are skipped; validate() says what is wrong.
    //
    //   <EXPRESSIONMAP name description>
    //     <GROUP name description>
    //       <ARTICULATION name symbol description timingOffsetMs keyLow keyHigh>
    //         <APPLIESTO root="Staccato"/> ...
    //         <OUTPUT type="keyswitch|controller|programChange" number value held bank/> ...
    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("EXPRESSIONMAP");
        xml->setAttribute ("name", name);
        xml->setAttribute ("description", description);

        for (auto& group : groups)
        {
            auto* g = xml->createNewChildElement ("GROUP");
            g->setAttribute ("name", group.name);
            g->setAttribute ("description", group.description);

            for (auto& articulation : group.articulations)
            {
                auto* a = g->createNewChildElement ("ARTICULATION");
                a->setAttribute ("name", articulation.name);
                a->setAttribute ("symbol", articulation.symbol);
                a->setAttribute ("description", articulation.description);
                a->setAttribute ("timingOffsetMs", articulation.timingOffsetMs);
                a->setAttribute ("keyLow", articulation.keyLow);
                a->setAttribute ("keyHigh", articulation.keyHigh);

                for (auto& root : articulation.appliesTo)
                    a->createNewChildElement ("APPLIESTO")->setAttribute ("root", root);

                for (auto& output : articulation.outputs)
                {
                    auto* o = a->createNewChildElement ("OUTPUT");
                    o->setAttribute ("type", typeToString (output.type));
                    o->setAttribute ("number", output.number);
                    o->setAttribute ("value", output.value);
                    o->setAttribute ("held", output.held);
                    o->setAttribute ("bank", output.bank);
                }
            }
        }

        return xml;
    }

    static ExpressionMap fromXml (const juce::XmlElement& xml)
    {
        ExpressionMap map;
        map.name = xml.getStringAttribute ("name");
        map.description = xml.getStringAttribute ("description");

        for (auto* g : xml.getChildWithTagNameIterator ("GROUP"))
        {
            Group group;
            group.name = g->getStringAttribute ("name");
            group.description = g->getStringAttribute ("description");

            for (auto* a : g->getChildWithTagNameIterator ("ARTICULATION"))
            {
                Articulation articulation;
                articulation.name = a->getStringAttribute ("name");
                articulation.symbol = a->getStringAttribute ("symbol");
                articulation.description = a->getStringAttribute ("description");
                articulation.timingOffsetMs = a->getDoubleAttribute ("timingOffsetMs", 0.0);
                articulation.keyLow = a->getIntAttribute ("keyLow", -1);
                articulation.keyHigh = a->getIntAttribute ("keyHigh", -1);

                for (auto* applies : a->getChildWithTagNameIterator ("APPLIESTO"))
                    articulation.appliesTo.add (applies->getStringAttribute ("root"));

                for (auto* o : a->getChildWithTagNameIterator ("OUTPUT"))
                {
                    Output output;

                    if (! typeFromString (o->getStringAttribute ("type"), output.type))
                        continue;

                    output.number = o->getIntAttribute ("number", 0);
                    output.value = o->getIntAttribute ("value", 100);
                    output.held = o->getBoolAttribute ("held", false);
                    output.bank = o->getIntAttribute ("bank", -1);
                    articulation.outputs.push_back (output);
                }

                group.articulations.push_back (std::move (articulation));
            }

            map.groups.push_back (std::move (group));
        }

        return map;
    }

private:
    juce::String groupNames() const
    {
        juce::StringArray list;

        for (auto& group : groups)
            list.add (group.name);

        return list.joinIntoString (", ");
    }

    size_t groupIndex (const juce::String& groupName) const
    {
        for (size_t g = 0; g < groups.size(); ++g)
            if (sameName (groups[g].name, groupName))
                return g;

        return groups.size();
    }

    static const char* typeToString (Output::Type type)
    {
        switch (type)
        {
            case Output::Type::keyswitch:      return "keyswitch";
            case Output::Type::controller:     return "controller";
            case Output::Type::programChange:  return "programChange";
        }

        return "keyswitch";
    }

    static bool typeFromString (const juce::String& text, Output::Type& type)
    {
        if (text == "keyswitch")      { type = Output::Type::keyswitch;      return true; }
        if (text == "controller")     { type = Output::Type::controller;     return true; }
        if (text == "programChange")  { type = Output::Type::programChange;  return true; }
        return false;
    }

    static juce::String names (const Group& group)
    {
        juce::StringArray list;

        for (auto& articulation : group.articulations)
            list.add (articulation.name);

        return list.isEmpty() ? "no articulations" : list.joinIntoString (", ");
    }

    static void validateOutput (const Output& output, const juce::String& label, juce::StringArray& problems)
    {
        const auto in = [] (int v, int lo, int hi) { return v >= lo && v <= hi; };

        switch (output.type)
        {
            case Output::Type::keyswitch:
                if (! in (output.number, 0, 127))   problems.add (label + ": keyswitch key " + juce::String (output.number) + " is outside 0-127");
                if (! in (output.value, 1, 127))    problems.add (label + ": keyswitch velocity " + juce::String (output.value) + " is outside 1-127");
                break;

            case Output::Type::controller:
                if (! in (output.number, 0, 127))   problems.add (label + ": CC number " + juce::String (output.number) + " is outside 0-127");
                if (! in (output.value, 0, 127))    problems.add (label + ": CC value " + juce::String (output.value) + " is outside 0-127");
                break;

            case Output::Type::programChange:
                if (! in (output.number, 0, 127))   problems.add (label + ": program " + juce::String (output.number) + " is outside 0-127");
                if (output.bank != -1 && ! in (output.bank, 0, 16383))
                    problems.add (label + ": bank " + juce::String (output.bank) + " is outside 0-16383 (or -1 for none)");
                break;
        }
    }
};
