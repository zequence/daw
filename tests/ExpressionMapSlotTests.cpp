#include "../src/model/ExpressionMap.h"
#include <juce_events/juce_events.h>

namespace
{
    using Map = ExpressionMap;
    using Out = ExpressionMap::Output;
    using Selection = ExpressionMap::Selection;

    Map::Articulation named (const juce::String& name, std::vector<std::pair<juce::String, juce::String>> defaults = {})
    {
        Map::Articulation a;
        a.name = name;
        a.defaults = std::move (defaults);
        return a;
    }

    Selection sel (const juce::String& root, std::vector<std::pair<juce::String, juce::String>> modifiers = {})
    {
        Selection s;
        s.root = root;
        s.modifiers = std::move (modifiers);
        return s;
    }

    Map::Slot slot (Selection s, std::vector<int> programs, int low = -1, int high = -1)
    {
        Map::Slot result;
        result.selection = std::move (s);

        for (auto p : programs)
            result.outputs.push_back ({ Out::Type::programChange, p, 0, false, -1 });

        result.keyLow = low;
        result.keyHigh = high;
        return result;
    }

    // A Synchron-like map (the merged Duality preset in small): colour as the root,
    // then Main, Legato, Release, Tempo - each slot with its own programs
    Map synchronLike()
    {
        Map map;
        map.name = "Strings";
        map.groups.push_back ({ "Color", "", { named ("Regular", { { "Main", "Long" } }),
                                               named ("Con sordino", { { "Main", "Long" } }) } });
        map.groups.push_back ({ "Main", "", { named ("Long"), named ("Rep.", { { "Tempo", "120" } }), named ("Staccato") } });
        map.groups.push_back ({ "Legato", "", { named ("Legato"), named ("Slur") } });
        map.groups.push_back ({ "Release", "", { named ("Soft") } });
        map.groups.push_back ({ "Tempo", "", { named ("120"), named ("130") } });

        map.slots = {
            slot (sel ("Regular", { { "Main", "Long" } }), { 112, 0, 64 }, 55, 98),
            slot (sel ("Regular", { { "Main", "Long" }, { "Legato", "Legato" } }), { 112, 1, 16, 64 }, 55, 98),
            slot (sel ("Regular", { { "Main", "Long" }, { "Legato", "Legato" }, { "Release", "Soft" } }), { 112, 1, 16, 65 }),
            slot (sel ("Regular", { { "Main", "Long" }, { "Release", "Soft" } }), { 112, 0, 65 }),
            slot (sel ("Regular", { { "Main", "Rep." }, { "Tempo", "120" } }), { 112, 80, 4, 16 }),
            slot (sel ("Regular", { { "Main", "Rep." }, { "Tempo", "130" } }), { 112, 80, 4, 17 }),
            slot (sel ("Regular", { { "Main", "Rep." }, { "Legato", "Legato" }, { "Tempo", "120" } }), { 112, 81, 4, 16 }),
            slot (sel ("Regular", { { "Main", "Staccato" } }), { 112, 3, 18 }, 55, 96),
            slot (sel ("Con sordino", { { "Main", "Long" } }), { 113, 0, 64 }),
            slot (sel ("Con sordino", { { "Main", "Staccato" } }), { 113, 3, 18 }),
        };
        return map;
    }

    juce::StringArray offeredNames (const Map& map, const Selection& s, const juce::String& group)
    {
        juce::StringArray result;

        for (auto& offered : map.offeredModifiers (s))
            if (Map::sameName (offered.group->name, group))
                for (auto* a : offered.articulations)
                    result.add (a->name);

        return result;
    }
}

class ExpressionMapSlotTests final : public juce::UnitTest
{
public:
    ExpressionMapSlotTests() : juce::UnitTest ("Expression map sound slots", "Model") {}

    void runTest() override
    {
        const auto map = synchronLike();

        beginTest ("a valid slot map; finding the slot of a combination (in any modifier order)");
        {
            expect (map.validate().isEmpty(), map.validate().joinIntoString ("; "));
            expect (map.findSlot (sel ("Regular", { { "Legato", "Legato" }, { "Main", "Long" } })) == &map.slots[1]);
            expect (map.findSlot (sel ("Regular", { { "Main", "Long" }, { "Tempo", "130" } })) == nullptr);
            expect (map.leadsToSlot (sel ("Regular", { { "Main", "Rep." } })));
            expect (! map.leadsToSlot (sel ("Con sordino", { { "Main", "Rep." } })));
        }

        beginTest ("choosing a root fills in its defaults: always a whole slot");
        {
            const auto choice = map.choose ({}, "Color", "Regular");
            expect (choice.ok(), choice.error);
            expect (choice.selection == sel ("Regular", { { "Main", "Long" } }));
        }

        beginTest ("the menu follows the slots: a group offers what goes with the choices before it");
        {
            const auto longNotes = sel ("Regular", { { "Main", "Long" } });
            expectEquals (offeredNames (map, longNotes, "Main").joinIntoString (","), juce::String ("Long,Rep.,Staccato"));
            expectEquals (offeredNames (map, longNotes, "Legato").joinIntoString (","), juce::String ("Legato"));
            expectEquals (offeredNames (map, longNotes, "Release").joinIntoString (","), juce::String ("Soft"));
            expect (offeredNames (map, longNotes, "Tempo").isEmpty(), "no tempo with Long");

            const auto sordino = sel ("Con sordino", { { "Main", "Long" } });
            expectEquals (offeredNames (map, sordino, "Main").joinIntoString (","), juce::String ("Long,Staccato"));
            expect (offeredNames (map, sordino, "Legato").isEmpty() && offeredNames (map, sordino, "Release").isEmpty(),
                    "con sordino has no legato or release slots");

            const auto rep = sel ("Regular", { { "Main", "Rep." }, { "Tempo", "120" } });
            expectEquals (offeredNames (map, rep, "Tempo").joinIntoString (","), juce::String ("120,130"));
            expect (map.offeredModifiers ({}).empty(), "nothing without a root");
        }

        beginTest ("choosing a Main keeps the later choices a slot still has, and fills in defaults");
        {
            auto choice = map.choose (sel ("Regular", { { "Main", "Long" } }), "Main", "Rep.");
            expect (choice.ok(), choice.error);
            expect (choice.selection == sel ("Regular", { { "Main", "Rep." }, { "Tempo", "120" } }));

            choice = map.choose (sel ("Regular", { { "Main", "Long" }, { "Legato", "Legato" } }), "Main", "Rep.");
            expect (choice.ok(), choice.error);
            expect (choice.selection == sel ("Regular", { { "Main", "Rep." }, { "Legato", "Legato" }, { "Tempo", "120" } }),
                    Map::labelOf (choice.selection));
            expect (choice.dropped.empty());
        }

        beginTest ("another item of a chosen group replaces it; unselecting one refills its default");
        {
            const auto rep120 = sel ("Regular", { { "Main", "Rep." }, { "Tempo", "120" } });
            auto choice = map.choose (rep120, "Tempo", "130");
            expect (choice.ok(), choice.error);
            expect (choice.selection == sel ("Regular", { { "Main", "Rep." }, { "Tempo", "130" } }));

            choice = map.choose (choice.selection, "Tempo", "130");
            expect (choice.ok(), choice.error);
            expect (choice.selection == rep120, "the default comes back");
        }

        beginTest ("changing the root: own choices that no slot has are reported, filled-in defaults go silently");
        {
            auto choice = map.choose (sel ("Regular", { { "Main", "Rep." }, { "Tempo", "120" } }), "Color", "Con sordino");
            expect (choice.ok(), choice.error);
            expect (choice.selection == sel ("Con sordino", { { "Main", "Long" } }));
            expect (choice.dropped.size() == 1 && choice.dropped[0].second == "Rep.", "Tempo 120 was Rep.'s default");

            choice = map.choose (sel ("Regular", { { "Main", "Staccato" } }), "Color", "Con sordino");
            expect (choice.selection == sel ("Con sordino", { { "Main", "Staccato" } }) && choice.dropped.empty(),
                    "a Main both colours have is kept");
        }

        beginTest ("what no slot has is refused, and the choice stays as it was");
        {
            const auto sordino = sel ("Con sordino", { { "Main", "Long" } });
            auto choice = map.choose (sordino, "Legato", "Legato");
            expect (! choice.ok() && choice.selection == sordino);

            choice = map.choose (sel ("Regular", { { "Main", "Long" } }), "Tempo", "130");
            expect (! choice.ok());
        }

        beginTest ("key range, problems and labels come from the slot");
        {
            int low = 0, high = 0;
            expect (map.playableRange (sel ("Regular", { { "Main", "Staccato" } }), low, high) && low == 55 && high == 96);
            expect (! map.playableRange (sel ("Regular", { { "Main", "Rep." }, { "Tempo", "120" } }), low, high), "no range given");
            expect (map.problemsOf (sel ("Regular", { { "Main", "Long" } })).isEmpty());
            expect (map.problemsOf (sel ("Regular", { { "Main", "Long" }, { "Tempo", "130" } })).joinIntoString (";").contains ("no sound slot"));
            expectEquals (Map::labelOf (map.slots[2].selection), juce::String ("Regular + Long + Legato + Soft"));
        }

        beginTest ("XML and JSON keep slots and defaults");
        {
            const auto xml = map.toXml();
            const auto fromXml = Map::fromXml (*xml);
            expectEquals (fromXml.toXml()->toString(), xml->toString());
            expect (fromXml.slots.size() == map.slots.size() && fromXml.groups[1].articulations[1].defaults.size() == 1);

            Map fromJson;
            expect (Map::fromVar (map.toVar(), fromJson).isEmpty());
            expectEquals (fromJson.toXml()->toString(), xml->toString());
        }

        beginTest ("renames follow into slots and defaults");
        {
            auto copy = map;
            expect (copy.renameArticulation ("Main", "Long", "Long notes").isEmpty());
            expect (copy.renameGroup ("Tempo", "Speed").isEmpty());
            expect (copy.validate().isEmpty(), copy.validate().joinIntoString ("; "));
            expect (copy.findSlot (sel ("Regular", { { "Main", "Long notes" } })) != nullptr);
            expect (copy.groups[0].articulations[0].defaults[0].second == "Long notes");
            expect (copy.choose ({}, "Color", "Regular").selection == sel ("Regular", { { "Main", "Long notes" } }));
            expect (copy.findSlot (sel ("Regular", { { "Main", "Rep." }, { "Speed", "130" } })) != nullptr);
        }

        beginTest ("validation: duplicates, unknown names, a root that alone isn't a slot, outputs on articulations");
        {
            auto broken = map;
            broken.slots.push_back (broken.slots[0]);
            broken.slots.push_back (slot (sel ("Regular", { { "Main", "Pizz." } }), { 1 }));
            broken.groups[0].articulations.push_back (named ("Flautando"));
            broken.groups[2].articulations[0].outputs.push_back ({ Out::Type::keyswitch, 24, 100, false, -1 });
            broken.groups[1].articulations[0].defaults.push_back ({ "Main", "Rep." });

            const auto problems = broken.validate().joinIntoString ("\n");
            expect (problems.contains ("same combination"), problems);
            expect (problems.contains ("has no 'Pizz.'"), problems);
            expect (problems.contains ("root 'Flautando' must give a sound slot"), problems);
            expect (problems.contains ("belong to the slots"), problems);
            expect (problems.contains ("not another modifier group"), problems);
        }
    }
};

static ExpressionMapSlotTests expressionMapSlotTests;
