#include <juce_audio_processors/juce_audio_processors.h>

#include "../src/model/PlaybackSequence.h"
#include "../src/engine/Transport.h"
#include "../src/engine/MidiSourceProcessor.h"

namespace
{
    constexpr auto Q = Ticks::perQuarterNote;      // at 120 bpm / 48 kHz: 40 ticks per sample
    constexpr juce::int64 seventyMs = 134400;      // ticks
    using Map = ExpressionMap;
    using Out = ExpressionMap::Output;

    Map::Articulation art (const juce::String& name, double offsetMs, std::vector<Out> outputs, std::initializer_list<const char*> appliesTo = {})
    {
        Map::Articulation a;
        a.name = name;
        a.timingOffsetMs = offsetMs;
        a.outputs = std::move (outputs);

        for (auto* r : appliesTo)
            a.appliesTo.add (r);

        return a;
    }

    // Staccato: tapped keyswitch 24; Legato: held keyswitch 25 + CC32=20, 70 ms early; Marcato: program 5 in bank 130.
    // Release group: Long (CC33=90, 10 ms later) for Legato.
    Map testMap()
    {
        Map map;
        map.name = "Strings";
        map.groups.push_back ({ "Articulation", "", {
            art ("Staccato", 0.0, { { Out::Type::keyswitch, 24, 100, false, -1 } }),
            art ("Legato", -70.0, { { Out::Type::keyswitch, 25, 90, true, -1 }, { Out::Type::controller, 32, 20, false, -1 } }),
            art ("Marcato", 0.0, { { Out::Type::programChange, 5, 0, false, 130 } }) } });
        map.groups.push_back ({ "Release", "", { art ("Long", 10.0, { { Out::Type::controller, 33, 90, false, -1 } }, { "Legato" }) } });
        return map;
    }

    MidiSequence::Note note (juce::int64 start, int key, const char* root = nullptr,
                             std::initializer_list<std::pair<const char*, const char*>> modifiers = {}, juce::int64 length = Q)
    {
        MidiSequence::Note n;
        n.startTick = start;
        n.lengthTicks = length;
        n.key = key;

        if (root != nullptr)
            n.articulation.root = root;

        for (auto& [group, name] : modifiers)
            n.articulation.modifiers.emplace_back (group, name);

        return n;
    }

    const MidiSequence::Note* findNote (const MidiSequence& seq, int key, int occurrence = 0)
    {
        for (auto& n : seq.getNotes())
            if (n.key == key && occurrence-- == 0)
                return &n;

        return nullptr;
    }

    std::vector<const MidiSequence::Control*> controlsOf (const MidiSequence& seq, int number)
    {
        std::vector<const MidiSequence::Control*> found;

        for (auto& c : seq.getControls())
            if (c.type == MidiSequence::ControlType::controller && c.number == number)
                found.push_back (&c);

        return found;
    }
}

class PlaybackSequenceTests final : public juce::UnitTest
{
public:
    PlaybackSequenceTests() : UnitTest ("Playback sequence") {}

    void runTest() override
    {
        const auto map = testMap();
        const auto tempo = TempoMap::create (120.0);

        beginTest ("nothing to generate: the written sequence itself is played");
        {
            const auto written = MidiSequence::create ({ note (0, 60), note (Q, 62) }, {});
            expect (playback::build (written, &map, *tempo, false).sequence == written);          // no articulations
            expect (playback::build (written, nullptr, *tempo, false).sequence == written);       // no map
            expect (playback::build (nullptr, &map, *tempo, false).sequence == nullptr);
            expect (playback::build (written, &map, *tempo, true).sequence != written);           // the default root applies to every note
        }

        beginTest ("a timing offset moves the note, keeps its length, and records where it was written");
        {
            const auto written = MidiSequence::create ({ note (4 * Q, 60, "Legato", {}, Q / 2) }, {});
            const auto result = playback::build (written, &map, *tempo, false);
            const auto* n = findNote (*result.sequence, 60);

            expect (n != nullptr);
            expectEquals (n->startTick, 4 * Q - seventyMs);
            expectEquals (n->lengthTicks, Q / 2);
            expectEquals (n->sourceTick, 4 * Q);
            expectEquals (n->written(), 4 * Q);
            expectEquals (result.earliestOffsetMs, -70.0);
        }

        beginTest ("a note at the very start goes below zero (the pre-roll will play it)");
        {
            const auto written = MidiSequence::create ({ note (0, 60, "Legato") }, {});
            const auto builtAtStart = playback::build (written, &map, *tempo, false);
            const auto sequenceAtStart = builtAtStart.sequence;
            const auto* n = findNote (*sequenceAtStart, 60);
            expect (n != nullptr && n->startTick == -seventyMs && n->written() == 0);
        }

        beginTest ("the shift is wall-clock time: at 60 bpm 70 ms is 67200 ticks");
        {
            const auto slow = TempoMap::create (60.0);
            const auto written = MidiSequence::create ({ note (4 * Q, 60, "Legato") }, {});
            expectEquals (findNote (*playback::build (written, &map, *slow, false).sequence, 60)->startTick, 4 * Q - 67200);
        }

        beginTest ("outputs are sent when the articulation CHANGES, exactly at the note's own tick, before its note-on");
        {
            // Staccato, Staccato, Legato, Legato, Staccato
            const auto written = MidiSequence::create ({ note (0, 60, "Staccato"), note (Q, 62, "Staccato"), note (2 * Q, 64, "Legato"),
                                                         note (3 * Q, 65, "Legato"), note (4 * Q, 67, "Staccato") }, {});
            const auto built = playback::build (written, &map, *tempo, false);   // the result owns the sequence
            const auto& seq = *built.sequence;

            // Keyswitch notes (24 = Staccato, 25 = Legato): at the changes only
            const auto ks24First = findNote (seq, 24, 0), ks24Second = findNote (seq, 24, 1), ks25 = findNote (seq, 25);
            expect (ks24First != nullptr && ks24Second != nullptr && ks25 != nullptr);
            expect (findNote (seq, 24, 2) == nullptr, "no third Staccato switch");
            expectEquals (ks24First->startTick, (juce::int64) 0);
            expectEquals (ks25->startTick, 2 * Q - seventyMs);            // at the SHIFTED legato note
            expectEquals (findNote (seq, 64)->startTick, 2 * Q - seventyMs);   // same tick, the keyswitch first
            expectEquals (ks24Second->startTick, 4 * Q);

            // Order in the sequence: the keyswitch precedes its note at the same tick
            const auto& all = seq.getNotes();
            const auto indexOf = [&all] (const MidiSequence::Note* p) { return (int) (p - all.data()); };
            expect (indexOf (ks25) < indexOf (findNote (seq, 64)));

            // A keyswitch remembers where it was written (its note's written time)
            expectEquals (ks25->written(), 2 * Q);

            // Legato's CC32 went out once, at the change, with the written time of its note
            const auto cc32 = controlsOf (seq, 32);
            expectEquals ((int) cc32.size(), 1);
            expectEquals (cc32[0]->tick, 2 * Q - seventyMs);
            expectEquals (cc32[0]->written(), 2 * Q);
            expectEquals (cc32[0]->value, 20);
        }

        beginTest ("keyswitch lengths: a tapped one is short, a held one lasts until the articulation changes");
        {
            const auto written = MidiSequence::create ({ note (0, 60, "Staccato"), note (2 * Q, 64, "Legato"), note (4 * Q, 65, "Legato"),
                                                         note (6 * Q, 67, "Staccato") }, {});
            const auto built = playback::build (written, &map, *tempo, false);   // the result owns the sequence
            const auto& seq = *built.sequence;

            expectEquals (findNote (seq, 24)->lengthTicks, (juce::int64) 57600);   // 30 ms tapped (1920 ticks per ms here)

            const auto* legatoSwitch = findNote (seq, 25);                          // held: from its note to the next change
            expectEquals (legatoSwitch->startTick, 2 * Q - seventyMs);
            expectEquals (legatoSwitch->startTick + legatoSwitch->lengthTicks, 6 * Q);   // the Staccato note at 6Q

            // If it is the last articulation, a held keyswitch ends with the last note
            const auto last = MidiSequence::create ({ note (0, 64, "Legato", {}, Q) }, {});
            const auto lastResult = playback::build (last, &map, *tempo, false);   // kept alive: findNote returns a pointer into it
            const auto* lastSwitch = findNote (*lastResult.sequence, 25);
            expectEquals (lastSwitch->startTick + lastSwitch->lengthTicks, Q - seventyMs);
        }

        beginTest ("modifiers: their outputs follow the root's, and their timing offsets add up");
        {
            const auto written = MidiSequence::create ({ note (4 * Q, 64, "Legato", { { "Release", "Long" } }) }, {});
            const auto result = playback::build (written, &map, *tempo, false);
            const auto& seq = *result.sequence;

            expectEquals (findNote (seq, 64)->startTick, 4 * Q - 60 * 1920);   // -70 + 10 = -60 ms; 1 ms = 1920 ticks here
            expectEquals (result.earliestOffsetMs, -60.0);
            expectEquals ((int) controlsOf (seq, 33).size(), 1);               // the modifier's CC33 went out too
            expectEquals (controlsOf (seq, 33)[0]->value, 90);
        }

        beginTest ("a program change with a bank sends bank select (CC0, CC32) first");
        {
            const auto written = MidiSequence::create ({ note (Q, 60, "Marcato") }, {});
            const auto built = playback::build (written, &map, *tempo, false);   // the result owns the sequence
            const auto& seq = *built.sequence;

            const auto bankMsb = controlsOf (seq, 0), bankLsb = controlsOf (seq, 32);
            expectEquals ((int) bankMsb.size(), 1);
            expectEquals (bankMsb[0]->value, 130 >> 7);
            expectEquals ((int) bankLsb.size(), 1);
            expectEquals (bankLsb[0]->value, 130 & 127);

            int programs = 0;

            for (auto& c : seq.getControls())
                if (c.type == MidiSequence::ControlType::programChange)
                {
                    ++programs;
                    expectEquals (c.value, 5);
                    expectEquals (c.tick, Q);
                }

            expectEquals (programs, 1);
        }

        beginTest ("an articulation the map doesn't have plays as none: no switch, no shift; a note with none sends nothing");
        {
            const auto written = MidiSequence::create ({ note (0, 60, "Staccato"), note (Q, 62, "Pizzicato"),
                                                         note (2 * Q, 64, nullptr), note (3 * Q, 65, "Staccato") }, {});
            const auto built = playback::build (written, &map, *tempo, false);   // the result owns the sequence
            const auto& seq = *built.sequence;

            expectEquals (findNote (seq, 62)->startTick, Q);   // unshifted
            expectEquals (findNote (seq, 64)->startTick, 2 * Q);
            expect (findNote (seq, 24, 1) == nullptr, "the instrument stayed on Staccato: no second switch");
            expect (findNote (seq, 25) == nullptr);
        }

        beginTest ("default root on: notes with none behave as the first root, once");
        {
            const auto written = MidiSequence::create ({ note (0, 60), note (Q, 62), note (2 * Q, 64, "Legato") }, {});

            const auto on = playback::build (written, &map, *tempo, true);
            expect (findNote (*on.sequence, 24) != nullptr && findNote (*on.sequence, 24, 1) == nullptr, "Staccato switched once at the first note");
            expect (findNote (*on.sequence, 25) != nullptr);

            const auto off = playback::build (written, &map, *tempo, false);
            expect (findNote (*off.sequence, 24) == nullptr, "off: nothing is sent for notes with none");
        }

        beginTest ("controls of the written sequence pass through untouched");
        {
            MidiSequence::Control dynamics;
            dynamics.tick = Q;
            dynamics.number = 1;
            dynamics.value = 77;
            const auto written = MidiSequence::create ({ note (2 * Q, 64, "Legato") }, { dynamics });
            const auto passThrough = playback::build (written, &map, *tempo, false);
            const auto cc1 = controlsOf (*passThrough.sequence, 1);

            expectEquals ((int) cc1.size(), 1);
            expectEquals (cc1[0]->tick, Q);
            expectEquals (cc1[0]->value, 77);
        }

        beginTest ("starting mid-piece re-sends the keyswitch of the articulation in effect (the chase)");
        {
            // Staccato at 0, Legato at 4Q (held keyswitch, until the next change), Staccato at 12Q
            const auto written = MidiSequence::create ({ note (0, 60, "Staccato"), note (4 * Q, 64, "Legato"), note (12 * Q, 67, "Staccato") }, {});
            const auto result = playback::build (written, &map, *tempo, false);

            struct Hit { juce::int64 sample; juce::MidiMessage message; };

            const auto playFrom = [&] (juce::int64 startTick, int blocks)
            {
                Transport transport;
                transport.prepare (48000.0);
                transport.setPreRollMs (-result.earliestOffsetMs);
                MidiSourceProcessor source (transport);
                source.setSequence (result.sequence);
                juce::AudioBuffer<float> audio (2, 4096);
                std::vector<Hit> hits;
                juce::int64 clock = 0;

                transport.locate (startTick);
                transport.play();

                for (int i = 0; i < blocks; ++i)
                {
                    transport.beginBlock (480);
                    juce::MidiBuffer midi;
                    source.processBlock (audio, midi);

                    for (const auto metadata : midi)
                        hits.push_back ({ clock + metadata.samplePosition, metadata.getMessage() });

                    clock += 480;
                }

                return hits;
            };

            const auto ons = [] (const std::vector<Hit>& hits, int key)
            {
                std::vector<juce::int64> samples;

                for (auto& h : hits)
                    if (h.message.isNoteOn() && h.message.getNoteNumber() == key)
                        samples.push_back (h.sample);

                return samples;
            };

            const auto offs = [] (const std::vector<Hit>& hits, int key)
            {
                std::vector<juce::int64> samples;

                for (auto& h : hits)
                    if (h.message.isNoteOff() && h.message.getNoteNumber() == key)
                        samples.push_back (h.sample);

                return samples;
            };

            // From bar 3 (8Q): the Legato's held keyswitch is still down, until the Staccato at 12Q
            {
                const auto hits = playFrom (8 * Q, 250);
                expect (ons (hits, 25) == std::vector<juce::int64> { 0 }, "legato's keyswitch is re-sent at the very start");
                expect (ons (hits, 24).size() == 1, "...and the Staccato's own switch at 12Q comes in due course");
                expect (offs (hits, 25) == std::vector<juce::int64> { 3360 + 96000 }, "a held keyswitch ends where the articulation does (12Q = 96000 samples on)");

                // The controller part of the state was chased too (CC32 = 20)
                int cc32 = 0;

                for (auto& h : hits)
                    if (h.message.isController() && h.message.getControllerNumber() == 32 && h.message.getControllerValue() == 20)
                        ++cc32;

                expectEquals (cc32, 1);
            }

            // From bar 4 (12Q + 2Q = 14Q): the Staccato's tapped keyswitch is replayed for its own 30 ms
            {
                const auto hits = playFrom (14 * Q, 10);
                expect (ons (hits, 24) == std::vector<juce::int64> { 0 });
                expect (offs (hits, 24) == std::vector<juce::int64> { 1440 }, "released 30 ms later");
                expect (ons (hits, 25).empty());
            }

            // Before any articulation was set: nothing to chase
            {
                const auto hits = playFrom (0, 40);
                expect (ons (hits, 24) == std::vector<juce::int64> { 3360 }, "the first note's own switch, in its own time");
            }

            // Starting exactly AT the legato (4Q): its switch is played in the pre-roll, not chased; the earlier one is
            {
                const auto hits = playFrom (4 * Q, 20);
                expect (ons (hits, 25) == std::vector<juce::int64> { 0 }, "the legato's keyswitch once, 70 ms before it");
                expect (ons (hits, 24) == std::vector<juce::int64> { 0 }, "the earlier articulation is chased");
                size_t at24 = 0, at25 = 0;

                for (size_t i = 0; i < hits.size(); ++i)
                {
                    if (hits[i].message.isNoteOn() && hits[i].message.getNoteNumber() == 24) at24 = i;
                    if (hits[i].message.isNoteOn() && hits[i].message.getNoteNumber() == 25) at25 = i;
                }

                expect (at24 < at25, "the chased state first, then the early switch: the legato wins");
            }
        }

        beginTest ("end to end: the legato sounds 70 ms early and its keyswitch lands in the same sample, first");
        {
            // Written at bar 2; played from the start with the pre-roll the sequence asks for
            const auto written = MidiSequence::create ({ note (4 * Q, 64, "Legato") }, {});
            const auto result = playback::build (written, &map, *tempo, false);

            Transport transport;
            transport.prepare (48000.0);
            transport.setPreRollMs (-result.earliestOffsetMs);
            MidiSourceProcessor source (transport);
            source.setSequence (result.sequence);
            juce::AudioBuffer<float> audio (2, 4096);

            transport.play();
            struct Hit { juce::int64 sample; juce::MidiMessage message; };
            std::vector<Hit> hits;
            juce::int64 clock = 0;

            for (int i = 0; i < 300; ++i)
            {
                transport.beginBlock (480);
                juce::MidiBuffer midi;
                source.processBlock (audio, midi);

                for (const auto metadata : midi)
                    hits.push_back ({ clock + metadata.samplePosition, metadata.getMessage() });

                clock += 480;
            }

            juce::int64 keyswitchOn = -1, noteOn = -1, cc32At = -1;
            size_t keyswitchIndex = 0, noteIndex = 0;

            for (size_t i = 0; i < hits.size(); ++i)
            {
                if (hits[i].message.isNoteOn() && hits[i].message.getNoteNumber() == 25)   { keyswitchOn = hits[i].sample; keyswitchIndex = i; }
                if (hits[i].message.isNoteOn() && hits[i].message.getNoteNumber() == 64)   { noteOn = hits[i].sample; noteIndex = i; }
                if (hits[i].message.isController() && hits[i].message.getControllerNumber() == 32 && hits[i].message.getControllerValue() == 20)
                    cc32At = hits[i].sample;
            }

            // Bar 2 is 96000 samples in; the pre-roll adds 3360; the note is written there but sounds 3360 samples earlier
            expectEquals (noteOn, (juce::int64) 3360 + 96000 - 3360);
            expectEquals (keyswitchOn, noteOn);   // the same sample...
            expect (keyswitchIndex < noteIndex, "...and the keyswitch comes first");
            expectEquals (cc32At, noteOn);
        }
    }
};

static PlaybackSequenceTests playbackSequenceTests;
