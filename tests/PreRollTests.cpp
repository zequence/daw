#include <juce_audio_processors/juce_audio_processors.h>

#include "../src/engine/Transport.h"
#include "../src/engine/MidiSourceProcessor.h"

// The pre-roll spike (MILESTONES.md "Timing offset"): a negative timing offset schedules an event
// earlier than it is written. These tests prove, at sample accuracy, that the transport can start
// before the position it displays, that only events of the part being played sound in that
// pre-roll, and that a loop plays the next lap's early events at the end of the lap.
//
// 48 kHz at 120 bpm: 1 sample = 40 ticks, a quarter note = 24000 samples, 70 ms = 3360 samples = 134400 ticks.
namespace
{
    constexpr auto Q = Ticks::perQuarterNote;
    constexpr double rate = 48000.0;
    constexpr juce::int64 preRoll = 3360;               // samples (70 ms)
    constexpr juce::int64 preRollTicks = preRoll * 40;
    constexpr int blockSize = 480;

    struct Hit
    {
        juce::int64 sample;     // absolute output sample (counted from the first block)
        juce::MidiMessage message;
    };

    // Transport + one MIDI source, driven block by block like the audio callback does
    struct Rig
    {
        Transport transport;
        MidiSourceProcessor source { transport };
        juce::AudioBuffer<float> audio { 2, 4096 };
        std::vector<Hit> hits;
        juce::int64 clock = 0;

        Rig()    { transport.prepare (rate); }

        void run (int blocks, int size = blockSize)
        {
            for (int i = 0; i < blocks; ++i)
                one (size);
        }

        const Transport::Block& one (int size = blockSize)
        {
            transport.beginBlock (size);
            juce::MidiBuffer midi;
            source.processBlock (audio, midi);

            for (const auto metadata : midi)
                hits.push_back ({ clock + metadata.samplePosition, metadata.getMessage() });

            clock += size;
            return transport.getBlock();
        }

        std::vector<juce::int64> noteOns (int key) const
        {
            std::vector<juce::int64> samples;

            for (auto& hit : hits)
                if (hit.message.isNoteOn() && hit.message.getNoteNumber() == key)
                    samples.push_back (hit.sample);

            return samples;
        }

        std::vector<juce::int64> noteOffs (int key) const
        {
            std::vector<juce::int64> samples;

            for (auto& hit : hits)
                if (hit.message.isNoteOff() && hit.message.getNoteNumber() == key)
                    samples.push_back (hit.sample);

            return samples;
        }

        std::vector<juce::int64> controllers (int number) const
        {
            std::vector<juce::int64> samples;

            for (auto& hit : hits)
                if (hit.message.isController() && hit.message.getControllerNumber() == number)
                    samples.push_back (hit.sample);

            return samples;
        }
    };

    // An event of a playback sequence: scheduled at 'scheduled', written at 'written'
    MidiSequence::Note note (juce::int64 scheduled, juce::int64 written, int key, juce::int64 length = Q)
    {
        MidiSequence::Note n;
        n.startTick = scheduled;
        n.lengthTicks = length;
        n.key = key;
        n.sourceTick = written;
        return n;
    }

    MidiSequence::Control cc (juce::int64 scheduled, juce::int64 written, int number, int value)
    {
        MidiSequence::Control c;
        c.tick = scheduled;
        c.number = number;
        c.value = value;
        c.sourceTick = written;
        return c;
    }

    MidiSequence::Ptr playback (std::vector<MidiSequence::Note> notes, std::vector<MidiSequence::Control> controls = {})
    {
        return MidiSequence::create (std::move (notes), std::move (controls), true);   // negative times allowed
    }

    bool allEqual (const std::vector<juce::int64>& actual, std::initializer_list<juce::int64> expected)
    {
        return actual == std::vector<juce::int64> (expected);
    }
}

class PreRollTests final : public juce::UnitTest
{
public:
    PreRollTests() : UnitTest ("Pre-roll (negative timing offsets)") {}

    void runTest() override
    {
        beginTest ("playing from the start: the transport runs 70 ms before the displayed position, which stays put");
        {
            Rig rig;
            rig.transport.setPreRollMs (70.0);
            rig.source.setSequence (playback ({ note (-preRollTicks, 0, 60) }));   // written at 0, shifted 70 ms earlier
            rig.transport.play();

            const auto& first = rig.one();
            expect (first.playing && first.inPreRoll && first.chaseAtStart);
            expectEquals (first.segments[0].startTick, -preRollTicks);   // the timeline begins BEFORE tick 0
            expectEquals (rig.transport.getPositionTicks(), (juce::int64) 0);   // ...but the readout does not move
            expectEquals (rig.transport.getPositionSeconds(), 0.0);

            for (int i = 1; i < 7; ++i)
            {
                const auto& b = rig.one();
                expect (b.inPreRoll && ! b.chaseAtStart);
                expectEquals (rig.transport.getPositionTicks(), (juce::int64) 0);
            }

            expectEquals (rig.transport.getBlock().segments[0].endTick, (juce::int64) 0);   // 7 blocks of 480 = exactly the pre-roll

            const auto& afterPreRoll = rig.one();
            expect (! afterPreRoll.inPreRoll);
            expectEquals (afterPreRoll.segments[0].startTick, (juce::int64) 0);
            expectEquals (rig.transport.getPositionTicks(), (juce::int64) blockSize * 40);   // now it moves

            // The note-on is the very first sample: 3360 samples before the transport's tick 0
            rig.run (60);   // let the note end
            expect (allEqual (rig.noteOns (60), { 0 }), "the early note starts at the first sample");
            expect (allEqual (rig.noteOffs (60), { 24000 }), "and keeps its length");
        }

        beginTest ("without a pre-roll nothing changes (the default)");
        {
            Rig rig;
            rig.source.setSequence (playback ({ note (0, 0, 60), note (Q, Q, 62) }));
            rig.transport.play();
            const auto& b = rig.one();
            expect (! b.inPreRoll && ! b.hasAhead);
            expectEquals (b.segments[0].startTick, (juce::int64) 0);
            expect (allEqual (rig.noteOns (60), { 0 }));
        }

        beginTest ("from the middle: only events written at or after the start sound in the pre-roll");
        {
            const auto start = 4 * Q;   // 96000 samples
            Rig rig;
            rig.transport.setPreRollMs (70.0);

            // A: ordinary, written 10 ms before the start: must stay silent although it lies inside the pre-roll window
            // B: written 10 ms after the start, shifted 70 ms earlier (60 ms before the start): sounds in the pre-roll
            // C: written 100 ms after the start, not shifted: plays normally
            // D: written 10 ms before the start, shifted LATER by 70 ms (60 ms after the start): plays normally
            const juce::int64 tenMs = 19200, hundredMs = 192000, seventyMs = preRollTicks;
            rig.source.setSequence (playback ({
                note (start - tenMs, start - tenMs, 61),                    // A
                note (start + tenMs - seventyMs, start + tenMs, 62),        // B
                note (start + hundredMs, start + hundredMs, 63),            // C
                note (start - tenMs + seventyMs, start - tenMs, 64),        // D
            }));

            rig.transport.locate (start);
            rig.transport.play();
            rig.run (40);

            expect (rig.noteOns (61).empty(), "A is before the start and must not sound");
            expect (allEqual (rig.noteOns (62), { 480 }), "B sounds 60 ms before the start: 480 samples into the pre-roll");
            expect (allEqual (rig.noteOns (63), { preRoll + 4800 }), "C sounds 100 ms after the start");
            expect (allEqual (rig.noteOns (64), { preRoll + 2880 }), "D (a late note written before the start) still sounds, 60 ms in");
        }

        beginTest ("controllers: the state before the start is chased, early ones are played, not chased");
        {
            const auto start = 4 * Q;
            Rig rig;
            rig.transport.setPreRollMs (70.0);
            rig.source.setSequence (playback ({}, {
                cc (start - 2 * Q, start - 2 * Q, 7, 50),                         // ordinary: the state at the start
                cc (start - 19200 * 6, start + 19200, 20, 99) }));                // written after the start, scheduled 60 ms before it
            rig.transport.locate (start);
            rig.transport.play();
            rig.run (10);

            expect (allEqual (rig.controllers (7), { 0 }), "the earlier controller is chased once, at the very start");
            expect (allEqual (rig.controllers (20), { 480 }), "the early one is played at its scheduled sample, not chased");
        }

        beginTest ("a locate while playing restarts with a pre-roll: sounding notes end, events before the new position still sound");
        {
            Rig rig;
            rig.transport.setPreRollMs (70.0);
            rig.source.setSequence (playback ({ note (-preRollTicks, 0, 60, 8 * Q), note (8 * Q - preRollTicks, 8 * Q, 62) }));
            rig.transport.play();
            rig.run (30);   // note 60 is sounding

            rig.transport.locate (8 * Q);
            const auto& b = rig.one();
            expect (b.killAtStart && b.chaseAtStart && b.inPreRoll);
            expectEquals (b.segments[0].startTick, 8 * Q - preRollTicks);
            expectEquals (rig.transport.getPositionTicks(), 8 * Q);   // shown at the new position at once

            expect (! rig.noteOffs (60).empty() && rig.noteOffs (60).back() == 30 * blockSize, "note 60 ends at the locate");
            expect (allEqual (rig.noteOns (62), { 30 * blockSize }), "the note written at the new position sounds on time (70 ms early)");
        }

        beginTest ("stopping during the pre-roll: no stuck notes, and the position is where playback was to start");
        {
            Rig rig;
            rig.transport.setPreRollMs (70.0);
            rig.source.setSequence (playback ({ note (-preRollTicks, 0, 60, 8 * Q) }));
            rig.transport.locate (0);
            rig.transport.play();
            rig.run (2);                       // the note is on, the pre-roll is not over
            rig.transport.stop();
            rig.one();

            expectEquals (rig.noteOffs (60).size(), (size_t) 1);
            expect (! rig.transport.isPlaying());
            expectEquals (rig.transport.getPositionTicks(), (juce::int64) 0);

            rig.transport.play();   // plays again from the start, not from the middle of the old pre-roll
            const auto& again = rig.one();
            expectEquals (again.segments[0].startTick, -preRollTicks);
        }

        beginTest ("loop: the next lap's early events sound at the end of this lap, once, and survive the wrap");
        {
            Rig rig;
            rig.transport.setPreRollMs (70.0);
            rig.transport.setLoopRegion (0, 4 * Q);   // 96000 samples
            rig.transport.setLooping (true);
            rig.source.setSequence (playback ({ note (-preRollTicks, 0, 60),        // written at the loop start, 70 ms early
                                                note (2 * Q, 2 * Q, 62) }));        // an ordinary note mid-lap
            rig.transport.play();
            rig.run (3 * 200 + 7);   // the pre-roll plus three laps

            // Lap 1 starts at global 3360 (after the pre-roll). The early note of each lap sounds 3360
            // samples before that lap begins: the first in the pre-roll, the others at the end of the lap before.
            // (the run ends 3360 samples before lap 4 would start, so lap 4's early note has begun too)
            expect (allEqual (rig.noteOns (60), { 0, 96000, 192000, 288000 }),
                    "one early note-on per lap, at 70 ms before the lap starts; none duplicated at the wrap");
            expect (allEqual (rig.noteOffs (60), { 24000, 120000, 216000 }), "each keeps its length across the wrap");
            expect (allEqual (rig.noteOns (62), { preRoll + 48000, preRoll + 96000 + 48000, preRoll + 2 * 96000 + 48000 }),
                    "ordinary notes play once per lap");
        }

        beginTest ("loop: an event written after the loop end never sounds, even when shifted into the loop");
        {
            Rig rig;
            rig.transport.setPreRollMs (70.0);
            rig.transport.setLoopRegion (0, 4 * Q);
            rig.transport.setLooping (true);
            rig.source.setSequence (playback ({ note (4 * Q + 19200 - preRollTicks, 4 * Q + 19200, 64),   // written 10 ms past the end
                                                note (2 * Q, 2 * Q, 62) }));
            rig.transport.play();
            rig.run (2 * 200 + 7);

            expect (rig.noteOns (64).empty(), "it belongs to the part after the loop, which is not playing");
            expectEquals (rig.noteOns (62).size(), (size_t) 2);
        }

        beginTest ("loop: stopping while a look-ahead note sounds ends it");
        {
            Rig rig;
            rig.transport.setPreRollMs (70.0);
            rig.transport.setLoopRegion (0, 4 * Q);
            rig.transport.setLooping (true);
            rig.source.setSequence (playback ({ note (-preRollTicks, 0, 60, 8 * Q) }));
            rig.transport.play();
            rig.run (7 + 199);                // up to the last block of lap 1: the next lap's note is already on
            expect (rig.noteOns (60).size() == 2, "the look-ahead note has started");
            rig.transport.stop();
            rig.run (2);
            expect (rig.noteOffs (60).size() == 2 && ! rig.transport.isPlaying(), "and is ended by the stop");
        }

        beginTest ("loop: a look-ahead note outlives its lap if the loop is switched off");
        {
            Rig rig;
            rig.transport.setPreRollMs (70.0);
            rig.transport.setLoopRegion (0, 4 * Q);
            rig.transport.setLooping (true);
            rig.source.setSequence (playback ({ note (-preRollTicks, 0, 60, 8 * Q) }));
            rig.transport.play();
            rig.run (7 + 199);
            rig.transport.setLooping (false);
            rig.run (2);

            // The next lap will not happen: its early note (begun at 96000) ends at once, at the next block
            expect (allEqual (rig.noteOffs (60), { 206 * blockSize }), "the orphaned look-ahead note is ended at once");

            rig.run (500);   // and the first note ends at its own length (2 bars): nothing is left sounding
            expect (rig.noteOffs (60).size() == rig.noteOns (60).size(), "every note-on has its note-off");
        }
    }
};

static PreRollTests preRollTests;
