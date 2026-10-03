#include "../src/AudioEngine.h"
#include "../src/TrackChannelProcessor.h"
#include "../src/model/DemoSequence.h"

// Full-stack check: real audio device, real VST3 instrument, demo sequence, measured at
// the track meter. Needs a working output device and TAL-NoiseMaker in the plugin cache;
// skips (without failing) when either is missing. NOTE: this briefly plays sound.
class EndToEndTests final : public juce::UnitTest
{
public:
    EndToEndTests() : UnitTest ("End to end (device + instrument)") {}

    void runTest() override
    {
        beginTest ("demo sequence produces audio through a real instrument");

        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);

        AudioEngine engine (settings);

        if (engine.getDeviceManager().getCurrentAudioDevice() == nullptr)
        {
            logMessage ("!!! No audio device available - skipping");
            return;
        }

        juce::PluginDescription description;
        bool found = false;

        for (auto& type : engine.getInstrumentTypes())
        {
            if (type.name == "TAL-NoiseMaker")
            {
                description = type;
                found = true;
                break;
            }
        }

        if (! found)
        {
            logMessage ("!!! TAL-NoiseMaker not in the plugin cache - skipping");
            return;
        }

        const auto track = engine.addTrack();

        std::atomic<int> loadResult { 0 };
        engine.loadInstrument (track, description,
                               [&loadResult] (bool ok, const juce::String&) { loadResult = ok ? 1 : -1; });

        pumpUntil ([&loadResult] { return loadResult.load() != 0; }, 15000);
        expect (loadResult == 1, "instrument failed to load");

        if (loadResult != 1)
            return;

        engine.setTrackSequence (track, makeDemoSequence());
        pump (400);   // let the graph rebuild with the new connections

        engine.getTransport().play();

        float maxPeak = 0.0f;
        const auto deadline = juce::Time::getMillisecondCounter() + 2500;

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            pump (100);

            if (auto* channel = engine.getChannel (track))
                maxPeak = juce::jmax (maxPeak, channel->takePeak());
        }

        engine.getTransport().stop();
        pump (300);

        logMessage ("Transport played to " + juce::String (engine.getTransport().getPositionTicks() / (double) Ticks::perQuarterNote, 2)
                      + " quarters; peak level " + juce::String (maxPeak, 4));

        expect (! engine.getTransport().isPlaying(), "transport did not stop");
        expect (maxPeak > 0.001f, "no audio came out of the instrument");
    }

private:
    static void pump (int milliseconds)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
    }

    template <typename Condition>
    static void pumpUntil (Condition&& done, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! done() && juce::Time::getMillisecondCounter() < deadline)
            pump (50);
    }
};

static EndToEndTests endToEndTests;
