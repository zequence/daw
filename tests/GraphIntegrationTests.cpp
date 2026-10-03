#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "../src/engine/Transport.h"
#include "../src/engine/MidiSourceProcessor.h"
#include "../src/model/DemoSequence.h"

namespace
{
    // Stand-in for an instrument: accepts MIDI, records what arrives.
    struct MidiCaptureProcessor final : juce::AudioProcessor
    {
        MidiCaptureProcessor()
            : AudioProcessor (BusesProperties()
                                  .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
        {
            buffer.clear();
            for (const auto metadata : midi)
                captured.push_back (metadata.getMessage());
        }

        const juce::String getName() const override             { return "Capture"; }
        bool acceptsMidi() const override                       { return true; }
        bool producesMidi() const override                      { return false; }
        void prepareToPlay (double, int) override               {}
        void releaseResources() override                        {}
        double getTailLengthSeconds() const override            { return 0.0; }
        juce::AudioProcessorEditor* createEditor() override     { return nullptr; }
        bool hasEditor() const override                         { return false; }
        int getNumPrograms() override                           { return 1; }
        int getCurrentProgram() override                        { return 0; }
        void setCurrentProgram (int) override                   {}
        const juce::String getProgramName (int) override        { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override  {}
        void setStateInformation (const void*, int) override    {}

        std::vector<juce::MidiMessage> captured;
    };
}

//==============================================================================
// The unit tests call MidiSourceProcessor::processBlock directly; this one checks the
// path the app actually uses - source node -> instrument node inside AudioProcessorGraph.
class GraphIntegrationTests final : public juce::UnitTest
{
public:
    GraphIntegrationTests() : UnitTest ("Graph integration") {}

    void runTest() override
    {
        beginTest ("sequence MIDI flows through the graph (topology before prepare)");
        runCase (false);

        beginTest ("sequence MIDI flows through the graph (topology after prepare, app order)");
        runCase (true);
    }

private:
    void runCase (bool prepareFirst)
    {
        Transport transport;
        transport.prepare (48000.0);

        juce::AudioProcessorGraph graph;
        graph.setPlayConfigDetails (0, 2, 48000.0, 480);

        if (prepareFirst)
            graph.prepareToPlay (48000.0, 480);

        using IO = juce::AudioProcessorGraph::AudioGraphIOProcessor;
        const auto outNode = graph.addNode (std::make_unique<IO> (IO::audioOutputNode))->nodeID;

        auto* source = new MidiSourceProcessor (transport);
        const auto sourceNode = graph.addNode (std::unique_ptr<juce::AudioProcessor> (source))->nodeID;

        auto* capture = new MidiCaptureProcessor();
        const auto captureNode = graph.addNode (std::unique_ptr<juce::AudioProcessor> (capture))->nodeID;

        const auto midiChannel = juce::AudioProcessorGraph::midiChannelIndex;
        expect (graph.addConnection ({ { sourceNode, midiChannel }, { captureNode, midiChannel } }),
                "MIDI connection was refused");

        for (int ch = 0; ch < 2; ++ch)
            expect (graph.addConnection ({ { captureNode, ch }, { outNode, ch } }), "audio connection was refused");

        source->setSequence (makeDemoSequence());

        if (prepareFirst)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);   // let the graph rebuild, as in the app
        else
            graph.prepareToPlay (48000.0, 480);

        transport.play();

        juce::AudioBuffer<float> buffer (2, 480);
        juce::MidiBuffer midi;

        for (int i = 0; i < 10; ++i)
        {
            transport.beginBlock (480);
            midi.clear();
            graph.processBlock (buffer, midi);
        }

        int noteOns = 0, controllers = 0;
        for (const auto& message : capture->captured)
        {
            noteOns += message.isNoteOn() ? 1 : 0;
            controllers += message.isController() ? 1 : 0;
        }

        expect (noteOns >= 1, "no note-ons arrived through the graph");
        expect (controllers >= 1, "no controllers arrived through the graph");
    }
};

static GraphIntegrationTests graphIntegrationTests;
