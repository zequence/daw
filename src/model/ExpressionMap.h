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
// SOUND SLOTS (MILESTONES.md "Sound slots"): a map with slots says what every
// valid combination of articulations sends - each slot is one combination (one
// root plus at most one modifier per group) with its own outputs, key range and
// timing offset; nothing is added up. Groups and articulations are then only
// names for the menu, which follows the slots: groups form a chain in map order,
// and an articulation is offered when some slot has it together with the choices
// of the groups before its own. Choosing an articulation fills in its defaults
// (Rep. -> Tempo 120), so a note's choice is always the combination of a slot.
// A map without slots still adds up articulation outputs (until every map has
// slots).
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

        // Maps with slots: chosen along with this articulation when their group
        // has nothing chosen yet, as (group, articulation): Rep. -> (Tempo, 120)
        std::vector<std::pair<juce::String, juce::String>> defaults;
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

        // Follow a rename in the map. Each returns whether this selection changed.
        bool renameRoot (const juce::String& oldName, const juce::String& newName)
        {
            if (! sameName (root, oldName) || root == newName)
                return false;

            root = newName;
            return true;
        }

        bool renameModifierGroup (const juce::String& oldName, const juce::String& newName)
        {
            auto changed = false;

            for (auto& modifier : modifiers)
                if (sameName (modifier.first, oldName) && modifier.first != newName)
                {
                    modifier.first = newName;
                    changed = true;
                }

            return changed;
        }

        bool renameModifier (const juce::String& groupName, const juce::String& oldName, const juce::String& newName)
        {
            auto changed = false;

            for (auto& modifier : modifiers)
                if (sameName (modifier.first, groupName) && sameName (modifier.second, oldName) && modifier.second != newName)
                {
                    modifier.second = newName;
                    changed = true;
                }

            return changed;
        }

        // {root, modifiers:[{group, name}]}
        juce::var toVar() const
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("root", root);

            juce::Array<juce::var> list;

            for (auto& [group, articulationName] : modifiers)
            {
                auto m = new juce::DynamicObject();
                m->setProperty ("group", group);
                m->setProperty ("name", articulationName);
                list.add (juce::var (m));
            }

            o->setProperty ("modifiers", list);
            return juce::var (o);
        }

        // Void/null = none. Returns an error sentence (empty = parsed).
        static juce::String fromVar (const juce::var& json, Selection& out)
        {
            out = {};

            if (json.isVoid() || json.isUndefined())
                return {};

            if (! json.isObject())
                return "an articulation must be an object {root, modifiers:[{group, name}]}";

            out.root = json.getProperty ("root", {}).toString();
            const auto list = json.getProperty ("modifiers", {});

            if (! list.isVoid() && ! list.isArray())
                return "'modifiers' must be an array of {group, name}";

            if (auto* array = list.getArray())
                for (auto& m : *array)
                {
                    if (! m.isObject() || ! m.hasProperty ("group") || ! m.hasProperty ("name"))
                        return "every modifier needs a group and a name: {group, name}";

                    out.modifiers.emplace_back (m.getProperty ("group", {}).toString(), m.getProperty ("name", {}).toString());
                }

            return {};
        }

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

    // A name (and instructions) for a key: the keys libraries label on their
    // keyboards ("C0: Legato", "C#0: repeat once"). The editor shows them on the
    // piano keys. A keyswitch articulation's key is named automatically (keyLabel).
    struct KeyName
    {
        int key = 0;                 // 0..127, unique in the map
        juce::String name;
        juce::String instruction;    // longer text, shown as the key's tooltip
    };

    std::vector<KeyName> keyNames;

    // One valid combination of articulations and what it does (maps with slots)
    struct Slot
    {
        Selection selection;             // the combination: a root plus at most one modifier per group
        std::vector<Output> outputs;     // sent in series exactly before the note
        double timingOffsetMs = 0.0;     // < 0 earlier, > 0 later
        int keyLow = -1, keyHigh = -1;   // playable key range (-1 = unspecified)
    };

    std::vector<Slot> slots;

    bool hasSlots() const noexcept    { return ! slots.empty(); }

    // A slot as JSON: {articulation:{root, modifiers}, outputs, timingOffsetMs, keyLow, keyHigh}
    static juce::var slotToVar (const Slot& slot)
    {
        auto o = new juce::DynamicObject();
        o->setProperty ("articulation", slot.selection.toVar());
        o->setProperty ("outputs", outputsToVar (slot.outputs));
        o->setProperty ("timingOffsetMs", slot.timingOffsetMs);
        o->setProperty ("keyLow", slot.keyLow);
        o->setProperty ("keyHigh", slot.keyHigh);
        return juce::var (o);
    }

    // Returns an error sentence (empty = parsed)
    static juce::String slotFromVar (const juce::var& json, Slot& out)
    {
        if (! json.isObject())
            return "a slot must be an object {articulation:{root, modifiers}, outputs, timingOffsetMs?, keyLow?, keyHigh?}";

        Slot slot;

        if (const auto error = Selection::fromVar (json.getProperty ("articulation", {}), slot.selection); error.isNotEmpty())
            return error;

        if (const auto error = outputsFromVar (json.getProperty ("outputs", {}), "the slot", slot.outputs); error.isNotEmpty())
            return error;

        slot.timingOffsetMs = (double) json.getProperty ("timingOffsetMs", 0.0);
        slot.keyLow = (int) json.getProperty ("keyLow", -1);
        slot.keyHigh = (int) json.getProperty ("keyHigh", -1);
        out = std::move (slot);
        return {};
    }

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
    // Sound slots: finding the slot of a choice, and what can still be chosen

    // Does the combination 'whole' contain everything in 'part'? (Same root, and
    // every modifier of 'part' is in 'whole'.)
    static bool contains (const Selection& whole, const Selection& part)
    {
        if (! sameName (whole.root, part.root))
            return false;

        for (auto& [groupName, articulationName] : part.modifiers)
            if (std::none_of (whole.modifiers.begin(), whole.modifiers.end(), [&] (const auto& m)
                              { return sameName (m.first, groupName) && sameName (m.second, articulationName); }))
                return false;

        return true;
    }

    // The slot that is exactly this combination (nullptr: none)
    const Slot* findSlot (const Selection& selection) const
    {
        for (auto& slot : slots)
            if (slot.selection.modifiers.size() == selection.modifiers.size() && contains (slot.selection, selection))
                return &slot;

        return nullptr;
    }

    // Can this (partial) choice still become a slot?
    bool leadsToSlot (const Selection& selection) const
    {
        return std::any_of (slots.begin(), slots.end(), [&] (const Slot& slot) { return contains (slot.selection, selection); });
    }

    // The modifiers in the map's group order (so equal choices compare equal)
    Selection canonical (Selection selection) const
    {
        std::stable_sort (selection.modifiers.begin(), selection.modifiers.end(),
                          [this] (const auto& a, const auto& b) { return groupIndex (a.first) < groupIndex (b.first); });
        return selection;
    }

    // The choice up to (not including) a group: the root and the modifiers of the
    // groups before it. What a group offers depends only on this.
    Selection before (const Selection& selection, size_t groupIndexLimit) const
    {
        Selection result;

        if (groupIndexLimit == 0)
            return result;

        result.root = selection.root;

        for (auto& modifier : selection.modifiers)
            if (groupIndex (modifier.first) < groupIndexLimit)
                result.modifiers.push_back (modifier);

        return result;
    }

    // Adds the defaults of everything chosen to the groups that have nothing
    // chosen (and the defaults of those defaults), as long as a slot has them
    Selection withDefaults (Selection selection) const
    {
        for (auto changed = true; changed;)
        {
            changed = false;
            std::vector<const Articulation*> chosen;

            if (! groups.empty())
                chosen.push_back (findArticulation (groups.front(), selection.root));

            for (auto& [groupName, articulationName] : selection.modifiers)
                chosen.push_back (findArticulation (groupName, articulationName));

            for (auto* articulation : chosen)
            {
                if (articulation == nullptr)
                    continue;

                for (auto& [groupName, articulationName] : articulation->defaults)
                {
                    const auto taken = std::any_of (selection.modifiers.begin(), selection.modifiers.end(),
                                                    [&] (const auto& m) { return sameName (m.first, groupName); });
                    auto candidate = selection;
                    candidate.modifiers.emplace_back (groupName, articulationName);

                    if (! taken && leadsToSlot (candidate))
                    {
                        selection = canonical (candidate);
                        changed = true;
                        break;
                    }
                }

                if (changed)
                    break;
            }
        }

        return selection;
    }

    // What the menu offers for a choice (maps with slots): per modifier group, the
    // articulations some slot has together with the choices of the groups before
    // it. Groups with nothing to offer are omitted. Without a root, nothing.
    struct Offered
    {
        const Group* group = nullptr;
        std::vector<const Articulation*> articulations;
    };

    std::vector<Offered> offeredModifiers (const Selection& selection) const
    {
        std::vector<Offered> result;

        if (selection.root.trim().isEmpty())
            return result;

        for (size_t g = 1; g < groups.size(); ++g)
        {
            Offered offered;
            offered.group = &groups[g];
            const auto prefix = before (selection, g);

            for (auto& articulation : groups[g].articulations)
            {
                auto candidate = prefix;
                candidate.modifiers.emplace_back (groups[g].name, articulation.name);

                if (leadsToSlot (candidate))
                    offered.articulations.push_back (&articulation);
            }

            if (! offered.articulations.empty())
                result.push_back (std::move (offered));
        }

        return result;
    }

    // Is this root used by any slot?
    bool rootHasSlots (const juce::String& rootName) const
    {
        Selection selection;
        selection.root = rootName;
        return leadsToSlot (selection);
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

        for (size_t k = 0; k < keyNames.size(); ++k)
        {
            auto& keyName = keyNames[k];

            if (keyName.key < 0 || keyName.key > 127)
                problems.add ("key name '" + keyName.name + "' is on key " + juce::String (keyName.key) + ", outside 0-127");

            if (keyName.name.trim().isEmpty())
                problems.add ("the name of key " + juce::String (keyName.key) + " is empty");

            for (size_t other = 0; other < k; ++other)
                if (keyNames[other].key == keyName.key)
                    problems.add ("key " + juce::String (keyName.key) + " is named twice ('" + keyNames[other].name
                                  + "' and '" + keyName.name + "')");
        }

        if (hasSlots())
            validateSlots (problems);

        return problems;
    }

    bool isValid() const    { return validate().isEmpty(); }

    //==========================================================================
    // What the editor writes on a piano key: its explicit name, else the
    // articulation(s) it switches to as a keyswitch ("Legato", or "Legato / Marcato"
    // when several share the key). 'instruction' receives the key's longer text.
    // Empty when the key has nothing to say.
    juce::String keyLabel (int key, juce::String* instruction = nullptr) const
    {
        for (auto& keyName : keyNames)
            if (keyName.key == key)
            {
                if (instruction != nullptr)
                    *instruction = keyName.instruction;

                return keyName.name;
            }

        juce::StringArray switched;

        for (auto& group : groups)
            for (auto& articulation : group.articulations)
                for (auto& output : articulation.outputs)
                    if (output.type == Output::Type::keyswitch && output.number == key && ! switched.contains (articulation.name))
                        switched.add (articulation.name);

        for (auto& slot : slots)
            for (auto& output : slot.outputs)
                if (output.type == Output::Type::keyswitch && output.number == key && ! switched.contains (labelOf (slot.selection)))
                    switched.add (labelOf (slot.selection));

        if (instruction != nullptr)
            *instruction = switched.isEmpty() ? juce::String() : "Keyswitch for " + switched.joinIntoString (", ");

        return switched.joinIntoString (" / ");
    }

    // The keys that are keyswitches (so they can be marked on the keyboard)
    bool isKeyswitch (int key) const
    {
        for (auto& group : groups)
            for (auto& articulation : group.articulations)
                for (auto& output : articulation.outputs)
                    if (output.type == Output::Type::keyswitch && output.number == key)
                        return true;

        for (auto& slot : slots)
            for (auto& output : slot.outputs)
                if (output.type == Output::Type::keyswitch && output.number == key)
                    return true;

        return false;
    }

    // "Long notes + Legato + Soft"
    static juce::String labelOf (const Selection& selection)
    {
        juce::StringArray parts;

        if (selection.root.trim().isNotEmpty())
            parts.add (selection.root);

        for (auto& modifier : selection.modifiers)
            parts.add (modifier.second);

        return parts.joinIntoString (" + ");
    }

    // The playable key range of a choice: the intersection of the ranges that the
    // root and the chosen modifiers define. false when none of them defines one
    // (the caller falls back to the channel's own range).
    bool playableRange (const Selection& selection, int& low, int& high) const
    {
        low = 0;
        high = 127;

        if (hasSlots())
        {
            const auto* slot = findSlot (selection);

            if (slot == nullptr || slot->keyLow < 0 || slot->keyHigh < slot->keyLow)
                return false;

            low = slot->keyLow;
            high = slot->keyHigh;
            return true;
        }

        auto defined = false;

        const auto narrow = [&] (const Articulation* articulation)
        {
            if (articulation == nullptr || articulation->keyLow < 0 || articulation->keyHigh < articulation->keyLow)
                return;

            low = juce::jmax (low, articulation->keyLow);
            high = juce::jmin (high, articulation->keyHigh);
            defined = true;
        };

        if (! groups.empty() && selection.root.trim().isNotEmpty())
            narrow (findArticulation (groups.front(), selection.root));

        for (auto& [groupName, articulationName] : selection.modifiers)
            narrow (findArticulation (groupName, articulationName));

        return defined;
    }

    //==========================================================================
    // JSON for the API (the same shape as the XML). Unlike reading a file, parsing
    // is strict: it says what is wrong, because a caller can fix it.
    juce::var toVar() const
    {
        auto map = new juce::DynamicObject();
        map->setProperty ("name", name);
        map->setProperty ("description", description);

        juce::Array<juce::var> groupList;

        for (auto& group : groups)
        {
            auto g = new juce::DynamicObject();
            g->setProperty ("name", group.name);
            g->setProperty ("description", group.description);

            juce::Array<juce::var> articulationList;

            for (auto& articulation : group.articulations)
            {
                auto a = new juce::DynamicObject();
                a->setProperty ("name", articulation.name);
                a->setProperty ("symbol", articulation.symbol);
                a->setProperty ("description", articulation.description);
                a->setProperty ("timingOffsetMs", articulation.timingOffsetMs);
                a->setProperty ("keyLow", articulation.keyLow);
                a->setProperty ("keyHigh", articulation.keyHigh);

                juce::Array<juce::var> appliesTo;

                for (auto& root : articulation.appliesTo)
                    appliesTo.add (root);

                a->setProperty ("appliesTo", appliesTo);

                if (! articulation.defaults.empty())
                {
                    juce::Array<juce::var> defaultList;

                    for (auto& [groupName, articulationName] : articulation.defaults)
                    {
                        auto d = new juce::DynamicObject();
                        d->setProperty ("group", groupName);
                        d->setProperty ("name", articulationName);
                        defaultList.add (juce::var (d));
                    }

                    a->setProperty ("defaults", defaultList);
                }

                juce::Array<juce::var> outputList;

                for (auto& output : articulation.outputs)
                {
                    auto o = new juce::DynamicObject();
                    o->setProperty ("type", typeToString (output.type));
                    o->setProperty ("number", output.number);
                    o->setProperty ("value", output.value);
                    o->setProperty ("held", output.held);
                    o->setProperty ("bank", output.bank);
                    outputList.add (juce::var (o));
                }

                a->setProperty ("outputs", outputList);
                articulationList.add (juce::var (a));
            }

            g->setProperty ("articulations", articulationList);
            groupList.add (juce::var (g));
        }

        map->setProperty ("groups", groupList);

        juce::Array<juce::var> keyList;

        for (auto& keyName : keyNames)
        {
            auto k = new juce::DynamicObject();
            k->setProperty ("key", keyName.key);
            k->setProperty ("name", keyName.name);
            k->setProperty ("instruction", keyName.instruction);
            keyList.add (juce::var (k));
        }

        map->setProperty ("keyNames", keyList);

        if (hasSlots())
        {
            juce::Array<juce::var> slotList;

            for (auto& slot : slots)
            {
                auto o = new juce::DynamicObject();
                o->setProperty ("articulation", slot.selection.toVar());
                o->setProperty ("outputs", outputsToVar (slot.outputs));
                o->setProperty ("timingOffsetMs", slot.timingOffsetMs);
                o->setProperty ("keyLow", slot.keyLow);
                o->setProperty ("keyHigh", slot.keyHigh);
                slotList.add (juce::var (o));
            }

            map->setProperty ("slots", slotList);
        }

        return juce::var (map);
    }

    // Returns an error sentence (empty = parsed). Whether the map is valid is
    // validate()'s question.
    static juce::String fromVar (const juce::var& json, ExpressionMap& out)
    {
        if (! json.isObject())
            return "an expression map must be an object {name, description?, groups:[...]}";

        ExpressionMap map;
        map.name = json.getProperty ("name", {}).toString();
        map.description = json.getProperty ("description", {}).toString();

        const auto groupList = json.getProperty ("groups", {});

        if (groupList.isVoid())
            return "the map needs 'groups': [{name, description?, articulations:[...]}] (the first is the root group)";

        if (! groupList.isArray())
            return "'groups' must be an array";

        int groupNumber = 0;

        for (auto& g : *groupList.getArray())
        {
            ++groupNumber;
            const auto groupLabel = "group " + juce::String (groupNumber);

            if (! g.isObject())
                return groupLabel + " must be an object {name, description?, articulations:[...]}";

            Group group;
            group.name = g.getProperty ("name", {}).toString();
            group.description = g.getProperty ("description", {}).toString();

            const auto articulationList = g.getProperty ("articulations", {});

            if (! articulationList.isVoid() && ! articulationList.isArray())
                return groupLabel + ": 'articulations' must be an array";

            int articulationNumber = 0;

            if (auto* list = articulationList.getArray())
                for (auto& a : *list)
                {
                    ++articulationNumber;
                    const auto label = "articulation " + juce::String (articulationNumber) + " of " + groupLabel;

                    if (! a.isObject())
                        return label + " must be an object {name, symbol?, description?, outputs?, ...}";

                    Articulation articulation;
                    articulation.name = a.getProperty ("name", {}).toString();
                    articulation.symbol = a.getProperty ("symbol", {}).toString();
                    articulation.description = a.getProperty ("description", {}).toString();
                    articulation.timingOffsetMs = (double) a.getProperty ("timingOffsetMs", 0.0);
                    articulation.keyLow = (int) a.getProperty ("keyLow", -1);
                    articulation.keyHigh = (int) a.getProperty ("keyHigh", -1);

                    const auto appliesTo = a.getProperty ("appliesTo", {});

                    if (! appliesTo.isVoid() && ! appliesTo.isArray())
                        return label + ": 'appliesTo' must be an array of root articulation names";

                    if (auto* roots = appliesTo.getArray())
                        for (auto& root : *roots)
                            articulation.appliesTo.add (root.toString());

                    const auto defaultList = a.getProperty ("defaults", {});

                    if (! defaultList.isVoid() && ! defaultList.isArray())
                        return label + ": 'defaults' must be an array of {group, name}";

                    if (auto* defaults = defaultList.getArray())
                        for (auto& d : *defaults)
                        {
                            if (! d.isObject() || ! d.hasProperty ("group") || ! d.hasProperty ("name"))
                                return label + ": every default needs a group and a name: {group, name}";

                            articulation.defaults.emplace_back (d.getProperty ("group", {}).toString(), d.getProperty ("name", {}).toString());
                        }

                    const auto outputList = a.getProperty ("outputs", {});

                    if (! outputList.isVoid() && ! outputList.isArray())
                        return label + ": 'outputs' must be an array of {type, number, value?, held?, bank?}";

                    int outputNumber = 0;

                    if (auto* outputs = outputList.getArray())
                        for (auto& o : *outputs)
                        {
                            ++outputNumber;
                            Output output;

                            if (! o.isObject() || ! typeFromString (o.getProperty ("type", {}).toString(), output.type))
                                return label + ", output " + juce::String (outputNumber)
                                         + ": 'type' must be one of keyswitch, controller, programChange (got '"
                                         + o.getProperty ("type", {}).toString() + "')";

                            output.number = (int) o.getProperty ("number", 0);
                            output.value = (int) o.getProperty ("value", 100);
                            output.held = (bool) o.getProperty ("held", false);
                            output.bank = (int) o.getProperty ("bank", -1);
                            articulation.outputs.push_back (output);
                        }

                    group.articulations.push_back (std::move (articulation));
                }

            map.groups.push_back (std::move (group));
        }

        const auto keyList = json.getProperty ("keyNames", {});

        if (! keyList.isVoid() && ! keyList.isArray())
            return "'keyNames' must be an array of {key, name, instruction?}";

        if (auto* keys = keyList.getArray())
            for (auto& k : *keys)
            {
                if (! k.isObject() || ! k.hasProperty ("key") || ! k.hasProperty ("name"))
                    return "every key name needs a key (0-127) and a name: {key, name, instruction?}";

                map.keyNames.push_back ({ (int) k.getProperty ("key", 0), k.getProperty ("name", {}).toString(),
                                          k.getProperty ("instruction", {}).toString() });
            }

        const auto slotList = json.getProperty ("slots", {});

        if (! slotList.isVoid() && ! slotList.isArray())
            return "'slots' must be an array of {articulation:{root, modifiers}, outputs, timingOffsetMs?, keyLow?, keyHigh?}";

        int slotNumber = 0;

        if (auto* list = slotList.getArray())
            for (auto& o : *list)
            {
                ++slotNumber;
                const auto label = "slot " + juce::String (slotNumber);

                if (! o.isObject())
                    return label + " must be an object {articulation, outputs, ...}";

                Slot slot;

                if (const auto error = Selection::fromVar (o.getProperty ("articulation", {}), slot.selection); error.isNotEmpty())
                    return label + ": " + error;

                if (const auto error = outputsFromVar (o.getProperty ("outputs", {}), label, slot.outputs); error.isNotEmpty())
                    return error;

                slot.timingOffsetMs = (double) o.getProperty ("timingOffsetMs", 0.0);
                slot.keyLow = (int) o.getProperty ("keyLow", -1);
                slot.keyHigh = (int) o.getProperty ("keyHigh", -1);
                map.slots.push_back (std::move (slot));
            }

        out = std::move (map);
        return {};
    }

    //==========================================================================
    // Renaming. Names are identifiers: a rename must stay unique ignoring case
    // (changing only the case of the same item is fine), and everything that
    // refers to the name follows - a root articulation's name in the modifiers'
    // applies-to lists here, and the notes' choices via Selection::rename*
    // (the engine does that across the tracks using the map).
    // Both return an error sentence (empty = renamed).
    juce::String renameGroup (const juce::String& groupName, const juce::String& newName)
    {
        const auto clean = newName.trim();
        auto* group = findMutableGroup (groupName);

        if (group == nullptr)
            return "no group '" + groupName + "' in map '" + name + "' (groups: " + groupNames() + ")";

        if (clean.isEmpty())
            return "a group needs a name";

        for (auto& other : groups)
            if (&other != group && sameName (other.name, clean))
                return "there is already a group '" + other.name + "' in map '" + name + "' (names ignore case)";

        const auto oldName = group->name;
        group->name = clean;

        for (auto& slot : slots)
            slot.selection.renameModifierGroup (oldName, clean);

        for (auto& g : groups)
            for (auto& articulation : g.articulations)
                for (auto& d : articulation.defaults)
                    if (sameName (d.first, oldName))
                        d.first = clean;

        return {};
    }

    juce::String renameArticulation (const juce::String& groupName, const juce::String& articulationName, const juce::String& newName)
    {
        const auto clean = newName.trim();
        auto* group = findMutableGroup (groupName);

        if (group == nullptr)
            return "no group '" + groupName + "' in map '" + name + "' (groups: " + groupNames() + ")";

        Articulation* target = nullptr;

        for (auto& articulation : group->articulations)
            if (sameName (articulation.name, articulationName))
                target = &articulation;

        if (target == nullptr)
            return "no articulation '" + articulationName + "' in group '" + group->name + "' (it has: " + names (*group) + ")";

        if (clean.isEmpty())
            return "an articulation needs a name";

        for (auto& other : group->articulations)
            if (&other != target && sameName (other.name, clean))
                return "group '" + group->name + "' already has an articulation '" + other.name + "' (names ignore case)";

        const auto oldName = target->name;
        target->name = clean;

        if (group == &groups.front())   // a root: the modifiers that list it follow
            for (size_t g = 1; g < groups.size(); ++g)
                for (auto& modifier : groups[g].articulations)
                    for (auto& applies : modifier.appliesTo)
                        if (sameName (applies, oldName))
                            applies = clean;

        for (auto& slot : slots)   // the slots and the defaults that name it
        {
            if (group == &groups.front())
                slot.selection.renameRoot (oldName, clean);
            else
                slot.selection.renameModifier (group->name, oldName, clean);
        }

        for (auto& g : groups)
            for (auto& articulation : g.articulations)
                for (auto& d : articulation.defaults)
                    if (sameName (d.first, group->name) && sameName (d.second, oldName))
                        d.second = clean;

        return {};
    }

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

        if (hasSlots())
            return chooseInSlots (current, *group, *articulation);

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

        if (hasSlots() && ! selection.isEmpty() && findSlot (selection) == nullptr)
            problems.add ("map '" + name + "' has no sound slot for '" + labelOf (selection) + "'");

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

                for (auto& [groupName, articulationName] : articulation.defaults)
                {
                    auto* d = a->createNewChildElement ("DEFAULT");
                    d->setAttribute ("group", groupName);
                    d->setAttribute ("name", articulationName);
                }

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

        for (auto& keyName : keyNames)
        {
            auto* k = xml->createNewChildElement ("KEYNAME");
            k->setAttribute ("key", keyName.key);
            k->setAttribute ("name", keyName.name);
            k->setAttribute ("instruction", keyName.instruction);
        }

        if (hasSlots())
        {
            auto* list = xml->createNewChildElement ("SLOTS");

            for (auto& slot : slots)
            {
                auto* e = list->createNewChildElement ("SLOT");
                e->setAttribute ("timingOffsetMs", slot.timingOffsetMs);
                e->setAttribute ("keyLow", slot.keyLow);
                e->setAttribute ("keyHigh", slot.keyHigh);
                e->addChildElement (slot.selection.toXml().release());
                outputsToXml (*e, slot.outputs);
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

                for (auto* d : a->getChildWithTagNameIterator ("DEFAULT"))
                    articulation.defaults.emplace_back (d->getStringAttribute ("group"), d->getStringAttribute ("name"));

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

        for (auto* k : xml.getChildWithTagNameIterator ("KEYNAME"))
            map.keyNames.push_back ({ k->getIntAttribute ("key", 0), k->getStringAttribute ("name"), k->getStringAttribute ("instruction") });

        if (auto* list = xml.getChildByName ("SLOTS"))
            for (auto* e : list->getChildWithTagNameIterator ("SLOT"))
            {
                Slot slot;
                slot.timingOffsetMs = e->getDoubleAttribute ("timingOffsetMs", 0.0);
                slot.keyLow = e->getIntAttribute ("keyLow", -1);
                slot.keyHigh = e->getIntAttribute ("keyHigh", -1);

                if (auto* a = e->getChildByName ("ARTICULATION"))
                    slot.selection = Selection::fromXml (*a);

                slot.outputs = outputsFromXml (*e);
                map.slots.push_back (std::move (slot));
            }

        return map;
    }

private:
    Group* findMutableGroup (const juce::String& groupName)
    {
        for (auto& group : groups)
            if (sameName (group.name, groupName))
                return &group;

        return nullptr;
    }

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

    // choose() for maps with slots. A group's articulation is chosen against the
    // choices before it (root, earlier groups); the later groups' choices are kept
    // when a slot still has them, else dropped (reported, except a default the old
    // choice had filled in); then defaults fill the empty groups. The result is
    // always the combination of a slot.
    Choice chooseInSlots (const Selection& current, const Group& group, const Articulation& articulation) const
    {
        Choice result;
        result.selection = current;
        const auto index = groupIndex (group.name);
        const auto isRoot = index == 0;

        const auto wasDefault = [this, &current] (const std::pair<juce::String, juce::String>& modifier)
        {
            std::vector<const Articulation*> chosen { groups.empty() ? nullptr : findArticulation (groups.front(), current.root) };

            for (auto& [g, a] : current.modifiers)
                chosen.push_back (findArticulation (g, a));

            for (auto* c : chosen)
                if (c != nullptr)
                    for (auto& d : c->defaults)
                        if (sameName (d.first, modifier.first) && sameName (d.second, modifier.second))
                            return true;

            return false;
        };

        const auto chosenHere = isRoot ? (sameName (current.root, articulation.name) && current.root.trim().isNotEmpty())
                                       : std::any_of (current.modifiers.begin(), current.modifiers.end(), [&] (const auto& m)
                                                      { return sameName (m.first, group.name) && sameName (m.second, articulation.name); });

        if (isRoot && chosenHere)
        {
            result.selection = {};   // unselecting the root: nothing is left
            result.dropped = current.modifiers;
            return result;
        }

        if (! isRoot && current.root.trim().isEmpty())
        {
            result.error = "modifiers need a root articulation; choose one from '" + groups.front().name + "' first ("
                           + names (groups.front()) + ")";
            return result;
        }

        Selection next;

        if (chosenHere)
        {
            next = current;   // toggled off; its defaults (if any) may fill the group again
            next.modifiers.erase (std::remove_if (next.modifiers.begin(), next.modifiers.end(),
                                                  [&] (const auto& m) { return sameName (m.first, group.name); }),
                                  next.modifiers.end());
        }
        else
        {
            next = before (current, index);

            if (isRoot)
                next.root = articulation.name;
            else
                next.modifiers.emplace_back (group.name, articulation.name);

            if (! leadsToSlot (next))
            {
                result.error = "no sound slot has '" + articulation.name + "' (group '" + group.name + "')"
                               + (isRoot ? juce::String() : " with '" + labelOf (before (current, index)) + "'");
                return result;
            }

            for (auto& modifier : current.modifiers)   // the later groups: keep what still fits
            {
                if (groupIndex (modifier.first) <= index)
                    continue;

                auto candidate = next;
                candidate.modifiers.push_back (modifier);

                if (leadsToSlot (candidate))
                    next = canonical (candidate);
                else if (! wasDefault (modifier))
                    result.dropped.push_back (modifier);
            }
        }

        next = withDefaults (canonical (next));

        if (findSlot (next) == nullptr)
        {
            result.dropped.clear();
            result.error = "'" + labelOf (next) + "' is not a sound slot of map '" + name + "'"
                           + (chosenHere ? juce::String (" ('") + articulation.name + "' can't be unselected here)"
                                         : juce::String (": choose more, or give the map defaults or a slot for it"));
            return result;
        }

        result.selection = next;
        return result;
    }

    void validateSlots (juce::StringArray& problems) const
    {
        for (size_t s = 0; s < slots.size(); ++s)
        {
            auto& slot = slots[s];
            const auto label = "slot " + juce::String ((int) s + 1) + " ('" + labelOf (slot.selection) + "')";

            if (slot.selection.root.trim().isEmpty() || findArticulation (groups.front(), slot.selection.root) == nullptr)
                problems.add (label + " needs a root articulation of group '" + groups.front().name + "'");

            juce::StringArray seen;

            for (auto& [groupName, articulationName] : slot.selection.modifiers)
            {
                const auto* group = findGroup (groupName);

                if (group == nullptr || group == &groups.front())
                    problems.add (label + ": '" + groupName + "' is not a modifier group of this map");
                else if (findArticulation (*group, articulationName) == nullptr)
                    problems.add (label + ": group '" + group->name + "' has no '" + articulationName + "'");

                if (seen.contains (groupName, true))
                    problems.add (label + " has two articulations of group '" + groupName + "'");

                seen.add (groupName);
            }

            for (size_t o = 0; o < slot.outputs.size(); ++o)
                validateOutput (slot.outputs[o], label + " (output " + juce::String ((int) o + 1) + ")", problems);

            if ((slot.keyLow != -1 || slot.keyHigh != -1)
                 && (slot.keyLow < 0 || slot.keyHigh > 127 || slot.keyLow > slot.keyHigh))
                problems.add (label + " has an invalid key range " + juce::String (slot.keyLow) + "-" + juce::String (slot.keyHigh));

            if (std::abs (slot.timingOffsetMs) > 5000.0)
                problems.add (label + " has a timing offset of " + juce::String (slot.timingOffsetMs) + " ms; the limit is 5000 ms either way");

            for (size_t other = 0; other < s; ++other)
                if (slots[other].selection.modifiers.size() == slot.selection.modifiers.size()
                     && contains (slots[other].selection, slot.selection))
                    problems.add (label + " is the same combination as slot " + juce::String ((int) other + 1));
        }

        for (size_t g = 0; g < groups.size(); ++g)
            for (auto& articulation : groups[g].articulations)
            {
                const auto label = "'" + articulation.name + "' in group '" + groups[g].name + "'";

                if (! articulation.outputs.empty() || ! articulation.appliesTo.isEmpty())
                    problems.add (label + " has outputs or an applies-to list; in a map with sound slots they belong to "
                                  "the slots (and what goes together follows from them)");

                for (auto& [groupName, articulationName] : articulation.defaults)
                {
                    const auto* group = findGroup (groupName);

                    if (group == nullptr || group == &groups.front() || group == &groups[g])
                        problems.add (label + " has a default in '" + groupName + "', which is not another modifier group");
                    else if (findArticulation (*group, articulationName) == nullptr)
                        problems.add (label + " has the default '" + articulationName + "', which group '" + group->name + "' doesn't have");
                }

                if (g == 0)
                {
                    Selection alone;
                    alone.root = articulation.name;

                    if (findSlot (withDefaults (alone)) == nullptr)
                        problems.add ("choosing the root '" + articulation.name + "' must give a sound slot: add one for it, or "
                                      "defaults that complete it (now: '" + labelOf (withDefaults (alone)) + "')");
                }
            }
    }

    static juce::var outputsToVar (const std::vector<Output>& outputs)
    {
        juce::Array<juce::var> list;

        for (auto& output : outputs)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("type", typeToString (output.type));
            o->setProperty ("number", output.number);
            o->setProperty ("value", output.value);
            o->setProperty ("held", output.held);
            o->setProperty ("bank", output.bank);
            list.add (juce::var (o));
        }

        return list;
    }

    static juce::String outputsFromVar (const juce::var& json, const juce::String& label, std::vector<Output>& out)
    {
        if (! json.isVoid() && ! json.isArray())
            return label + ": 'outputs' must be an array of {type, number, value?, held?, bank?}";

        int number = 0;

        if (auto* list = json.getArray())
            for (auto& o : *list)
            {
                ++number;
                Output output;

                if (! o.isObject() || ! typeFromString (o.getProperty ("type", {}).toString(), output.type))
                    return label + ", output " + juce::String (number) + ": 'type' must be one of keyswitch, controller, "
                           "programChange (got '" + o.getProperty ("type", {}).toString() + "')";

                output.number = (int) o.getProperty ("number", 0);
                output.value = (int) o.getProperty ("value", 100);
                output.held = (bool) o.getProperty ("held", false);
                output.bank = (int) o.getProperty ("bank", -1);
                out.push_back (output);
            }

        return {};
    }

    static void outputsToXml (juce::XmlElement& parent, const std::vector<Output>& outputs)
    {
        for (auto& output : outputs)
        {
            auto* o = parent.createNewChildElement ("OUTPUT");
            o->setAttribute ("type", typeToString (output.type));
            o->setAttribute ("number", output.number);
            o->setAttribute ("value", output.value);
            o->setAttribute ("held", output.held);
            o->setAttribute ("bank", output.bank);
        }
    }

    static std::vector<Output> outputsFromXml (const juce::XmlElement& parent)
    {
        std::vector<Output> outputs;

        for (auto* o : parent.getChildWithTagNameIterator ("OUTPUT"))
        {
            Output output;

            if (! typeFromString (o->getStringAttribute ("type"), output.type))
                continue;

            output.number = o->getIntAttribute ("number", 0);
            output.value = o->getIntAttribute ("value", 100);
            output.held = o->getBoolAttribute ("held", false);
            output.bank = o->getIntAttribute ("bank", -1);
            outputs.push_back (output);
        }

        return outputs;
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
