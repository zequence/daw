#include "../src/api/CommandDispatcher.h"
#include "../src/model/ExpressionMapLibrary.h"

namespace
{
    using Map = ExpressionMap;
    using Out = ExpressionMap::Output;

    juce::var params (std::initializer_list<std::pair<juce::String, juce::var>> pairs)
    {
        auto o = new juce::DynamicObject();
        for (auto& [key, value] : pairs)
            o->setProperty (juce::Identifier (key), value);
        return juce::var (o);
    }

    Map makeMap (const juce::String& name)
    {
        Map map;
        map.name = name;
        Map::Articulation a;
        a.name = "Sustain";
        a.outputs.push_back ({ Out::Type::keyswitch, 24, 100, false, -1 });
        map.groups.push_back ({ "Articulation", "", { a } });
        return map;
    }

    juce::String errorOf (const juce::var& reply)    { return reply["error"].toString(); }
}

class ExpressionMapLibraryTests final : public juce::UnitTest
{
public:
    ExpressionMapLibraryTests() : UnitTest ("Expression map library") {}

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("OrchestralDAWMapLibraryTest");
        dir.deleteRecursively();

        beginTest ("save, list, find and remove (names ignore case)");
        {
            expect (expressionMapLibrary::list (dir).empty());
            expectEquals (expressionMapLibrary::save (dir, makeMap ("Strings")), juce::String());
            expectEquals (expressionMapLibrary::save (dir, makeMap ("strings")), juce::String());   // replaces
            expectEquals ((int) expressionMapLibrary::list (dir).size(), 1);
            expect (expressionMapLibrary::find (dir, "STRINGS").has_value());

            expect (expressionMapLibrary::remove (dir, "nope").contains ("no library map"));
            expectEquals (expressionMapLibrary::remove (dir, "Strings"), juce::String());
            expect (expressionMapLibrary::list (dir).empty());
        }

        beginTest ("templates are found, valid, and protected");
        {
            const auto templates = expressionMapLibrary::templates();
            expect (! templates.empty());

            for (auto& t : templates)
            {
                expect (t.validate().isEmpty());
                expect (expressionMapLibrary::find (dir, t.name).has_value());
                expect (expressionMapLibrary::remove (dir, t.name).contains ("built-in"));
                expect (expressionMapLibrary::save (dir, t).contains ("built-in"));
            }
        }

        beginTest ("invalid maps are not saved; unreadable files are skipped and explained");
        {
            auto bad = makeMap ("Bad");
            bad.groups.clear();
            expect (expressionMapLibrary::save (dir, bad).contains ("not valid"));

            dir.createDirectory();
            dir.getChildFile ("junk.xml").replaceWithText ("not xml at all");
            dir.getChildFile ("cubase.xml").replaceWithText ("<InstrumentMap/>");
            expect (expressionMapLibrary::list (dir).empty());

            juce::String error;
            expect (! expressionMapLibrary::readFile (dir.getChildFile ("cubase.xml"), error).has_value());
            expect (error.contains ("not one of this application's"), error);
            expect (! expressionMapLibrary::readFile (dir.getChildFile ("junk.xml"), error).has_value());
            expect (error.contains ("not an XML file"), error);
        }

        beginTest ("commands: library, add (with 'as'), export and import");
        {
            juce::PropertiesFile::Options options;
            options.storageFormat = juce::PropertiesFile::storeAsXML;
            juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                               .getChildFile ("OrchestralDAWTestSettings.xml"), options);
            AudioEngine engine (settings);
            engine.setMapLibraryDir (dir);
            CommandDispatcher api (engine);

            auto reply = api.run ("expressionmap.set", params ({ { "map", makeMap ("Brass").toVar() } }));
            expect (reply["ok"], errorOf (reply));
            reply = api.run ("expressionmap.saveToLibrary", params ({ { "name", "brass" } }));
            expect (reply["ok"], errorOf (reply));

            const auto list = api.run ("expressionmap.libraryList")["result"];
            auto foundBrass = false, foundTemplate = false;

            for (int i = 0; i < list.size(); ++i)
            {
                foundBrass = foundBrass || list[i]["name"].toString() == "Brass";
                foundTemplate = foundTemplate || (bool) list[i]["template"];
            }

            expect (foundBrass && foundTemplate);

            reply = api.run ("expressionmap.addFromLibrary", params ({ { "name", "Brass" } }));
            expect (! reply["ok"] && errorOf (reply).contains ("already has"), errorOf (reply));

            reply = api.run ("expressionmap.addFromLibrary", params ({ { "name", "Brass" }, { "as", "Brass 2" } }));
            expect (reply["ok"], errorOf (reply));
            expect (engine.getExpressionMap ("Brass 2").has_value());

            reply = api.run ("expressionmap.addFromLibrary", params ({ { "name", "Nothing" } }));
            expect (! reply["ok"] && errorOf (reply).contains ("existing:"), errorOf (reply));

            const auto file = dir.getChildFile ("exported/brass.xml");
            reply = api.run ("expressionmap.export", params ({ { "name", "Brass" }, { "path", file.getFullPathName() } }));
            expect (reply["ok"], errorOf (reply));

            reply = api.run ("expressionmap.import", params ({ { "path", file.getFullPathName() }, { "as", "Brass 3" } }));
            expect (reply["ok"], errorOf (reply));
            expect (engine.getExpressionMap ("Brass 3").has_value());

            reply = api.run ("expressionmap.import", params ({ { "path", file.getFullPathName() } }));
            expect (! reply["ok"] && errorOf (reply).contains ("already has"), errorOf (reply));

            reply = api.run ("expressionmap.deleteFromLibrary", params ({ { "name", "Brass" } }));
            expect (reply["ok"], errorOf (reply));
            expect (engine.getExpressionMap ("Brass").has_value());   // the project's copy stays
        }

        dir.deleteRecursively();
    }
};

static ExpressionMapLibraryTests expressionMapLibraryTests;
