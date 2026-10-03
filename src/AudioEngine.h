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
    void setInstrumentChannelName (InstrumentId, int midiChannel, const juce::String&);
    juce::String getInstrumentChannelName (InstrumentId, int midiChannel) const;
    int getNumLoadedInstruments() const;

    //==============================================================================
    // Audio channels (one per instrument for now; device inputs and summing later)
    AudioChannelProcessor* getAudioChannel (AudioChannelId) const;
    AudioChannelId getAudioChannelForInstrument (InstrumentId) const;          // 0 if none
    std::vector<AudioChannelId> getAudioChannelIds() const;
    juce::String getAudioChannelName (AudioChannelId) const;
    InstrumentId getAudioChannelInput (AudioChannelId) const;                  // 0 = none

    //==============================================================================
    // MIDI tracks
    TrackId addTrack();
    void removeTrack (TrackId);
    std::vector<TrackId> getTrackIds() const;
    juce::String getTrackName (TrackId) const;
    void setTrackName (TrackId, const juce::String&);

    struct TrackOutput
    {
        InstrumentId instrument = 0;
        int midiChannel = 1;
    };

    void addTrackOutput (TrackId, InstrumentId, int midiChannel);
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

    void saveSettings();
    juce::PropertiesFile& getSettingsFile()   { return settings; }

    //==============================================================================
    // Observable state (DESIGN.md): every mutation emits an event here (message
    // thread). The API server forwards them to subscribed connections.
    std::function<void (const juce::var&)> eventSink;

private:
    struct Instrument
    {
        NodeID pluginNode;
        juce::String name;
        AudioChannelId audioChannel = 0;
        std::map<int, juce::String> channelNames;   // 1..16; absent = unnamed
    };

    struct AudioChannel
    {
        NodeID node;
        InstrumentId input = 0;                     // 0 = none (device inputs later)
        juce::String name;
    };

    struct Output
    {
        InstrumentId instrument = 0;
        int midiChannel = 1;
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

    Track* findTrack (TrackId);
    const Track* findTrack (TrackId) const;
    Instrument* findInstrument (InstrumentId);
    const Instrument* findInstrument (InstrumentId) const;
    MidiRouteProcessor* getRoute (const Output&) const;

    void restoreProjectTracks (const juce::XmlElement& root, const std::map<int, InstrumentId>& instrumentIds,
                               juce::StringArray& warnings);
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

    std::map<TrackId, Track> tracks;
    std::map<InstrumentId, Instrument> instruments;
    std::vector<Marker> markers;
    std::map<AudioChannelId, AudioChannel> audioChannels;
    TrackId nextTrackId = 1;
    InstrumentId nextInstrumentId = 1;
    AudioChannelId nextAudioChannelId = 1;
    TrackId armedTrack = 0;

    // Async plugin-creation callbacks hold a weak_ptr to this so they can detect engine destruction.
    std::shared_ptr<int> lifetimeToken = std::make_shared<int>();

    JUCE_DECLARE_NON_COPYABLE (AudioEngine)
};
