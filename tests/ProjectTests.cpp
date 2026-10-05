#include "../src/AudioEngine.h"
#include "../src/engine/AudioChannelProcessor.h"
#include "../src/model/DemoSequence.h"

namespace
{
    constexpr auto Q = Ticks::perQuarterNote;
}

//==============================================================================
class SerializationTests final : public juce::UnitTest
{
public:
    SerializationTests() : UnitTest ("Serialization") {}

    void runTest() override
    {
        beginTest ("tempo map xml round trip");
        {
            auto original = TempoMap::create (97.5)
                                ->withTempoChange (8 * Q, 140.0)
                                ->withMeterChange (8 * Q, 7, 8);

            auto restored = TempoMap::fromXml (*original->toXml());

            expectEquals ((int) restored->getTempoChanges().size(), 2);
            expectEquals ((int) restored->getMeterChanges().size(), 2);
            expectEquals (restored->getTempoAt (0), 97.5);
            expectEquals (restored->getTempoAt (9 * Q), 140.0);
            expectEquals (restored->getMeterAt (9 * Q).numerator, 7);
            expectEquals (restored->ticksToSeconds (16 * Q), original->ticksToSeconds (16 * Q));
        }

        beginTest ("midi sequence xml round trip");
        {
            auto original = makeDemoSequence();
            auto restored = MidiSequence::fromXml (*original->toXml());

            expectEquals ((int) restored->getNotes().size(), (int) original->getNotes().size());
            expectEquals ((int) restored->getControls().size(), (int) original->getControls().size());
            expectEquals (restored->getLengthTicks(), original->getLengthTicks());

            for (size_t i = 0; i < original->getNotes().size(); ++i)
            {
                expectEquals (restored->getNotes()[i].startTick, original->getNotes()[i].startTick);
                expectEquals (restored->getNotes()[i].lengthTicks, original->getNotes()[i].lengthTicks);
                expectEquals (restored->getNotes()[i].key, original->getNotes()[i].key);
                expectEquals (restored->getNotes()[i].velocity, original->getNotes()[i].velocity);
            }
        }
    }
};

//==============================================================================
// Full save -> clear -> load round trip through the real engine, with a real plugin.
// Needs TAL-NoiseMaker in the cache; skips (without failing) when missing.
class ProjectRoundTripTests final : public juce::UnitTest
{
public:
    ProjectRoundTripTests() : UnitTest ("Project round trip") {}

    void runTest() override
    {
        beginTest ("save, clear and load restore the whole project");

        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);

        AudioEngine engine (settings);

        juce::PluginDescription description;
        bool found = false;

        for (auto& type : engine.getInstrumentTypes())
            if (type.name == "TAL-NoiseMaker")
            {
                description = type;
                found = true;
                break;
            }

        if (! found)
        {
            logMessage ("!!! TAL-NoiseMaker not in the plugin cache - skipping");
            return;
        }

        // --- Build a project ---
        std::atomic<int> instrumentId { -1 };
        engine.addInstrument (description, [&instrumentId] (auto id, const juce::String&) { instrumentId = id; });
        pumpUntil ([&instrumentId] { return instrumentId.load() != -1; });
        expect (instrumentId > 0, "instrument failed to load");

        if (instrumentId <= 0)
            return;

        const auto track = engine.addTrack();
        engine.setTrackName (track, "Lead");
        engine.addTrackOutput (track, instrumentId, 5);
        engine.setTrackSequence (track, makeDemoSequence());
        engine.setTrackMuted (track, true);
        engine.setInstrumentChannelName (instrumentId, 5, "Solo");
        engine.setTempoBpm (93.0);

        auto* channelBefore = engine.getAudioChannel (engine.getAudioChannelForInstrument (instrumentId));
        expect (channelBefore != nullptr);
        channelBefore->setGain (0.5f);

        auto* pluginBefore = engine.getInstrumentPlugin (instrumentId);
        expect (pluginBefore != nullptr && pluginBefore->getParameters().size() > 4);
        // A VST3 plugin takes a host-set parameter into its processor on its next audio block, and
        // its saved state comes from the processor: wait until the state has it (slow Debug builds
        // could otherwise save before a block ran)
        juce::MemoryBlock stateBefore;
        pluginBefore->getStateInformation (stateBefore);
        pluginBefore->getParameters()[4]->setValue (0.731f);

        pumpUntil ([pluginBefore, &stateBefore]
        {
            juce::MemoryBlock now;
            pluginBefore->getStateInformation (now);
            return now != stateBefore;
        }, 5000);

        // --- Save ---
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("OrchestralDAWRoundTrip.odaw");
        expect (engine.saveProject (file), "save failed");

        // --- Clear and verify emptiness ---
        engine.clearProject();
        expect (engine.getTrackIds().empty() && engine.getInstruments().empty());

        // --- Load ---
        std::atomic<int> loadDone { 0 };
        juce::String loadWarnings;
        engine.loadProject (file, [&loadDone, &loadWarnings] (bool ok, const juce::String& warnings)
        {
            loadWarnings = warnings;
            loadDone = ok ? 1 : -1;
        });
        pumpUntil ([&loadDone] { return loadDone.load() != 0; });

        expect (loadDone == 1, "load failed");
        expectEquals (loadWarnings, juce::String());

        // --- Verify ---
        const auto trackIds = engine.getTrackIds();
        expectEquals ((int) trackIds.size(), 1);
        const auto newTrack = trackIds.front();

        expectEquals (engine.getTrackName (newTrack), juce::String ("Lead"));
        expect (engine.isTrackMuted (newTrack));
        expectEquals (engine.getArmedTrack(), newTrack);

        const auto outputs = engine.getTrackOutputs (newTrack);
        expectEquals ((int) outputs.size(), 1);
        expectEquals (outputs.front().midiChannel, 5);

        const auto newInstrument = outputs.front().instrument;
        expect (engine.getInstrumentName (newInstrument).contains ("NoiseMaker"));
        expectEquals (engine.getInstrumentChannelName (newInstrument, 5), juce::String ("Solo"));

        auto sequence = engine.getTrackSequence (newTrack);
        expect (sequence != nullptr);

        if (sequence != nullptr)
            expectEquals ((int) sequence->getNotes().size(), (int) makeDemoSequence()->getNotes().size());

        expectWithinAbsoluteError (engine.getTempoBpm(), 93.0, 0.01);

        auto* channelAfter = engine.getAudioChannel (engine.getAudioChannelForInstrument (newInstrument));
        expect (channelAfter != nullptr);

        if (channelAfter != nullptr)
            expectWithinAbsoluteError (channelAfter->getGain(), 0.5f, 0.001f);

        auto* pluginAfter = engine.getInstrumentPlugin (newInstrument);
        expect (pluginAfter != nullptr && pluginAfter->getParameters().size() > 4);

        if (pluginAfter != nullptr && pluginAfter->getParameters().size() > 4)
            expectWithinAbsoluteError (pluginAfter->getParameters()[4]->getValue(), 0.731f, 0.01f);

        file.deleteFile();
    }

private:
    template <typename Condition>
    static void pumpUntil (Condition&& isDone, int timeoutMs = 15000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! isDone() && juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    }
};

static SerializationTests serializationTests;
static ProjectRoundTripTests projectRoundTripTests;
