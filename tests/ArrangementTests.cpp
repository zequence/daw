#include "../src/api/CommandDispatcher.h"
#include "../src/model/PhraseBlocks.h"

namespace
{
    constexpr auto Q = Ticks::perQuarterNote;

    MidiSequence::Ptr sequenceOf (std::vector<MidiSequence::Note> notes,
                                  std::vector<MidiSequence::Control> controls = {})
    {
        return MidiSequence::create (std::move (notes), std::move (controls));
    }
}

//==============================================================================
class PhraseBlockTests final : public juce::UnitTest
{
public:
    PhraseBlockTests() : UnitTest ("Phrase blocks") {}

    void runTest() override
    {
        const auto map = TempoMap::create();   // 4/4: a bar = 4Q

        beginTest ("contiguous notes form one block");
        {
            const auto blocks = computePhraseBlocks (
                *sequenceOf ({ { 0, Q, 1, 60, 100 }, { Q, Q, 1, 62, 100 }, { 2 * Q, 2 * Q, 1, 64, 100 } }), *map);

            expectEquals ((int) blocks.size(), 1);
            expectEquals (blocks[0].startTick, (juce::int64) 0);
            expectEquals (blocks[0].endTick, 4 * Q);
            expectEquals (blocks[0].noteCount, 3);
        }

        beginTest ("two bars of silence split blocks; the next one starts at its bar");
        {
            const auto blocks = computePhraseBlocks (
                *sequenceOf ({ { 0, Q, 1, 60, 100 },            // ends at Q
                               { Q + 8 * Q, Q, 1, 62, 100 } }), // exactly two bars later
                *map);

            expectEquals ((int) blocks.size(), 2);
            expectEquals (blocks[1].startTick, 8 * Q);   // the bar of the note at 9Q
        }

        beginTest ("silence just under two bars does not split");
        {
            const auto blocks = computePhraseBlocks (
                *sequenceOf ({ { 0, Q, 1, 60, 100 }, { Q + 8 * Q - 1, Q, 1, 62, 100 } }), *map);

            expectEquals ((int) blocks.size(), 1);
        }

        beginTest ("a block starts at the beginning of its first note's bar");
        {
            const auto blocks = computePhraseBlocks (*sequenceOf ({ { 6 * Q + 1, Q, 1, 60, 100 } }), *map);
            expectEquals (blocks[0].startTick, 4 * Q);
        }

        beginTest ("overlapping long notes extend the block end");
        {
            const auto blocks = computePhraseBlocks (
                *sequenceOf ({ { 0, 8 * Q, 1, 60, 100 }, { Q, Q, 1, 62, 100 }, { 9 * Q, Q, 1, 64, 100 } }), *map);

            // The 8Q pedal note bridges what would otherwise be a gap
            expectEquals ((int) blocks.size(), 1);
            expectEquals (blocks[0].endTick, 10 * Q);
        }

        beginTest ("controller-only streams get one block");
        {
            const auto blocks = computePhraseBlocks (
                *sequenceOf ({}, { { Q, MidiSequence::ControlType::controller, 1, 1, 60 },
                                   { 8 * Q, MidiSequence::ControlType::controller, 1, 1, 90 } }), *map);

            expectEquals ((int) blocks.size(), 1);
            expectEquals (blocks[0].startTick, (juce::int64) 0);   // the bar of the first controller
            expectEquals (blocks[0].noteCount, 0);
        }

        beginTest ("empty sequence yields no blocks");
        {
            expect (computePhraseBlocks (*sequenceOf ({}), *map).empty());
        }
    }
};

//==============================================================================
class MarkerTests final : public juce::UnitTest
{
public:
    MarkerTests() : UnitTest ("Markers") {}

    void runTest() override
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);

        AudioEngine engine (settings);
        CommandDispatcher api (engine);

        auto params = [] (std::initializer_list<std::pair<juce::String, juce::var>> pairs)
        {
            auto o = new juce::DynamicObject();
            for (auto& [key, value] : pairs)
                o->setProperty (juce::Identifier (key), value);
            return juce::var (o);
        };

        beginTest ("markers add sorted, rename on same tick, remove");
        {
            expect (api.run ("marker.add", params ({ { "bar", 9 }, { "name", "chorus" } }))["ok"]);
            expect (api.run ("marker.add", params ({ { "bar", 1 }, { "name", "intro" } }))["ok"]);
            expect (api.run ("marker.add", params ({ { "bar", 5 }, { "name", "verse" } }))["ok"]);

            auto list = api.run ("marker.list")["result"];
            expectEquals ((int) list.getArray()->size(), 3);
            expectEquals (list[0]["name"].toString(), juce::String ("intro"));
            expectEquals ((int) list[1]["bar"], 5);
            expectEquals (list[2]["name"].toString(), juce::String ("chorus"));

            // Same tick renames rather than duplicating
            expect (api.run ("marker.add", params ({ { "bar", 5 }, { "name", "verse 1" } }))["ok"]);
            list = api.run ("marker.list")["result"];
            expectEquals ((int) list.getArray()->size(), 3);
            expectEquals (list[1]["name"].toString(), juce::String ("verse 1"));

            expect (api.run ("marker.remove", params ({ { "tick", list[1]["tick"] } }))["ok"]);
            expectEquals ((int) api.run ("marker.list")["result"].getArray()->size(), 2);
        }

        beginTest ("markers survive a project round trip");
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("OrchestralDAWMarkers.odaw");
            expect (engine.saveProject (file));

            engine.clearProject();
            expect (engine.getMarkers().empty());

            std::atomic<int> done { 0 };
            engine.loadProject (file, [&done] (bool ok, const juce::String&) { done = ok ? 1 : -1; });

            const auto deadline = juce::Time::getMillisecondCounter() + 5000;
            while (done == 0 && juce::Time::getMillisecondCounter() < deadline)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

            expect (done == 1);
            expectEquals ((int) engine.getMarkers().size(), 2);
            expectEquals (engine.getMarkers()[0].name, juce::String ("intro"));
            file.deleteFile();
        }
    }
};

static PhraseBlockTests phraseBlockTests;
static MarkerTests markerTests;
