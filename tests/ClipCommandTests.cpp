#include "../src/api/CommandDispatcher.h"

namespace
{
    constexpr auto Q = Ticks::perQuarterNote;

    juce::var params (std::initializer_list<std::pair<juce::String, juce::var>> pairs)
    {
        auto o = new juce::DynamicObject();
        for (auto& [key, value] : pairs)
            o->setProperty (juce::Identifier (key), value);
        return juce::var (o);
    }

    juce::var note (juce::int64 start, juce::int64 length, int key, int velocity = 100)
    {
        return params ({ { "start", start }, { "length", length }, { "key", key }, { "velocity", velocity } });
    }
}

class ClipCommandTests final : public juce::UnitTest
{
public:
    ClipCommandTests() : UnitTest ("Clip commands") {}

    void runTest() override
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);

        AudioEngine engine (settings);
        CommandDispatcher api (engine);

        const auto trackId = (int) api.run ("track.create")["result"]["id"];
        const auto tid = juce::var (trackId);

        auto notesOf = [&engine, trackId]() { return engine.getTrackSequence (trackId)->getNotes(); };

        beginTest ("updateNotes moves and transposes by index");
        {
            juce::Array<juce::var> notes { note (0, Q, 60), note (Q, Q, 64), note (2 * Q, Q, 67) };
            expect (api.run ("clip.set", params ({ { "trackId", tid }, { "notes", notes } }))["ok"]);

            // Move the middle note (index 1) later and up a third
            juce::Array<juce::var> edits { params ({ { "index", 1 }, { "start", 3 * Q }, { "key", 69 } }) };
            expect (api.run ("clip.updateNotes", params ({ { "trackId", tid }, { "notes", edits } }))["ok"]);

            expectEquals ((int) notesOf().size(), 3);
            expectEquals (notesOf()[2].startTick, 3 * Q);   // re-sorted to the end
            expectEquals (notesOf()[2].key, 69);

            const auto bad = api.run ("clip.updateNotes",
                                      params ({ { "trackId", tid },
                                                { "notes", juce::Array<juce::var> { params ({ { "index", 99 } }) } } }));
            expect (! bad["ok"]);
        }

        beginTest ("removeNotes removes exactly the given indices");
        {
            // Current notes (sorted): [0]={0,60} [1]={2Q,67} [2]={3Q,69}
            expect (api.run ("clip.removeNotes",
                             params ({ { "trackId", tid }, { "indices", juce::Array<juce::var> { 0, 2 } } }))["ok"]);
            expectEquals ((int) notesOf().size(), 1);
            expectEquals (notesOf()[0].startTick, 2 * Q);
        }

        beginTest ("quantize snaps starts with strength");
        {
            juce::Array<juce::var> notes { note (Q / 10, Q, 60), note (Q + Q / 5, Q, 62) };
            api.run ("clip.set", params ({ { "trackId", tid }, { "notes", notes } }));

            expect (api.run ("clip.quantize", params ({ { "trackId", tid }, { "grid", Q } }))["ok"]);
            expectEquals (notesOf()[0].startTick, (juce::int64) 0);
            expectEquals (notesOf()[1].startTick, Q);

            expect (! api.run ("clip.quantize", params ({ { "trackId", tid }, { "grid", 0 } }))["ok"]);
        }

        beginTest ("eraseRange removes inside and truncates across the boundary");
        {
            juce::Array<juce::var> notes { note (0, 4 * Q, 60),      // crosses the range start -> truncated
                                           note (2 * Q, Q, 62),      // inside -> removed
                                           note (5 * Q, Q, 64) };    // after -> untouched
            api.run ("clip.set", params ({ { "trackId", tid }, { "notes", notes } }));

            expect (api.run ("clip.eraseRange",
                             params ({ { "trackId", tid }, { "start", 2 * Q }, { "end", 4 * Q } }))["ok"]);

            expectEquals ((int) notesOf().size(), 2);
            expectEquals (notesOf()[0].lengthTicks, 2 * Q);          // truncated at the range start
            expectEquals (notesOf()[1].startTick, 5 * Q);
        }

        beginTest ("copyRange repeats an ostinato; moveRange relocates");
        {
            juce::Array<juce::var> notes { note (0, Q / 2, 60), note (Q, Q / 2, 62) };
            api.run ("clip.set", params ({ { "trackId", tid }, { "notes", notes } }));

            // Repeat bar 1 (2 quarters used of a 4Q bar... use the 2Q span) three times from 2Q
            expect (api.run ("clip.copyRange",
                             params ({ { "trackId", tid }, { "start", 0 }, { "end", 2 * Q },
                                       { "destStart", 2 * Q }, { "times", 3 } }))["ok"]);
            expectEquals ((int) notesOf().size(), 8);
            expectEquals (notesOf()[7].startTick, 7 * Q);

            expect (api.run ("clip.moveRange",
                             params ({ { "trackId", tid }, { "start", 0 }, { "end", 2 * Q },
                                       { "destStart", 8 * Q } }))["ok"]);
            expectEquals ((int) notesOf().size(), 8);
            expectEquals (notesOf()[0].startTick, 2 * Q);            // original first notes moved away
            expectEquals (notesOf()[7].startTick, 9 * Q);
        }

        beginTest ("setControlRange replaces one controller's window only");
        {
            juce::Array<juce::var> controls {
                params ({ { "tick", 0 },     { "type", 0 }, { "number", 1 },  { "value", 10 } }),
                params ({ { "tick", Q },     { "type", 0 }, { "number", 1 },  { "value", 20 } }),
                params ({ { "tick", 2 * Q }, { "type", 0 }, { "number", 1 },  { "value", 30 } }),
                params ({ { "tick", Q },     { "type", 0 }, { "number", 11 }, { "value", 99 } }) };

            api.run ("clip.set", params ({ { "trackId", tid }, { "notes", juce::Array<juce::var>() },
                                           { "controls", controls } }));

            // Replace CC1 inside [Q, 2Q) with two new points
            juce::Array<juce::var> events { params ({ { "tick", Q }, { "value", 55 } }),
                                            params ({ { "tick", Q + 1000 }, { "value", 56 } }) };

            expect (api.run ("clip.setControlRange",
                             params ({ { "trackId", tid }, { "type", 0 }, { "number", 1 },
                                       { "start", Q }, { "end", 2 * Q }, { "events", events } }))["ok"]);

            const auto& after = engine.getTrackSequence (trackId)->getControls();
            expectEquals ((int) after.size(), 5);   // CC1: 10,55,56,30 + CC11: 99

            int cc1 = 0, cc11 = 0;
            for (auto& control : after)
                (control.number == 1 ? cc1 : cc11) += 1;

            expectEquals (cc1, 4);
            expectEquals (cc11, 1);

            // Empty events erases the controller in the window; CC11 survives
            expect (api.run ("clip.setControlRange",
                             params ({ { "trackId", tid }, { "type", 0 }, { "number", 1 },
                                       { "start", 0 }, { "end", 4 * Q },
                                       { "events", juce::Array<juce::var>() } }))["ok"]);

            const auto& erased = engine.getTrackSequence (trackId)->getControls();
            expectEquals ((int) erased.size(), 1);
            expectEquals (erased[0].number, 11);

            // Pitch bend values clamp to 14 bits
            juce::Array<juce::var> bend { params ({ { "tick", 0 }, { "value", 20000 } }) };
            expect (api.run ("clip.setControlRange",
                             params ({ { "trackId", tid }, { "type", 1 }, { "number", 0 },
                                       { "start", 0 }, { "end", Q }, { "events", bend } }))["ok"]);

            const auto& withBend = engine.getTrackSequence (trackId)->getControls();
            bool foundBend = false;

            for (auto& control : withBend)
                if (control.type == MidiSequence::ControlType::pitchBend)
                {
                    foundBend = true;
                    expectEquals (control.value, 16383);
                }

            expect (foundBend);

            api.run ("clip.clear", params ({ { "trackId", tid } }));
        }

        beginTest ("undo/redo walk the clip history");
        {
            api.run ("clip.set", params ({ { "trackId", tid },
                                           { "notes", juce::Array<juce::var> { note (0, Q, 48) } } }));
            expectEquals ((int) notesOf().size(), 1);

            expect (api.run ("clip.undo", params ({ { "trackId", tid } }))["ok"]);
            expect (engine.getTrackSequence (trackId) == nullptr);   // back to the cleared state

            expect (api.run ("clip.redo", params ({ { "trackId", tid } }))["ok"]);
            expectEquals ((int) notesOf().size(), 1);
            expectEquals (notesOf()[0].key, 48);

            // Drain the undo stack; it must bottom out politely
            while (api.run ("clip.undo", params ({ { "trackId", tid } }))["ok"]) {}
            expect (engine.getTrackSequence (trackId) == nullptr);
            expect (! (bool) api.run ("clip.undo", params ({ { "trackId", tid } }))["ok"]);
        }
    }
};

static ClipCommandTests clipCommandTests;
