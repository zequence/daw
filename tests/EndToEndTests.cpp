#include "../src/AudioEngine.h"
#include "../src/engine/AudioChannelProcessor.h"
#include "../src/model/DemoSequence.h"
#include "TestFlags.h"

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

        if (skipAudibleTests)
        {
            logMessage ("!!! --quiet: skipping audible test");
            return;
        }

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

        std::atomic<int> loadedInstrument { -1 };
        engine.addInstrument (description,
                              [&loadedInstrument] (auto id, const juce::String&) { loadedInstrument = id; });

        pumpUntil ([&loadedInstrument] { return loadedInstrument.load() != -1; }, 15000);
        expect (loadedInstrument > 0, "instrument failed to load");

        if (loadedInstrument <= 0)
            return;

        engine.addTrackOutput (track, loadedInstrument, 1);
        engine.setTrackSequence (track, makeDemoSequence());
        pump (400);   // let the graph rebuild with the new connections

        engine.getTransport().play();

        float maxPeak = 0.0f;
        const auto deadline = juce::Time::getMillisecondCounter() + 2500;
        auto* channel = engine.getAudioChannel (engine.getAudioChannelForInstrument (loadedInstrument));
        expect (channel != nullptr, "instrument has no audio channel");

        // Keep the audible part of the test quiet; the meter is measured after this gain.
        if (channel != nullptr)
            channel->setGain (0.05f);

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            pump (50);

            if (channel != nullptr)
                maxPeak = juce::jmax (maxPeak, channel->getLastPeak());
        }

        engine.getTransport().stop();
        pump (300);

        logMessage ("Transport played to " + juce::String (engine.getTransport().getPositionTicks() / (double) Ticks::perQuarterNote, 2)
                      + " quarters; peak level " + juce::String (maxPeak, 4));

        expect (! engine.getTransport().isPlaying(), "transport did not stop");
        expect (maxPeak > 0.001f, "no audio came out of the instrument");

        //======================================================================
        // Live input reaches ONLY the armed track (live MIDI is wired to every
        // source permanently and gated by arming - no graph changes on arm).
        beginTest ("live input plays through the armed track only");

        engine.setTrackSequence (track, nullptr);
        const auto otherTrack = engine.addTrack();   // no outputs: arming it must silence live input
        pump (400);

        const auto playLiveAndMeasure = [&]
        {
            const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;
            auto on = juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100);
            on.setTimeStamp (now);
            engine.getLiveMidiCollector().addMessageToQueue (on);

            float peak = 0.0f;
            const auto until = juce::Time::getMillisecondCounter() + 700;

            while (juce::Time::getMillisecondCounter() < until)
            {
                pump (50);

                if (channel != nullptr)
                    peak = juce::jmax (peak, channel->getLastPeak());
            }

            auto off = juce::MidiMessage::noteOff (1, 64);
            off.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
            engine.getLiveMidiCollector().addMessageToQueue (off);
            pump (600);   // let the release tail die before the next measurement
            return peak;
        };

        engine.setArmedTrack (otherTrack);
        pump (100);
        const auto unarmedPeak = playLiveAndMeasure();

        engine.setArmedTrack (track);
        pump (100);
        const auto armedPeak = playLiveAndMeasure();

        logMessage ("live peak - other track armed: " + juce::String (unarmedPeak, 4)
                      + ", instrument's track armed: " + juce::String (armedPeak, 4));

        expect (armedPeak > 0.001f, "live input didn't reach the armed track's instrument");
        expect (unarmedPeak < 0.0005f, "live input leaked to an unarmed track's instrument");
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
