#include "AudioEngine.h"
#include "TrackChannelProcessor.h"
#include "UserData.h"
#include "engine/MidiSourceProcessor.h"

namespace
{
    constexpr auto audioStateKey = "audioDeviceState";
    using IOProcessor = juce::AudioProcessorGraph::AudioGraphIOProcessor;
}

AudioEngine::AudioEngine (juce::PropertiesFile& settingsToUse)
    : settings (settingsToUse)
{
    juce::addDefaultFormatsToManager (formatManager);
    reloadPluginCache();

    masterTempoMap = TempoMap::create (120.0);
    transport.setTempoMap (masterTempoMap);

    auto savedAudio = settings.getXmlValue (audioStateKey);
    deviceManager.initialise (0, 2, savedAudio.get(), true);
    enableAllMidiInputsIfFirstRun (savedAudio != nullptr);

    audioOutNode = graph.addNode (std::make_unique<IOProcessor> (IOProcessor::audioOutputNode))->nodeID;
    midiInNode   = graph.addNode (std::make_unique<IOProcessor> (IOProcessor::midiInputNode))->nodeID;

    // Permanent tap on the live MIDI input for recording.
    auto recorderNodePtr = graph.addNode (std::make_unique<MidiRecorderProcessor> (transport));
    recorderNode = recorderNodePtr->nodeID;
    graph.addConnection ({ { midiInNode, juce::AudioProcessorGraph::midiChannelIndex },
                           { recorderNode, juce::AudioProcessorGraph::midiChannelIndex } });
    recorder = std::make_unique<MidiRecorder> (static_cast<MidiRecorderProcessor&> (*recorderNodePtr->getProcessor()));

    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&ioCallback);
    deviceManager.addMidiInputDeviceCallback ({}, &player);
}

AudioEngine::~AudioEngine()
{
    saveSettings();

    deviceManager.removeMidiInputDeviceCallback ({}, &player);
    deviceManager.removeAudioCallback (&ioCallback);
    player.setProcessor (nullptr);
    graph.clear();
}

void AudioEngine::enableAllMidiInputsIfFirstRun (bool hadSavedState)
{
    if (hadSavedState)
        return;

    for (auto& device : juce::MidiInput::getAvailableDevices())
        deviceManager.setMidiInputDeviceEnabled (device.identifier, true);
}

void AudioEngine::saveSettings()
{
    if (auto audioState = deviceManager.createStateXml())
        settings.setValue (audioStateKey, audioState.get());

    settings.saveIfNeeded();
}

void AudioEngine::savePluginCache()
{
    if (auto xml = knownPlugins.createXml())
    {
        juce::TemporaryFile temp (UserData::getPluginCacheFile());

        if (xml->writeTo (temp.getFile()))
            temp.overwriteTargetFileWithTemporary();
    }
}

void AudioEngine::reloadPluginCache()
{
    if (auto xml = juce::parseXML (UserData::getPluginCacheFile()))
        knownPlugins.recreateFromXml (*xml);
}

juce::File AudioEngine::getDeadMansPedalFile() const
{
    return UserData::getDir().getChildFile ("RecentlyCrashedPlugins.txt");
}

juce::File AudioEngine::getScannerExecutable()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
               .getSiblingFile ("OrchestralDAWScanner.exe");
}

juce::Array<juce::PluginDescription> AudioEngine::getInstrumentTypes() const
{
    juce::Array<juce::PluginDescription> result;

    for (auto& type : knownPlugins.getTypes())
        if (type.isInstrument)
            result.add (type);

    std::sort (result.begin(), result.end(), [] (const auto& a, const auto& b)
    {
        if (a.manufacturerName != b.manufacturerName)
            return a.manufacturerName.compareIgnoreCase (b.manufacturerName) < 0;

        return a.name.compareIgnoreCase (b.name) < 0;
    });

    return result;
}

//==============================================================================
AudioEngine::Track* AudioEngine::findTrack (TrackId id)
{
    auto it = tracks.find (id);
    return it != tracks.end() ? &it->second : nullptr;
}

const AudioEngine::Track* AudioEngine::findTrack (TrackId id) const
{
    auto it = tracks.find (id);
    return it != tracks.end() ? &it->second : nullptr;
}

AudioEngine::TrackId AudioEngine::addTrack()
{
    const auto id = nextTrackId++;

    Track track;
    track.channelNode = graph.addNode (std::make_unique<TrackChannelProcessor>())->nodeID;
    track.midiSourceNode = graph.addNode (std::make_unique<MidiSourceProcessor> (transport))->nodeID;

    for (int ch = 0; ch < 2; ++ch)
        graph.addConnection ({ { track.channelNode, ch }, { audioOutNode, ch } });

    tracks[id] = track;

    if (armedTrack == 0)
        setArmedTrack (id);

    return id;
}

void AudioEngine::removeTrack (TrackId id)
{
    auto* track = findTrack (id);

    if (track == nullptr)
        return;

    graph.removeNode (track->instrumentNode);
    graph.removeNode (track->channelNode);
    graph.removeNode (track->midiSourceNode);
    tracks.erase (id);

    if (armedTrack == id)
        setArmedTrack (tracks.empty() ? 0 : tracks.begin()->first);
}

void AudioEngine::loadInstrument (TrackId id, const juce::PluginDescription& description, LoadCallback callback)
{
    auto* track = findTrack (id);

    if (track == nullptr)
        return;

    const auto generation = ++track->loadGeneration;

    auto* device = deviceManager.getCurrentAudioDevice();
    const auto sampleRate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
    const auto blockSize  = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;

    std::weak_ptr<int> alive = lifetimeToken;
    const auto startMs = juce::Time::getMillisecondCounterHiRes();
    juce::Logger::writeToLog ("Loading plugin: " + description.name + " (" + description.fileOrIdentifier + ")");

    formatManager.createPluginInstanceAsync (description, sampleRate, blockSize,
        [this, alive, id, generation, callback, startMs, name = description.name]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            if (alive.expired())
                return;

            juce::Logger::writeToLog ((instance != nullptr ? "Loaded plugin: " : "FAILED to load plugin: ") + name
                                      + " in " + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - startMs)) + " ms"
                                      + (error.isNotEmpty() ? " - " + error : juce::String()));

            auto* t = findTrack (id);

            // Track removed, or a newer load was requested while this one was in flight.
            if (t == nullptr || t->loadGeneration != generation)
                return;

            if (instance == nullptr)
            {
                if (callback) callback (false, error);
                return;
            }

            graph.removeNode (t->instrumentNode);
            t->instrumentNode = graph.addNode (std::move (instance))->nodeID;
            connectInstrument (*t);
            updateMidiRouting();

            if (callback) callback (true, {});
        });
}

void AudioEngine::clearInstrument (TrackId id)
{
    if (auto* track = findTrack (id))
    {
        ++track->loadGeneration;
        graph.removeNode (track->instrumentNode);
        track->instrumentNode = {};
    }
}

void AudioEngine::connectInstrument (const Track& track)
{
    auto* node = graph.getNodeForId (track.instrumentNode);

    if (node == nullptr)
        return;

    const auto midiChannel = juce::AudioProcessorGraph::midiChannelIndex;
    const auto sequencerLinked = graph.addConnection ({ { track.midiSourceNode, midiChannel },
                                                        { track.instrumentNode, midiChannel } });
    if (! sequencerLinked)
        juce::Logger::writeToLog ("WARNING: sequencer MIDI connection was refused by the graph");

    // Only the first stereo pair for now; multi-output routing comes later.
    const auto numOuts = node->getProcessor()->getTotalNumOutputChannels();

    if (numOuts == 1)
    {
        graph.addConnection ({ { track.instrumentNode, 0 }, { track.channelNode, 0 } });
        graph.addConnection ({ { track.instrumentNode, 0 }, { track.channelNode, 1 } });
    }
    else
    {
        for (int ch = 0; ch < juce::jmin (2, numOuts); ++ch)
            graph.addConnection ({ { track.instrumentNode, ch }, { track.channelNode, ch } });
    }
}

juce::AudioPluginInstance* AudioEngine::getInstrument (TrackId id) const
{
    if (auto* track = findTrack (id))
        if (auto* node = graph.getNodeForId (track->instrumentNode))
            return dynamic_cast<juce::AudioPluginInstance*> (node->getProcessor());

    return nullptr;
}

int AudioEngine::getNumLoadedInstruments() const
{
    int count = 0;

    for (auto& [id, track] : tracks)
        if (graph.getNodeForId (track.instrumentNode) != nullptr)
            ++count;

    return count;
}

TrackChannelProcessor* AudioEngine::getChannel (TrackId id) const
{
    if (auto* track = findTrack (id))
        if (auto* node = graph.getNodeForId (track->channelNode))
            return dynamic_cast<TrackChannelProcessor*> (node->getProcessor());

    return nullptr;
}

//==============================================================================
void AudioEngine::setTrackSequence (TrackId id, MidiSequence::Ptr sequence)
{
    if (auto* track = findTrack (id))
    {
        track->sequence = sequence;

        auto* node = graph.getNodeForId (track->midiSourceNode);
        auto* source = node != nullptr ? dynamic_cast<MidiSourceProcessor*> (node->getProcessor()) : nullptr;

        juce::Logger::writeToLog ("Track " + juce::String (id)
                                  + (sequence != nullptr ? ": sequence set (" + juce::String ((int) sequence->getNotes().size()) + " notes)"
                                                         : ": sequence cleared")
                                  + (source == nullptr ? " - NO SOURCE NODE!" : ""));

        if (source != nullptr)
            source->setSequence (std::move (sequence));
    }
}

MidiSequence::Ptr AudioEngine::getTrackSequence (TrackId id) const
{
    if (auto* track = findTrack (id))
        return track->sequence;

    return nullptr;
}

double AudioEngine::getTempoBpm() const
{
    return masterTempoMap->getTempoAt (0);
}

void AudioEngine::setTempoBpm (double bpm)
{
    const auto tick = transport.getPositionTicks();   // keep the playhead musically stable

    masterTempoMap = masterTempoMap->withTempoChange (0, bpm);
    transport.setTempoMap (masterTempoMap);
    transport.locate (tick);
}

bool AudioEngine::startRecording()
{
    if (findTrack (armedTrack) == nullptr || isRecording())
        return false;

    recordingSawPlayback = false;
    recorder->start (armedTrack);
    juce::Logger::writeToLog ("Recording started on track " + juce::String (armedTrack));

    if (! transport.isPlaying())
        transport.play();

    return true;
}

void AudioEngine::stopRecording()
{
    if (! isRecording())
        return;

    const auto trackId = recorder->getTrackId();
    const auto result = recorder->finish (transport.getPositionTicks());

    juce::Logger::writeToLog ("Recording stopped on track " + juce::String (trackId) + ": "
                              + juce::String ((int) result.notes.size()) + " notes, "
                              + juce::String ((int) result.controls.size()) + " control events");
    mergeIntoTrack (trackId, result);
}

void AudioEngine::pollRecording()
{
    if (! isRecording())
        return;

    recorder->poll();

    // Commit each loop pass so it's audible on the next one.
    if (recorder->consumeWrapFlag())
        mergeIntoTrack (recorder->getTrackId(), recorder->takePending());

    // The transport reports playing only once the audio thread has confirmed it.
    if (transport.isPlaying())
        recordingSawPlayback = true;
    else if (recordingSawPlayback)
        stopRecording();
}

void AudioEngine::mergeIntoTrack (TrackId id, const MidiRecorder::Result& result)
{
    if (result.isEmpty())
        return;

    auto* track = findTrack (id);

    if (track == nullptr)
        return;

    auto notes = result.notes;
    auto controls = result.controls;

    if (track->sequence != nullptr)
    {
        const auto& existing = *track->sequence;
        notes.insert (notes.end(), existing.getNotes().begin(), existing.getNotes().end());
        controls.insert (controls.end(), existing.getControls().begin(), existing.getControls().end());
    }

    setTrackSequence (id, MidiSequence::create (std::move (notes), std::move (controls)));
}

juce::int64 AudioEngine::getLoopEndTicks() const
{
    juce::int64 length = 0;

    for (auto& [id, track] : tracks)
        if (track.sequence != nullptr)
            length = juce::jmax (length, track.sequence->getLengthTicks());

    const auto& map = *masterTempoMap;

    if (length <= 0)
        return map.getTicksPerBar (0) * 2;

    return map.getBarStart (length - 1) + map.getTicksPerBar (length - 1);
}

//==============================================================================
void AudioEngine::setArmedTrack (TrackId id)
{
    armedTrack = id;
    updateMidiRouting();
}

void AudioEngine::updateMidiRouting()
{
    const auto midiChannel = juce::AudioProcessorGraph::midiChannelIndex;

    for (auto& [id, track] : tracks)
    {
        const juce::AudioProcessorGraph::Connection connection { { midiInNode, midiChannel },
                                                                 { track.instrumentNode, midiChannel } };

        if (id == armedTrack)
            graph.addConnection (connection);
        else
            graph.removeConnection (connection);
    }
}
