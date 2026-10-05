#include "../src/AudioEngine.h"

namespace
{
    ExpressionMap makeMap (const juce::String& name)
    {
        ExpressionMap map;
        map.name = name;
        ExpressionMap::Articulation staccato, legato;
        staccato.name = "Staccato";
        staccato.outputs.push_back ({ ExpressionMap::Output::Type::controller, 32, 10, false, -1 });
        legato.name = "Legato";
        legato.timingOffsetMs = -70.0;
        legato.outputs.push_back ({ ExpressionMap::Output::Type::keyswitch, 12, 100, false, -1 });
        legato.outputs.push_back ({ ExpressionMap::Output::Type::controller, 7, 90, false, -1 });
        map.groups.push_back ({ "Articulation", "", { staccato, legato } });
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

// The project's collection of maps works anywhere; assigning a map to an
// instrument channel needs a loaded instrument (TAL-NoiseMaker, else Twin 3 -
// a light one) and skips, without failing, when neither is in the plugin cache.
class ExpressionMapEngineTests final : public juce::UnitTest
{
public:
    ExpressionMapEngineTests() : UnitTest ("Expression maps in the engine") {}

    void runTest() override
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);
        AudioEngine engine (settings);

        beginTest ("the project's maps: add, replace, case-insensitive lookup, refuse invalid");
        {
            expect (engine.getExpressionMaps().empty());
            expectEquals (engine.setExpressionMap (makeMap ("Strings")), juce::String());
            expect (engine.getExpressionMap ("strings").has_value());
            expectEquals ((int) engine.getExpressionMaps().size(), 1);

            // Same name (ignoring case) replaces
            auto changed = makeMap ("STRINGS");
            changed.groups[0].articulations.push_back ({ "Tremolo" });
            expectEquals (engine.setExpressionMap (changed), juce::String());
            expectEquals ((int) engine.getExpressionMaps().size(), 1);
            expectEquals ((int) engine.getExpressionMap ("Strings")->groups[0].articulations.size(), 3);

            // An invalid map is refused with the reason
            auto bad = makeMap ("Broken");
            bad.groups[0].articulations[0].outputs[0].number = 999;
            const auto error = engine.setExpressionMap (bad);
            expect (error.contains ("not valid") && error.contains ("CC number 999"), error);
            expect (! engine.getExpressionMap ("Broken").has_value());
        }

        beginTest ("remove and rename name what exists when they fail");
        {
            engine.setExpressionMap (makeMap ("Brass"));
            expect (engine.removeExpressionMap ("nothing").contains ("existing: Strings, Brass")
                      || engine.removeExpressionMap ("nothing").contains ("existing: STRINGS, Brass"));
            expect (engine.renameExpressionMap ("Brass", "strings").contains ("already an expression map"));
            expect (engine.renameExpressionMap ("Nope", "X").contains ("no expression map 'Nope'"));
            expect (engine.renameExpressionMap ("Brass", " ").contains ("needs a name"));

            expectEquals (engine.renameExpressionMap ("Brass", "Horns"), juce::String());
            expect (engine.getExpressionMap ("Horns").has_value() && ! engine.getExpressionMap ("Brass").has_value());

            // Only the case changes: allowed, it is the same map
            expectEquals (engine.renameExpressionMap ("Horns", "HORNS"), juce::String());
            expectEquals (engine.getExpressionMap ("horns")->name, juce::String ("HORNS"));

            expectEquals (engine.removeExpressionMap ("horns"), juce::String());
            expect (! engine.getExpressionMap ("Horns").has_value());
        }

        beginTest ("maps are saved with the project and restored by a load");
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("OrchestralDAWMapsRoundTrip.odaw");
            expect (engine.saveProject (file), "save failed");

            engine.clearProject();
            expect (engine.getExpressionMaps().empty());

            std::atomic<int> loadDone { 0 };
            engine.loadProject (file, [&loadDone] (bool ok, const juce::String&) { loadDone = ok ? 1 : -1; });
            pumpUntil ([&loadDone] { return loadDone.load() != 0; });
            expect (loadDone == 1, "load failed");

            const auto restored = engine.getExpressionMap ("Strings");
            expect (restored.has_value());

            if (restored.has_value())
            {
                expectEquals ((int) restored->groups[0].articulations.size(), 3);
                expectEquals (restored->groups[0].articulations[1].timingOffsetMs, -70.0);
                expectEquals ((int) restored->groups[0].articulations[1].outputs.size(), 2);
            }

            file.deleteFile();
        }

        runChannelTests (engine);
    }

private:
    void runChannelTests (AudioEngine& engine)
    {
        beginTest ("instrument channels: assigning a map");

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
            logMessage ("!!! no TAL-NoiseMaker or Twin 3 in the plugin cache - skipping the channel tests");
            return;
        }

        engine.clearProject();
        engine.setExpressionMap (makeMap ("Strings"));
        engine.setExpressionMap (makeMap ("Brass"));

        std::atomic<int> instrumentId { -1 };
        engine.addInstrument (description, [&instrumentId] (auto id, const juce::String&) { instrumentId = id; });
        pumpUntil ([&instrumentId] { return instrumentId.load() != -1; });
        expect (instrumentId > 0, "instrument failed to load");

        if (instrumentId <= 0)
            return;

        // Helpful failures
        expect (engine.setInstrumentChannelMap (9999, 1, 1, "Strings").contains ("no instrument with id 9999"));
        const auto missing = engine.setInstrumentChannelMap (instrumentId, 1, 3, "Woodwinds");
        expect (missing.contains ("no expression map 'Woodwinds'") && missing.contains ("Strings"), missing);

        // A manual channel with no name is created for the assignment, and cleared again with none
        expectEquals (engine.setInstrumentChannelMap (instrumentId, 1, 3, "strings"), juce::String());
        auto channels = engine.getInstrumentMidiChannels (instrumentId);
        expectEquals ((int) channels.size(), 1);
        expectEquals (channels[0].expressionMap, juce::String ("Strings"));   // stored with the map's own spelling
        expectEquals (channels[0].name, juce::String());

        // Naming and un-naming the channel doesn't lose the map
        engine.setInstrumentChannelName (instrumentId, 3, "Violins");
        engine.setInstrumentChannelName (instrumentId, 3, "");
        channels = engine.getInstrumentMidiChannels (instrumentId);
        expectEquals ((int) channels.size(), 1);
        expectEquals (channels[0].expressionMap, juce::String ("Strings"));

        // A track on that channel sees the map
        const auto track = engine.addTrack();
        engine.addTrackOutput (track, instrumentId, 3);
        const auto trackMap = engine.getTrackExpressionMap (track);
        expect (trackMap.has_value() && trackMap->name == "Strings");

        // Renaming the map follows on the channel; removing it leaves the name behind (a missing map)
        expectEquals (engine.renameExpressionMap ("Strings", "Violins map"), juce::String());
        expectEquals (engine.getInstrumentMidiChannels (instrumentId)[0].expressionMap, juce::String ("Violins map"));
        expect (engine.getTrackExpressionMap (track).has_value());
        engine.removeExpressionMap ("Violins map");
        expectEquals (engine.getInstrumentMidiChannels (instrumentId)[0].expressionMap, juce::String ("Violins map"));
        expect (! engine.getTrackExpressionMap (track).has_value());

        // Back to Strings, then saved and loaded with the project
        engine.setExpressionMap (makeMap ("Strings"));
        expectEquals (engine.setInstrumentChannelMap (instrumentId, 1, 3, "Strings"), juce::String());

        // Synced channels: sync doesn't own the map, it survives a re-sync of the same player
        AudioEngine::MidiChannelInfo synced;
        synced.midiPort = 1;
        synced.midiChannel = 5;
        synced.name = "1st Violins";
        synced.veproInstanceId = "{instance}";
        synced.veproChannelAddress = "7";
        synced.veproPluginId = "Vienna Synchron Player";
        engine.setSyncedInstrumentChannels (instrumentId, { synced });
        expectEquals (engine.setInstrumentChannelMap (instrumentId, 1, 5, "Brass"), juce::String());   // allowed on synced channels
        engine.setSyncedInstrumentChannels (instrumentId, { synced });
        bool carried = false;

        for (auto& c : engine.getInstrumentMidiChannels (instrumentId))
            if (c.midiChannel == 5)
                carried = (c.expressionMap == "Brass");

        expect (carried, "a re-sync of the same player lost the map");

        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("OrchestralDAWMapChannelRoundTrip.odaw");
        expect (engine.saveProject (file), "save failed");
        engine.clearProject();

        std::atomic<int> loadDone { 0 };
        engine.loadProject (file, [&loadDone] (bool ok, const juce::String&) { loadDone = ok ? 1 : -1; });
        pumpUntil ([&loadDone] { return loadDone.load() != 0; });
        expect (loadDone == 1, "load failed");

        const auto outputs = engine.getTrackOutputs (engine.getTrackIds().front());
        const auto newInstrument = outputs.front().instrument;
        const auto reloaded = engine.getTrackExpressionMap (engine.getTrackIds().front());
        expect (reloaded.has_value() && reloaded->name == "Strings");

        bool syncedKept = false;

        for (auto& c : engine.getInstrumentMidiChannels (newInstrument))
            if (c.midiChannel == 5)
                syncedKept = (c.expressionMap == "Brass" && c.synced);

        expect (syncedKept, "the synced channel's map was not saved and loaded");
        file.deleteFile();
    }
};

static ExpressionMapEngineTests expressionMapEngineTests;
