#include "../src/engine/AudioChannelProcessor.h"

// The mixer strip (AudioChannelProcessor): pan law, mute, solo-silencing and the meters
class MixerTests final : public juce::UnitTest
{
public:
    MixerTests() : UnitTest ("Mixer") {}

    void runTest() override
    {
        // One block of a steady 0.5 on both sides through a strip, twice (the second has no ramp)
        const auto run = [] (AudioChannelProcessor& strip)
        {
            juce::MidiBuffer midi;
            strip.prepareToPlay (48000.0, 64);

            for (int pass = 0; pass < 2; ++pass)
            {
                juce::AudioBuffer<float> buffer (2, 64);

                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.5f, 64);

                strip.processBlock (buffer, midi);

                if (pass == 1)
                    return std::pair<float, float> { buffer.getSample (0, 63), buffer.getSample (1, 63) };
            }

            return std::pair<float, float> {};
        };

        beginTest ("centre pan leaves the level as it was; hard left sends it all left (+3 dB)");
        {
            AudioChannelProcessor strip;
            auto [left, right] = run (strip);
            expectWithinAbsoluteError (left, 0.5f, 1.0e-4f);
            expectWithinAbsoluteError (right, 0.5f, 1.0e-4f);

            strip.setPan (-1.0f);
            std::tie (left, right) = run (strip);
            expectWithinAbsoluteError (left, 0.5f * juce::MathConstants<float>::sqrt2, 1.0e-4f);
            expectWithinAbsoluteError (right, 0.0f, 1.0e-4f);

            // Constant power: left^2 + right^2 is the same wherever it is panned
            strip.setPan (0.4f);
            std::tie (left, right) = run (strip);
            expectWithinAbsoluteError (left * left + right * right, 0.5f, 1.0e-3f);
        }

        beginTest ("mute and solo-silencing make it silent; the meters follow the output");
        {
            AudioChannelProcessor strip;
            strip.setGain (0.5f);
            auto [left, right] = run (strip);
            expectWithinAbsoluteError (left, 0.25f, 1.0e-4f);
            expectWithinAbsoluteError (strip.getLastPeak(), 0.25f, 1.0e-4f);
            expectWithinAbsoluteError (strip.getLastRms(), 0.25f, 1.0e-4f);

            strip.setMuted (true);
            std::tie (left, right) = run (strip);
            expectEquals (left + right, 0.0f);

            strip.setMuted (false);
            strip.setSoloSilenced (true);
            std::tie (left, right) = run (strip);
            expectEquals (left + right, 0.0f);
            expectEquals (strip.getLastPeak(), 0.0f);
        }

        {
            // The master bus to the device output: the graph's output node has no channels until
            // the graph is configured, so the connection must be (re)made after that (AudioEngine
            // ::connectMasterOutput) - made before, it's refused and nothing is heard
            beginTest ("the master bus reaches the output once the graph has its channels");
            using IO = juce::AudioProcessorGraph::AudioGraphIOProcessor;
            juce::AudioProcessorGraph graph;
            const auto out = graph.addNode (std::make_unique<IO> (IO::audioOutputNode))->nodeID;
            const auto master = graph.addNode (std::make_unique<AudioChannelProcessor>())->nodeID;
            expect (! graph.addConnection ({ { master, 0 }, { out, 0 } }), "refused before the graph has channels");

            graph.setPlayConfigDetails (0, 2, 48000.0, 64);
            graph.prepareToPlay (48000.0, 64);
            expect (graph.addConnection ({ { master, 0 }, { out, 0 } }));
            expect (graph.addConnection ({ { master, 1 }, { out, 1 } }));
        }
    }
};

static MixerTests mixerTests;
