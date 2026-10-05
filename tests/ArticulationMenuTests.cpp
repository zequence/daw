#include "../src/model/ArticulationMenu.h"
#include <juce_events/juce_events.h>

namespace
{
    using Map = ExpressionMap;
    using Selection = ExpressionMap::Selection;

    Map::Articulation art (const juce::String& name, const juce::String& symbol, std::initializer_list<const char*> appliesTo = {})
    {
        Map::Articulation a;
        a.name = name;
        a.symbol = symbol;
        a.description = name + " description";
        a.outputs.push_back ({ ExpressionMap::Output::Type::controller, 32, 10, false, -1 });

        for (auto* root : appliesTo)
            a.appliesTo.add (root);

        return a;
    }

    // Roots Staccato(.)/Legato(~)/Marcato(no symbol); Release: Short+Soft (Staccato, Marcato), Long (Legato);
    // Mute: Con sord (all); Dynamics: Sfz (Marcato only)
    Map testMap()
    {
        Map map;
        map.name = "Test";
        map.groups.push_back ({ "Articulation", "", { art ("Staccato", "."), art ("Legato", "~"), art ("Marcato", "") } });
        map.groups.push_back ({ "Release", "", { art ("Short", "s", { "Staccato", "Marcato" }), art ("Soft", "o", { "Staccato", "Marcato" }),
                                                 art ("Long", "l", { "Legato" }) } });
        map.groups.push_back ({ "Mute", "", { art ("Con sord", "m") } });
        map.groups.push_back ({ "Dynamics", "", { art ("Sfz", "!", { "Marcato" }) } });
        return map;
    }

    Selection sel (const char* root, std::initializer_list<std::pair<const char*, const char*>> modifiers = {})
    {
        Selection s;
        s.root = root;

        for (auto& [group, name] : modifiers)
            s.modifiers.emplace_back (group, name);

        return s;
    }

    const articulations::MenuItem* find (const std::vector<articulations::MenuItem>& menu, const juce::String& name)
    {
        for (auto& item : menu)
            if (item.kind == articulations::MenuItem::Kind::item && item.name == name)
                return &item;

        return nullptr;
    }

    juce::StringArray headers (const std::vector<articulations::MenuItem>& menu)
    {
        juce::StringArray names;

        for (auto& item : menu)
            if (item.kind == articulations::MenuItem::Kind::header)
                names.add (item.text);

        return names;
    }
}

class ArticulationMenuTests final : public juce::UnitTest
{
public:
    ArticulationMenuTests() : UnitTest ("Articulation menu logic") {}

    void runTest() override
    {
        using namespace articulations;
        const auto map = testMap();

        beginTest ("effective choice: the first root is the default only when the setting is on and there is no root");
        {
            expect (effective (map, {}, false).isEmpty());

            const auto implicit = effective (map, {}, true);
            expectEquals (implicit.root, juce::String ("Staccato"));
            expect (implicit.modifiers.empty());

            const auto kept = effective (map, sel ("Legato", { { "Release", "Long" } }), true);   // a real choice is never replaced
            expectEquals (kept.root, juce::String ("Legato"));
            expectEquals ((int) kept.modifiers.size(), 1);

            Map empty;
            empty.name = "E";
            expect (effective (empty, {}, true).isEmpty());   // nothing to default to
        }

        beginTest ("resolves() agrees with problemsOf() for every kind of choice");
        {
            const Selection cases[] = {
                {},                                                            // none
                sel ("Staccato"),                                              // fine
                sel ("staccato", { { "release", "short" } }),                  // fine, other case
                sel ("Legato", { { "Release", "Short" } }),                    // doesn't apply
                sel ("Pizzicato"),                                             // root not in map
                sel ("", { { "Mute", "Con sord" } }),                          // modifier without a root
                sel ("Staccato", { { "Nowhere", "X" } }),                      // group missing
                sel ("Staccato", { { "Release", "Nope" } }),                   // articulation missing
                sel ("Staccato", { { "Release", "Short" }, { "Release", "Soft" } }),   // two from one group
                sel ("Staccato", { { "Articulation", "Legato" } }),            // the root group as a modifier group
                sel ("Marcato", { { "Release", "Short" }, { "Mute", "Con sord" }, { "Dynamics", "Sfz" } }),   // fine
            };

            for (auto& c : cases)
                expect (resolves (map, c) == map.problemsOf (c).isEmpty(), "disagree on: " + label (c));
        }

        beginTest ("labels and symbols; unknown names are shown as written");
        {
            expectEquals (label ({}), juce::String());
            expectEquals (label (sel ("Legato", { { "Release", "Long" }, { "Mute", "Con sord" } })), juce::String ("Legato + Long + Con sord"));

            expectEquals (symbols (map, sel ("Legato", { { "Release", "Long" } })), juce::String ("~l"));
            expectEquals (symbols (map, sel ("Marcato")), juce::String ("Mar"));                    // no symbol: the start of the name
            expectEquals (symbols (map, sel ("Pizzicato")), juce::String ("Piz"));                  // not in the map: as written
            expectEquals (symbols (map, sel ("Staccato", { { "Mute", "Con sord" } })), juce::String (".m"));
            expectEquals (symbols (map, {}), juce::String());
        }

        beginTest ("menu without a choice: the roots, all available, none ticked, no modifiers");
        {
            const auto menu = buildMenu (map, { Selection() });
            expectEquals (headers (menu).joinIntoString (","), juce::String ("Articulation"));

            for (auto* rootName : { "Staccato", "Legato", "Marcato" })
            {
                const auto* item = find (menu, rootName);
                expect (item != nullptr && item->isRoot && item->enabled && ! item->ticked, rootName);
            }

            expectEquals (find (menu, "Staccato")->text, juce::String (".  Staccato"));   // symbol + name
            expectEquals (find (menu, "Marcato")->text, juce::String ("Marcato"));
            expectEquals (find (menu, "Staccato")->description, juce::String ("Staccato description"));
        }

        beginTest ("menu with a root: only the modifiers that apply, groups with none left disappear");
        {
            auto menu = buildMenu (map, { sel ("Staccato") });
            expectEquals (headers (menu).joinIntoString (","), juce::String ("Articulation,Release,Mute"));   // no Dynamics: Sfz is Marcato-only
            expect (find (menu, "Short") != nullptr && find (menu, "Soft") != nullptr);
            expect (find (menu, "Long") == nullptr);   // Legato-only
            expect (find (menu, "Con sord") != nullptr);
            expect (find (menu, "Staccato")->ticked && ! find (menu, "Legato")->ticked);

            menu = buildMenu (map, { sel ("Marcato") });
            expectEquals (headers (menu).joinIntoString (","), juce::String ("Articulation,Release,Mute,Dynamics"));

            menu = buildMenu (map, { sel ("Legato") });
            expectEquals (headers (menu).joinIntoString (","), juce::String ("Articulation,Release,Mute"));
            expect (find (menu, "Long") != nullptr && find (menu, "Short") == nullptr);
        }

        beginTest ("menu: a chosen modifier is ticked and the rest of its group is greyed out until it is unselected");
        {
            const auto menu = buildMenu (map, { sel ("Staccato", { { "Release", "Short" } }) });
            const auto* chosen = find (menu, "Short");
            expect (chosen->ticked && chosen->enabled);   // it can be toggled off
            expect (! find (menu, "Soft")->ticked && ! find (menu, "Soft")->enabled);
            expect (find (menu, "Con sord")->enabled);    // another group is unaffected
            expect (find (menu, "Legato")->enabled);      // roots are never greyed
        }

        beginTest ("menu over several notes: only what applies to all, ticked only when every note has it");
        {
            // Staccato and Legato share no Release modifier: the group is not offered; Mute applies to both
            auto menu = buildMenu (map, { sel ("Staccato"), sel ("Legato") });
            expectEquals (headers (menu).joinIntoString (","), juce::String ("Articulation,Mute"));

            // Both Staccato, only one has Con sord: not ticked; both: ticked
            menu = buildMenu (map, { sel ("Staccato", { { "Mute", "Con sord" } }), sel ("Staccato") });
            expect (! find (menu, "Con sord")->ticked && find (menu, "Staccato")->ticked);
            menu = buildMenu (map, { sel ("Staccato", { { "Mute", "Con sord" } }), sel ("Staccato", { { "Mute", "Con sord" } }) });
            expect (find (menu, "Con sord")->ticked);

            // A note without a root: roots only (modifiers need a root)
            menu = buildMenu (map, { sel ("Staccato"), Selection() });
            expectEquals (headers (menu).joinIntoString (","), juce::String ("Articulation"));

            // A root that is not in the map counts as no root
            menu = buildMenu (map, { sel ("Pizzicato") });
            expectEquals (headers (menu).joinIntoString (","), juce::String ("Articulation"));

            // Greyed out if ANY note's group is taken by another item
            menu = buildMenu (map, { sel ("Staccato", { { "Release", "Short" } }), sel ("Staccato") });
            expect (! find (menu, "Soft")->enabled);
            expect (find (menu, "Short")->enabled && ! find (menu, "Short")->ticked);   // chosen on one note only
        }

        beginTest ("applying an item to every target: results, and the modifiers it would drop, without repeats");
        {
            const std::vector<Selection> targets { sel ("Staccato", { { "Release", "Short" }, { "Mute", "Con sord" } }),
                                                   sel ("Staccato", { { "Release", "Short" } }),
                                                   sel ("Marcato", { { "Release", "Short" } }) };

            // Legato: Short doesn't apply to it, so it would be dropped on the first two notes (and is listed once)
            auto applied = apply (map, targets, "Articulation", "Legato");
            expect (applied.ok(), applied.error);
            expectEquals ((int) applied.results.size(), 3);
            expectEquals ((int) applied.dropped.size(), 1);
            expectEquals (applied.dropped[0].second, juce::String ("Short"));
            expectEquals (applied.results[0].root, juce::String ("Legato"));
            expectEquals ((int) applied.results[0].modifiers.size(), 1);   // Con sord stays

            // Marcato for the first two notes: everything still applies, nothing dropped
            // (the third already is Marcato - choosing it again would toggle it off)
            applied = apply (map, { targets[0], targets[1] }, "Articulation", "Marcato");
            expect (applied.ok() && applied.dropped.empty(), applied.error);

            // A refusal names the note and changes nothing
            applied = apply (map, { sel ("Staccato"), sel ("Legato") }, "Release", "Short");
            expect (! applied.ok() && applied.error.contains ("note 2") && applied.error.contains ("doesn't apply"), applied.error);
            expect (applied.results.empty());
        }

        beginTest ("equal choices");
        {
            expect (allEqual ({ sel ("Staccato"), sel ("staccato") }));
            expect (! allEqual ({ sel ("Staccato"), sel ("Legato") }));
            expect (allEqual ({ Selection() }));
        }
    }
};

static ArticulationMenuTests articulationMenuTests;
