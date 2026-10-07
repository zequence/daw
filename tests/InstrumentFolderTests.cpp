#include "../src/AudioEngine.h"

// Instrument folders in the MIDI tree (AudioEngine::getSidebarItems): an instrument's tracks (those
// whose first output it is) gather under it where its first track would stand, then its audio;
// collapsed by default
class InstrumentFolderTests final : public juce::UnitTest
{
public:
    InstrumentFolderTests() : UnitTest ("Instrument folders") {}

    template <typename Condition>
    void pumpUntil (Condition&& isDone, int timeoutMs = 15000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! isDone() && juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    }

    void runTest() override
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);
        AudioEngine engine (settings);

        juce::PluginDescription description;
        bool found = false;

        for (auto& type : engine.getInstrumentTypes())
            if (type.name == "TAL-NoiseMaker" || type.name == "Twin 3")
            {
                description = type;
                found = true;
                break;
            }

        if (! found)
        {
            logMessage ("!!! no TAL-NoiseMaker or Twin 3 in the plugin cache - skipping the instrument folder tests");
            return;
        }

        std::atomic<int> instrument { -1 };
        engine.addInstrument (description, [&instrument] (auto id, const juce::String&) { instrument = id; });
        pumpUntil ([&instrument] { return instrument.load() != -1; });

        if (instrument <= 0)
        {
            expect (false, "instrument failed to load");
            return;
        }

        const auto first = engine.addTrack ("A"), loose = engine.addTrack ("Loose"), second = engine.addTrack ("B");
        engine.addTrackOutput (first, instrument, 1);
        engine.addTrackOutput (second, instrument, 2);
        const auto channel = engine.getAudioChannelForInstrument (instrument);

        beginTest ("collapsed by default: one row where its first track stands");
        {
            const auto items = engine.getSidebarItems (true, true);
            expectEquals ((int) items.size(), 2);
            expectEquals (items[0].instrument, (int) instrument);
            expectEquals (items[1].member, loose);
            expect (engine.getArrangeTrackOrder() == std::vector<AudioEngine::TrackId> { loose });
        }

        beginTest ("expanded: its tracks, then its audio");
        {
            engine.setInstrumentExpanded (instrument, true);
            const auto items = engine.getSidebarItems (true, true);
            expectEquals ((int) items.size(), 5);
            expectEquals (items[0].instrument, (int) instrument);
            expectEquals (items[1].member, first);
            expectEquals (items[2].member, second);
            expectEquals (items[3].channel, channel);
            expectEquals (items[4].member, loose);
            expectEquals (items[1].depth, items[0].depth + 1);
            expect (engine.getInstrumentTracks (instrument) == std::vector<AudioEngine::TrackId> { first, second });
        }

        beginTest ("its tracks in a collapsed folder: it stays there (not listed at the end)");
        {
            const auto folder = engine.addFolder (true, "Strings");
            engine.setTrackFolder (first, folder);
            engine.setTrackFolder (second, folder);
            engine.setFolderCollapsed (folder, true);
            const auto items = engine.getSidebarItems (true, true);

            for (auto& item : items)
                expect (item.instrument != (int) instrument, "listed outside its collapsed folder");

            engine.setFolderCollapsed (folder, false);
            const auto open = engine.getSidebarItems (true, true);
            expect (std::any_of (open.begin(), open.end(), [&] (auto& item) { return item.instrument == (int) instrument && item.parent == folder; }));
        }

        beginTest ("buses: made on request, listed last, channels routed to them, back to the master when one goes");
        {
            expect (engine.getBusIds().empty(), "no buses until one is made");
            const auto bus = engine.addBus ("Strings bus");
            expect (engine.isBus (bus));
            expectEquals (engine.getAudioChannelName (bus), juce::String ("Strings bus"));

            const auto items = engine.getSidebarItems (true, true);
            expectEquals (items.back().channel, bus);

            expect (engine.setAudioChannelOutput (channel, bus));
            expectEquals (engine.getAudioChannelOutput (channel), bus);
            expect (! engine.setAudioChannelOutput (bus, bus), "not into itself");

            engine.removeBus (bus);
            expectEquals (engine.getAudioChannelOutput (channel), 0);
            expect (engine.getBusIds().empty());
        }

                beginTest ("membership follows routing");
        {
            engine.clearTrackOutputs (second);
            expect (engine.getInstrumentTracks (instrument) == std::vector<AudioEngine::TrackId> { first });
            expectEquals (engine.getTrackInstrument (second), 0);
        }
    }
};

static InstrumentFolderTests instrumentFolderTests;
