#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <optional>

#include "engine/Transport.h"
#include "engine/MidiRecorder.h"
#include "model/ExpressionMap.h"
#include "model/MidiSequence.h"

class AudioChannelProcessor;
class MidiSourceProcessor;
class MidiRouteProcessor;

// Owns the audio device, the plugin catalogue and the processing graph.
//
// Signal flow (see DESIGN.md):
//   MIDI tracks --MIDI--> instruments (rack) --audio--> audio channels --> master out
//
// A track owns clips and sends MIDI through any number of outputs, each targeting one
// (instrument, MIDI channel). Instruments are plugin instances shared by any number of
// tracks. Every instrument currently gets one audio channel (strip with gain/mute/meter);
// device inputs and summing come later. Track mute/solo act on MIDI, before the
// instruments; audio channel mute acts on audio.
class AudioEngine
{
public:
    using TrackId = int;
    using InstrumentId = int;
    using AudioChannelId = int;
    using FolderId = int;
    using NodeID  = juce::AudioProcessorGraph::NodeID;

    explicit AudioEngine (juce::PropertiesFile& settings);
    ~AudioEngine();

    juce::AudioDeviceManager& getDeviceManager()          { return deviceManager; }
    juce::AudioPluginFormatManager& getFormatManager()    { return formatManager; }
    juce::KnownPluginList& getKnownPlugins()              { return knownPlugins; }
    juce::MidiMessageCollector& getLiveMidiCollector()    { return player.getMidiMessageCollector(); }
    juce::File getDeadMansPedalFile() const;
    static juce::File getScannerExecutable();

    // The plugin list lives in the user data dir (see UserData.h) and is shared by all builds.
    void reloadPluginCache();
    void savePluginCache();

    // Instruments only, sorted by manufacturer.
    juce::Array<juce::PluginDescription> getInstrumentTypes() const;

    //==============================================================================
    // The instrument rack
    using InstrumentCallback = std::function<void (InstrumentId, const juce::String& error)>;
    void addInstrument (const juce::PluginDescription&, InstrumentCallback);   // id 0 + error on failure
    void removeInstrument (InstrumentId);                                      // detaches any track outputs

    std::vector<std::pair<InstrumentId, juce::String>> getInstruments() const;
    juce::AudioPluginInstance* getInstrumentPlugin (InstrumentId) const;
    juce::String getInstrumentName (InstrumentId) const;
    void setInstrumentName (InstrumentId, const juce::String&);   // renames its audio channel too (if unchanged)
    int getNumLoadedInstruments() const;

    // An instrument's MIDI channels live on (port, channel). Ports are the
    // plugin's own VST3 MIDI event buses (see getInstrumentMidiPortCount).
    // 'synced' entries are inherited from a VE Pro server instance: their name and
    // binding are immutable and refresh on sync; manual entries stay editable.
    struct MidiChannelInfo
    {
        int midiPort = 1;
        int midiChannel = 1;
        juce::String name;
        bool synced = false;

        // Synced channels: where the player lives on the server, and its playable
        // key range once fetched (-1 = unknown; Synchron Player only)
        juce::String veproInstanceId, veproChannelAddress, veproPluginId;
        int keyLow = -1, keyHigh = -1;

        // The expression map this instrument channel uses (a project map, by name;
        // empty = none). Every track playing this channel shares it. Editable on
        // synced channels too: it is the one thing sync does not own.
        juce::String expressionMap;
    };

    // How many MIDI ports the plugin itself offers (its VST3 MIDI event input
    // buses - VE Pro mirrors its server's port setting, e.g. 8 or 16). Outputs on
    // port N reach event bus N-1 directly, like Cubase. 1 for non-VST3 plugins.
    int getInstrumentMidiPortCount (InstrumentId) const;

    bool setInstrumentChannelName (InstrumentId, int midiChannel, const juce::String&, int midiPort = 1);
    juce::String getInstrumentChannelName (InstrumentId, int midiChannel, int midiPort = 1) const;
    std::vector<MidiChannelInfo> getInstrumentMidiChannels (InstrumentId) const;
    void setSyncedInstrumentChannels (InstrumentId, std::vector<MidiChannelInfo>);   // replaces the synced set
    void setInstrumentChannelKeyRange (InstrumentId, int midiPort, int midiChannel, int low, int high);

    // The channel info behind a track's first output (nullopt when it has none)
    std::optional<MidiChannelInfo> getTrackChannelInfo (TrackId) const;

    //==============================================================================
    // Expression maps (MILESTONES.md "Articulation / expression maps"). Maps are
    // project data, named (case-insensitive); instrument channels refer to them
    // by name, so several channels can share one. The mutating calls return an
    // error sentence (empty = done) that names what exists.
    std::vector<ExpressionMap> getExpressionMaps() const;
    std::optional<ExpressionMap> getExpressionMap (const juce::String& name) const;
    juce::String setExpressionMap (ExpressionMap);                                   // add, or replace the map of that name; must validate
    juce::String removeExpressionMap (const juce::String& name);                     // channels using it keep the name (shown as a missing map)
    juce::String renameExpressionMap (const juce::String& name, const juce::String& newName);   // channels follow the rename

    // Rename a group (articulationName empty) or an articulation inside a map. The notes of every
    // track playing a channel that uses the map follow, in one undo step; "applies to" lists follow
    // a renamed root. notesChanged (optional) receives how many notes were rewritten.
    juce::String renameExpressionMapItem (const juce::String& mapName, const juce::String& groupName,
                                          const juce::String& articulationName, const juce::String& newName,
                                          int* notesChanged = nullptr);
    juce::String setInstrumentChannelMap (InstrumentId, int midiPort, int midiChannel, const juce::String& mapName);   // "" = none
    std::optional<ExpressionMap> getTrackExpressionMap (TrackId) const;              // the map of the track's channel, if it has a valid one

    //==============================================================================
    // Audio channels (one per instrument for now; device inputs and summing later)
    AudioChannelProcessor* getAudioChannel (AudioChannelId) const;
    AudioChannelId getAudioChannelForInstrument (InstrumentId) const;          // 0 if none
    std::vector<AudioChannelId> getAudioChannelIds() const;
    juce::String getAudioChannelName (AudioChannelId) const;
    InstrumentId getAudioChannelInput (AudioChannelId) const;                  // 0 = none

    //==============================================================================
    // MIDI tracks
    TrackId addTrack (const juce::String& name = {});   // empty = "Track N"
    void removeTrack (TrackId);
    std::vector<TrackId> getTrackIds() const;
    juce::String getTrackName (TrackId) const;
    void setTrackName (TrackId, const juce::String&);

    // Colors are "#rrggbb" strings (the VE Pro server's format); empty = none.
    void setTrackColour (TrackId, const juce::String& hex);
    juce::String getTrackColour (TrackId) const;
    void setFolderColour (FolderId, const juce::String& hex);
    juce::String getFolderColour (FolderId) const;

    static juce::Colour colourFromHex (const juce::String& hex, juce::Colour fallback = {})
    {
        if (hex.length() == 7 && hex.startsWithChar ('#'))
            return juce::Colour::fromString ("ff" + hex.substring (1));

        return fallback;
    }

    struct TrackOutput
    {
        InstrumentId instrument = 0;
        int midiChannel = 1;
        int midiPort = 1;      // port 1 = the plugin's own MIDI input
    };

    void addTrackOutput (TrackId, InstrumentId, int midiChannel, int midiPort = 1);
    void clearTrackOutputs (TrackId);
    std::vector<TrackOutput> getTrackOutputs (TrackId) const;

    void setTrackMuted (TrackId, bool);       // MIDI mute: stops events, releases held notes
    bool isTrackMuted (TrackId) const;
    void setTrackSoloed (TrackId, bool);
    bool isTrackSoloed (TrackId) const;

    // Recording mode (DESIGN.md): false = add to existing (merge), true = replace on
    // first input (existing material plays until you play; from then it's erased
    // under the playhead until recording stops).
    void setTrackRecordReplace (TrackId, bool);
    bool isTrackRecordReplace (TrackId) const;

    void setTrackSequence (TrackId, MidiSequence::Ptr);   // records clip history (undo)
    MidiSequence::Ptr getTrackSequence (TrackId) const;
    void addToTrackSequence (TrackId, std::vector<MidiSequence::Note>, std::vector<MidiSequence::Control>);

    // Per-track clip history. Sequences are immutable, so history is a stack of pointers.
    bool undoTrackSequence (TrackId);
    bool redoTrackSequence (TrackId);
    bool canUndoClip (TrackId) const;
    bool canRedoClip (TrackId) const;

    void setArmedTrack (TrackId);             // arms just this track (live MIDI follows its outputs)
    TrackId getArmedTrack() const noexcept    { return armedTrack; }   // the primary armed track

    // Several tracks can be armed at once (multi-selection with auto-record):
    // live input plays through all of them and a take records into all of them,
    // each by its own record mode. 'primary' is the one the editor follows.
    void setArmedTracks (const std::set<TrackId>&, TrackId primary);
    bool isTrackArmed (TrackId id) const      { return armedTracks.count (id) > 0; }

    //==============================================================================
    // Folders group channels in the sidebars, Cubase-style. Two independent trees:
    // one for MIDI tracks, one for audio channels; folders nest arbitrarily.
    // Membership lives on the track/channel (0 = root). Purely organisational -
    // no effect on routing or playback.
    FolderId addFolder (bool midiDomain, const juce::String& name = {}, FolderId parent = 0);
    void removeFolder (FolderId);             // children and members move to its parent
    std::vector<FolderId> getFolderIds (bool midiDomain) const;
    bool folderExists (FolderId) const;
    bool isFolderMidiDomain (FolderId) const;
    juce::String getFolderName (FolderId) const;
    void setFolderName (FolderId, const juce::String&);
    FolderId getFolderParent (FolderId) const;
    bool setFolderParent (FolderId, FolderId parent);   // false: unknown id, domain mismatch or cycle
    bool isFolderCollapsed (FolderId) const;
    void setFolderCollapsed (FolderId, bool);

    void setTrackFolder (TrackId, FolderId);            // folder 0 = root; must be the midi tree
    FolderId getTrackFolder (TrackId) const;
    void setAudioChannelFolder (AudioChannelId, FolderId);
    FolderId getAudioChannelFolder (AudioChannelId) const;

    // The sidebar/arrangement display order: a depth-first walk of one domain's
    // tree. Each item is either a folder or a member (track/channel id in 'member');
    // siblings - folders and members in one sequence - follow their explicit
    // position (set by moveSidebarItems, i.e. dragging). With skipCollapsed, a
    // collapsed folder still appears but its contents don't.
    struct SidebarItem
    {
        FolderId folder = 0;
        int member = 0;
        int depth = 0;
        FolderId parent = 0;

        bool operator== (const SidebarItem& other) const noexcept
        {
            return folder == other.folder && member == other.member
                    && depth == other.depth && parent == other.parent;
        }
    };

    std::vector<SidebarItem> getSidebarItems (bool midiDomain, bool skipCollapsed) const;
    std::vector<TrackId> getArrangeTrackOrder() const;   // visible tracks, tree order

    // Move folders and/or members (tracks for midi, channels for audio) into
    // 'parent' at child index 'index', keeping the given order as one group.
    // One call = one event = one history entry. False: unknown ids, a domain
    // mismatch, or a folder moved into itself/its own subtree.
    bool moveSidebarItems (bool midiDomain, const std::vector<FolderId>& folderIds,
                           const std::vector<int>& memberIds, FolderId parent, int index);

    //==============================================================================
    Transport& getTransport()                 { return transport; }

    double getTempoBpm() const;
    void setTempoBpm (double bpm);

    // Markers: named positions dividing the project into parts (DESIGN.md).
    struct Marker
    {
        juce::int64 tick = 0;
        juce::String name;
    };

    const std::vector<Marker>& getMarkers() const    { return markers; }
    void addMarker (juce::int64 tick, const juce::String& name);   // same tick = rename
    void removeMarker (juce::int64 tick);

    // End of the bar containing the last event of any track's sequence (used as the loop end).
    juce::int64 getLoopEndTicks() const;

    //==============================================================================
    // Recording captures live MIDI onto the armed track, merging with any existing clip.
    bool startRecording();        // starts the transport too, if stopped
    void stopRecording();         // finalizes and merges; playback continues
    bool isRecording() const      { return recorder != nullptr && recorder->isRecording(); }
    void pollRecording();         // call regularly from a UI timer while the app runs

    //==============================================================================
    // Projects. Loading is asynchronous (instruments instantiate one by one); 'done'
    // reports success plus any warnings (e.g. a plugin that no longer exists).
    bool saveProject (const juce::File&);
    void loadProject (const juce::File&, std::function<void (bool ok, juce::String warnings)> done);
    void clearProject();

    // Batch many graph edits (big syncs, loads): no render-sequence rebuild at
    // all until the outermost endGraphBatch, which rebuilds once. Nestable.
    void beginGraphBatch();
    void endGraphBatch();

    // Long operations (project loads, VE Pro syncs, plugin scans, instrument
    // loads) report here so the UI can show a busy overlay explaining why the
    // app isn't responding. onChanged fires synchronously on every change (the
    // UI paints immediately - the message thread may be about to block).
    // progress: 0..1, or < 0 = unknown.
    //
    // begin/end nest: an inner operation (an instrument load inside a project
    // load) leaves the outer one's title and detail alone, and the overlay
    // closes when the outermost end() arrives. Every begin needs exactly one end.
    struct BusyStatus
    {
        bool active = false;
        juce::String title, detail;
        double progress = -1.0;
        std::function<void()> onChanged;

        void begin (const juce::String& newTitle)
        {
            if (depth++ > 0)
                return;

            active = true;
            title = newTitle;
            detail.clear();
            progress = -1.0;
            if (onChanged) onChanged();
        }

        void update (const juce::String& newDetail, double newProgress = -1.0)
        {
            detail = newDetail;
            progress = newProgress;
            if (onChanged) onChanged();
        }

        void end()
        {
            if (depth == 0 || --depth > 0)
                return;

            active = false;
            if (onChanged) onChanged();
        }

    private:
        int depth = 0;
    };

    BusyStatus& getBusyStatus() noexcept    { return busyStatus; }

    // True when anything changed since the last save/load/clear (every emitted
    // mutation marks the project dirty).
    bool isProjectDirty() const noexcept    { return projectDirty; }
    void markProjectClean() noexcept        { projectDirty = false; }

    // The UI update mechanism (ISSUES.md "Global"): every mutation bumps this,
    // so a timer-driven view compares ONE number to know whether anything it
    // might display has changed, instead of hand-picking state to watch.
    int getStateRevision() const noexcept   { return stateRevision; }
    void requestRepaint() noexcept          { ++stateRevision; }   // view-only change (theme): repaint without dirtying the project

    void saveSettings();
    juce::PropertiesFile& getSettingsFile()   { return settings; }

    //==============================================================================
    // Observable state (DESIGN.md): every mutation emits an event here (message
    // thread). The API server forwards them to subscribed connections.
    std::function<void (const juce::var&)> eventSink;

    //==============================================================================
    // History snapshots (the history UI's time-travel). Light by design: structure
    // plus shared immutable pointers - no plugin state. The instrument rack is NOT
    // rewound (instances stay loaded); routing, clips, tempo, markers and channel
    // levels are.
    struct HistorySnapshot
    {
        struct TrackState
        {
            TrackId id = 0;
            juce::String name;
            bool muted = false, soloed = false, recordReplace = false;
            std::vector<TrackOutput> outputs;
            MidiSequence::Ptr sequence;
            FolderId folder = 0;
            int position = 0;
            juce::String colour;
        };

        struct ChannelState
        {
            AudioChannelId id = 0;
            float gain = 1.0f;
            bool muted = false;
            FolderId folder = 0;
            int position = 0;
        };

        struct FolderState
        {
            FolderId id = 0;
            juce::String name;
            bool midiDomain = true;
            FolderId parent = 0;
            bool collapsed = false;
            int position = 0;
            juce::String colour;
        };

        std::vector<TrackState> tracks;
        TrackId armedTrack = 0;
        TempoMap::Ptr tempoMap;
        std::vector<Marker> markers;
        std::vector<ChannelState> channels;
        struct ChannelMapState   // which map an instrument channel uses (only channels that have one)
        {
            InstrumentId instrument = 0;
            int midiPort = 1, midiChannel = 1;
            juce::String map;
        };

        std::vector<FolderState> folders;
        std::vector<ExpressionMap> expressionMaps;
        std::vector<ChannelMapState> channelMaps;
    };

    HistorySnapshot captureHistorySnapshot() const;
    void applyHistorySnapshot (const HistorySnapshot&);   // emits one "historyTravelled" event

private:
    struct Instrument
    {
        NodeID pluginNode;                          // all MIDI ports address this plugin
        juce::String name;
        AudioChannelId audioChannel = 0;
        std::vector<MidiChannelInfo> midiChannels;  // named channels (manual + synced)
    };

    struct AudioChannel
    {
        NodeID node;
        InstrumentId input = 0;                     // 0 = none (device inputs later)
        juce::String name;
        FolderId folder = 0;                        // 0 = root
        int position = 0;                           // order among siblings
    };

    struct Folder
    {
        juce::String name;
        bool midiDomain = true;                     // which sidebar tree it belongs to
        FolderId parent = 0;                        // 0 = root; always the same domain
        bool collapsed = false;
        int position = 0;                           // order among siblings
        juce::String colour;                        // "#rrggbb"; empty = none
    };

    struct Output
    {
        InstrumentId instrument = 0;
        int midiChannel = 1;
        int midiPort = 1;
        NodeID routeNode;
    };

    struct Track
    {
        NodeID midiSourceNode;
        juce::String name;
        MidiSequence::Ptr sequence;                 // message-thread copy, for UI queries
        std::vector<MidiSequence::Ptr> undoStack, redoStack;
        std::vector<Output> outputs;
        bool muted = false, soloed = false;
        bool recordReplace = false;                 // false = add, true = replace on first input
        FolderId folder = 0;                        // 0 = root
        int position = 0;                           // order among siblings
        juce::String colour;                        // "#rrggbb"; empty = none
    };

    // Runs the transport once per device callback, before the graph renders the block.
    struct IOCallback final : juce::AudioIODeviceCallback
    {
        explicit IOCallback (AudioEngine& e) : engine (e) {}

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            engine.transport.prepare (device->getCurrentSampleRate());
            engine.player.audioDeviceAboutToStart (device);
        }

        void audioDeviceStopped() override { engine.player.audioDeviceStopped(); }

        void audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                               float* const* output, int numOutputs, int numSamples,
                                               const juce::AudioIODeviceCallbackContext& context) override
        {
            engine.transport.beginBlock (numSamples);
            engine.player.audioDeviceIOCallbackWithContext (input, numInputs, output, numOutputs,
                                                            numSamples, context);
        }

        AudioEngine& engine;
    };

    // Sibling ordering (folders and members share one position sequence per parent)
    struct ChildRef
    {
        bool isFolder = false;
        int id = 0;
        int position = 0;
    };

    std::vector<ChildRef> getChildrenOf (bool midiDomain, FolderId parent) const;   // sorted
    int nextChildPosition (bool midiDomain, FolderId parent) const;
    void setChildPosition (bool midiDomain, const ChildRef&, int position);

    Track* findTrack (TrackId);
    const Track* findTrack (TrackId) const;
    Instrument* findInstrument (InstrumentId);
    const Instrument* findInstrument (InstrumentId) const;
    MidiRouteProcessor* getRoute (const Output&) const;

    void restoreProjectTracks (const juce::XmlElement& root, const std::map<int, InstrumentId>& instrumentIds,
                               const std::map<int, FolderId>& folderIds, juce::StringArray& warnings);
    void applySequence (Track&, MidiSequence::Ptr);   // pushes to the source node, no history
    static MidiSequence::Ptr eraseRangeFrom (const MidiSequence::Ptr&, juce::int64 start, juce::int64 end);
    MidiSourceProcessor* getSource (TrackId) const;
    void updateMidiRouting();                 // keeps midiIn -> route connections matching the armed track
    void emitEvent (const juce::String& type, juce::DynamicObject::Ptr data = nullptr);
    void emitTrackChanged (TrackId, const juce::String& change);
    void emitClipChanged (TrackId);
    void applyMuteAndSolo();
    void mergeIntoTrack (TrackId, const MidiRecorder::Result&);
    void enableAllMidiInputsIfFirstRun (bool hadSavedState);

    juce::PropertiesFile& settings;
    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    TempoMap::Ptr masterTempoMap;             // message-thread authority; transport gets snapshots
    Transport transport;
    IOCallback ioCallback { *this };

    NodeID audioOutNode, midiInNode, recorderNode;
    std::unique_ptr<MidiRecorder> recorder;
    bool recordingSawPlayback = false;

    // The active take records into every armed track, each by its own mode
    struct TakeTarget
    {
        TrackId trackId = 0;
        bool replace = false;
        MidiSequence::Ptr preTakeSequence;     // replace mode: the state before the take
    };

    std::vector<TakeTarget> takeTargets;
    MidiRecorder::Result takeStash;            // loop-pass commits, kept for replace targets
    juce::int64 replaceFromTick = -1;          // first played event (shared: one input stream)
    bool anyReplaceTarget() const;

    bool projectDirty = false;
    int stateRevision = 0;          // bumped by every emitEvent; polled by the UI
    int graphBatchDepth = 0;        // > 0: graph edits defer their rebuild (see updateKind)
    BusyStatus busyStatus;
    juce::AudioProcessorGraph::UpdateKind updateKind() const noexcept;
    bool historySuppress = false;   // mute event emission while applying a snapshot

    std::map<TrackId, Track> tracks;
    std::map<InstrumentId, Instrument> instruments;
    std::vector<Marker> markers;
    std::vector<ExpressionMap> expressionMaps;   // project data; instrument channels refer to them by name
    std::map<AudioChannelId, AudioChannel> audioChannels;
    std::map<FolderId, Folder> folders;
    TrackId nextTrackId = 1;
    InstrumentId nextInstrumentId = 1;
    AudioChannelId nextAudioChannelId = 1;
    FolderId nextFolderId = 1;
    TrackId armedTrack = 0;                   // primary
    std::set<TrackId> armedTracks;            // all armed (contains the primary)

    // Async plugin-creation callbacks hold a weak_ptr to this so they can detect engine destruction.
    std::shared_ptr<int> lifetimeToken = std::make_shared<int>();

    JUCE_DECLARE_NON_COPYABLE (AudioEngine)
};
