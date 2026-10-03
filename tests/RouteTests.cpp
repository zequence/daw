#include <juce_audio_processors/juce_audio_processors.h>

#include "../src/engine/MidiRouteProcessor.h"

class RouteTests final : public juce::UnitTest
{
public:
    RouteTests() : UnitTest ("MidiRouteProcessor") {}

    void runTest() override
    {
        juce::AudioBuffer<float> audio (1, 512);

        beginTest ("channel messages are rewritten to the target channel");
        {
            MidiRouteProcessor route (5);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 10);
            midi.addEvent (juce::MidiMessage::controllerEvent (3, 1, 64), 20);
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 30);

            route.processBlock (audio, midi);

            int index = 0;
            for (const auto metadata : midi)
            {
                expectEquals (metadata.getMessage().getChannel(), 5);
                ++index;
            }
            expectEquals (index, 3);
        }

        beginTest ("disabling the route releases held notes and drops events");
        {
            MidiRouteProcessor route (2);

            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90), 0);
            route.processBlock (audio, midi);
            expectEquals (countEvents (midi), 1);

            route.setRouteEnabled (false);
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOn (1, 65, (juce::uint8) 90), 0);   // should be dropped
            route.processBlock (audio, midi);

            const auto events = allEvents (midi);
            expectEquals ((int) events.size(), 1);
            expect (events[0].isNoteOff());
            expectEquals (events[0].getNoteNumber(), 64);
            expectEquals (events[0].getChannel(), 2);

            // Stays silent while disabled
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOn (1, 66, (juce::uint8) 90), 0);
            route.processBlock (audio, midi);
            expectEquals (countEvents (midi), 0);

            // And passes again when re-enabled
            route.setRouteEnabled (true);
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 90), 0);
            route.processBlock (audio, midi);
            expectEquals (countEvents (midi), 1);
        }

        beginTest ("changing the channel mid-note releases the old channel first");
        {
            MidiRouteProcessor route (3);

            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 90), 0);
            route.processBlock (audio, midi);

            route.setTargetChannel (9);
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOn (1, 74, (juce::uint8) 90), 100);
            route.processBlock (audio, midi);

            const auto events = allEvents (midi);
            expectEquals ((int) events.size(), 2);
            expect (events[0].isNoteOff());
            expectEquals (events[0].getChannel(), 3);
            expectEquals (events[0].getNoteNumber(), 72);
            expect (events[1].isNoteOn());
            expectEquals (events[1].getChannel(), 9);
        }
    }

private:
    static int countEvents (const juce::MidiBuffer& midi)
    {
        int count = 0;
        for (const auto metadata : midi) { juce::ignoreUnused (metadata); ++count; }
        return count;
    }

    static std::vector<juce::MidiMessage> allEvents (const juce::MidiBuffer& midi)
    {
        std::vector<juce::MidiMessage> events;
        for (const auto metadata : midi)
            events.push_back (metadata.getMessage());
        return events;
    }
};

static RouteTests routeTests;
