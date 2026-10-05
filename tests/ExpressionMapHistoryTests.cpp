#include "../src/api/CommandDispatcher.h"
#include "../src/engine/HistoryManager.h"

namespace
{
    juce::var params (std::initializer_list<std::pair<juce::String, juce::var>> pairs)
    {
        auto o = new juce::DynamicObject();
        for (auto& [key, value] : pairs)
            o->setProperty (juce::Identifier (key), value);
        return juce::var (o);
    }

    ExpressionMap makeMap (const juce::String& name)
    {
        ExpressionMap map;
        map.name = name;
        ExpressionMap::Articulation staccato;
        staccato.name = "Staccato";
        staccato.outputs.push_back ({ ExpressionMap::Output::Type::controller, 32, 10, false, -1 });
        map.groups.push_back ({ "Articulation", "", { staccato } });
        return map;
    }

    template <typename Condition>
    void pumpUntil (Condition&& isDone, int timeoutMs = 15000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! isDone() && juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    }
}

// Expression maps and the map each instrument channel uses are part of the
// project history: edits become entries and time travel restores them.
class ExpressionMapHistoryTests final : public juce::UnitTest
{
public:
    ExpressionMapHistoryTests() : UnitTest ("Expression maps in the history") {}

    void runTest() override
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);

        AudioEngine engine (settings);
        CommandDispatcher api (engine);
        HistoryManager history (engine);
        api.setHistoryManager (&history);
        engine.eventSink = [&history] (const juce::var& event) { history.onEngineEvent (event); };

        const auto pump = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); };
        const auto entries = [&api] { return *api.run ("history.list")["result"].getArray(); };

        beginTest ("map edits are entries, and time travel restores the maps");

        api.run ("expressionmap.set", params ({ { "map", makeMap ("Strings").toVar() } }));
        pump();
        api.run ("expressionmap.set", params ({ { "map", makeMap ("Brass").toVar() } }));
        pump();
        api.run ("expressionmap.rename", params ({ { "name", "Brass" }, { "newName", "Horns" } }));
        pump();
        api.run ("expressionmap.remove", params ({ { "name", "Strings" } }));
        pump();

        auto list = entries();
        expectEquals (list.size(), 5);   // Start + 4 edits
        expectEquals (list[1]["category"].toString(), juce::String ("expressionmap"));
        expect (list[1]["description"].toString().contains ("Expression map 'Strings'"), list[1]["description"].toString());
        expect (list[3]["description"].toString().contains ("Rename expression map 'Brass' to 'Horns'"), list[3]["description"].toString());
        expect (list[4]["description"].toString().contains ("Remove expression map 'Strings'"), list[4]["description"].toString());

        expectEquals ((int) engine.getExpressionMaps().size(), 1);   // Horns

        // Back to after the second map was added: Strings and Brass
        api.run ("history.travel", params ({ { "id", list[2]["id"] } }));
        pump();
        expect (engine.getExpressionMap ("Strings").has_value() && engine.getExpressionMap ("Brass").has_value());
        expect (! engine.getExpressionMap ("Horns").has_value());

        // Back to the start: none; forward to the end: Horns only
        api.run ("history.travel", params ({ { "id", list[0]["id"] } }));
        pump();
        expect (engine.getExpressionMaps().empty());

        api.run ("history.travel", params ({ { "id", list[4]["id"] } }));
        pump();
        expectEquals ((int) engine.getExpressionMaps().size(), 1);
        expect (engine.getExpressionMap ("Horns").has_value());

        runChannelTests (engine, api, history);
    }

private:
    void runChannelTests (AudioEngine& engine, CommandDispatcher& api, HistoryManager&)
    {
        beginTest ("which map a channel uses is history too");

        juce::PluginDescription description;
        bool found = false;

        for (const auto* wanted : { "TAL-NoiseMaker", "Twin 3" })
        {
            for (auto& type : engine.getInstrumentTypes())
                if (type.name == wanted)
                {
                    description = type;
                    found = true;
                    break;
                }

            if (found)
                break;
        }

        if (! found)
        {
            logMessage ("!!! no TAL-NoiseMaker or Twin 3 in the plugin cache - skipping the channel history test");
            return;
        }

        std::atomic<int> instrumentId { -1 };
        engine.addInstrument (description, [&instrumentId] (auto id, const juce::String&) { instrumentId = id; });
        pumpUntil ([&instrumentId] { return instrumentId.load() != -1; });
        expect (instrumentId > 0, "instrument failed to load");

        if (instrumentId <= 0)
            return;

        const auto pump = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); };
        pump();

        const auto before = *api.run ("history.list")["result"].getArray();
        const auto beforeId = before[before.size() - 1]["id"];

        auto reply = api.run ("instrument.setChannelMap", params ({ { "instrumentId", instrumentId.load() }, { "channel", 4 }, { "map", "Horns" } }));
        expect (reply["ok"], reply["error"].toString());
        pump();

        const auto after = *api.run ("history.list")["result"].getArray();
        expectEquals (after.size(), before.size() + 1);
        expect (after[after.size() - 1]["description"].toString().contains ("expression map"),
                after[after.size() - 1]["description"].toString());

        expectEquals (engine.getInstrumentMidiChannels (instrumentId).size(), (size_t) 1);

        // Travel back: the assignment is gone, and so is the channel entry that only held it
        api.run ("history.travel", params ({ { "id", beforeId } }));
        pump();
        expect (engine.getInstrumentMidiChannels (instrumentId).empty());

        // Forward again: it is back
        api.run ("history.travel", params ({ { "id", after[after.size() - 1]["id"] } }));
        pump();
        const auto channels = engine.getInstrumentMidiChannels (instrumentId);
        expectEquals ((int) channels.size(), 1);

        if (! channels.empty())
        {
            expectEquals (channels[0].midiChannel, 4);
            expectEquals (channels[0].expressionMap, juce::String ("Horns"));
        }

        //======================================================================
        beginTest ("renaming inside a map is ONE history entry; travelling back restores the map and the notes");

        const auto trackId = (int) api.run ("track.create")["result"]["id"];
        engine.addTrackOutput (trackId, instrumentId, 4);

        auto articulatedNote = params ({ { "start", 0 }, { "length", 960000 }, { "key", 60 } });
        articulatedNote.getDynamicObject()->setProperty ("articulation", params ({ { "root", "Staccato" } }));
        api.run ("clip.set", params ({ { "trackId", trackId }, { "notes", juce::Array<juce::var> { articulatedNote } } }));
        pump();

        const auto beforeRename = *api.run ("history.list")["result"].getArray();

        reply = api.run ("expressionmap.renameArticulation", params ({ { "name", "Horns" }, { "group", "Articulation" },
                                                                       { "articulation", "Staccato" }, { "newName", "Spiccato" } }));
        expect (reply["ok"], reply["error"].toString());
        expectEquals ((int) reply["result"]["notesChanged"], 1);
        pump();

        const auto afterRename = *api.run ("history.list")["result"].getArray();
        expectEquals (afterRename.size(), beforeRename.size() + 1);   // the map edit and the clip rewrite are one entry
        expect (afterRename[afterRename.size() - 1]["description"].toString().contains ("Rename 'Staccato' to 'Spiccato' in expression map 'Horns'"),
                afterRename[afterRename.size() - 1]["description"].toString());

        expectEquals (engine.getTrackSequence (trackId)->getNotes()[0].articulation.root, juce::String ("Spiccato"));
        expectEquals (engine.getExpressionMap ("Horns")->groups[0].articulations[0].name, juce::String ("Spiccato"));

        api.run ("history.travel", params ({ { "id", beforeRename[beforeRename.size() - 1]["id"] } }));
        pump();
        expectEquals (engine.getTrackSequence (trackId)->getNotes()[0].articulation.root, juce::String ("Staccato"));
        expectEquals (engine.getExpressionMap ("Horns")->groups[0].articulations[0].name, juce::String ("Staccato"));
    }
};

static ExpressionMapHistoryTests expressionMapHistoryTests;
