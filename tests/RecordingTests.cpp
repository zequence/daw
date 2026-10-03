#include <juce_audio_processors/juce_audio_processors.h>

#include "../src/engine/MidiRecorder.h"

namespace
{
    constexpr auto Q = Ticks::perQuarterNote;
    constexpr double rate = 48000.0;   // 120 bpm: 40 ticks/sample, 4800-sample block = 0.2 quarters

    struct Rig
    {
        Transport transport;
        MidiRecorderProcessor processor { transport };
        MidiRecorder recorder { processor };
        juce::AudioBuffer<float> audio { 1, 4800 };

        Rig() { transport.prepare (rate); }

        // Run one audio block, feeding the given events (offset within the block).
        void block (int numSamples, std::initializer_list<std::pair<juce::MidiMessage, int>> events = {})
        {
            juce::MidiBuffer midi;
            for (auto& [message, offset] : events)
                midi.addEvent (message, offset);

            transport.beginBlock (numSamples);
            processor.processBlock (audio, midi);
        }
    };
}

class RecordingTests final : public juce::UnitTest
{
public:
    RecordingTests() : UnitTest ("MidiRecorder") {}

    void runTest() override
    {
        beginTest ("a note gets its start, length and velocity from the stream");
        {
            Rig rig;
            rig.recorder.start (1);
            rig.transport.play();

            rig.block (4800, { { juce::MidiMessage::noteOn (1, 60, (juce::uint8) 97), 2400 } });
            rig.block (4800);
            rig.block (4800, { { juce::MidiMessage::noteOff (1, 60), 1200 } });

            const auto result = rig.recorder.finish (rig.transport.getPositionTicks());
            expectEquals ((int) result.notes.size(), 1);
            expectEquals (result.notes[0].startTick, (juce::int64) 2400 * 40);
            expectEquals (result.notes[0].lengthTicks, (juce::int64) (2 * 4800 + 1200 - 2400) * 40);
            expectEquals (result.notes[0].velocity, 97);
            expect (result.controls.empty());
        }

        beginTest ("note-on with velocity 0 ends the note");
        {
            Rig rig;
            rig.recorder.start (1);
            rig.transport.play();

            rig.block (4800, { { juce::MidiMessage::noteOn (1, 64, (juce::uint8) 80), 0 },
                               { juce::MidiMessage::noteOn (1, 64, (juce::uint8) 0), 480 } });

            const auto result = rig.recorder.finish (rig.transport.getPositionTicks());
            expectEquals ((int) result.notes.size(), 1);
            expectEquals (result.notes[0].lengthTicks, (juce::int64) 480 * 40);
        }

        beginTest ("controllers and pitch bend are captured with ticks");
        {
            Rig rig;
            rig.recorder.start (1);
            rig.transport.play();

            rig.block (4800, { { juce::MidiMessage::controllerEvent (1, 1, 88), 960 },
                               { juce::MidiMessage::pitchWheel (1, 10000), 1920 } });

            const auto result = rig.recorder.finish (rig.transport.getPositionTicks());
            expectEquals ((int) result.controls.size(), 2);
            expect (result.controls[0].type == MidiSequence::ControlType::controller);
            expectEquals (result.controls[0].number, 1);
            expectEquals (result.controls[0].value, 88);
            expectEquals (result.controls[0].tick, (juce::int64) 960 * 40);
            expect (result.controls[1].type == MidiSequence::ControlType::pitchBend);
            expectEquals (result.controls[1].value, 10000);
        }

        beginTest ("a held note is closed at the finish position");
        {
            Rig rig;
            rig.recorder.start (1);
            rig.transport.play();

            rig.block (4800, { { juce::MidiMessage::noteOn (1, 72, (juce::uint8) 70), 0 } });
            rig.block (4800);

            const auto result = rig.recorder.finish (rig.transport.getPositionTicks());
            expectEquals ((int) result.notes.size(), 1);
            expectEquals (result.notes[0].lengthTicks, (juce::int64) 9600 * 40);
        }

        beginTest ("loop wrap closes held notes at the loop end and raises the wrap flag");
        {
            Rig rig;
            rig.transport.setLoopRegion (0, 4 * Q);
            rig.transport.setLooping (true);
            rig.transport.locate (4 * Q - 1920);      // 48 samples before the loop end
            rig.recorder.start (1);
            rig.transport.play();

            // Note starts 24 samples before the wrap; its off arrives after the wrap.
            rig.block (480, { { juce::MidiMessage::noteOn (1, 60, (juce::uint8) 90), 24 },
                              { juce::MidiMessage::noteOff (1, 60), 100 } });

            rig.recorder.poll();
            expect (rig.recorder.consumeWrapFlag(), "wrap flag not raised");
            expect (! rig.recorder.consumeWrapFlag(), "wrap flag not consumed");

            const auto result = rig.recorder.finish (rig.transport.getPositionTicks());
            expectEquals ((int) result.notes.size(), 1);
            expectEquals (result.notes[0].startTick, 4 * Q - 1920 + 24 * 40);
            expectEquals (result.notes[0].startTick + result.notes[0].lengthTicks, 4 * Q);
        }

        beginTest ("nothing is captured while inactive or stopped");
        {
            Rig rig;

            // Transport playing but recorder never started
            rig.transport.play();
            rig.block (4800, { { juce::MidiMessage::noteOn (1, 60, (juce::uint8) 90), 0 } });

            rig.recorder.start (1);
            rig.transport.stop();
            rig.block (4800, { { juce::MidiMessage::noteOn (1, 62, (juce::uint8) 90), 0 } });   // stopped: ignored

            const auto result = rig.recorder.finish (rig.transport.getPositionTicks());
            expect (result.isEmpty());
        }

        beginTest ("takePending leaves open notes running across the commit");
        {
            Rig rig;
            rig.recorder.start (1);
            rig.transport.play();

            rig.block (4800, { { juce::MidiMessage::noteOn (1, 60, (juce::uint8) 90), 0 },
                               { juce::MidiMessage::controllerEvent (1, 11, 101), 0 } });
            rig.recorder.poll();

            const auto partial = rig.recorder.takePending();
            expectEquals ((int) partial.controls.size(), 1);
            expectEquals ((int) partial.notes.size(), 0);      // still held

            rig.block (4800, { { juce::MidiMessage::noteOff (1, 60), 0 } });
            const auto rest = rig.recorder.finish (rig.transport.getPositionTicks());
            expectEquals ((int) rest.notes.size(), 1);
            expectEquals (rest.notes[0].lengthTicks, (juce::int64) 4800 * 40);
        }
    }
};

static RecordingTests recordingTests;
