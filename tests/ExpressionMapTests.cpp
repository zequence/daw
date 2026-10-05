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
        a.output.type = type;
        a.output.number = number;
        a.output.value = value;
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
        beginTest ("a well-formed map is valid; articulations of one group may share a CC (UACC)");
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
            map.groups[0].articulations[0].output.number = 200;
            expect (anyContains (map.validate(), "CC number 200"));

            map = spitfireLike();
            map.groups[2].articulations[0].output.number = -1;
            map.groups[2].articulations[0].output.value = 0;
            const auto problems = map.validate();
            expect (anyContains (problems, "keyswitch key -1"));
            expect (anyContains (problems, "keyswitch velocity 0"));

            map = spitfireLike();
            map.groups[0].articulations[0].output = { Out::Type::programChange, 5, 0, false, 99999 };
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

        beginTest ("conflict: a root and a modifier that works with it both send the same CC");
        {
            auto map = spitfireLike();
            map.groups[1].articulations[0].output = { Out::Type::controller, 32, 99, false, -1 };   // Short on CC32 like the roots
            const auto problems = map.validate();
            expect (anyContains (problems, "'Staccato' (group 'Articulation') and 'Short' (group 'Release')"));
            expect (anyContains (problems, "both send CC 32"));
        }

        beginTest ("no conflict when the modifier doesn't work with that root");
        {
            auto map = spitfireLike();
            // Short only applies to Staccato; Legato may share its CC with it
            map.groups[1].articulations[0].output = { Out::Type::controller, 20, 1, false, -1 };
            map.groups[0].articulations[0].output = { Out::Type::controller, 21, 1, false, -1 };   // Staccato on 21
            map.groups[0].articulations[1].output = { Out::Type::controller, 20, 1, false, -1 };   // Legato on 20
            expect (map.isValid(), map.validate().joinIntoString ("; "));
        }

        beginTest ("conflict: modifiers of two groups share a CC or a keyswitch and a root");
        {
            auto map = spitfireLike();
            map.groups[2].articulations[0].output = { Out::Type::controller, 33, 5, false, -1 };   // Mute on CC33 like Release
            expect (anyContains (map.validate(), "'Short' (group 'Release') and 'Con sord' (group 'Mute')"));

            // Different roots only: Short works with Staccato, the second modifier only with Legato
            auto disjoint = spitfireLike();
            disjoint.groups[2].articulations[0].output = { Out::Type::controller, 33, 5, false, -1 };
            disjoint.groups[2].articulations[0].appliesTo = { "Legato" };
            disjoint.groups[1].articulations[1].appliesTo = { "Tremolo" };   // Long: Tremolo only
            expect (disjoint.isValid(), disjoint.validate().joinIntoString ("; "));

            // Two keyswitches on the same key
            auto keys = spitfireLike();
            keys.groups[1].articulations[0].output = { Out::Type::keyswitch, 24, 100, false, -1 };
            expect (anyContains (keys.validate(), "keyswitch key 24"));

            // Two keyswitches on different keys are fine (root + modifier keyswitches)
            keys.groups[1].articulations[0].output.number = 25;
            keys.groups[1].articulations[1].output = { Out::Type::keyswitch, 26, 100, false, -1 };
            expect (keys.isValid(), keys.validate().joinIntoString ("; "));
        }

        beginTest ("conflict: any two program changes that are active together clash");
        {
            auto map = spitfireLike();
            map.groups[0].articulations[0].output = { Out::Type::programChange, 1, 0, false, -1 };
            map.groups[1].articulations[0].output = { Out::Type::programChange, 2, 0, false, -1 };   // different program, still one channel
            expect (anyContains (map.validate(), "both send a program change"));
        }

        beginTest ("same group: sharing a keyswitch, CC or program change is fine");
        {
            Map map;
            map.name = "Pairs";
            map.groups.push_back ({ "Articulation", "",
                                    { art ("A", Out::Type::keyswitch, 12), art ("B", Out::Type::keyswitch, 12),
                                      art ("C", Out::Type::programChange, 3), art ("D", Out::Type::programChange, 3) } });
            expect (map.isValid(), map.validate().joinIntoString ("; "));
        }
    }
};

static ExpressionMapTests expressionMapTests;
