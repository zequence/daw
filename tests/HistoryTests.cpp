#include "../src/api/CommandDispatcher.h"
#include "../src/engine/HistoryManager.h"

// Global history: actions become entries, time-travel restores state, a new edit
// cuts the future, project.new resets the timeline.
class HistoryTests final : public juce::UnitTest
{
public:
    HistoryTests() : UnitTest ("History") {}

    void runTest() override
    {
        constexpr auto Q = Ticks::perQuarterNote;

        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);

        AudioEngine engine (settings);
        CommandDispatcher api (engine);
        HistoryManager history (engine);
        api.setHistoryManager (&history);
        engine.eventSink = [&history] (const juce::var& event) { history.onEngineEvent (event); };

        auto params = [] (std::initializer_list<std::pair<juce::String, juce::var>> pairs)
        {
            auto o = new juce::DynamicObject();
            for (auto& [key, value] : pairs)
                o->setProperty (juce::Identifier (key), value);
            return juce::var (o);
        };

        auto pump = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); };

        auto listEntries = [&api] (const juce::var& filter = {})
        {
            return *api.run ("history.list", filter)["result"].getArray();
        };

        //======================================================================
        beginTest ("actions become coalesced entries");

        const auto trackId = (int) api.run ("track.create", params ({ { "name", "Hist" } }))["result"]["id"];
        pump();
        api.run ("clip.addNotes", params ({ { "trackId", trackId },
                                            { "notes", juce::Array<juce::var> {
                                                  params ({ { "start", 0 }, { "length", Q }, { "key", 60 } }) } } }));
        pump();
        api.run ("tempo.set", params ({ { "bpm", 101.0 } }));
        pump();
        api.run ("marker.add", params ({ { "bar", 3 }, { "name", "verse" } }));
        pump();

        auto entries = listEntries();
        expectEquals (entries.size(), 5);   // Start + 4 actions (create burst = ONE entry)
        expect (entries[1]["description"].toString().contains ("Hist"));
        expectEquals (entries[1]["category"].toString(), juce::String ("track"));
        expectEquals (entries[2]["category"].toString(), juce::String ("clip"));
        expectEquals (entries[3]["category"].toString(), juce::String ("tempo"));
        expectEquals (entries[4]["category"].toString(), juce::String ("marker"));
        expect ((bool) entries[4]["current"]);

        const auto afterCreateId = (int) entries[1]["id"];
        const auto afterMarkerId = (int) entries[4]["id"];

        //======================================================================
        beginTest ("category and track filters");

        expectEquals (listEntries (params ({ { "category", "clip" } })).size(), 1);
        expectEquals (listEntries (params ({ { "trackId", trackId } })).size(), 2);   // create + clip edit

        //======================================================================
        beginTest ("time-travel restores earlier state");

        expect ((bool) api.run ("history.travel", params ({ { "id", afterCreateId } }))["ok"]);
        expect (engine.getTrackSequence (trackId) == nullptr, "clip should be gone");
        expectWithinAbsoluteError (engine.getTempoBpm(), 120.0, 0.01);
        expect (engine.getMarkers().empty(), "marker should be gone");
        expectEquals ((int) engine.getTrackIds().size(), 1);

        // ...and forward again
        expect ((bool) api.run ("history.travel", params ({ { "id", afterMarkerId } }))["ok"]);
        expect (engine.getTrackSequence (trackId) != nullptr, "clip should be back");
        expectWithinAbsoluteError (engine.getTempoBpm(), 101.0, 0.01);
        expectEquals ((int) engine.getMarkers().size(), 1);

        //======================================================================
        beginTest ("a new edit after travelling back cuts the future");

        expect ((bool) api.run ("history.travel", params ({ { "id", afterCreateId } }))["ok"]);
        api.run ("track.rename", params ({ { "trackId", trackId }, { "name", "Branched" } }));
        pump();

        entries = listEntries();
        expectEquals (entries.size(), 3);   // Start, create, rename - tempo/marker/clip future gone
        expect (entries[2]["description"].toString().contains ("name"));
        expect ((bool) entries[2]["current"]);
        expectWithinAbsoluteError (engine.getTempoBpm(), 120.0, 0.01);

        //======================================================================
        beginTest ("travelling across a removed track recreates it");

        api.run ("track.remove", params ({ { "trackId", trackId } }));
        pump();
        expect (engine.getTrackIds().empty());

        entries = listEntries();
        expect ((bool) api.run ("history.travel", params ({ { "id", (int) entries[entries.size() - 2]["id"] } }))["ok"]);
        expectEquals ((int) engine.getTrackIds().size(), 1);
        expectEquals (engine.getTrackName (trackId), juce::String ("Branched"));

        //======================================================================
        beginTest ("project.new resets the timeline");

        api.run ("project.new");
        pump();

        entries = listEntries();
        expectEquals (entries.size(), 1);
        expectEquals (entries[0]["description"].toString(), juce::String ("Start"));

        engine.eventSink = nullptr;
    }
};

static HistoryTests historyTests;
