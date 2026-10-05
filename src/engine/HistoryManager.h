#pragma once

#include "../AudioEngine.h"

// The global action history (ISSUES.md milestone): every engine mutation becomes an
// entry (events arriving in one message-loop burst coalesce into one entry, so one
// user gesture or command = one entry), each carrying a light snapshot for
// time-travel. Project load/new reset the timeline; the plugin rack itself is never
// rewound. Feed onEngineEvent() from the engine's event sink.
class HistoryManager final : private juce::AsyncUpdater
{
public:
    explicit HistoryManager (AudioEngine& e) : engine (e)
    {
        reset();
    }

    ~HistoryManager() override
    {
        cancelPendingUpdate();
    }

    struct Entry
    {
        int id = 0;
        juce::Time time;
        juce::String description, category;   // track | clip | instrument | marker | tempo | recording | project
        int trackId = 0;
        AudioEngine::HistorySnapshot state;   // the project right after this action
    };

    const std::vector<Entry>& getEntries() const noexcept { return entries; }
    int getCurrentEntryId() const noexcept                { return entries.empty() ? 0 : entries[(size_t) current].id; }

    //==============================================================================
    void onEngineEvent (const juce::var& event)
    {
        const auto type = event.getProperty ("event", {}).toString();

        // Not history material: our own travels, state-free notifications, view state.
        // Arming follows track selection - not an edit (and each entry snapshots
        // the whole project, which is costly with 1000+ tracks). Key ranges are
        // fetched server facts, not edits.
        if (type == "historyTravelled" || type == "recordingStarted" || type == "projectSaved"
             || type == "folderViewChanged"
             || (type == "trackChanged" && event.getProperty ("change", {}).toString() == "armed")
             || (type == "instrumentChanged" && event.getProperty ("change", {}).toString() == "keyRange"))
            return;

        if (type == "projectCleared" || type == "projectLoaded")
        {
            resetPending = true;
            burstActive = false;
            triggerAsyncUpdate();
            return;
        }

        // Within one burst, the most descriptive event names the entry
        const auto priority = priorityFor (type);

        if (! burstActive || priority >= burstPriority)
        {
            burstPriority = priority;
            burstEvent = event;
        }

        burstActive = true;
        triggerAsyncUpdate();
    }

    bool travelTo (int entryId)
    {
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (entries[i].id == entryId)
            {
                engine.applyHistorySnapshot (entries[i].state);
                current = (int) i;

                if (onChanged)
                    onChanged();

                return true;
            }
        }

        return false;
    }

    std::function<void()> onChanged;

private:
    void handleAsyncUpdate() override
    {
        if (resetPending)
        {
            resetPending = false;
            reset();
            return;
        }

        if (! burstActive)
            return;

        burstActive = false;
        burstPriority = -1;

        // A new action cuts off any "future" we had travelled back from
        entries.resize ((size_t) current + 1);

        Entry entry;
        entry.id = ++lastId;
        entry.time = juce::Time::getCurrentTime();
        entry.category = categoryFor (burstEvent.getProperty ("event", {}).toString());
        entry.trackId = (int) burstEvent.getProperty ("trackId", burstEvent.getProperty ("id", 0));
        entry.description = describe (burstEvent);
        entry.state = engine.captureHistorySnapshot();

        entries.push_back (std::move (entry));
        current = (int) entries.size() - 1;

        if (onChanged)
            onChanged();
    }

    void reset()
    {
        entries.clear();

        Entry base;
        base.id = ++lastId;
        base.time = juce::Time::getCurrentTime();
        base.description = "Start";
        base.category = "project";
        base.state = engine.captureHistorySnapshot();
        entries.push_back (std::move (base));
        current = 0;

        if (onChanged)
            onChanged();
    }

    static int priorityFor (const juce::String& type)
    {
        if (type == "recordingFinished") return 90;
        if (type == "trackAdded" || type == "trackRemoved"
             || type == "instrumentAdded" || type == "instrumentRemoved") return 80;
        if (type == "folderAdded" || type == "folderRemoved") return 75;
        if (type == "sidebarMoved") return 45;
        if (type == "markerAdded" || type == "markerRemoved") return 70;
        if (type == "tempoChanged") return 60;
        if (type == "expressionMapChanged") return 55;   // names a map edit that also rewrote clips
        if (type == "clipChanged") return 50;
        return 10;   // trackChanged and anything else
    }

    static juce::String categoryFor (const juce::String& type)
    {
        if (type.startsWith ("track")) return "track";
        if (type.startsWith ("folder")) return "folder";
        if (type.startsWith ("channel")) return "channel";
        if (type.startsWith ("clip")) return "clip";
        if (type.startsWith ("instrument")) return "instrument";
        if (type.startsWith ("marker")) return "marker";
        if (type.startsWith ("expressionMap")) return "expressionmap";
        if (type.startsWith ("tempo")) return "tempo";
        if (type.startsWith ("recording")) return "recording";
        return "project";
    }

    juce::String trackName (int id) const
    {
        const auto name = engine.getTrackName (id);
        return name.isNotEmpty() ? name : "track " + juce::String (id);
    }

    juce::String describe (const juce::var& event) const
    {
        const auto type = event.getProperty ("event", {}).toString();

        if (type == "trackAdded")        return "Add track '" + event.getProperty ("name", {}).toString() + "'";
        if (type == "trackRemoved")      return "Remove track " + event.getProperty ("id", {}).toString();
        if (type == "instrumentAdded")   return "Load instrument '" + event.getProperty ("name", {}).toString() + "'";
        if (type == "instrumentRemoved") return "Remove instrument " + event.getProperty ("id", {}).toString();
        if (type == "tempoChanged")      return "Tempo " + juce::String ((double) event.getProperty ("bpm", 0.0), 1) + " bpm";
        if (type == "markerAdded")       return "Marker '" + event.getProperty ("name", {}).toString() + "'";
        if (type == "markerRemoved")     return "Remove marker";
        if (type == "expressionMapRemoved") return "Remove expression map '" + event.getProperty ("name", {}).toString() + "'";

        if (type == "expressionMapChanged" && event.hasProperty ("renamedFrom"))
            return "Rename '" + event.getProperty ("renamedFrom", {}).toString() + "' to '" + event.getProperty ("renamedTo", {}).toString()
                   + "' in expression map '" + event.getProperty ("name", {}).toString() + "'";

        if (type == "expressionMapChanged")
            return event.hasProperty ("oldName")
                     ? "Rename expression map '" + event.getProperty ("oldName", {}).toString() + "' to '" + event.getProperty ("name", {}).toString() + "'"
                     : "Expression map '" + event.getProperty ("name", {}).toString() + "'";

        if (type == "instrumentChanged")
            return "Instrument '" + engine.getInstrumentName ((int) event.getProperty ("id", 0)) + "': "
                   + (event.getProperty ("change", {}).toString() == "expressionMap" ? juce::String ("expression map")
                                                                                    : event.getProperty ("change", {}).toString());
        if (type == "folderAdded")       return "Add folder '" + event.getProperty ("name", {}).toString() + "'";
        if (type == "folderRemoved")     return "Remove folder " + event.getProperty ("folderId", {}).toString();

        if (type == "folderChanged")
            return "Folder '" + engine.getFolderName ((int) event.getProperty ("folderId", 0)) + "': "
                   + event.getProperty ("change", {}).toString();

        if (type == "channelChanged")
            return "Channel " + event.getProperty ("channelId", {}).toString() + ": "
                   + event.getProperty ("change", {}).toString();

        if (type == "sidebarMoved")
            return "Re-order " + event.getProperty ("domain", {}).toString() + " sidebar ("
                   + event.getProperty ("count", 0).toString() + " moved)";

        if (type == "recordingFinished")
            return "Record " + event.getProperty ("notes", 0).toString() + " notes on '"
                   + trackName ((int) event.getProperty ("trackId", 0)) + "'";

        if (type == "clipChanged")
            return "Edit clip on '" + trackName ((int) event.getProperty ("trackId", 0)) + "' ("
                   + event.getProperty ("notes", 0).toString() + " notes)";

        if (type == "trackChanged")
            return "Track '" + trackName ((int) event.getProperty ("id", 0)) + "': "
                   + event.getProperty ("change", {}).toString();

        return type;
    }

    AudioEngine& engine;
    std::vector<Entry> entries;
    int current = 0, lastId = 0;

    juce::var burstEvent;
    int burstPriority = -1;
    bool burstActive = false, resetPending = false;

    JUCE_DECLARE_NON_COPYABLE (HistoryManager)
};
