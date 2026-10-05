#include "../src/model/ExpressionMap.h"
#include <juce_events/juce_events.h>

namespace
{
    using Map = ExpressionMap;
    using Out = ExpressionMap::Output;

    Map::Articulation art (const juce::String& name, Out::Type type, int number, int value = 100)
    {
        Map::Articulation a;
        a.name = name;
        a.outputs.push_back ({ type, number, value, false, -1 });
        return a;
    }

    Map::Articulation uacc (const juce::String& name, int value)   { return art (name, Out::Type::controller, 32, value); }

    // A Spitfire-like map: roots on UACC (CC32), a Release group and a Mute group
    Map spitfireLike()
    {
        Map map;
        map.name = "Strings";
        map.groups.push_back ({ "Articulation", "Main articulations",
                                { uacc ("Staccato", 10), uacc ("Legato", 20), uacc ("Tremolo", 30) } });

        auto shortRelease = art ("Short", Out::Type::controller, 33, 10);
        shortRelease.appliesTo = { "Staccato" };
        auto longRelease = art ("Long", Out::Type::controller, 33, 90);
        longRelease.appliesTo = { "Legato", "Tremolo" };
        map.groups.push_back ({ "Release", "How the note ends", { shortRelease, longRelease } });

        map.groups.push_back ({ "Mute", "", { art ("Con sord", Out::Type::keyswitch, 24) } });
        return map;
    }

    bool anyContains (const juce::StringArray& problems, const juce::String& text)
    {
        for (auto& p : problems)
            if (p.containsIgnoreCase (text))
                return true;

        return false;
    }
}

class ExpressionMapTests final : public juce::UnitTest
{
public:
    ExpressionMapTests() : UnitTest ("Expression maps") {}

    void runTest() override
    {
        beginTest ("a well-formed map is valid");
        {
            const auto map = spitfireLike();
            const auto problems = map.validate();
            expect (problems.isEmpty(), problems.joinIntoString ("; "));
            expect (map.isValid());
        }

        beginTest ("lookups ignore case and surrounding spaces");
        {
            const auto map = spitfireLike();
            expect (map.findGroup ("release") != nullptr);
            expect (map.findGroup ("  RELEASE ") != nullptr);
            expect (map.findArticulation ("release", "SHORT") != nullptr);
            expect (map.findArticulation ("Release", "Missing") == nullptr);
            expect (map.findArticulation ("Missing", "Short") == nullptr);
            expect (map.rootGroup() == map.findGroup ("Articulation"));
        }

        beginTest ("applicability: an empty list means every root; otherwise only the listed ones");
        {
            const auto map = spitfireLike();
            const auto* shortRelease = map.findArticulation ("Release", "Short");
            const auto* conSord = map.findArticulation ("Mute", "Con sord");
            expect (Map::appliesToRoot (*shortRelease, "staccato"));
            expect (! Map::appliesToRoot (*shortRelease, "Legato"));
            expect (Map::appliesToRoot (*conSord, "Legato"));
            expect (Map::appliesToRoot (*conSord, "Anything"));
        }

        beginTest ("the first root articulation is the default root");
        {
            const auto map = spitfireLike();
            expect (map.firstRoot() != nullptr && map.firstRoot()->name == "Staccato");

            Map empty;
            expect (empty.firstRoot() == nullptr);
            empty.groups.push_back ({ "Articulation", "", {} });
            expect (empty.firstRoot() == nullptr);   // a root group without articulations
        }

        beginTest ("structure: root group required, names present and unique ignoring case");
        {
            Map empty;
            empty.name = "X";
            expect (anyContains (empty.validate(), "root group"));

            auto noName = spitfireLike();
            noName.name = "  ";
            expect (anyContains (noName.validate(), "needs a name"));

            auto dupGroup = spitfireLike();
            dupGroup.groups[2].name = "release";
            expect (anyContains (dupGroup.validate(), "two groups are named"));

            auto dupArticulation = spitfireLike();
            dupArticulation.groups[0].articulations[1].name = "STACCATO";
            expect (anyContains (dupArticulation.validate(), "two articulations named"));

            // The same articulation name in different groups is fine
            auto sameAcrossGroups = spitfireLike();
            sameAcrossGroups.groups[2].articulations[0].name = "Staccato";
            expect (sameAcrossGroups.isValid(), sameAcrossGroups.validate().joinIntoString ("; "));

            auto unnamed = spitfireLike();
            unnamed.groups[1].articulations[0].name = "";
            expect (anyContains (unnamed.validate(), "needs a name"));
        }

        beginTest ("applies-to must name existing root articulations; roots can't have one");
        {
            auto bad = spitfireLike();
            bad.groups[1].articulations[0].appliesTo = { "Pizzicato" };
            const auto problems = bad.validate();
            expect (anyContains (problems, "'Pizzicato', which is not a root articulation"));
            expect (anyContains (problems, "Staccato, Legato, Tremolo"));   // names what exists

            auto root = spitfireLike();
            root.groups[0].articulations[0].appliesTo = { "Legato" };
            expect (anyContains (root.validate(), "root articulation and can't have"));
        }

        beginTest ("values: outputs, key range and timing offset are range-checked");
        {
            auto map = spitfireLike();
            map.groups[0].articulations[0].outputs[0].number = 200;
            expect (anyContains (map.validate(), "CC number 200"));

            map = spitfireLike();
            map.groups[2].articulations[0].outputs[0].number = -1;
            map.groups[2].articulations[0].outputs[0].value = 0;
            const auto problems = map.validate();
            expect (anyContains (problems, "keyswitch key -1"));
            expect (anyContains (problems, "keyswitch velocity 0"));

            map = spitfireLike();
            map.groups[0].articulations[0].outputs = { { Out::Type::programChange, 5, 0, false, 99999 } };
            expect (anyContains (map.validate(), "bank 99999"));

            map = spitfireLike();
            map.groups[0].articulations[1].keyLow = 80;
            map.groups[0].articulations[1].keyHigh = 60;
            expect (anyContains (map.validate(), "invalid key range"));

            map = spitfireLike();
            map.groups[0].articulations[1].keyLow = 55;
            map.groups[0].articulations[1].keyHigh = 103;
            map.groups[0].articulations[1].timingOffsetMs = -70.0;
            expect (map.isValid(), map.validate().joinIntoString ("; "));   // the legato case

            map.groups[0].articulations[1].timingOffsetMs = -9000.0;
            expect (anyContains (map.validate(), "timing offset"));
        }

        beginTest ("an articulation sends several outputs in series; none at all is fine too");
        {
            auto map = spitfireLike();
            // Legato: keyswitch, then a CC, then a program change (with a bank)
            map.groups[0].articulations[1].outputs = { { Out::Type::keyswitch, 12, 100, false, -1 },
                                                       { Out::Type::controller, 7, 90, false, -1 },
                                                       { Out::Type::programChange, 4, 0, false, 2 } };
            expect (map.isValid(), map.validate().joinIntoString ("; "));
            expectEquals ((int) map.groups[0].articulations[1].outputs.size(), 3);

            // An articulation that needs nothing sent
            map.groups[0].articulations[2].outputs.clear();
            expect (map.isValid(), map.validate().joinIntoString ("; "));

            // A bad value in the third output names the output
            map = spitfireLike();
            map.groups[0].articulations[0].outputs = { { Out::Type::keyswitch, 12, 100, false, -1 },
                                                       { Out::Type::controller, 7, 90, false, -1 },
                                                       { Out::Type::controller, 7, 400, false, -1 } };
            expect (anyContains (map.validate(), "(output 3): CC value 400"));
        }

        beginTest ("no restrictions on combining keys, CCs and program changes (left to the user)");
        {
            // A root and a modifier that works with it on the same CC
            auto map = spitfireLike();
            map.groups[1].articulations[0].outputs = { { Out::Type::controller, 32, 99, false, -1 } };
            expect (map.isValid(), map.validate().joinIntoString ("; "));

            // Two modifiers of different groups on the same CC, and on the same keyswitch key
            map = spitfireLike();
            map.groups[2].articulations[0].outputs = { { Out::Type::controller, 33, 5, false, -1 } };
            expect (map.isValid(), map.validate().joinIntoString ("; "));
            map.groups[1].articulations[0].outputs = { { Out::Type::keyswitch, 24, 100, false, -1 } };
            expect (map.isValid(), map.validate().joinIntoString ("; "));

            // Identical program changes active together, and the same CC repeated inside one articulation
            map = spitfireLike();
            map.groups[0].articulations[0].outputs = { { Out::Type::programChange, 1, 0, false, -1 } };
            map.groups[1].articulations[0].outputs = { { Out::Type::programChange, 1, 0, false, -1 } };
            map.groups[0].articulations[1].outputs = { { Out::Type::controller, 16, 0, false, -1 },
                                                       { Out::Type::controller, 16, 64, false, -1 } };
            expect (map.isValid(), map.validate().joinIntoString ("; "));
        }
    }
};

static ExpressionMapTests expressionMapTests;
