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

            // Prerequisites: with only the colour chosen, only Main is offered (Legato needs Long,
            // Tempo needs Rep. - chosen, not merely possible)
            const auto colourOnly = sel ("Regular");
            expectEquals (offeredNames (map, colourOnly, "Main").joinIntoString (","), juce::String ("Long,Rep.,Staccato"));
            expect (offeredNames (map, colourOnly, "Legato").isEmpty() && offeredNames (map, colourOnly, "Tempo").isEmpty()
                     && offeredNames (map, colourOnly, "Release").isEmpty());

            // Rep. with Legato: the tempos are still offered (a slot has Rep. + Legato + 120)
            expectEquals (offeredNames (map, sel ("Regular", { { "Main", "Rep." }, { "Legato", "Legato" }, { "Tempo", "120" } }), "Tempo")
                              .joinIntoString (","), juce::String ("120"));
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
            auto coloured = map;
            coloured.slots[0].colour = "#eac13a";
            expect (Map::fromXml (*coloured.toXml()).slots[0].colour == "#eac13a");
            Map colouredBack;
            expect (Map::fromVar (coloured.toVar(), colouredBack).isEmpty() && colouredBack.slots[0].colour == "#eac13a");
            coloured.slots[1].colour = "yellow";
            expect (coloured.validate().joinIntoString (";").contains ("#rrggbb"));

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

        beginTest ("a map with outputs on its articulations converts to one slot per combination");
        {
            Map additive;
            additive.name = "Keyswitches";
            Map::Articulation staccato, legato, shortRelease, mute;
            staccato.name = "Staccato";
            staccato.outputs = { { Out::Type::keyswitch, 24, 100, false, -1 } };
            staccato.keyLow = 40;
            staccato.keyHigh = 90;
            legato.name = "Legato";
            legato.outputs = { { Out::Type::keyswitch, 25, 100, false, -1 } };
            legato.timingOffsetMs = -70.0;
            shortRelease.name = "Short";
            shortRelease.appliesTo = { "Staccato" };
            shortRelease.outputs = { { Out::Type::controller, 33, 10, false, -1 } };
            mute.name = "Con sord";
            mute.outputs = { { Out::Type::keyswitch, 30, 100, false, -1 } };
            mute.timingOffsetMs = 10.0;
            additive.groups.push_back ({ "Articulation", "", { staccato, legato } });
            additive.groups.push_back ({ "Release", "", { shortRelease } });
            additive.groups.push_back ({ "Mute", "", { mute } });

            expect (additive.convertToSlots().isEmpty());
            expectEquals ((int) additive.slots.size(), 6);   // Staccato x (none/Short) x (none/Con sord) + Legato x (none/Con sord)
            expect (additive.validate().isEmpty(), additive.validate().joinIntoString ("; "));
            expect (additive.groups[0].articulations[0].outputs.empty() && additive.groups[1].articulations[0].appliesTo.isEmpty());

            const auto* all = additive.findSlot (sel ("Staccato", { { "Release", "Short" }, { "Mute", "Con sord" } }));
            expect (all != nullptr && all->outputs.size() == 3 && all->keyLow == 40 && all->timingOffsetMs == 10.0);
            const auto* legatoMute = additive.findSlot (sel ("Legato", { { "Mute", "Con sord" } }));
            expect (legatoMute != nullptr && legatoMute->timingOffsetMs == -60.0);
            expect (additive.findSlot (sel ("Legato", { { "Release", "Short" } })) == nullptr, "Short applied only to Staccato");
            expect (additive.convertToSlots().isNotEmpty(), "only once");
        }

        // A real one: generated by vsl-manager (tools/daw_maps.py) from the merged Duality 1st Violins preset
        const auto file = juce::File (__FILE__).getParentDirectory().getChildFile ("data/DAW+ Duality 1st Violins.xml");
        const auto xml = juce::XmlDocument::parse (file);
        expect (xml != nullptr, "fixture missing: " + file.getFullPathName());

        if (xml == nullptr)
            return;

        const auto duality = Map::fromXml (*xml);

        beginTest ("the generated Synchron map is valid and its menu follows the preset");
        {
            expect (duality.validate().isEmpty(), duality.validate().joinIntoString ("\n"));
            expectEquals ((int) duality.slots.size(), 177);

            // A colour alone: its default Main
            auto choice = duality.choose ({}, "Color", "Con sordino");
            expect (choice.ok() && choice.selection == sel ("Con sordino", { { "Main", "Long" } }), choice.error);

            const auto regularLong = sel ("Regular", { { "Main", "Long" } });
            expectEquals (offeredNames (duality, regularLong, "Legato").joinIntoString (","), juce::String ("Legato,Lyrical,Slur,Portamento"));
            expect (offeredNames (duality, regularLong, "Tempo").isEmpty(), "no tempo with Long");

            // Ponticello: only what that layer has
            expectEquals (offeredNames (duality, sel ("Ponticello", { { "Main", "Long" } }), "Main").joinIntoString (","),
                          juce::String ("Long,Portato long,Portato,Staccato,Spiccato,Spiccato tight,Tremolo / Flutter"));
            expect (offeredNames (duality, sel ("Ponticello", { { "Main", "Long" } }), "Legato").isEmpty());

            // Rep. fills in 120; the transition Legato is offered with it, Portamento isn't
            choice = duality.choose (regularLong, "Main", "Rep.");
            expect (choice.ok() && choice.selection == sel ("Regular", { { "Main", "Rep." }, { "Tempo", "120" } }), Map::labelOf (choice.selection));
            expectEquals (offeredNames (duality, choice.selection, "Legato").joinIntoString (","), juce::String ("Legato,Slur"));

            // Each colour has its own programs for the same name
            const auto* sordino = duality.findSlot (sel ("Con sordino", { { "Main", "Staccato" } }));
            const auto* ponticello = duality.findSlot (sel ("Ponticello", { { "Main", "Staccato" } }));
            expect (sordino != nullptr && ponticello != nullptr);

            if (sordino != nullptr && ponticello != nullptr)
            {
                expectEquals (sordino->outputs[0].number, 113);
                expectEquals (ponticello->outputs[0].number, 115);
                expect (sordino->outputs[1].number != ponticello->outputs[1].number, "a different main slot per layer");
            }
        }
    }
};

static ExpressionMapSlotTests expressionMapSlotTests;
