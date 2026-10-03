#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

class TrackChannelProcessor;

// Owns the audio device, the plugin catalogue and the processing graph.
// Each track is: [instrument plugin] -> [TrackChannelProcessor] -> master output.
// Live MIDI (hardware inputs + on-screen keyboard) is routed to the armed track only.
class AudioEngine
{
public:
    using TrackId = int;
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
    TrackId addTrack();
    void removeTrack (TrackId);

    using LoadCallback = std::function<void (bool success, const juce::String& error)>;
    void loadInstrument (TrackId, const juce::PluginDescription&, LoadCallback);
    void clearInstrument (TrackId);

    juce::AudioPluginInstance* getInstrument (TrackId) const;
    TrackChannelProcessor* getChannel (TrackId) const;
    int getNumLoadedInstruments() const;

    void setArmedTrack (TrackId);
    TrackId getArmedTrack() const noexcept                { return armedTrack; }

    void saveSettings();

private:
    struct Track
    {
        NodeID instrumentNode, channelNode;
        int loadGeneration = 0;
    };

    Track* findTrack (TrackId);
    const Track* findTrack (TrackId) const;
    void connectInstrument (const Track&);
    void updateMidiRouting();
    void enableAllMidiInputsIfFirstRun (bool hadSavedState);

    juce::PropertiesFile& settings;
    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;

    NodeID audioOutNode, midiInNode;
    std::map<TrackId, Track> tracks;
    TrackId nextTrackId = 1, armedTrack = 0;

    // Async plugin-creation callbacks hold a weak_ptr to this so they can detect engine destruction.
    std::shared_ptr<int> lifetimeToken = std::make_shared<int>();

    JUCE_DECLARE_NON_COPYABLE (AudioEngine)
};
