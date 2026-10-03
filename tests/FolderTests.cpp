#include "../src/api/CommandDispatcher.h"

namespace
{
    juce::var sendFolderCmd (CommandDispatcher& dispatcher, const juce::String& cmd, const juce::String& paramsJson = "{}")
    {
        juce::var reply;
        dispatcher.dispatch (R"({"id":1,"cmd":")" + cmd + R"(","params":)" + paramsJson + "}",
                             [&reply] (const juce::var& r) { reply = r; });
        return reply;
    }
}

//==============================================================================
class FolderTests final : public juce::UnitTest
{
public:
    FolderTests() : UnitTest ("Folders") {}

    void runTest() override
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);

        AudioEngine engine (settings);
        CommandDispatcher dispatcher (engine);

        beginTest ("nesting, membership and sidebar order");
        {
            const auto strings = engine.addFolder (true, "Strings");
            const auto violins = engine.addFolder (true, "Violins", strings);
            const auto t1 = engine.addTrack ("Vln 1");
            const auto t2 = engine.addTrack ("Celli");
            const auto t3 = engine.addTrack ("Piano");

            engine.setTrackFolder (t1, violins);
            engine.setTrackFolder (t2, strings);

            // Depth-first: Strings(0) > Violins(1) > Vln1(2), Celli(1), then Piano at root
            const auto items = engine.getSidebarItems (true, true);
            expectEquals ((int) items.size(), 5);
            expect (items[0].folder == strings && items[0].depth == 0);
            expect (items[1].folder == violins && items[1].depth == 1);
            expect (items[2].member == t1 && items[2].depth == 2);
            expect (items[3].member == t2 && items[3].depth == 1);
            expect (items[4].member == t3 && items[4].depth == 0);

            const auto order = engine.getArrangeTrackOrder();
            expect (order == std::vector<AudioEngine::TrackId> { t1, t2, t3 });

            // Collapsing Strings hides everything inside it
            engine.setFolderCollapsed (strings, true);
            const auto collapsed = engine.getSidebarItems (true, true);
            expectEquals ((int) collapsed.size(), 2);   // Strings + Piano
            expect (engine.getArrangeTrackOrder() == std::vector<AudioEngine::TrackId> { t3 });
            engine.setFolderCollapsed (strings, false);

            // Cycles and cross-domain parents are rejected
            expect (! engine.setFolderParent (strings, violins));
            expect (! engine.setFolderParent (strings, strings));
            const auto audioFolder = engine.addFolder (false, "Mix");
            expect (! engine.setFolderParent (audioFolder, strings));

            // Removing a folder moves its contents up
            engine.removeFolder (violins);
            expect (engine.getTrackFolder (t1) == strings);
            expect (! engine.folderExists (violins));

            // History snapshots restore the tree
            const auto before = engine.captureHistorySnapshot();
            engine.setTrackFolder (t1, 0);
            engine.removeFolder (strings);
            engine.applyHistorySnapshot (before);
            expect (engine.folderExists (strings));
            expect (engine.getTrackFolder (t1) == strings);

            engine.clearProject();
        }

        beginTest ("folder commands");
        {
            const auto created = sendFolderCmd (dispatcher, "folder.create", R"({"domain":"midi","name":"Brass"})");
            expect (created.getProperty ("ok", false));
            const auto folderId = (int) created["result"].getProperty ("folderId", 0);
            expect (folderId > 0);

            expect (! sendFolderCmd (dispatcher, "folder.create", R"({"domain":"nope"})").getProperty ("ok", false));

            const auto sub = sendFolderCmd (dispatcher, "folder.create",
                                            R"({"domain":"midi","name":"Trumpets","parent":)" + juce::String (folderId) + "}");
            const auto subId = (int) sub["result"].getProperty ("folderId", 0);

            // Cycle through the API fails politely
            expect (! sendFolderCmd (dispatcher, "folder.setParent",
                                     R"({"folderId":)" + juce::String (folderId) + R"(,"parent":)" + juce::String (subId) + "}")
                        .getProperty ("ok", false));

            const auto track = sendFolderCmd (dispatcher, "track.create", R"({"name":"Trp 1"})");
            const auto trackId = (int) track["result"].getProperty ("id", 0);

            expect (sendFolderCmd (dispatcher, "track.setFolder",
                                   R"({"trackId":)" + juce::String (trackId) + R"(,"folderId":)" + juce::String (subId) + "}")
                       .getProperty ("ok", false));
            expect (engine.getTrackFolder (trackId) == subId);

            // An audio folder can't take a track
            const auto mix = sendFolderCmd (dispatcher, "folder.create", R"({"domain":"audio","name":"Mix"})");
            const auto mixId = (int) mix["result"].getProperty ("folderId", 0);
            expect (! sendFolderCmd (dispatcher, "track.setFolder",
                                     R"({"trackId":)" + juce::String (trackId) + R"(,"folderId":)" + juce::String (mixId) + "}")
                        .getProperty ("ok", false));

            const auto list = sendFolderCmd (dispatcher, "folder.list", R"({"domain":"midi"})")["result"];
            expect (list.getArray() != nullptr && list.getArray()->size() == 2);

            engine.clearProject();
        }

        beginTest ("folders survive a project round-trip");
        {
            const auto strings = engine.addFolder (true, "Strings");
            const auto violins = engine.addFolder (true, "Violins", strings);
            engine.setFolderCollapsed (violins, true);

            const auto t1 = engine.addTrack ("Vln 1");
            engine.setTrackFolder (t1, violins);

            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("folder-roundtrip.odaw");
            expect (engine.saveProject (file));

            bool loaded = false;
            engine.loadProject (file, [&loaded] (bool ok, const juce::String&) { loaded = ok; });
            expect (loaded);   // no instruments, so the load completes synchronously

            const auto items = engine.getSidebarItems (true, false);
            expectEquals ((int) items.size(), 3);
            expectEquals (engine.getFolderName (items[0].folder), juce::String ("Strings"));
            expectEquals (engine.getFolderName (items[1].folder), juce::String ("Violins"));
            expect (items[1].depth == 1);
            expect (engine.isFolderCollapsed (items[1].folder));
            expect (items[2].member != 0 && items[2].depth == 2);
            expect (engine.getTrackFolder (items[2].member) == items[1].folder);

            // Collapsed folders hide members from the arrangement order
            expect (engine.getArrangeTrackOrder().empty());

            file.deleteFile();
            engine.clearProject();
        }
    }
};

static FolderTests folderTests;
