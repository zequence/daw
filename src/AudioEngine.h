#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "engine/Transport.h"
#include "engine/MidiRecorder.h"
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

    // An instrument's MIDI channels live on (port, channel). Port 1 is the plugin's
    // own MIDI input; further ports exist for multiport instruments (VE Pro via
    // Event Input plugins - routing for ports >= 2 lands with that support).
    // 'synced' entries are inherited from a VE Pro server instance: their name and
    // binding are immutable and refresh on sync; manual entries stay editable.
    struct MidiChannelInfo
    {
        int midiPort = 1;
        int midiChannel = 1;
        juce::String name;
        bool synced = false;
    };

    bool setInstrumentChannelName (InstrumentId, int midiChannel, const juce::String&, int midiPort = 1);
    juce::String getInstrumentChannelName (InstrumentId, int midiChannel, int midiPort = 1) const;
    std::vector<MidiChannelInfo> getInstrumentMidiChannels (InstrumentId) const;
    void setSyncedInstrumentChannels (InstrumentId, std::vector<MidiChannelInfo>);   // replaces the synced set

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

    void setArmedTrack (TrackId);             // live MIDI follows the armed track's outputs
    TrackId getArmedTrack() const noexcept    { return armedTrack; }

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

    // True when anything changed since the last save/load/clear (every emitted
    // mutation marks the project dirty).
    bool isProjectDirty() const noexcept    { return projectDirty; }
    void markProjectClean() noexcept        { projectDirty = false; }

    // The UI update mechanism (ISSUES.md "Global"): every mutation bumps this,
    // so a timer-driven view compares ONE number to know whether anything it
    // might display has changed, instead of hand-picking state to watch.
    int getStateRevision() const noexcept   { return stateRevision; }

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
        std::vector<FolderState> folders;
    };

    HistorySnapshot captureHistorySnapshot() const;
    void applyHistorySnapshot (const HistorySnapshot&);   // emits one "historyTravelled" event

private:
    struct Instrument
    {
        NodeID pluginNode;                          // the plugin = MIDI port 1
        juce::String name;
        AudioChannelId audioChannel = 0;
        std::vector<MidiChannelInfo> midiChannels;  // named channels (manual + synced)
        std::map<int, NodeID> portNodes;            // ports >= 2 (VE Pro Event Input, later)
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

    // Replace-recording state for the active take
    bool takeIsReplace = false;
    MidiSequence::Ptr preTakeSequence;
    juce::int64 replaceFromTick = -1;

    bool projectDirty = false;
    int stateRevision = 0;          // bumped by every emitEvent; polled by the UI
    bool historySuppress = false;   // mute event emission while applying a snapshot

    std::map<TrackId, Track> tracks;
    std::map<InstrumentId, Instrument> instruments;
    std::vector<Marker> markers;
    std::map<AudioChannelId, AudioChannel> audioChannels;
    std::map<FolderId, Folder> folders;
    TrackId nextTrackId = 1;
    InstrumentId nextInstrumentId = 1;
    AudioChannelId nextAudioChannelId = 1;
    FolderId nextFolderId = 1;
    TrackId armedTrack = 0;

    // Async plugin-creation callbacks hold a weak_ptr to this so they can detect engine destruction.
    std::shared_ptr<int> lifetimeToken = std::make_shared<int>();

    JUCE_DECLARE_NON_COPYABLE (AudioEngine)
};
