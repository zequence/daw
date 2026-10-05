#include "../src/engine/MidiRouteProcessor.h"

class MidiMonitorTests final : public juce::UnitTest
{
public:
    MidiMonitorTests() : juce::UnitTest ("MIDI monitor", "Engine") {}

    void runTest() override
    {
        beginTest ("a route records what it hands the instrument: plain messages, in order, with their offsets");
        {
            MidiMonitor monitor;
            MidiRouteProcessor route (3, 2);   // channel 3, port 2 (wrapped for the VST3 host)
            route.prepareToPlay (48000.0, 512);
            route.setMonitor (&monitor, [] { return (juce::int64) 1234; }, 7, 9);

            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::programChange (1, 112), 100);
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 90), 100);

            route.processBlock (audio, midi);
            expect (monitor.drain().empty(), "nothing is recorded until it is started");

            monitor.setEnabled (true);
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 10);
            midi.addEvent (juce::MidiMessage::programChange (1, 112), 20);
            midi.addEvent (juce::MidiMessage::noteOn (1, 63, (juce::uint8) 80), 20);
            route.processBlock (audio, midi);

            const auto events = monitor.drain();
            expectEquals ((int) events.size(), 3);

            if (events.size() == 3)
            {
                expect (events[0].bytes[0] == 0x82 && events[0].bytes[1] == 60 && events[0].offset == 10, "note off, channel 3, unwrapped");
                expect (events[1].bytes[0] == 0xc2 && events[1].bytes[1] == 112 && events[1].size == 2, "program change");
                expect (events[2].bytes[0] == 0x92 && events[2].bytes[1] == 63 && events[2].offset == 20, "note on after it");
                expect (events[0].port == 2 && events[0].trackId == 7 && events[0].instrumentId == 9 && events[0].tick == 1234);
                expect (events[2].timeMs() > events[0].timeMs(), "spacing from the sample offsets");
            }
        }

        beginTest ("MIDI clock and other real-time messages don't reach the instrument");
        {
            MidiRouteProcessor route (1);
            juce::AudioBuffer<float> audio (2, 64);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::midiClock(), 0);
            midi.addEvent (juce::MidiMessage::midiStart(), 1);
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 90), 2);
            route.processBlock (audio, midi);
            expectEquals (midi.getNumEvents(), 1);
            expect ((*midi.begin()).getMessage().isNoteOn());
        }

        beginTest ("a full monitor drops, it never blocks the audio thread");
        {
            MidiMonitor monitor;
            MidiMonitor::Event event;

            for (int i = 0; i < 9000; ++i)
                monitor.push (event);

            expectEquals ((int) monitor.drain().size(), 8191);   // an AbstractFifo keeps one slot free
            expect (monitor.takeDropped() > 0);
        }
    }
};

static MidiMonitorTests midiMonitorTests;
