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
            audioChannels[channelId] = channel;

            instrument.audioChannel = channelId;
            instruments[id] = std::move (instrument);

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
AudioEngine::TrackId AudioEngine::addTrack()
{
    const auto id = nextTrackId++;

    Track track;
    track.name = "Track " + juce::String (id);
    track.midiSourceNode = graph.addNode (std::make_unique<MidiSourceProcessor> (transport))->nodeID;
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

    for (auto& output : track->outputs)
        graph.removeNode (output.routeNode);

    graph.removeNode (track->midiSourceNode);
    tracks.erase (id);

    if (armedTrack == id)
        setArmedTrack (tracks.empty() ? 0 : tracks.begin()->first);

    applyMuteAndSolo();
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
        if (name.isNotEmpty())
            track->name = name;
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
}

void AudioEngine::clearTrackOutputs (TrackId id)
{
    if (auto* track = findTrack (id))
    {
        for (auto& output : track->outputs)
            graph.removeNode (output.routeNode);

        track->outputs.clear();
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
    }
}

bool AudioEngine::isTrackSoloed (TrackId id) const
{
    auto* track = findTrack (id);
    return track != nullptr && track->soloed;
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
}

//==============================================================================
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
        }
    }

    for (auto& [id, track] : tracks)
    {
        auto* e = root.createNewChildElement ("TRACK");
        e->setAttribute ("name", track.name);
        e->setAttribute ("muted", track.muted);
        e->setAttribute ("soloed", track.soloed);
        e->setAttribute ("armed", id == armedTrack);

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
    masterTempoMap = TempoMap::create (120.0);
    transport.setTempoMap (masterTempoMap);

    juce::Logger::writeToLog ("Project cleared");
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

    struct LoadState
    {
        std::unique_ptr<juce::XmlElement> xml;
        std::vector<juce::XmlElement*> instrumentElements;
        size_t next = 0;
        std::map<int, InstrumentId> idMap;
        juce::StringArray warnings;
        std::function<void (bool, juce::String)> done;
    };

    auto state = std::make_shared<LoadState>();
    state->xml = std::move (xml);
    state->done = std::move (done);

    for (auto* e : state->xml->getChildWithTagNameIterator ("INSTRUMENT"))
        state->instrumentElements.push_back (e);

    // Instruments instantiate asynchronously, one after another; then the tracks.
    auto step = std::make_shared<std::function<void()>>();

    *step = [this, state, step]
    {
        if (state->next >= state->instrumentElements.size())
        {
            restoreProjectTracks (*state->xml, state->idMap, state->warnings);

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
                    if (auto* audioChannel = getAudioChannel (getAudioChannelForInstrument (newId)))
                    {
                        audioChannel->setGain ((float) a->getDoubleAttribute ("gain", 1.0));
                        audioChannel->setMuted (a->getBoolAttribute ("muted"));
                    }
                }

                (*step)();
            });
    };

    (*step)();
}

void AudioEngine::restoreProjectTracks (const juce::XmlElement& root, const std::map<int, InstrumentId>& instrumentIds,
                                        juce::StringArray& warnings)
{
    for (auto* e : root.getChildWithTagNameIterator ("TRACK"))
    {
        const auto trackId = addTrack();
        setTrackName (trackId, e->getStringAttribute ("name"));
        setTrackMuted (trackId, e->getBoolAttribute ("muted"));
        setTrackSoloed (trackId, e->getBoolAttribute ("soloed"));

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
