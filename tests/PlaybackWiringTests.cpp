#include "../src/api/CommandDispatcher.h"

namespace
{
    constexpr auto Q = Ticks::perQuarterNote;
    constexpr juce::int64 seventyMs = 134400;   // ticks at 120 bpm

    juce::var params (std::initializer_list<std::pair<juce::String, juce::var>> pairs)
    {
        auto o = new juce::DynamicObject();
        for (auto& [key, value] : pairs)
            o->setProperty (juce::Identifier (key), value);
        return juce::var (o);
    }

    ExpressionMap makeMap (const juce::String& name, double legatoOffsetMs)
    {
        using Out = ExpressionMap::Output;
        ExpressionMap map;
        map.name = name;

        ExpressionMap::Articulation staccato, legato;
        staccato.name = "Staccato";
        staccato.outputs.push_back ({ Out::Type::keyswitch, 24, 100, false, -1 });
        legato.name = "Legato";
        legato.timingOffsetMs = legatoOffsetMs;
        legato.outputs.push_back ({ Out::Type::keyswitch, 25, 90, false, -1 });
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

    const MidiSequence::Note* findNote (const MidiSequence::Ptr& seq, int key)
    {
        if (seq != nullptr)
            for (auto& n : seq->getNotes())
                if (n.key == key)
                    return &n;

        return nullptr;
    }
}

// The engine plays a GENERATED sequence per track (MILESTONES.md "Playback"): shifted notes and
// switch events from the track's expression map, rebuilt whenever something it depends on changes,
// with the transport's pre-roll following. Needs a loaded instrument (TAL-NoiseMaker, else Twin 3).
class PlaybackWiringTests final : public juce::UnitTest
{
public:
    PlaybackWiringTests() : UnitTest ("Playback wiring") {}

    void runTest() override
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);
        settings.removeValue ("editorFirstRootIsDefault");
        AudioEngine engine (settings);
        CommandDispatcher api (engine);

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
            logMessage ("!!! no TAL-NoiseMaker or Twin 3 in the plugin cache - skipping the playback wiring tests");
            return;
        }

        std::atomic<int> instrumentId { -1 };
        engine.addInstrument (description, [&instrumentId] (auto id, const juce::String&) { instrumentId = id; });
        pumpUntil ([&instrumentId] { return instrumentId.load() != -1; });
        expect (instrumentId > 0, "instrument failed to load");

        if (instrumentId <= 0)
            return;

        engine.setExpressionMap (makeMap ("Strings", -70.0));
        engine.setInstrumentChannelMap (instrumentId, 1, 1, "Strings");

        const auto trackId = (int) api.run ("track.create")["result"]["id"];
        const auto tid = juce::var (trackId);

        // A legato note at bar 2 and a staccato note at bar 3
        api.run ("clip.set", params ({ { "trackId", tid }, { "notes", juce::Array<juce::var> {
            params ({ { "start", 4 * Q }, { "length", Q }, { "key", 64 }, { "articulation", params ({ { "root", "Legato" } }) } }),
            params ({ { "start", 8 * Q }, { "length", Q }, { "key", 65 }, { "articulation", params ({ { "root", "Staccato" } }) } }) } } }));

        beginTest ("a track whose channel has no map plays its written sequence, with no pre-roll");
        {
            engine.addTrackOutput (trackId, instrumentId, 2);   // channel 2 has no map
            expect (engine.getTrackPlaybackSequence (trackId) == engine.getTrackSequence (trackId));
            expectEquals (engine.getTransport().getPreRollMs(), 0.0);
        }

        beginTest ("on a channel with a map it plays the generated sequence, and the pre-roll follows");
        {
            engine.clearTrackOutputs (trackId);
            engine.addTrackOutput (trackId, instrumentId, 1);

            const auto playback = engine.getTrackPlaybackSequence (trackId);
            expect (playback != engine.getTrackSequence (trackId));

            const auto* legato = findNote (playback, 64);
            expect (legato != nullptr);
            expectEquals (legato->startTick, 4 * Q - seventyMs);   // 70 ms early
            expectEquals (legato->written(), 4 * Q);
            expect (findNote (playback, 25) != nullptr && findNote (playback, 25)->startTick == 4 * Q - seventyMs, "its keyswitch");
            expect (findNote (playback, 24) != nullptr, "and the staccato's");

            expectEquals (engine.getTransport().getPreRollMs(), 70.0);

            // The written sequence is untouched: that is what the editor shows and the project saves
            expectEquals (findNote (engine.getTrackSequence (trackId), 64)->startTick, 4 * Q);
        }

        beginTest ("editing the map regenerates the playback sequence and the pre-roll");
        {
            engine.setExpressionMap (makeMap ("Strings", -120.0));
            expectEquals (engine.getTransport().getPreRollMs(), 120.0);
            expectEquals (findNote (engine.getTrackPlaybackSequence (trackId), 64)->startTick, 4 * Q - 230400);

            engine.setExpressionMap (makeMap ("Strings", 0.0));   // no offset: no pre-roll
            expectEquals (engine.getTransport().getPreRollMs(), 0.0);
            expectEquals (findNote (engine.getTrackPlaybackSequence (trackId), 64)->startTick, 4 * Q);
            expect (findNote (engine.getTrackPlaybackSequence (trackId), 25) != nullptr, "the switch is still there");

            engine.setExpressionMap (makeMap ("Strings", -70.0));
        }

        beginTest ("clip edits regenerate: a new note, undo");
        {
            api.run ("clip.addNotes", params ({ { "trackId", tid }, { "notes", juce::Array<juce::var> {
                params ({ { "start", 12 * Q }, { "length", Q }, { "key", 67 }, { "articulation", params ({ { "root", "Legato" } }) } }) } } }));

            const auto* added = findNote (engine.getTrackPlaybackSequence (trackId), 67);
            expect (added != nullptr && added->startTick == 12 * Q - seventyMs);

            api.run ("clip.undo", params ({ { "trackId", tid } }));
            expect (findNote (engine.getTrackPlaybackSequence (trackId), 67) == nullptr, "undo removes it from playback too");
        }

        beginTest ("the tempo changes what 70 ms is in ticks");
        {
            engine.setTempoBpm (60.0);   // a quarter note is a second: 70 ms = 67200 ticks
            expectEquals (findNote (engine.getTrackPlaybackSequence (trackId), 64)->startTick, 4 * Q - 67200);
            engine.setTempoBpm (120.0);
            expectEquals (findNote (engine.getTrackPlaybackSequence (trackId), 64)->startTick, 4 * Q - seventyMs);
        }

        beginTest ("removing the map, or the channel's assignment, goes back to the written sequence");
        {
            engine.setInstrumentChannelMap (instrumentId, 1, 1, "");
            expect (engine.getTrackPlaybackSequence (trackId) == engine.getTrackSequence (trackId));
            expectEquals (engine.getTransport().getPreRollMs(), 0.0);

            engine.setInstrumentChannelMap (instrumentId, 1, 1, "Strings");
            expect (engine.getTrackPlaybackSequence (trackId) != engine.getTrackSequence (trackId));
            expectEquals (engine.getTransport().getPreRollMs(), 70.0);

            engine.removeExpressionMap ("Strings");
            expect (engine.getTrackPlaybackSequence (trackId) == engine.getTrackSequence (trackId), "a missing map plays as none");

            engine.setExpressionMap (makeMap ("Strings", -70.0));
            expectEquals (engine.getTransport().getPreRollMs(), 70.0);
        }

        beginTest ("the 'first root as default' setting reaches playback (notes with none)");
        {
            api.run ("clip.set", params ({ { "trackId", tid }, { "notes", juce::Array<juce::var> {
                params ({ { "start", 0 }, { "length", Q }, { "key", 60 } }) } } }));
            expect (findNote (engine.getTrackPlaybackSequence (trackId), 24) == nullptr, "off: nothing is sent for a note with none");

            settings.setValue ("editorFirstRootIsDefault", true);
            engine.refreshAllPlayback();
            expect (findNote (engine.getTrackPlaybackSequence (trackId), 24) != nullptr, "on: it plays as the first root");

            settings.removeValue ("editorFirstRootIsDefault");
            engine.refreshAllPlayback();
            expect (findNote (engine.getTrackPlaybackSequence (trackId), 24) == nullptr);
        }

        beginTest ("a saved project comes back with its playback sequence and pre-roll");
        {
            api.run ("clip.set", params ({ { "trackId", tid }, { "notes", juce::Array<juce::var> {
                params ({ { "start", 4 * Q }, { "length", Q }, { "key", 64 }, { "articulation", params ({ { "root", "Legato" } }) } }) } } }));

            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("OrchestralDAWPlaybackRoundTrip.odaw");
            expect (engine.saveProject (file), "save failed");
            engine.clearProject();
            expectEquals (engine.getTransport().getPreRollMs(), 0.0);

            std::atomic<int> loadDone { 0 };
            engine.loadProject (file, [&loadDone] (bool ok, const juce::String&) { loadDone = ok ? 1 : -1; });
            pumpUntil ([&loadDone] { return loadDone.load() != 0; });
            expect (loadDone == 1, "load failed");

            const auto ids = engine.getTrackIds();
            expectEquals ((int) ids.size(), 1);

            if (! ids.empty())
            {
                const auto* legato = findNote (engine.getTrackPlaybackSequence (ids.front()), 64);
                expect (legato != nullptr && legato->startTick == 4 * Q - seventyMs);
                expectEquals (engine.getTransport().getPreRollMs(), 70.0);
            }

            file.deleteFile();
        }
    }
};

static PlaybackWiringTests playbackWiringTests;
