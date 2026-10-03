#include "AudioEngine.h"
#include "UserData.h"
#include "engine/AudioChannelProcessor.h"
#include "engine/MidiSourceProcessor.h"
#include "engine/MidiRouteProcessor.h"
#include "engine/MidiRecorderProcessor.h"

namespace
{
    constexpr auto audioStateKey = "audioDeviceState";
    constexpr auto midiChannelIndex = juce::AudioProcessorGraph::midiChannelIndex;
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
    graph.addConnection ({ { midiInNode, midiChannelIndex }, { recorderNode, midiChannelIndex } });
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
void AudioEngine::emitEvent (const juce::String& type, juce::DynamicObject::Ptr data)
{
    projectDirty = true;   // every emitted mutation dirties the project
    ++stateRevision;       // ...and tells every polling view to repaint (see getStateRevision)

    if (eventSink == nullptr || historySuppress)
        return;

    auto object = data != nullptr ? data : juce::DynamicObject::Ptr (new juce::DynamicObject());
    object->setProperty ("event", type);
    eventSink (juce::var (object.get()));
}

void AudioEngine::emitTrackChanged (TrackId id, const juce::String& change)
{
    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    data->setProperty ("change", change);
    emitEvent ("trackChanged", data);
}

void AudioEngine::emitClipChanged (TrackId id)
{
    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("trackId", id);

    if (auto* track = findTrack (id); track != nullptr && track->sequence != nullptr)
    {
        data->setProperty ("notes", (int) track->sequence->getNotes().size());
        data->setProperty ("controls", (int) track->sequence->getControls().size());
    }
    else
    {
        data->setProperty ("notes", 0);
        data->setProperty ("controls", 0);
    }

    emitEvent ("clipChanged", data);
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

AudioEngine::Instrument* AudioEngine::findInstrument (InstrumentId id)
{
    auto it = instruments.find (id);
    return it != instruments.end() ? &it->second : nullptr;
}

const AudioEngine::Instrument* AudioEngine::findInstrument (InstrumentId id) const
{
    auto it = instruments.find (id);
    return it != instruments.end() ? &it->second : nullptr;
}

MidiSourceProcessor* AudioEngine::getSource (TrackId id) const
{
    if (auto* track = findTrack (id))
        if (auto* node = graph.getNodeForId (track->midiSourceNode))
            return dynamic_cast<MidiSourceProcessor*> (node->getProcessor());

    return nullptr;
}

MidiSequence::Ptr AudioEngine::eraseRangeFrom (const MidiSequence::Ptr& sequence, juce::int64 start, juce::int64 end)
{
    if (sequence == nullptr || end <= start)
        return sequence;

    auto notes = sequence->getNotes();
    auto controls = sequence->getControls();

    std::erase_if (notes, [start, end] (const auto& n) { return n.startTick >= start && n.startTick < end; });

    for (auto& note : notes)
        if (note.startTick < start && note.startTick + note.lengthTicks > start)
            note.lengthTicks = start - note.startTick;

    std::erase_if (controls, [start, end] (const auto& c) { return c.tick >= start && c.tick < end; });

    if (notes.empty() && controls.empty())
        return nullptr;

    return MidiSequence::create (std::move (notes), std::move (controls));
}

MidiRouteProcessor* AudioEngine::getRoute (const Output& output) const
{
    if (auto* node = graph.getNodeForId (output.routeNode))
        return dynamic_cast<MidiRouteProcessor*> (node->getProcessor());

    return nullptr;
}

//==============================================================================
void AudioEngine::addInstrument (const juce::PluginDescription& description, InstrumentCallback callback)
{
    auto* device = deviceManager.getCurrentAudioDevice();
    const auto sampleRate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
    const auto blockSize  = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;

    std::weak_ptr<int> alive = lifetimeToken;
    const auto startMs = juce::Time::getMillisecondCounterHiRes();
    juce::Logger::writeToLog ("Loading instrument: " + description.name + " (" + description.fileOrIdentifier + ")");

    formatManager.createPluginInstanceAsync (description, sampleRate, blockSize,
        [this, alive, callback, startMs, name = description.name]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            if (alive.expired())
                return;

            juce::Logger::writeToLog ((instance != nullptr ? "Loaded instrument: " : "FAILED to load instrument: ") + name
                                      + " in " + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - startMs)) + " ms"
                                      + (error.isNotEmpty() ? " - " + error : juce::String()));

            if (instance == nullptr)
            {
                if (callback) callback (0, error);
                return;
            }

            const auto numOuts = instance->getTotalNumOutputChannels();
            const auto id = nextInstrumentId++;

            Instrument instrument;
            instrument.name = name;
            instrument.pluginNode = graph.addNode (std::move (instance))->nodeID;

            // Give the instrument its audio channel strip.
            AudioChannel channel;
            channel.name = name;
            channel.node = graph.addNode (std::make_unique<AudioChannelProcessor>())->nodeID;

            for (int ch = 0; ch < 2; ++ch)
                graph.addConnection ({ { channel.node, ch }, { audioOutNode, ch } });

            // Only the first stereo pair for now; multi-output routing comes later.
            if (numOuts == 1)
            {
                graph.addConnection ({ { instrument.pluginNode, 0 }, { channel.node, 0 } });
                graph.addConnection ({ { instrument.pluginNode, 0 }, { channel.node, 1 } });
            }
            else
            {
                for (int ch = 0; ch < juce::jmin (2, numOuts); ++ch)
                    graph.addConnection ({ { instrument.pluginNode, ch }, { channel.node, ch } });
            }

            const auto channelId = nextAudioChannelId++;
            channel.input = id;
            channel.position = nextChildPosition (false, 0);
            audioChannels[channelId] = channel;

            instrument.audioChannel = channelId;
            instruments[id] = std::move (instrument);

            auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
            data->setProperty ("id", id);
            data->setProperty ("name", name);
            data->setProperty ("audioChannelId", channelId);
            emitEvent ("instrumentAdded", data);

            if (callback) callback (id, {});
        });
}

void AudioEngine::removeInstrument (InstrumentId id)
{
    auto* instrument = findInstrument (id);

    if (instrument == nullptr)
        return;

    for (auto& [trackId, track] : tracks)
    {
        for (auto it = track.outputs.begin(); it != track.outputs.end();)
        {
            if (it->instrument == id)
            {
                graph.removeNode (it->routeNode);
                it = track.outputs.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    graph.removeNode (instrument->pluginNode);

    if (auto channelIt = audioChannels.find (instrument->audioChannel); channelIt != audioChannels.end())
    {
        graph.removeNode (channelIt->second.node);
        audioChannels.erase (channelIt);
    }

    instruments.erase (id);

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    emitEvent ("instrumentRemoved", data);
}

std::vector<std::pair<AudioEngine::InstrumentId, juce::String>> AudioEngine::getInstruments() const
{
    std::vector<std::pair<InstrumentId, juce::String>> result;

    for (auto& [id, instrument] : instruments)
        result.emplace_back (id, instrument.name);

    return result;
}

juce::AudioPluginInstance* AudioEngine::getInstrumentPlugin (InstrumentId id) const
{
    if (auto* instrument = findInstrument (id))
        if (auto* node = graph.getNodeForId (instrument->pluginNode))
            return dynamic_cast<juce::AudioPluginInstance*> (node->getProcessor());

    return nullptr;
}

juce::String AudioEngine::getInstrumentName (InstrumentId id) const
{
    if (auto* instrument = findInstrument (id))
        return instrument->name;

    return {};
}

void AudioEngine::setInstrumentChannelName (InstrumentId id, int midiChannel, const juce::String& name)
{
    if (auto* instrument = findInstrument (id))
    {
        if (name.isEmpty())
            instrument->channelNames.erase (midiChannel);
        else
            instrument->channelNames[midiChannel] = name;
    }
}

juce::String AudioEngine::getInstrumentChannelName (InstrumentId id, int midiChannel) const
{
    if (auto* instrument = findInstrument (id))
        if (auto it = instrument->channelNames.find (midiChannel); it != instrument->channelNames.end())
            return it->second;

    return {};
}

int AudioEngine::getNumLoadedInstruments() const
{
    return (int) instruments.size();
}

//==============================================================================
AudioChannelProcessor* AudioEngine::getAudioChannel (AudioChannelId id) const
{
    if (auto it = audioChannels.find (id); it != audioChannels.end())
        if (auto* node = graph.getNodeForId (it->second.node))
            return dynamic_cast<AudioChannelProcessor*> (node->getProcessor());

    return nullptr;
}

AudioEngine::AudioChannelId AudioEngine::getAudioChannelForInstrument (InstrumentId id) const
{
    if (auto* instrument = findInstrument (id))
        return instrument->audioChannel;

    return 0;
}

//==============================================================================
AudioEngine::TrackId AudioEngine::addTrack (const juce::String& name)
{
    const auto id = nextTrackId++;

    Track track;
    track.name = name.isNotEmpty() ? name : "Track " + juce::String (id);
    track.midiSourceNode = graph.addNode (std::make_unique<MidiSourceProcessor> (transport))->nodeID;
    track.position = nextChildPosition (true, 0);
    tracks[id] = track;

    if (armedTrack == 0)
        setArmedTrack (id);

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    data->setProperty ("name", tracks[id].name);
    emitEvent ("trackAdded", data);

    return id;
}

void AudioEngine::removeTrack (TrackId id)
{
    auto* track = findTrack (id);

    if (track == nullptr)
        return;

    for (auto& output : track->outputs)
        graph.removeNode (output.routeNode);

    graph.removeNode (track->midiSourceNode);
    tracks.erase (id);

    if (armedTrack == id)
        setArmedTrack (tracks.empty() ? 0 : tracks.begin()->first);

    applyMuteAndSolo();

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    emitEvent ("trackRemoved", data);
}

std::vector<AudioEngine::TrackId> AudioEngine::getTrackIds() const
{
    std::vector<TrackId> result;

    for (auto& [id, track] : tracks)
        result.push_back (id);

    return result;
}

juce::String AudioEngine::getTrackName (TrackId id) const
{
    if (auto* track = findTrack (id))
        return track->name;

    return {};
}

void AudioEngine::setTrackName (TrackId id, const juce::String& name)
{
    if (auto* track = findTrack (id))
    {
        if (name.isNotEmpty())
        {
            track->name = name;
            emitTrackChanged (id, "name");
        }
    }
}

std::vector<AudioEngine::AudioChannelId> AudioEngine::getAudioChannelIds() const
{
    std::vector<AudioChannelId> result;

    for (auto& [id, channel] : audioChannels)
        result.push_back (id);

    return result;
}

juce::String AudioEngine::getAudioChannelName (AudioChannelId id) const
{
    if (auto it = audioChannels.find (id); it != audioChannels.end())
        return it->second.name;

    return {};
}

AudioEngine::InstrumentId AudioEngine::getAudioChannelInput (AudioChannelId id) const
{
    if (auto it = audioChannels.find (id); it != audioChannels.end())
        return it->second.input;

    return 0;
}

void AudioEngine::addTrackOutput (TrackId trackId, InstrumentId instrumentId, int midiChannel)
{
    auto* track = findTrack (trackId);
    auto* instrument = findInstrument (instrumentId);

    if (track == nullptr || instrument == nullptr)
        return;

    Output output;
    output.instrument = instrumentId;
    output.midiChannel = juce::jlimit (1, 16, midiChannel);
    output.routeNode = graph.addNode (std::make_unique<MidiRouteProcessor> (output.midiChannel))->nodeID;

    graph.addConnection ({ { track->midiSourceNode, midiChannelIndex }, { output.routeNode, midiChannelIndex } });
    graph.addConnection ({ { output.routeNode, midiChannelIndex }, { instrument->pluginNode, midiChannelIndex } });

    if (trackId == armedTrack)
        graph.addConnection ({ { midiInNode, midiChannelIndex }, { output.routeNode, midiChannelIndex } });

    track->outputs.push_back (output);
    applyMuteAndSolo();

    juce::Logger::writeToLog ("Track " + juce::String (trackId) + " output -> "
                              + instrument->name + " ch " + juce::String (output.midiChannel));
    emitTrackChanged (trackId, "outputs");
}

void AudioEngine::clearTrackOutputs (TrackId id)
{
    if (auto* track = findTrack (id))
    {
        for (auto& output : track->outputs)
            graph.removeNode (output.routeNode);

        track->outputs.clear();
        emitTrackChanged (id, "outputs");
    }
}

std::vector<AudioEngine::TrackOutput> AudioEngine::getTrackOutputs (TrackId id) const
{
    std::vector<TrackOutput> result;

    if (auto* track = findTrack (id))
        for (auto& output : track->outputs)
            result.push_back ({ output.instrument, output.midiChannel });

    return result;
}

void AudioEngine::setTrackMuted (TrackId id, bool muted)
{
    if (auto* track = findTrack (id))
    {
        track->muted = muted;
        applyMuteAndSolo();
        emitTrackChanged (id, "muted");
    }
}

bool AudioEngine::isTrackMuted (TrackId id) const
{
    auto* track = findTrack (id);
    return track != nullptr && track->muted;
}

void AudioEngine::setTrackSoloed (TrackId id, bool soloed)
{
    if (auto* track = findTrack (id))
    {
        track->soloed = soloed;
        applyMuteAndSolo();
        emitTrackChanged (id, "soloed");
    }
}

bool AudioEngine::isTrackSoloed (TrackId id) const
{
    auto* track = findTrack (id);
    return track != nullptr && track->soloed;
}

void AudioEngine::setTrackRecordReplace (TrackId id, bool replace)
{
    if (auto* track = findTrack (id))
    {
        track->recordReplace = replace;
        emitTrackChanged (id, "recordMode");
    }
}

bool AudioEngine::isTrackRecordReplace (TrackId id) const
{
    auto* track = findTrack (id);
    return track != nullptr && track->recordReplace;
}

void AudioEngine::applyMuteAndSolo()
{
    const auto anySolo = std::any_of (tracks.begin(), tracks.end(),
                                      [] (const auto& entry) { return entry.second.soloed; });

    for (auto& [id, track] : tracks)
    {
        const auto audible = ! track.muted && (! anySolo || track.soloed);

        for (auto& output : track.outputs)
            if (auto* route = getRoute (output))
                route->setRouteEnabled (audible);
    }
}

//==============================================================================
void AudioEngine::applySequence (Track& track, MidiSequence::Ptr sequence)
{
    track.sequence = sequence;

    if (auto* node = graph.getNodeForId (track.midiSourceNode))
        if (auto* source = dynamic_cast<MidiSourceProcessor*> (node->getProcessor()))
            source->setSequence (std::move (sequence));
}

void AudioEngine::setTrackSequence (TrackId id, MidiSequence::Ptr sequence)
{
    if (auto* track = findTrack (id))
    {
        constexpr size_t maxHistory = 200;

        track->undoStack.push_back (track->sequence);

        if (track->undoStack.size() > maxHistory)
            track->undoStack.erase (track->undoStack.begin());

        track->redoStack.clear();

        juce::Logger::writeToLog ("Track " + juce::String (id)
                                  + (sequence != nullptr ? ": sequence set (" + juce::String ((int) sequence->getNotes().size()) + " notes)"
                                                         : ": sequence cleared"));
        applySequence (*track, std::move (sequence));
        emitClipChanged (id);
    }
}

bool AudioEngine::undoTrackSequence (TrackId id)
{
    auto* track = findTrack (id);

    if (track == nullptr || track->undoStack.empty())
        return false;

    track->redoStack.push_back (track->sequence);
    applySequence (*track, track->undoStack.back());
    track->undoStack.pop_back();
    emitClipChanged (id);
    return true;
}

bool AudioEngine::redoTrackSequence (TrackId id)
{
    auto* track = findTrack (id);

    if (track == nullptr || track->redoStack.empty())
        return false;

    track->undoStack.push_back (track->sequence);
    applySequence (*track, track->redoStack.back());
    track->redoStack.pop_back();
    emitClipChanged (id);
    return true;
}

bool AudioEngine::canUndoClip (TrackId id) const
{
    auto* track = findTrack (id);
    return track != nullptr && ! track->undoStack.empty();
}

bool AudioEngine::canRedoClip (TrackId id) const
{
    auto* track = findTrack (id);
    return track != nullptr && ! track->redoStack.empty();
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

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("bpm", getTempoBpm());
    emitEvent ("tempoChanged", data);
}

//==============================================================================
AudioEngine::HistorySnapshot AudioEngine::captureHistorySnapshot() const
{
    HistorySnapshot snapshot;

    for (auto& [id, track] : tracks)
        snapshot.tracks.push_back ({ id, track.name, track.muted, track.soloed, track.recordReplace,
                                     getTrackOutputs (id), track.sequence, track.folder, track.position });

    snapshot.armedTrack = armedTrack;
    snapshot.tempoMap = masterTempoMap;
    snapshot.markers = markers;

    for (auto& [id, channel] : audioChannels)
        if (auto* processor = getAudioChannel (id))
            snapshot.channels.push_back ({ id, processor->getGain(), processor->isMuted(),
                                           channel.folder, channel.position });

    for (auto& [id, folder] : folders)
        snapshot.folders.push_back ({ id, folder.name, folder.midiDomain, folder.parent,
                                      folder.collapsed, folder.position });

    return snapshot;
}

void AudioEngine::applyHistorySnapshot (const HistorySnapshot& snapshot)
{
    historySuppress = true;

    // Folders restore wholesale (ids are stable, nothing in the graph references them)
    folders.clear();

    for (auto& state : snapshot.folders)
    {
        folders[state.id] = { state.name, state.midiDomain, state.parent, state.collapsed, state.position };
        nextFolderId = juce::jmax (nextFolderId, state.id + 1);
    }

    // Tracks that don't exist in the snapshot disappear
    {
        std::vector<TrackId> existing = getTrackIds();

        for (auto id : existing)
            if (std::none_of (snapshot.tracks.begin(), snapshot.tracks.end(),
                              [id] (const auto& t) { return t.id == id; }))
                removeTrack (id);
    }

    for (auto& state : snapshot.tracks)
    {
        if (findTrack (state.id) == nullptr)   // recreate with the same id
        {
            Track track;
            track.name = state.name;
            track.midiSourceNode = graph.addNode (std::make_unique<MidiSourceProcessor> (transport))->nodeID;
            tracks[state.id] = std::move (track);
            nextTrackId = juce::jmax (nextTrackId, state.id + 1);
        }

        auto* track = findTrack (state.id);
        track->name = state.name;
        track->recordReplace = state.recordReplace;
        track->folder = folderExists (state.folder) ? state.folder : 0;
        track->position = state.position;
        setTrackMuted (state.id, state.muted);
        setTrackSoloed (state.id, state.soloed);

        clearTrackOutputs (state.id);
        for (auto& output : state.outputs)
            addTrackOutput (state.id, output.instrument, output.midiChannel);   // gone instruments: no-op

        applySequence (*track, state.sequence);
    }

    setArmedTrack (findTrack (snapshot.armedTrack) != nullptr ? snapshot.armedTrack
                                                              : (tracks.empty() ? 0 : tracks.begin()->first));

    masterTempoMap = snapshot.tempoMap != nullptr ? snapshot.tempoMap : TempoMap::create (120.0);
    transport.setTempoMap (masterTempoMap);
    markers = snapshot.markers;

    for (auto& channel : snapshot.channels)
    {
        if (auto* processor = getAudioChannel (channel.id))
        {
            processor->setGain (channel.gain);
            processor->setMuted (channel.muted);
        }

        if (auto it = audioChannels.find (channel.id); it != audioChannels.end())
        {
            it->second.folder = folderExists (channel.folder) ? channel.folder : 0;
            it->second.position = channel.position;
        }
    }

    historySuppress = false;
    juce::Logger::writeToLog ("History: travelled (" + juce::String ((int) snapshot.tracks.size()) + " tracks)");
    emitEvent ("historyTravelled");
}

//==============================================================================
void AudioEngine::addMarker (juce::int64 tick, const juce::String& name)
{
    tick = juce::jmax ((juce::int64) 0, tick);
    const auto resolvedName = name.isNotEmpty() ? name : juce::String ("Marker");

    removeMarker (tick);
    markers.push_back ({ tick, resolvedName });
    std::sort (markers.begin(), markers.end(), [] (const Marker& a, const Marker& b) { return a.tick < b.tick; });

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("tick", tick);
    data->setProperty ("name", resolvedName);
    emitEvent ("markerAdded", data);
}

void AudioEngine::removeMarker (juce::int64 tick)
{
    if (std::erase_if (markers, [tick] (const Marker& m) { return m.tick == tick; }) > 0)
    {
        auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
        data->setProperty ("tick", tick);
        emitEvent ("markerRemoved", data);
    }
}

//==============================================================================
AudioEngine::FolderId AudioEngine::addFolder (bool midiDomain, const juce::String& name, FolderId parent)
{
    if (parent != 0 && (! folderExists (parent) || folders[parent].midiDomain != midiDomain))
        parent = 0;

    const auto id = nextFolderId++;
    const auto resolvedName = name.isNotEmpty() ? name : "Folder " + juce::String (id);
    folders[id] = { resolvedName, midiDomain, parent, false, nextChildPosition (midiDomain, parent) };

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("folderId", id);
    data->setProperty ("name", resolvedName);
    data->setProperty ("domain", midiDomain ? "midi" : "audio");
    data->setProperty ("parent", parent);
    emitEvent ("folderAdded", data);
    return id;
}

void AudioEngine::removeFolder (FolderId id)
{
    const auto it = folders.find (id);

    if (it == folders.end())
        return;

    const auto parent = it->second.parent;

    // Children and members move up to the removed folder's parent
    for (auto& [childId, child] : folders)
        if (child.parent == id)
            child.parent = parent;

    for (auto& [trackId, track] : tracks)
        if (track.folder == id)
            track.folder = parent;

    for (auto& [channelId, channel] : audioChannels)
        if (channel.folder == id)
            channel.folder = parent;

    folders.erase (it);

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("folderId", id);
    emitEvent ("folderRemoved", data);
}

std::vector<AudioEngine::FolderId> AudioEngine::getFolderIds (bool midiDomain) const
{
    std::vector<FolderId> ids;

    for (auto& [id, folder] : folders)
        if (folder.midiDomain == midiDomain)
            ids.push_back (id);

    return ids;
}

bool AudioEngine::folderExists (FolderId id) const           { return folders.count (id) > 0; }

bool AudioEngine::isFolderMidiDomain (FolderId id) const
{
    const auto it = folders.find (id);
    return it != folders.end() && it->second.midiDomain;
}

juce::String AudioEngine::getFolderName (FolderId id) const
{
    const auto it = folders.find (id);
    return it != folders.end() ? it->second.name : juce::String();
}

void AudioEngine::setFolderName (FolderId id, const juce::String& name)
{
    const auto it = folders.find (id);

    if (it == folders.end() || it->second.name == name || name.isEmpty())
        return;

    it->second.name = name;

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("folderId", id);
    data->setProperty ("name", name);
    data->setProperty ("change", "renamed");
    emitEvent ("folderChanged", data);
}

AudioEngine::FolderId AudioEngine::getFolderParent (FolderId id) const
{
    const auto it = folders.find (id);
    return it != folders.end() ? it->second.parent : 0;
}

bool AudioEngine::setFolderParent (FolderId id, FolderId parent)
{
    const auto it = folders.find (id);

    if (it == folders.end() || id == parent)
        return false;

    if (parent != 0)
    {
        const auto parentIt = folders.find (parent);

        if (parentIt == folders.end() || parentIt->second.midiDomain != it->second.midiDomain)
            return false;

        // No cycles: the new parent must not sit below this folder
        for (auto walk = parent; walk != 0; walk = folders.at (walk).parent)
            if (walk == id)
                return false;
    }

    if (it->second.parent == parent)
        return true;

    it->second.position = nextChildPosition (it->second.midiDomain, parent);   // land at the end
    it->second.parent = parent;

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("folderId", id);
    data->setProperty ("parent", parent);
    data->setProperty ("change", "parent");
    emitEvent ("folderChanged", data);
    return true;
}

bool AudioEngine::isFolderCollapsed (FolderId id) const
{
    const auto it = folders.find (id);
    return it != folders.end() && it->second.collapsed;
}

void AudioEngine::setFolderCollapsed (FolderId id, bool collapsed)
{
    const auto it = folders.find (id);

    if (it == folders.end() || it->second.collapsed == collapsed)
        return;

    it->second.collapsed = collapsed;

    // View state, not an edit: HistoryManager skips this event type
    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("folderId", id);
    data->setProperty ("collapsed", collapsed);
    emitEvent ("folderViewChanged", data);
}

void AudioEngine::setTrackFolder (TrackId trackId, FolderId folderId)
{
    auto* track = findTrack (trackId);

    if (track == nullptr)
        return;

    if (folderId != 0 && ! isFolderMidiDomain (folderId))
        folderId = 0;

    if (track->folder == folderId)
        return;

    track->position = nextChildPosition (true, folderId);   // land at the end
    track->folder = folderId;
    emitTrackChanged (trackId, "folder");
}

AudioEngine::FolderId AudioEngine::getTrackFolder (TrackId trackId) const
{
    const auto* track = findTrack (trackId);
    return track != nullptr && folderExists (track->folder) ? track->folder : 0;
}

void AudioEngine::setAudioChannelFolder (AudioChannelId channelId, FolderId folderId)
{
    const auto it = audioChannels.find (channelId);

    if (it == audioChannels.end())
        return;

    if (folderId != 0 && (! folderExists (folderId) || isFolderMidiDomain (folderId)))
        folderId = 0;

    if (it->second.folder == folderId)
        return;

    it->second.position = nextChildPosition (false, folderId);   // land at the end
    it->second.folder = folderId;

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("channelId", channelId);
    data->setProperty ("folderId", folderId);
    data->setProperty ("change", "folder");
    emitEvent ("channelChanged", data);
}

AudioEngine::FolderId AudioEngine::getAudioChannelFolder (AudioChannelId channelId) const
{
    const auto it = audioChannels.find (channelId);
    return it != audioChannels.end() && folderExists (it->second.folder) ? it->second.folder : 0;
}

std::vector<AudioEngine::ChildRef> AudioEngine::getChildrenOf (bool midiDomain, FolderId parent) const
{
    std::vector<ChildRef> children;

    // Members whose folder no longer exists show at the root
    const auto effectiveFolder = [this] (FolderId f) { return folderExists (f) ? f : 0; };

    for (auto& [id, folder] : folders)
        if (folder.midiDomain == midiDomain && folder.parent == parent)
            children.push_back ({ true, id, folder.position });

    if (midiDomain)
    {
        for (auto& [id, track] : tracks)
            if (effectiveFolder (track.folder) == parent)
                children.push_back ({ false, id, track.position });
    }
    else
    {
        for (auto& [id, channel] : audioChannels)
            if (effectiveFolder (channel.folder) == parent)
                children.push_back ({ false, id, channel.position });
    }

    // Position decides; legacy ties fall back to folders-first, then id
    std::sort (children.begin(), children.end(), [] (const ChildRef& a, const ChildRef& b)
    {
        if (a.position != b.position)   return a.position < b.position;
        if (a.isFolder != b.isFolder)   return a.isFolder;
        return a.id < b.id;
    });

    return children;
}

int AudioEngine::nextChildPosition (bool midiDomain, FolderId parent) const
{
    const auto children = getChildrenOf (midiDomain, parent);
    return children.empty() ? 0 : children.back().position + 1;
}

void AudioEngine::setChildPosition (bool midiDomain, const ChildRef& child, int position)
{
    if (child.isFolder)
    {
        if (auto it = folders.find (child.id); it != folders.end())
            it->second.position = position;
    }
    else if (midiDomain)
    {
        if (auto* track = findTrack (child.id))
            track->position = position;
    }
    else
    {
        if (auto it = audioChannels.find (child.id); it != audioChannels.end())
            it->second.position = position;
    }
}

std::vector<AudioEngine::SidebarItem> AudioEngine::getSidebarItems (bool midiDomain, bool skipCollapsed) const
{
    std::vector<SidebarItem> items;

    const std::function<void (FolderId, int)> visit = [&] (FolderId parent, int depth)
    {
        for (auto& child : getChildrenOf (midiDomain, parent))
        {
            if (child.isFolder)
            {
                items.push_back ({ child.id, 0, depth, parent });

                if (! (skipCollapsed && isFolderCollapsed (child.id)))
                    visit (child.id, depth + 1);
            }
            else
            {
                items.push_back ({ 0, child.id, depth, parent });
            }
        }
    };

    visit (0, 0);
    return items;
}

bool AudioEngine::moveSidebarItems (bool midiDomain, const std::vector<FolderId>& folderIds,
                                    const std::vector<int>& memberIds, FolderId parent, int index)
{
    if (folderIds.empty() && memberIds.empty())
        return false;

    if (parent != 0 && (! folderExists (parent) || isFolderMidiDomain (parent) != midiDomain))
        return false;

    // Validate the moved nodes; a folder may not move into itself or its own subtree
    for (auto folderId : folderIds)
    {
        const auto it = folders.find (folderId);

        if (it == folders.end() || it->second.midiDomain != midiDomain)
            return false;

        for (auto walk = parent; walk != 0; walk = folders.at (walk).parent)
            if (walk == folderId)
                return false;
    }

    for (auto memberId : memberIds)
        if (midiDomain ? (findTrack (memberId) == nullptr) : (audioChannels.count (memberId) == 0))
            return false;

    const auto isMoved = [&] (const ChildRef& child)
    {
        return child.isFolder ? std::find (folderIds.begin(), folderIds.end(), child.id) != folderIds.end()
                              : std::find (memberIds.begin(), memberIds.end(), child.id) != memberIds.end();
    };

    // Target sibling list without the moved nodes; clamp the insertion index to it
    std::vector<ChildRef> siblings;

    for (auto& child : getChildrenOf (midiDomain, parent))
        if (! isMoved (child))
            siblings.push_back (child);

    index = juce::jlimit (0, (int) siblings.size(), index);

    // The moved nodes as one group, in the order the caller gave (folders may mix
    // with members; the UI passes them in visual order via the two lists)
    std::vector<ChildRef> moved;

    for (auto folderId : folderIds)
        moved.push_back ({ true, folderId, 0 });

    for (auto memberId : memberIds)
        moved.push_back ({ false, memberId, 0 });

    siblings.insert (siblings.begin() + index, moved.begin(), moved.end());

    // Re-parent the moved nodes and renumber the whole sibling list
    for (auto folderId : folderIds)
        folders[folderId].parent = parent;

    for (auto memberId : memberIds)
    {
        if (midiDomain)
            findTrack (memberId)->folder = parent;
        else
            audioChannels[memberId].folder = parent;
    }

    for (int i = 0; i < (int) siblings.size(); ++i)
        setChildPosition (midiDomain, siblings[(size_t) i], i);

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("domain", midiDomain ? "midi" : "audio");
    data->setProperty ("parent", parent);
    data->setProperty ("count", (int) (folderIds.size() + memberIds.size()));
    emitEvent ("sidebarMoved", data);
    return true;
}

std::vector<AudioEngine::TrackId> AudioEngine::getArrangeTrackOrder() const
{
    std::vector<TrackId> order;

    for (auto& item : getSidebarItems (true, true))
        if (item.member != 0)
            order.push_back (item.member);

    return order;
}

//==============================================================================
bool AudioEngine::startRecording()
{
    auto* track = findTrack (armedTrack);

    if (track == nullptr || isRecording())
        return false;

    takeIsReplace = track->recordReplace;
    preTakeSequence = track->sequence;
    replaceFromTick = -1;

    // Replace mode: the track's own material is silent for the whole take.
    if (takeIsReplace)
        if (auto* source = getSource (armedTrack))
            source->setSuppressed (true);

    recordingSawPlayback = false;
    recorder->start (armedTrack);
    juce::Logger::writeToLog ("Recording started on track " + juce::String (armedTrack));

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("trackId", armedTrack);
    emitEvent ("recordingStarted", data);

    if (! transport.isPlaying())
        transport.play();

    return true;
}

void AudioEngine::stopRecording()
{
    if (! isRecording())
        return;

    const auto trackId = recorder->getTrackId();
    const auto stopTick = transport.getPositionTicks();
    const auto result = recorder->finish (stopTick);

    if (auto* source = getSource (trackId))
        source->setSuppressed (false);

    // finish() drains the FIFO, so the first input may only be known now.
    if (takeIsReplace && replaceFromTick < 0 && recorder->getFirstEventTick() >= 0)
        replaceFromTick = recorder->getFirstEventTick();

    juce::Logger::writeToLog ("Recording stopped on track " + juce::String (trackId) + ": "
                              + juce::String ((int) result.notes.size()) + " notes, "
                              + juce::String ((int) result.controls.size()) + " control events"
                              + (takeIsReplace ? " (replace mode)" : ""));

    if (takeIsReplace && replaceFromTick >= 0)
    {
        // One undoable step: pre-take material erased from first input to stop, plus the take.
        auto base = eraseRangeFrom (preTakeSequence, replaceFromTick, juce::jmax (replaceFromTick + 1, stopTick));

        auto notes = result.notes;
        auto controls = result.controls;

        if (base != nullptr)
        {
            notes.insert (notes.end(), base->getNotes().begin(), base->getNotes().end());
            controls.insert (controls.end(), base->getControls().begin(), base->getControls().end());
        }

        if (auto* track = findTrack (trackId))
            applySequence (*track, preTakeSequence);   // so the undo snapshot is the pre-take state

        setTrackSequence (trackId, notes.empty() && controls.empty()
                                       ? nullptr
                                       : MidiSequence::create (std::move (notes), std::move (controls)));
    }
    else
    {
        mergeIntoTrack (trackId, result);
    }

    takeIsReplace = false;
    preTakeSequence = nullptr;

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("trackId", trackId);
    data->setProperty ("notes", (int) result.notes.size());
    data->setProperty ("controls", (int) result.controls.size());
    emitEvent ("recordingFinished", data);
}

void AudioEngine::pollRecording()
{
    if (! isRecording())
        return;

    recorder->poll();

    if (takeIsReplace)
    {
        recorder->consumeWrapFlag();   // replace mode commits once, at stop

        if (replaceFromTick < 0 && recorder->getFirstEventTick() >= 0)
            replaceFromTick = recorder->getFirstEventTick();
    }
    else if (recorder->consumeWrapFlag())
    {
        // Add mode: commit each loop pass so it's audible on the next one.
        mergeIntoTrack (recorder->getTrackId(), recorder->takePending());
    }

    // The transport reports playing only once the audio thread has confirmed it.
    if (transport.isPlaying())
        recordingSawPlayback = true;
    else if (recordingSawPlayback)
        stopRecording();
}

void AudioEngine::addToTrackSequence (TrackId id, std::vector<MidiSequence::Note> notes,
                                      std::vector<MidiSequence::Control> controls)
{
    auto* track = findTrack (id);

    if (track == nullptr || (notes.empty() && controls.empty()))
        return;

    if (track->sequence != nullptr)
    {
        const auto& existing = *track->sequence;
        notes.insert (notes.end(), existing.getNotes().begin(), existing.getNotes().end());
        controls.insert (controls.end(), existing.getControls().begin(), existing.getControls().end());
    }

    setTrackSequence (id, MidiSequence::create (std::move (notes), std::move (controls)));
}

void AudioEngine::mergeIntoTrack (TrackId id, const MidiRecorder::Result& result)
{
    addToTrackSequence (id, result.notes, result.controls);
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
bool AudioEngine::saveProject (const juce::File& file)
{
    juce::XmlElement root ("ORCHESTRAL_DAW_PROJECT");
    root.setAttribute ("version", 1);
    root.addChildElement (masterTempoMap->toXml().release());

    for (auto& marker : markers)
    {
        auto* m = root.createNewChildElement ("MARKER");
        m->setAttribute ("tick", juce::String (marker.tick));
        m->setAttribute ("name", marker.name);
    }

    for (auto& [id, folder] : folders)
    {
        auto* f = root.createNewChildElement ("FOLDER");
        f->setAttribute ("id", id);
        f->setAttribute ("name", folder.name);
        f->setAttribute ("midi", folder.midiDomain);
        f->setAttribute ("parent", folder.parent);
        f->setAttribute ("collapsed", folder.collapsed);
        f->setAttribute ("position", folder.position);
    }

    for (auto& [id, instrument] : instruments)
    {
        auto* e = root.createNewChildElement ("INSTRUMENT");
        e->setAttribute ("id", id);
        e->setAttribute ("name", instrument.name);

        if (auto* plugin = getInstrumentPlugin (id))
        {
            e->addChildElement (plugin->getPluginDescription().createXml().release());

            juce::MemoryBlock state;
            plugin->getStateInformation (state);

            if (state.getSize() > 0)
                e->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
        }

        for (auto& [channel, channelName] : instrument.channelNames)
        {
            auto* c = e->createNewChildElement ("CHANNELNAME");
            c->setAttribute ("channel", channel);
            c->setAttribute ("name", channelName);
        }

        if (auto* audioChannel = getAudioChannel (instrument.audioChannel))
        {
            auto* a = e->createNewChildElement ("AUDIOCHANNEL");
            a->setAttribute ("gain", audioChannel->getGain());
            a->setAttribute ("muted", audioChannel->isMuted());
            a->setAttribute ("folder", getAudioChannelFolder (instrument.audioChannel));

            if (auto it = audioChannels.find (instrument.audioChannel); it != audioChannels.end())
                a->setAttribute ("position", it->second.position);
        }
    }

    for (auto& [id, track] : tracks)
    {
        auto* e = root.createNewChildElement ("TRACK");
        e->setAttribute ("name", track.name);
        e->setAttribute ("muted", track.muted);
        e->setAttribute ("soloed", track.soloed);
        e->setAttribute ("armed", id == armedTrack);
        e->setAttribute ("recordReplace", track.recordReplace);
        e->setAttribute ("folder", getTrackFolder (id));
        e->setAttribute ("position", track.position);

        for (auto& output : track.outputs)
        {
            auto* o = e->createNewChildElement ("OUTPUT");
            o->setAttribute ("instrument", output.instrument);
            o->setAttribute ("channel", output.midiChannel);
        }

        if (track.sequence != nullptr)
            e->addChildElement (track.sequence->toXml().release());
    }

    juce::TemporaryFile temp (file);
    const auto ok = root.writeTo (temp.getFile()) && temp.overwriteTargetFileWithTemporary();

    juce::Logger::writeToLog ((ok ? "Saved project: " : "FAILED to save project: ") + file.getFullPathName());

    if (ok)
    {
        auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
        data->setProperty ("path", file.getFullPathName());
        emitEvent ("projectSaved", data);
        markProjectClean();
    }

    return ok;
}

void AudioEngine::clearProject()
{
    if (isRecording())
        recorder->finish (transport.getPositionTicks());   // discard the take

    transport.stop();
    transport.setLooping (false);
    transport.returnToZero();

    for (auto& [id, track] : tracks)
    {
        for (auto& output : track.outputs)
            graph.removeNode (output.routeNode);

        graph.removeNode (track.midiSourceNode);
    }

    tracks.clear();

    for (auto& [id, instrument] : instruments)
        graph.removeNode (instrument.pluginNode);

    instruments.clear();

    for (auto& [id, channel] : audioChannels)
        graph.removeNode (channel.node);

    audioChannels.clear();

    armedTrack = 0;
    markers.clear();
    folders.clear();
    masterTempoMap = TempoMap::create (120.0);
    transport.setTempoMap (masterTempoMap);

    juce::Logger::writeToLog ("Project cleared");
    emitEvent ("projectCleared");
    markProjectClean();
}

void AudioEngine::loadProject (const juce::File& file, std::function<void (bool, juce::String)> done)
{
    auto xml = juce::parseXML (file);

    if (xml == nullptr || ! xml->hasTagName ("ORCHESTRAL_DAW_PROJECT"))
    {
        if (done)
            done (false, file.getFileName() + " is not an Orchestral DAW project");
        return;
    }

    juce::Logger::writeToLog ("Loading project: " + file.getFullPathName());
    clearProject();

    if (auto* tempoXml = xml->getChildByName ("TEMPOMAP"))
    {
        masterTempoMap = TempoMap::fromXml (*tempoXml);
        transport.setTempoMap (masterTempoMap);
    }

    for (auto* m : xml->getChildWithTagNameIterator ("MARKER"))
        addMarker (m->getStringAttribute ("tick").getLargeIntValue(), m->getStringAttribute ("name"));

    // Folders: create first (ids change), then wire parents and collapse states.
    std::map<int, FolderId> folderIdMap;

    for (auto* f : xml->getChildWithTagNameIterator ("FOLDER"))
        folderIdMap[f->getIntAttribute ("id")] = addFolder (f->getBoolAttribute ("midi", true),
                                                            f->getStringAttribute ("name"));

    for (auto* f : xml->getChildWithTagNameIterator ("FOLDER"))
    {
        const auto id = folderIdMap[f->getIntAttribute ("id")];

        if (auto it = folderIdMap.find (f->getIntAttribute ("parent")); it != folderIdMap.end())
            setFolderParent (id, it->second);

        setFolderCollapsed (id, f->getBoolAttribute ("collapsed"));

        if (f->hasAttribute ("position"))
            folders[id].position = f->getIntAttribute ("position");
    }

    struct LoadState
    {
        std::unique_ptr<juce::XmlElement> xml;
        std::vector<juce::XmlElement*> instrumentElements;
        size_t next = 0;
        std::map<int, InstrumentId> idMap;
        std::map<int, FolderId> folderIdMap;
        juce::StringArray warnings;
        juce::String path;
        std::function<void (bool, juce::String)> done;
    };

    auto state = std::make_shared<LoadState>();
    state->xml = std::move (xml);
    state->folderIdMap = std::move (folderIdMap);
    state->path = file.getFullPathName();
    state->done = std::move (done);

    for (auto* e : state->xml->getChildWithTagNameIterator ("INSTRUMENT"))
        state->instrumentElements.push_back (e);

    // Instruments instantiate asynchronously, one after another; then the tracks.
    auto step = std::make_shared<std::function<void()>>();

    *step = [this, state, step]
    {
        if (state->next >= state->instrumentElements.size())
        {
            restoreProjectTracks (*state->xml, state->idMap, state->folderIdMap, state->warnings);

            auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
            data->setProperty ("path", state->path);
            emitEvent ("projectLoaded", data);
            markProjectClean();

            if (state->done)
                state->done (true, state->warnings.joinIntoString ("\n"));

            *step = nullptr;   // break the shared_ptr self-reference
            return;
        }

        auto* element = state->instrumentElements[state->next++];
        const auto savedName = element->getStringAttribute ("name", "instrument");

        juce::PluginDescription description;
        auto* pluginXml = element->getChildByName ("PLUGIN");

        if (pluginXml == nullptr || ! description.loadFromXml (*pluginXml))
        {
            state->warnings.add (savedName + ": missing plugin description");
            (*step)();
            return;
        }

        addInstrument (description,
            [this, state, step, element, savedName] (InstrumentId newId, const juce::String& error)
            {
                if (newId == 0)
                {
                    state->warnings.add (savedName + ": " + error);
                    (*step)();
                    return;
                }

                state->idMap[element->getIntAttribute ("id")] = newId;

                if (auto* stateElement = element->getChildByName ("STATE"))
                {
                    juce::MemoryBlock block;

                    if (block.fromBase64Encoding (stateElement->getAllSubText().trim()) && block.getSize() > 0)
                        if (auto* plugin = getInstrumentPlugin (newId))
                            plugin->setStateInformation (block.getData(), (int) block.getSize());
                }

                for (auto* c : element->getChildWithTagNameIterator ("CHANNELNAME"))
                    setInstrumentChannelName (newId, c->getIntAttribute ("channel"), c->getStringAttribute ("name"));

                if (auto* a = element->getChildByName ("AUDIOCHANNEL"))
                {
                    const auto channelId = getAudioChannelForInstrument (newId);

                    if (auto* audioChannel = getAudioChannel (channelId))
                    {
                        audioChannel->setGain ((float) a->getDoubleAttribute ("gain", 1.0));
                        audioChannel->setMuted (a->getBoolAttribute ("muted"));
                    }

                    if (auto it = state->folderIdMap.find (a->getIntAttribute ("folder"));
                        it != state->folderIdMap.end())
                        setAudioChannelFolder (channelId, it->second);

                    if (a->hasAttribute ("position"))
                        if (auto channelIt = audioChannels.find (channelId); channelIt != audioChannels.end())
                            channelIt->second.position = a->getIntAttribute ("position");
                }

                (*step)();
            });
    };

    (*step)();
}

void AudioEngine::restoreProjectTracks (const juce::XmlElement& root, const std::map<int, InstrumentId>& instrumentIds,
                                        const std::map<int, FolderId>& folderIds, juce::StringArray& warnings)
{
    for (auto* e : root.getChildWithTagNameIterator ("TRACK"))
    {
        const auto trackId = addTrack();
        setTrackName (trackId, e->getStringAttribute ("name"));
        setTrackMuted (trackId, e->getBoolAttribute ("muted"));
        setTrackSoloed (trackId, e->getBoolAttribute ("soloed"));
        setTrackRecordReplace (trackId, e->getBoolAttribute ("recordReplace"));

        if (auto it = folderIds.find (e->getIntAttribute ("folder")); it != folderIds.end())
            setTrackFolder (trackId, it->second);

        if (e->hasAttribute ("position"))
            findTrack (trackId)->position = e->getIntAttribute ("position");

        for (auto* o : e->getChildWithTagNameIterator ("OUTPUT"))
        {
            const auto savedInstrument = o->getIntAttribute ("instrument");

            if (auto it = instrumentIds.find (savedInstrument); it != instrumentIds.end())
                addTrackOutput (trackId, it->second, o->getIntAttribute ("channel", 1));
            else
                warnings.add (getTrackName (trackId) + ": output skipped (its instrument didn't load)");
        }

        if (auto* sequenceXml = e->getChildByName ("SEQUENCE"))
            setTrackSequence (trackId, MidiSequence::fromXml (*sequenceXml));

        if (e->getBoolAttribute ("armed"))
            setArmedTrack (trackId);
    }

    // A freshly loaded project starts with clean clip histories.
    for (auto& [id, track] : tracks)
    {
        track.undoStack.clear();
        track.redoStack.clear();
    }
}

//==============================================================================
void AudioEngine::setArmedTrack (TrackId id)
{
    armedTrack = id;
    updateMidiRouting();

    if (id != 0)
        emitTrackChanged (id, "armed");
}

void AudioEngine::updateMidiRouting()
{
    for (auto& [id, track] : tracks)
    {
        for (auto& output : track.outputs)
        {
            const juce::AudioProcessorGraph::Connection connection { { midiInNode, midiChannelIndex },
                                                                     { output.routeNode, midiChannelIndex } };

            if (id == armedTrack)
                graph.addConnection (connection);
            else
                graph.removeConnection (connection);
        }
    }
}
