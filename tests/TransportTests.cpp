#include <juce_audio_processors/juce_audio_processors.h>

#include "../src/engine/Transport.h"
#include "../src/engine/MidiSourceProcessor.h"

namespace
{
    constexpr auto Q = Ticks::perQuarterNote;
    constexpr double rate = 48000.0;   // at 120 bpm: 1 quarter = 24000 samples, 40 ticks/sample

    struct Event
    {
        juce::MidiMessage message;
        int offset;
    };

    std::vector<Event> collect (const juce::MidiBuffer& midi)
    {
        std::vector<Event> events;
        for (const auto metadata : midi)
            events.push_back ({ metadata.getMessage(), metadata.samplePosition });
        return events;
    }
}

//==============================================================================
class TransportTests final : public juce::UnitTest
{
public:
    TransportTests() : UnitTest ("Transport") {}

    void runTest() override
    {
        beginTest ("linear playback produces contiguous tick segments");
        {
            Transport t;
            t.prepare (rate);
            t.play();

            t.beginBlock (4800);
            const auto& b1 = t.getBlock();
            expect (b1.playing && b1.chaseAtStart && ! b1.killAtStart);
            expectEquals (b1.numSegments, 1);
            expectEquals (b1.segments[0].startTick, (juce::int64) 0);
            expectEquals (b1.segments[0].endTick, (juce::int64) 192000);   // 4800 samples * 40 ticks

            t.beginBlock (4800);
            const auto& b2 = t.getBlock();
            expect (! b2.chaseAtStart);
            expectEquals (b2.segments[0].startTick, (juce::int64) 192000);
            expectEquals (b2.segments[0].endTick, (juce::int64) 384000);
        }

        beginTest ("stop kills notes; position survives");
        {
            Transport t;
            t.prepare (rate);
            t.setReturnOnStop (false);   // stop at current time
            t.play();
            t.beginBlock (4800);
            t.stop();
            t.beginBlock (4800);

            const auto& b = t.getBlock();
            expect (! b.playing && b.killAtStart);
            expectEquals (b.numSegments, 0);
            expectEquals (t.getPositionTicks(), (juce::int64) 192000);
        }

        beginTest ("stop returns to where playback started (the default mode)");
        {
            Transport t;
            t.prepare (rate);
            t.locate (96000);
            t.beginBlock (0);
            t.play();
            t.beginBlock (4800);
            t.beginBlock (4800);
            t.stop();
            t.beginBlock (4800);

            expectEquals (t.getPositionTicks(), (juce::int64) 96000);
        }

        beginTest ("locate while playing requests kill and chase");
        {
            Transport t;
            t.prepare (rate);
            t.play();
            t.beginBlock (4800);

            t.locate (4 * Q);
            t.beginBlock (4800);
            const auto& b = t.getBlock();
            expect (b.killAtStart && b.chaseAtStart);
            expectEquals (b.segments[0].startTick, 4 * Q);
        }

        beginTest ("loop wraps mid-block into two segments");
        {
            Transport t;
            t.prepare (rate);
            t.setLoopRegion (0, 4 * Q);          // 96000 samples
            t.setLooping (true);
            t.locate (4 * Q - 1920);             // 48 samples before the loop end
            t.play();

            t.beginBlock (480);
            const auto& b = t.getBlock();
            expect (b.looped);
            expectEquals (b.numSegments, 2);
            expectEquals (b.segments[0].startTick, 4 * Q - 1920);
            expectEquals (b.segments[0].endTick, 4 * Q);
            expectEquals (b.segments[0].numSamples, 48);
            expectEquals (b.segments[1].startTick, (juce::int64) 0);
            expectEquals (b.segments[1].offset, 48);
            expectEquals (b.segments[1].numSamples, 432);
            expectEquals (t.getPositionTicks(), (juce::int64) 432 * 40);
        }

        beginTest ("landing exactly on the loop end wraps the next block");
        {
            Transport t;
            t.prepare (rate);
            t.setLoopRegion (0, 4 * Q);
            t.setLooping (true);
            t.play();

            for (int i = 0; i < 20; ++i)         // 20 * 4800 = 96000 samples: exactly the loop
                t.beginBlock (4800);

            t.beginBlock (4800);
            const auto& b = t.getBlock();
            expect (b.killAtStart && b.chaseAtStart);
            expectEquals (b.numSegments, 1);
            expectEquals (b.segments[0].startTick, (juce::int64) 0);
        }
    }
};

//==============================================================================
class MidiSourceTests final : public juce::UnitTest
{
public:
    MidiSourceTests() : UnitTest ("MidiSourceProcessor") {}

    void runTest() override
    {
        juce::AudioBuffer<float> audio (1, 4800);
        juce::MidiBuffer midi;

        // Two back-to-back C4 quarters (retrigger case) and a CC1 change half way through the first.
        auto seq = MidiSequence::create (
            { { 0, Q, 1, 60, 96 }, { Q, Q, 1, 60, 96 } },
            { { 0, MidiSequence::ControlType::controller, 1, 1, 50 },
              { Q / 2, MidiSequence::ControlType::controller, 1, 1, 80 } });

        beginTest ("events render with sample-accurate offsets");
        {
            Transport t;
            t.prepare (rate);
            MidiSourceProcessor source (t);
            source.setSequence (seq);

            t.play();
            t.beginBlock (4800);
            source.processBlock (audio, midi);

            auto events = collect (midi);
            expectEquals ((int) events.size(), 2);
            expect (events[0].message.isController() && events[0].message.getControllerValue() == 50);
            expect (events[1].message.isNoteOn() && events[1].message.getNoteNumber() == 60);
            expectEquals (events[1].offset, 0);

            // Block 3 covers ticks [384000, 576000): CC1=80 at tick 480000 -> offset 2400
            t.beginBlock (4800);
            source.processBlock (audio, midi);
            expectEquals ((int) collect (midi).size(), 0);

            t.beginBlock (4800);
            source.processBlock (audio, midi);
            auto cc = collect (midi);
            expectEquals ((int) cc.size(), 1);
            expect (cc[0].message.isController() && cc[0].message.getControllerValue() == 80);
            expectEquals (cc[0].offset, 2400);
        }

        beginTest ("retriggered note sends off before on at the same sample");
        {
            Transport t;
            t.prepare (rate);
            MidiSourceProcessor source (t);
            source.setSequence (seq);
            t.play();

            std::vector<Event> atBoundary;

            for (int block = 0; block < 6; ++block)    // block 5 covers ticks [960000, 1152000)
            {
                t.beginBlock (4800);
                source.processBlock (audio, midi);

                if (block == 5)
                    atBoundary = collect (midi);
            }

            expectEquals ((int) atBoundary.size(), 2);
            expect (atBoundary[0].message.isNoteOff());
            expect (atBoundary[1].message.isNoteOn());
            expectEquals (atBoundary[0].offset, 0);
            expectEquals (atBoundary[1].offset, 0);
        }

        beginTest ("stop releases held notes");
        {
            Transport t;
            t.prepare (rate);
            MidiSourceProcessor source (t);
            source.setSequence (seq);
            t.play();
            t.beginBlock (4800);
            source.processBlock (audio, midi);   // C4 now sounding

            t.stop();
            t.beginBlock (4800);
            source.processBlock (audio, midi);

            auto events = collect (midi);
            expectEquals ((int) events.size(), 1);
            expect (events[0].message.isNoteOff() && events[0].message.getNoteNumber() == 60);
            expectEquals (events[0].offset, 0);
        }

        beginTest ("locate chases the latest controller values, not notes");
        {
            Transport t;
            t.prepare (rate);
            MidiSourceProcessor source (t);
            source.setSequence (seq);

            t.locate (Q * 3 / 2);   // between the CC1=80 change and the second note
            t.play();
            t.beginBlock (4800);
            source.processBlock (audio, midi);

            auto events = collect (midi);
            expectEquals ((int) events.size(), 1);
            expect (events[0].message.isController()
                      && events[0].message.getControllerNumber() == 1
                      && events[0].message.getControllerValue() == 80);
            expectEquals (events[0].offset, 0);
        }

        beginTest ("loop wrap kills and retriggers notes that span the loop");
        {
            auto longNote = MidiSequence::create ({ { 0, 8 * Q, 1, 60, 96 } }, {});

            Transport t;
            t.prepare (rate);
            t.setLoopRegion (0, 4 * Q);
            t.setLooping (true);
            MidiSourceProcessor source (t);
            source.setSequence (longNote);
            t.play();

            t.beginBlock (4800);
            source.processBlock (audio, midi);
            expectEquals ((int) collect (midi).size(), 1);   // the initial note-on

            for (int block = 1; block < 20; ++block)         // up to the exact loop end
            {
                t.beginBlock (4800);
                source.processBlock (audio, midi);
                expectEquals ((int) collect (midi).size(), 0);
            }

            t.beginBlock (4800);                             // wraps to the loop start
            source.processBlock (audio, midi);

            auto events = collect (midi);
            expectEquals ((int) events.size(), 2);
            expect (events[0].message.isNoteOff());
            expect (events[1].message.isNoteOn());
            expectEquals (events[0].offset, 0);
            expectEquals (events[1].offset, 0);
        }
    }
};

static TransportTests transportTests;
static MidiSourceTests midiSourceTests;
