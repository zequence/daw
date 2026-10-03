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

        beginTest ("explicit ordering and group moves (the drag operation)");
        {
            const auto t1 = engine.addTrack ("A");
            const auto t2 = engine.addTrack ("B");
            const auto t3 = engine.addTrack ("C");
            const auto t4 = engine.addTrack ("D");

            // Creation order first
            expect (engine.getArrangeTrackOrder() == std::vector<AudioEngine::TrackId> { t1, t2, t3, t4 });

            // Move D to the front
            expect (engine.moveSidebarItems (true, {}, { t4 }, 0, 0));
            expect (engine.getArrangeTrackOrder() == std::vector<AudioEngine::TrackId> { t4, t1, t2, t3 });

            // Group move: A and C (visual order) land together before B
            expect (engine.moveSidebarItems (true, {}, { t1, t3 }, 0, 1));
            expect (engine.getArrangeTrackOrder() == std::vector<AudioEngine::TrackId> { t4, t1, t3, t2 });

            // Into a folder at the end; the folder itself re-orders among siblings
            const auto f = engine.addFolder (true, "F");
            expect (engine.moveSidebarItems (true, {}, { t4, t2 }, f, 0));
            expect (engine.getTrackFolder (t4) == f && engine.getTrackFolder (t2) == f);
            expect (engine.moveSidebarItems (true, { f }, {}, 0, 0));

            const auto items = engine.getSidebarItems (true, true);
            expect (items[0].folder == f);
            expect (items[1].member == t4 && items[2].member == t2);
            expect (items[3].member == t1 && items[4].member == t3);

            // A folder can't move into its own subtree
            const auto inner = engine.addFolder (true, "Inner", f);
            expect (! engine.moveSidebarItems (true, { f }, {}, inner, 0));

            // Order survives a save/load round-trip
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("order-roundtrip.odaw");
            expect (engine.saveProject (file));

            std::vector<juce::String> namesBefore, namesAfter;
            for (auto id : engine.getArrangeTrackOrder()) namesBefore.push_back (engine.getTrackName (id));

            bool loaded = false;
            engine.loadProject (file, [&loaded] (bool ok, const juce::String&) { loaded = ok; });
            expect (loaded);

            for (auto id : engine.getArrangeTrackOrder()) namesAfter.push_back (engine.getTrackName (id));
            expect (namesBefore == namesAfter);   // ids change on load, the order doesn't

            // ...and a history snapshot restores it
            const auto before = engine.captureHistorySnapshot();
            const auto order = engine.getArrangeTrackOrder();
            expect (engine.moveSidebarItems (true, {}, { order.back() }, 0, 0));
            expect (engine.getArrangeTrackOrder() != order);
            engine.applyHistorySnapshot (before);
            expect (engine.getArrangeTrackOrder() == order);

            file.deleteFile();
            engine.clearProject();
        }

        beginTest ("sidebar.move command");
        {
            const auto a = (int) sendFolderCmd (dispatcher, "track.create", R"({"name":"One"})")["result"]["id"];
            const auto b = (int) sendFolderCmd (dispatcher, "track.create", R"({"name":"Two"})")["result"]["id"];

            expect (sendFolderCmd (dispatcher, "sidebar.move",
                                   R"({"domain":"midi","members":[)" + juce::String (b) + R"(],"parent":0,"index":0})")
                        .getProperty ("ok", false));
            expect (engine.getArrangeTrackOrder() == std::vector<AudioEngine::TrackId> { b, a });

            expect (! sendFolderCmd (dispatcher, "sidebar.move",
                                     R"({"domain":"midi","members":[9999],"parent":0,"index":0})")
                        .getProperty ("ok", false));

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
