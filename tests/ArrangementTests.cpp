#include "../src/api/CommandDispatcher.h"
#include "../src/model/PhraseBlocks.h"
#include "../src/model/PlayheadSteps.h"
#include "../src/ui/TimeAxis.h"

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

        beginTest ("Left/Right playhead stops: note ends ahead, note starts behind, else a grid step");
        {
            // Parallel notes at 0 (Q long and 2Q long), one at 4Q
            const auto seq = sequenceOf ({ { 0, Q, 1, 60, 100 }, { 0, 2 * Q, 1, 64, 100 }, { 4 * Q, Q, 1, 62, 100 } });
            const auto grid = Q / 4;

            expectEquals (nextPlayheadStop (0, true, seq.get(), grid), Q);              // the closer end
            expectEquals (nextPlayheadStop (Q, true, seq.get(), grid), 2 * Q);
            expectEquals (nextPlayheadStop (2 * Q, true, seq.get(), grid), 5 * Q);      // the next note's end
            expectEquals (nextPlayheadStop (5 * Q, true, seq.get(), grid), 5 * Q + grid); // nothing ahead
            expectEquals (nextPlayheadStop (5 * Q, false, seq.get(), grid), 4 * Q);     // a start behind
            expectEquals (nextPlayheadStop (4 * Q, false, seq.get(), grid), (juce::int64) 0);
            expectEquals (nextPlayheadStop (0, false, seq.get(), grid), (juce::int64) 0); // clamps at the start
            expectEquals (nextPlayheadStop (Q, false, nullptr, grid), Q - grid);
            expectEquals (nextPlayheadStop (Q + 5, true, nullptr, grid), Q + grid);   // off the grid: to the next line
            expectEquals (nextPlayheadStop (Q + 5, false, nullptr, grid), Q);
        }

        beginTest ("notes of different regions never share a block; overlapping blocks stack");
        {
            auto withRegion = [] (MidiSequence::Note note, int region) { note.region = region; return note; };

            // Touching: bar 1 (region 0) and bar 2 (region 1)
            const auto touching = sequenceOf ({ { 0, 4 * Q, 1, 60, 100 }, withRegion ({ 4 * Q, 4 * Q, 1, 62, 100 }, 1) });
            const auto blocks = computePhraseBlocks (*touching, *map);
            expectEquals ((int) blocks.size(), 2);
            expectEquals (blocks[0].layers, 1);   // touching is not overlapping

            // Overlapping: region 1 starts inside region 0
            const auto overlapping = sequenceOf ({ { 0, 8 * Q, 1, 60, 100 }, withRegion ({ 4 * Q, 8 * Q, 1, 62, 100 }, 1) });
            const auto stacked = computePhraseBlocks (*overlapping, *map);
            expectEquals ((int) stacked.size(), 2);
            expectEquals (stacked[0].layers, 2);
            expect (stacked[0].layer != stacked[1].layer);

            // Region ids survive the XML round trip (project files)
            const auto loaded = MidiSequence::fromXml (*overlapping->toXml());
            expectEquals (loaded->getNotes()[1].region, 1);

            // A note ADDED inside a region's span joins it; existing notes keep theirs
            std::vector<MidiSequence::Note> added { { 5 * Q, Q, 1, 64, 100 } };
            MidiSequence::joinRegions (added, overlapping->getNotes());
            expectEquals (added[0].region, 1);
        }

        beginTest ("overlapped notes across regions: the earlier stops, the later lasts as long");
        {
            auto withRegion = [] (MidiSequence::Note note, int region) { note.region = region; return note; };

            // C4 from 0 to 4Q (region 0); C4 again at 1Q for 1Q (region 1): cut at 1Q, the later lasts to 4Q
            const auto seq = sequenceOf ({ { 0, 4 * Q, 1, 60, 100 }, withRegion ({ Q, Q, 1, 60, 90 }, 1),
                                           { 0, 4 * Q, 1, 64, 100 } });   // another key: untouched
            const auto cut = MidiSequence::withRegionOverlapsCut (seq);
            const auto& notes = cut->getNotes();

            for (auto& n : notes)
            {
                if (n.key == 60 && n.region == 0) expectEquals (n.lengthTicks, Q);
                if (n.key == 60 && n.region == 1) { expectEquals (n.startTick, Q); expectEquals (n.lengthTicks, 3 * Q); }
                if (n.key == 64)                  expectEquals (n.lengthTicks, 4 * Q);
            }

            // Within one region nothing changes (the same sequence comes back)
            const auto single = sequenceOf ({ { 0, 4 * Q, 1, 60, 100 }, { Q, Q, 1, 60, 90 } });
            expect (MidiSequence::withRegionOverlapsCut (single) == single);
        }

        beginTest ("overlapped notes: nested, same start and growing never leave two notes sounding on one key");
        {
            auto withRegion = [] (MidiSequence::Note note, int region) { note.region = region; return note; };

            // Same start in two regions: one note left
            auto same = MidiSequence::withRegionOverlapsCut (sequenceOf ({ { 0, 2 * Q, 1, 60, 100 }, withRegion ({ 0, Q, 1, 60, 90 }, 1) }));
            expectEquals ((int) same->getNotes().size(), 1);
            expectEquals (same->getNotes()[0].lengthTicks, 2 * Q);

            // Growing stops at the next note on the key (here one of its own region)
            auto grow = MidiSequence::withRegionOverlapsCut (sequenceOf ({ { 0, 8 * Q, 1, 60, 100 },
                                                                          withRegion ({ Q, Q, 1, 60, 90 }, 1),
                                                                          withRegion ({ 3 * Q, Q, 1, 60, 90 }, 1) }));
            for (auto& n : grow->getNotes())
                if (n.region == 1 && n.startTick == Q)
                    expectEquals (n.lengthTicks, 2 * Q);

            // Randomized: across regions, no two notes on one key ever sound together
            juce::Random random (1234);

            for (int round = 0; round < 300; ++round)
            {
                std::vector<MidiSequence::Note> notes;

                for (int k = 0; k < 24; ++k)
                    notes.push_back (withRegion ({ random.nextInt (16) * Q / 2, (1 + random.nextInt (8)) * Q / 2, 1,
                                                   60 + random.nextInt (2), 100 }, random.nextInt (3)));

                const auto cut = MidiSequence::withRegionOverlapsCut (sequenceOf (notes));
                const auto& out = cut->getNotes();
                bool parallel = false;

                for (size_t i = 0; i < out.size(); ++i)
                    for (size_t j = i + 1; j < out.size(); ++j)
                        if (out[i].key == out[j].key && out[i].region != out[j].region
                             && out[j].startTick < out[i].startTick + out[i].lengthTicks
                             && out[i].startTick < out[j].startTick + out[j].lengthTicks)
                            parallel = true;

                expect (! parallel, "round " + juce::String (round) + ": two regions' notes sound together on one key");

                if (parallel)
                    break;
            }
        }

        beginTest ("the grid (and snap) follows the zoom: bars, then halves, quarters...");
        {
            TimeAxis axis;

            axis.ticksPerPixel = 4 * Q / 10.0;   // a bar is 10 px: bars only
            expectEquals (axis.gridStep (*map, 0), 4 * Q);

            axis.ticksPerPixel = Q / 15.0;       // a quarter is 15 px: quarters
            expectEquals (axis.gridStep (*map, 0), Q);

            axis.ticksPerPixel = Q / 60.0;       // a sixteenth is 15 px
            expectEquals (axis.gridStep (*map, 0), Q / 4);

            // Snapping: to the nearest line counted from the bar; off = untouched
            expectEquals (axis.snapToGrid (*map, Q / 4 + 10), Q / 4);
            axis.snap = false;
            expectEquals (axis.snapToGrid (*map, Q / 4 + 10), Q / 4 + 10);
            axis.snap = true;

            // In 3/4 a half note doesn't fit the bar evenly: the lines count from each bar,
            // and nothing snaps past the next bar line
            const auto waltz = map->withMeterChange (0, 3, 4);
            axis.ticksPerPixel = 2 * Q / 20.0;   // a half note is 20 px, a quarter 10
            expectEquals (axis.gridStep (*waltz, 0), 2 * Q);
            expectEquals (axis.snapToGrid (*waltz, 2 * Q + Q / 2 + 1), 3 * Q);   // the next bar, not 4Q
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
