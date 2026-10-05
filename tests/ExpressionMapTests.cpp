#include "../src/model/ExpressionMap.h"
#include "../src/model/MidiSequence.h"
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

    // For the choosing rules: roots Staccato/Legato/Marcato; Release (Short and
    // Soft for Staccato+Marcato, Long for Legato), Mute (everything), Dynamics (Marcato only)
    Map selectionMap()
    {
        Map map;
        map.name = "Rules";
        map.groups.push_back ({ "Articulation", "", { art ("Staccato", Out::Type::controller, 32, 10),
                                                      art ("Legato", Out::Type::controller, 32, 20),
                                                      art ("Marcato", Out::Type::controller, 32, 30) } });

        auto shortRelease = art ("Short", Out::Type::controller, 33, 10);
        auto softRelease = art ("Soft", Out::Type::controller, 33, 20);
        auto longRelease = art ("Long", Out::Type::controller, 33, 90);
        shortRelease.appliesTo = softRelease.appliesTo = { "Staccato", "Marcato" };
        longRelease.appliesTo = { "Legato" };
        map.groups.push_back ({ "Release", "", { shortRelease, softRelease, longRelease } });

        map.groups.push_back ({ "Mute", "", { art ("Con sord", Out::Type::keyswitch, 24) } });

        auto sfz = art ("Sfz", Out::Type::controller, 34, 127);
        sfz.appliesTo = { "Marcato" };
        map.groups.push_back ({ "Dynamics", "", { sfz } });
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

        beginTest ("XML round trip keeps everything, including order, text and outputs in series");
        {
            auto map = spitfireLike();
            map.description = "Line one\nLine two & <more>";
            map.groups[0].description = "Main \"articulations\"";
            map.groups[0].articulations[1].symbol = juce::String::fromUTF8 ("\xe2\x99\xa9");   // a music note
            map.groups[0].articulations[1].keyLow = 55;
            map.groups[0].articulations[1].keyHigh = 103;
            map.groups[0].articulations[1].timingOffsetMs = -70.5;
            map.groups[0].articulations[1].outputs = { { Out::Type::keyswitch, 12, 90, true, -1 },
                                                       { Out::Type::controller, 7, 64, false, -1 },
                                                       { Out::Type::programChange, 4, 0, false, 130 } };
            map.groups[0].articulations[2].outputs.clear();
            map.groups[0].articulations[2].name = "Trem|olo";   // names are free text
            map.groups[1].articulations[1].appliesTo = { "Legato", "Trem|olo" };

            const auto xml = map.toXml();
            const auto reread = Map::fromXml (*juce::XmlDocument::parse (xml->toString()));

            expectEquals (reread.name, map.name);
            expectEquals (reread.description, map.description);
            expectEquals ((int) reread.groups.size(), 3);

            for (size_t g = 0; g < map.groups.size(); ++g)
            {
                expectEquals (reread.groups[g].name, map.groups[g].name);
                expectEquals (reread.groups[g].description, map.groups[g].description);
                expectEquals ((int) reread.groups[g].articulations.size(), (int) map.groups[g].articulations.size());

                for (size_t a = 0; a < map.groups[g].articulations.size(); ++a)
                {
                    auto& x = map.groups[g].articulations[a];
                    auto& y = reread.groups[g].articulations[a];
                    expectEquals (y.name, x.name);
                    expectEquals (y.symbol, x.symbol);
                    expectEquals (y.description, x.description);
                    expectEquals (y.timingOffsetMs, x.timingOffsetMs);
                    expectEquals (y.keyLow, x.keyLow);
                    expectEquals (y.keyHigh, x.keyHigh);
                    expect (y.appliesTo == x.appliesTo);
                    expectEquals ((int) y.outputs.size(), (int) x.outputs.size());

                    for (size_t o = 0; o < x.outputs.size(); ++o)
                    {
                        expect (y.outputs[o].type == x.outputs[o].type);
                        expectEquals (y.outputs[o].number, x.outputs[o].number);
                        expectEquals (y.outputs[o].value, x.outputs[o].value);
                        expect (y.outputs[o].held == x.outputs[o].held);
                        expectEquals (y.outputs[o].bank, x.outputs[o].bank);
                    }
                }
            }

            expect (reread.isValid(), reread.validate().joinIntoString ("; "));
        }

        beginTest ("reading is forgiving: defaults for missing attributes, unknown output types skipped");
        {
            const auto xml = juce::XmlDocument::parse (
                R"(<EXPRESSIONMAP name="Tiny"><GROUP name="Root"><ARTICULATION name="A">
                   <OUTPUT type="sysex" number="1"/><OUTPUT type="controller" number="32"/></ARTICULATION></GROUP></EXPRESSIONMAP>)");
            const auto map = Map::fromXml (*xml);
            expectEquals (map.name, juce::String ("Tiny"));
            const auto& a = map.groups[0].articulations[0];
            expectEquals ((int) a.outputs.size(), 1);   // the unknown type was skipped
            expectEquals (a.outputs[0].number, 32);
            expectEquals (a.outputs[0].value, 100);     // default
            expectEquals (a.outputs[0].bank, -1);
            expectEquals (a.keyLow, -1);
            expectEquals (a.timingOffsetMs, 0.0);
        }

        beginTest ("a note carries its articulation: root plus modifiers, saved with the sequence");
        {
            using Note = MidiSequence::Note;
            Note plain { 0, 480, 1, 60, 100 };

            Note rootOnly { 480, 480, 1, 62, 100 };
            rootOnly.articulation.root = "Staccato";

            Note full { 960, 480, 1, 64, 100 };
            full.articulation.root = "Legato";
            full.articulation.modifiers = { { "Release", "Short" }, { "Mute \"Pro\"", "Con sord" } };

            // Names that aren't in any map are kept as they are (a visible error later, never erased)
            Note orphan { 1440, 480, 1, 65, 100 };
            orphan.articulation.root = "No such articulation";

            const auto original = MidiSequence::create ({ plain, rootOnly, full, orphan }, {});
            const auto xml = original->toXml();

            // A note without an articulation writes no extra element (small files, old readers unaffected)
            expect (xml->getChildWithTagNameIterator ("NOTE").begin() != xml->getChildWithTagNameIterator ("NOTE").end());
            expect (xml->getChildByName ("NOTE")->getChildByName ("ARTICULATION") == nullptr);

            const auto restored = MidiSequence::fromXml (*juce::XmlDocument::parse (xml->toString()));
            expectEquals ((int) restored->getNotes().size(), 4);

            auto& notes = restored->getNotes();
            expect (notes[0].articulation.isEmpty());
            expect (notes[1].articulation == rootOnly.articulation);
            expect (notes[2].articulation == full.articulation);
            expectEquals ((int) notes[2].articulation.modifiers.size(), 2);
            expectEquals (notes[2].articulation.modifiers[1].first, juce::String ("Mute \"Pro\""));
            expect (notes[3].articulation == orphan.articulation);

            // Selections compare names ignoring case
            auto lower = full.articulation;
            lower.root = "legato";
            lower.modifiers[0].second = "SHORT";
            expect (lower == full.articulation);
            lower.modifiers[0].second = "Long";
            expect (lower != full.articulation);

            // A project written before articulations existed loads with none
            const auto old = juce::XmlDocument::parse ("<SEQUENCE><NOTE start=\"0\" length=\"480\" channel=\"1\" key=\"60\" velocity=\"100\"/></SEQUENCE>");
            expect (MidiSequence::fromXml (*old)->getNotes().front().articulation.isEmpty());
        }

        beginTest ("choosing: roots switch directly and toggle off; the first choice needs no modifiers");
        {
            const auto map = selectionMap();
            Map::Selection none;

            auto choice = map.choose (none, "articulation", "STACCATO");   // names ignore case
            expect (choice.ok(), choice.error);
            expectEquals (choice.selection.root, juce::String ("Staccato"));   // stored with the map's spelling
            expect (choice.dropped.empty());

            // Another root replaces it, no unselecting first
            choice = map.choose (choice.selection, "Articulation", "Legato");
            expect (choice.ok() && choice.selection.root == "Legato", choice.error);

            // Choosing the selected root again unselects it
            choice = map.choose (choice.selection, "Articulation", "Legato");
            expect (choice.ok() && choice.selection.isEmpty(), choice.error);
        }

        beginTest ("choosing: modifiers need a root, apply to it, and are exclusive within their group");
        {
            const auto map = selectionMap();

            // No root yet
            auto choice = map.choose ({}, "Release", "Short");
            expect (! choice.ok() && choice.error.contains ("need a root"), choice.error);
            expect (choice.selection.isEmpty());

            Map::Selection staccato;
            staccato.root = "Staccato";

            // Doesn't apply to this root (and says what it applies to)
            choice = map.choose (staccato, "Release", "Long");
            expect (! choice.ok() && choice.error.contains ("doesn't apply to the root 'Staccato'")
                      && choice.error.contains ("it applies to: Legato"), choice.error);

            // Applies; chosen
            choice = map.choose (staccato, "release", "short");
            expect (choice.ok(), choice.error);
            expectEquals ((int) choice.selection.modifiers.size(), 1);
            expectEquals (choice.selection.modifiers[0].first, juce::String ("Release"));
            expectEquals (choice.selection.modifiers[0].second, juce::String ("Short"));

            // The group is exclusive: its other items are unavailable until this one is unselected
            const auto withShort = choice.selection;
            choice = map.choose (withShort, "Release", "Soft");
            expect (! choice.ok() && choice.error.contains ("already has 'Short' chosen"), choice.error);
            expect (choice.selection == withShort);   // unchanged on failure

            // Choosing it again toggles it off, then the other one can be chosen
            choice = map.choose (withShort, "Release", "Short");
            expect (choice.ok() && choice.selection.modifiers.empty(), choice.error);
            choice = map.choose (choice.selection, "Release", "Soft");
            expect (choice.ok() && choice.selection.modifiers[0].second == "Soft", choice.error);

            // Different groups don't exclude each other, and the order is the map's group order
            auto both = map.choose (choice.selection, "Mute", "Con sord");
            expect (both.ok(), both.error);
            both = map.choose (map.choose (staccato, "Mute", "Con sord").selection, "Release", "Soft");
            expect (both.ok(), both.error);
            expectEquals (both.selection.modifiers[0].first, juce::String ("Release"));
            expectEquals (both.selection.modifiers[1].first, juce::String ("Mute"));
        }

        beginTest ("choosing: changing the root keeps modifiers that still apply and reports the ones it would drop");
        {
            const auto map = selectionMap();
            Map::Selection selection;
            selection.root = "Staccato";
            selection.modifiers = { { "Release", "Short" }, { "Mute", "Con sord" } };

            // Legato: Con sord applies to every root and stays; Short is Staccato-only and would be dropped
            auto choice = map.choose (selection, "Articulation", "Legato");
            expect (choice.ok(), choice.error);
            expectEquals (choice.selection.root, juce::String ("Legato"));
            expectEquals ((int) choice.selection.modifiers.size(), 1);
            expectEquals (choice.selection.modifiers[0].second, juce::String ("Con sord"));
            expectEquals ((int) choice.dropped.size(), 1);
            expectEquals (choice.dropped[0].second, juce::String ("Short"));

            // Marcato also takes Short: nothing dropped
            choice = map.choose (selection, "Articulation", "Marcato");
            expect (choice.ok() && choice.dropped.empty() && choice.selection.modifiers.size() == 2, choice.error);

            // Unselecting the root drops every modifier
            choice = map.choose (selection, "Articulation", "Staccato");
            expect (choice.ok() && choice.selection.isEmpty(), choice.error);
            expectEquals ((int) choice.dropped.size(), 2);

            // A modifier that isn't in the map at all is dropped too (it can't apply to anything)
            selection.modifiers.emplace_back ("Release", "Vanished");
            choice = map.choose (selection, "Articulation", "Marcato");
            expect (choice.ok(), choice.error);
            expectEquals ((int) choice.dropped.size(), 1);
            expectEquals (choice.dropped[0].second, juce::String ("Vanished"));
        }

        beginTest ("choosing: unknown groups and articulations name what exists");
        {
            const auto map = selectionMap();
            auto choice = map.choose ({}, "Dynamicss", "Soft");
            expect (! choice.ok() && choice.error.contains ("no group 'Dynamicss'")
                      && choice.error.contains ("Articulation, Release, Mute, Dynamics"), choice.error);

            choice = map.choose ({}, "Articulation", "Pizz");
            expect (! choice.ok() && choice.error.contains ("it has: Staccato, Legato, Marcato"), choice.error);

            Map noGroups;
            noGroups.name = "Empty";
            expect (! noGroups.choose ({}, "A", "B").ok());
        }

        beginTest ("available modifiers: only those that apply to the root; groups with nothing left are left out");
        {
            const auto map = selectionMap();

            // Staccato: Release (Short, Soft) and Mute; Dynamics only works with Marcato
            const auto forStaccato = map.availableModifiers ("staccato");
            expectEquals ((int) forStaccato.size(), 2);
            expectEquals (forStaccato[0].group->name, juce::String ("Release"));
            expectEquals ((int) forStaccato[0].articulations.size(), 2);
            expectEquals (forStaccato[1].group->name, juce::String ("Mute"));

            // Legato: Release has only Long for it, plus Mute; no Dynamics
            const auto forLegato = map.availableModifiers ("Legato");
            expectEquals ((int) forLegato.size(), 2);
            expectEquals ((int) forLegato[0].articulations.size(), 1);
            expectEquals (forLegato[0].articulations[0]->name, juce::String ("Long"));

            // Marcato: Release (Short, Soft), Mute and Dynamics
            expectEquals ((int) map.availableModifiers ("Marcato").size(), 3);
        }

        beginTest ("problems of a selection: missing root, group or articulation, inapplicable and doubled modifiers");
        {
            const auto map = selectionMap();

            Map::Selection fine;
            fine.root = "Staccato";
            fine.modifiers = { { "Release", "Short" } };
            expect (map.problemsOf (fine).isEmpty());
            expect (map.problemsOf ({}).isEmpty());   // none is fine

            Map::Selection noRoot;
            noRoot.modifiers = { { "Mute", "Con sord" } };
            expect (anyContains (map.problemsOf (noRoot), "need a root"));

            Map::Selection missingRoot;
            missingRoot.root = "Pizzicato";
            const auto missing = map.problemsOf (missingRoot);
            expect (anyContains (missing, "root articulation 'Pizzicato' is not in map"));
            expect (anyContains (missing, "it has: Staccato, Legato, Marcato"));

            Map::Selection bad;
            bad.root = "Staccato";
            bad.modifiers = { { "Nowhere", "X" }, { "Release", "Nope" }, { "Release", "Long" }, { "Articulation", "Legato" } };
            const auto problems = map.problemsOf (bad);
            expect (anyContains (problems, "group 'Nowhere' is not in map"));
            expect (anyContains (problems, "'Nope' is not in group 'Release'"));
            expect (anyContains (problems, "two articulations are chosen from group 'Release'"));
            expect (anyContains (problems, "'Long' (group 'Release') doesn't apply to the root 'Staccato'"));
            expect (anyContains (problems, "'Articulation' is the root group"));
        }

        beginTest ("renaming: stays unique ignoring case, a case-only change is fine, errors name what exists");
        {
            auto map = selectionMap();

            expectEquals (map.renameGroup ("release", "Tail"), juce::String());
            expect (map.findGroup ("Tail") != nullptr && map.findGroup ("Release") == nullptr);

            expectEquals (map.renameGroup ("Tail", "TAIL"), juce::String());   // the same group, other case
            expectEquals (map.groups[1].name, juce::String ("TAIL"));

            expect (map.renameGroup ("Mute", "tail").contains ("already a group 'TAIL'"));
            expect (map.renameGroup ("Nope", "X").contains ("no group 'Nope'"));
            expect (map.renameGroup ("Mute", "  ").contains ("needs a name"));
            expectEquals (map.groups[2].name, juce::String ("Mute"));   // unchanged by the refusals

            expectEquals (map.renameArticulation ("Mute", "Con sord", "Muted"), juce::String());
            expectEquals (map.groups[2].articulations[0].name, juce::String ("Muted"));
            expect (map.renameArticulation ("Articulation", "Legato", "staccato").contains ("already has an articulation 'Staccato'"));
            expect (map.renameArticulation ("Articulation", "Pizz", "X").contains ("it has: Staccato, Legato, Marcato"));
            expect (map.renameArticulation ("Nowhere", "Legato", "X").contains ("no group 'Nowhere'"));
            expect (map.renameArticulation ("Articulation", "Legato", "").contains ("needs a name"));

            // The same articulation name in another group is fine
            expectEquals (map.renameArticulation ("TAIL", "Short", "Staccato"), juce::String());

            expect (map.isValid(), map.validate().joinIntoString ("; "));
        }

        beginTest ("renaming a root articulation updates the modifiers' applies-to lists");
        {
            auto map = selectionMap();   // Short and Soft apply to Staccato + Marcato, Long to Legato, Sfz to Marcato
            expectEquals (map.renameArticulation ("Articulation", "Staccato", "Spiccato"), juce::String());

            expectEquals (map.findArticulation ("Release", "Short")->appliesTo.joinIntoString (","), juce::String ("Spiccato,Marcato"));
            expectEquals (map.findArticulation ("Release", "Long")->appliesTo.joinIntoString (","), juce::String ("Legato"));
            expect (map.isValid(), map.validate().joinIntoString ("; "));   // nothing is left pointing at the old name

            // Renaming a modifier doesn't touch any list
            expectEquals (map.renameArticulation ("Release", "Short", "Brief"), juce::String());
            expectEquals (map.findArticulation ("Release", "Soft")->appliesTo.joinIntoString (","), juce::String ("Spiccato,Marcato"));
        }

        beginTest ("selections follow a rename");
        {
            Map::Selection selection;
            selection.root = "Staccato";
            selection.modifiers = { { "Release", "Short" }, { "Mute", "Con sord" } };

            expect (! selection.renameRoot ("Legato", "X"));                  // not this root
            expect (selection.renameRoot ("STACCATO", "Spiccato"));            // names ignore case
            expectEquals (selection.root, juce::String ("Spiccato"));
            expect (! selection.renameRoot ("Staccato", "Spiccato"));          // already renamed

            expect (selection.renameModifierGroup ("release", "Tail"));
            expectEquals (selection.modifiers[0].first, juce::String ("Tail"));
            expectEquals (selection.modifiers[1].first, juce::String ("Mute"));

            expect (selection.renameModifier ("Tail", "short", "Brief"));
            expectEquals (selection.modifiers[0].second, juce::String ("Brief"));
            expect (! selection.renameModifier ("Mute", "Brief", "X"));        // a different group's articulation
            expect (! selection.renameModifier ("Nowhere", "Con sord", "X"));
            expectEquals (selection.modifiers[1].second, juce::String ("Con sord"));
        }

        beginTest ("key names: explicit names win, keyswitches are named after their articulations");
        {
            auto map = selectionMap();
            map.groups[0].articulations[0].outputs = { { Out::Type::keyswitch, 24, 100, false, -1 } };   // Staccato on C0... key 24
            map.groups[0].articulations[1].outputs = { { Out::Type::keyswitch, 25, 100, false, -1 } };   // Legato on 25
            map.groups[2].articulations[0].outputs = { { Out::Type::keyswitch, 25, 100, false, -1 } };   // Con sord shares 25

            juce::String instruction;
            expectEquals (map.keyLabel (24, &instruction), juce::String ("Staccato"));
            expectEquals (instruction, juce::String ("Keyswitch for Staccato"));
            expectEquals (map.keyLabel (25), juce::String ("Legato / Con sord"));   // shared: both named
            expectEquals (map.keyLabel (60), juce::String());                       // nothing to say
            expect (map.isKeyswitch (24) && map.isKeyswitch (25) && ! map.isKeyswitch (26));

            // An explicit name replaces the automatic one and brings its instruction
            map.keyNames.push_back ({ 24, "Short notes", "Hold for staccato, release for legato" });
            expectEquals (map.keyLabel (24, &instruction), juce::String ("Short notes"));
            expectEquals (instruction, juce::String ("Hold for staccato, release for legato"));

            // A key can be named without being a keyswitch (instructions for the player)
            map.keyNames.push_back ({ 26, "Repeat once", "" });
            expectEquals (map.keyLabel (26), juce::String ("Repeat once"));
            expect (! map.isKeyswitch (26));

            expect (map.isValid(), map.validate().joinIntoString ("; "));
        }

        beginTest ("key names are checked, and saved in both XML and JSON");
        {
            auto map = spitfireLike();
            map.keyNames = { { 12, "Legato", "Hold" }, { 12, "Again", "" }, { 200, "High", "" }, { 14, "  ", "" } };
            const auto problems = map.validate();
            expect (anyContains (problems, "key 12 is named twice ('Legato' and 'Again')"));
            expect (anyContains (problems, "outside 0-127"));
            expect (anyContains (problems, "the name of key 14 is empty"));

            map.keyNames = { { 12, "Legato", "Hold the key\nthen play" }, { 13, "Repeat", "" } };
            expect (map.isValid(), map.validate().joinIntoString ("; "));

            const auto viaXml = Map::fromXml (*juce::XmlDocument::parse (map.toXml()->toString()));
            expectEquals ((int) viaXml.keyNames.size(), 2);
            expectEquals (viaXml.keyNames[0].instruction, juce::String ("Hold the key\nthen play"));

            Map viaJson;
            expectEquals (Map::fromVar (map.toVar(), viaJson), juce::String());
            expectEquals ((int) viaJson.keyNames.size(), 2);
            expectEquals (viaJson.keyNames[1].name, juce::String ("Repeat"));

            Map bad;
            const auto json = map.toVar();
            json.getDynamicObject()->setProperty ("keyNames", juce::Array<juce::var> { juce::var (5) });
            expect (Map::fromVar (json, bad).contains ("every key name needs a key"));
        }

        beginTest ("playable range of a choice: the intersection of what the root and the modifiers define");
        {
            const auto sel = [] (const char* root, std::initializer_list<std::pair<const char*, const char*>> modifiers = {})
            {
                Map::Selection s;
                s.root = root;

                for (auto& [group, articulation] : modifiers)
                    s.modifiers.emplace_back (group, articulation);

                return s;
            };

            auto map = selectionMap();
            map.groups[0].articulations[0].keyLow = 40;     // Staccato 40-90
            map.groups[0].articulations[0].keyHigh = 90;
            map.groups[1].articulations[0].keyLow = 50;     // Short 50-100
            map.groups[1].articulations[0].keyHigh = 100;

            int low = -1, high = -1;
            expect (map.playableRange (sel ("Staccato"), low, high) && low == 40 && high == 90);
            expect (map.playableRange (sel ("Staccato", { { "Release", "Short" } }), low, high) && low == 50 && high == 90);   // both narrow it

            // A modifier alone defines nothing without a root; a root without a range defines nothing either
            expect (! map.playableRange (sel ("Legato"), low, high));
            expect (map.playableRange (sel ("Legato", { { "Release", "Long" } }), low, high) == false);   // Long has no range
            expect (! map.playableRange ({}, low, high));
            expectEquals (low, 0);    // untouched defaults when nothing defines a range
            expectEquals (high, 127);
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
