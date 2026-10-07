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

                beginTest ("a grouped folder sums the audio inside it on its own bus, named as the folder");
        {
            const auto folder = engine.addFolder (true, "Winds");
            engine.setTrackFolder (first, folder);
            engine.setTrackFolder (second, folder);
            engine.setFolderGrouped (folder, true);
            const auto bus = engine.getFolderGroupBus (folder);

            expect (engine.isGroupBus (bus));
            expectEquals (engine.getAudioChannelOutput (channel), bus);
            expectEquals (engine.getAudioChannelName (bus), juce::String ("Winds"));

            engine.setFolderName (folder, "Woodwinds");
            expectEquals (engine.getAudioChannelName (bus), juce::String ("Woodwinds"));

            engine.setTrackFolder (first, 0);   // the instrument's folder leaves the group: back to the master
            engine.setTrackFolder (second, 0);
            expectEquals (engine.getAudioChannelOutput (channel), 0);

            engine.setTrackFolder (first, folder);
            engine.setTrackFolder (second, folder);
            engine.setFolderGrouped (folder, false);
            expectEquals (engine.getAudioChannelOutput (channel), 0);
            expect (engine.getBusIds().empty(), "its bus goes with the group");
        }

                beginTest ("an instrument with one output is its channel: tagged itself, never grouped");
        {
            engine.setInstrumentExpanded (instrument, true);
            auto instrumentTagged = [&]
            {
                for (auto& item : engine.getSidebarItems (true, true))
                    if (item.instrument == (int) instrument)
                        return item.tagged;

                return false;
            };

            expect (engine.isSingleOutputInstrument (instrument));
            expect (instrumentTagged(), "one output: the instrument carries its channel's tag");

            engine.setInstrumentGrouped (instrument, true);
            expect (! engine.isInstrumentGrouped (instrument), "one output: nothing to group");
            expectEquals (engine.getAudioChannelOutput (channel), 0);
            expect (engine.getBusIds().empty());
        }

                beginTest ("groups within groups: an instrument in a grouped folder feeds the folder's group, untagged");
        {
            const auto outerFolder = engine.addFolder (true, "Orchestra");
            const auto folder = engine.addFolder (true, "Brass");
            engine.setFolderParent (folder, outerFolder);
            engine.setTrackFolder (first, folder);
            engine.setTrackFolder (second, folder);
            engine.setFolderGrouped (folder, true);
            engine.setFolderGrouped (outerFolder, true);

            const auto inner = engine.getFolderGroupBus (folder), outer = engine.getFolderGroupBus (outerFolder);
            expectEquals (engine.getAudioChannelOutput (channel), inner);
            expectEquals (engine.getAudioChannelOutput (inner), outer);
            expect (! engine.setAudioChannelOutput (outer, inner), "no loops");

            bool instrumentTagged = true;

            for (auto& item : engine.getSidebarItems (true, true))
                if (item.instrument == (int) instrument)
                    instrumentTagged = item.tagged;

            expect (! instrumentTagged, "inside a group: no tag");

            engine.setFolderGrouped (outerFolder, false);
            expectEquals (engine.getAudioChannelOutput (inner), 0);
            engine.setFolderGrouped (folder, false);
            expectEquals (engine.getAudioChannelOutput (channel), 0);
            engine.setTrackFolder (first, 0);
            engine.setTrackFolder (second, 0);
        }

                beginTest ("audio tracks: rows in the tree where they're put, tagged; mono or stereo; removed");
        {
            const auto folder = engine.addFolder (true, "Takes");
            const auto vocal = engine.addAudioTrack ("Vocal", false, folder);
            expect (engine.isAudioTrack (vocal) && ! engine.isAudioTrackStereo (vocal));
            expectEquals (engine.getAudioChannelName (vocal), juce::String ("Vocal"));

            bool found = false;

            for (auto& item : engine.getSidebarItems (true, false))
                if (item.channel == vocal)
                    found = item.parent == folder && item.tagged;

            expect (found, "in its folder, with a tag");

            engine.setAudioTrackColour (vocal, "ff5599cc");
            expectEquals (engine.getChannelTagColour (vocal), juce::String ("ff5599cc"));

            engine.removeAudioTrack (vocal);
            expect (! engine.isAudioTrack (vocal));
        }

                beginTest ("moving: folders, tracks, audio tracks and instruments together; an instrument's tracks reordered in it");
        {
            using N = AudioEngine::TreeNode;
            const auto folder = engine.addFolder (true, "Target");
            const auto take = engine.addAudioTrack ("Take");

            // The rows of a folder, as (kind, id): its slots in order
            auto slotsOf = [&] (AudioEngine::FolderId parent)
            {
                std::vector<std::pair<char, int>> rows;

                for (auto& item : engine.getSidebarItems (true, false))
                    if (item.parent == parent && engine.isTreeSlot (item))
                        rows.push_back (item.folder != 0 ? std::pair<char, int> { 'f', item.folder }
                                        : item.instrument != 0 ? std::pair<char, int> { 'i', item.instrument }
                                        : item.member != 0 ? std::pair<char, int> { 't', item.member }
                                                           : std::pair<char, int> { 'a', item.channel });
                return rows;
            };

            expect (engine.moveTreeNodes ({ { N::Kind::audioTrack, take }, { N::Kind::track, loose }, { N::Kind::instrument, instrument } }, folder, 0));
            expect (slotsOf (folder) == std::vector<std::pair<char, int>> { { 'a', take }, { 't', loose }, { 'i', instrument } },
                    "all three in the folder, in the order given");
            expectEquals (engine.getTrackFolder (first), folder);
            expectEquals (engine.getTrackFolder (second), folder);

            expect (engine.moveTreeNodes ({ { N::Kind::track, loose } }, folder, 0));   // to the top: the audio track after it, not in its place
            expect (slotsOf (folder) == std::vector<std::pair<char, int>> { { 't', loose }, { 'a', take }, { 'i', instrument } });

            expect (! engine.moveTreeNodes ({ { N::Kind::folder, folder } }, folder, 0), "not into itself");

            engine.reorderInstrumentTracks (instrument, { second, first });
            expect (engine.getInstrumentTracks (instrument) == std::vector<AudioEngine::TrackId> { second, first });
            engine.reorderInstrumentTracks (instrument, { first, second });

            expect (engine.moveTreeNodes ({ { N::Kind::instrument, instrument }, { N::Kind::track, loose } }, 0, 0));
            engine.removeAudioTrack (take);
            engine.removeFolder (folder);
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
