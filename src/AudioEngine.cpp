#include "AudioEngine.h"
#include "model/PlaybackSequence.h"
#include "ui/EditorSettings.h"
#include "UserData.h"
#include "engine/AudioChannelProcessor.h"
#include "engine/MidiSourceProcessor.h"
#include "engine/MidiRouteProcessor.h"
#include "engine/MidiRecorderProcessor.h"
#include <pluginterfaces/vst/ivstcomponent.h>   // MIDI port count = VST3 event input buses

namespace
{
    constexpr auto audioStateKey = "audioDeviceState";
    constexpr auto midiChannelIndex = juce::AudioProcessorGraph::midiChannelIndex;
    using IOProcessor = juce::AudioProcessorGraph::AudioGraphIOProcessor;

}

// Graph edits never rebuild synchronously: JUCE's default (sync) rebuilds the
// whole render sequence on every call, which made big syncs/loads quadratic.
// Normally edits coalesce into one async rebuild; inside a batch (vepro.sync,
// project loads) they don't rebuild at all until the batch ends - otherwise
// each async plugin load waited behind a rebuild of the ever-growing graph.
juce::AudioProcessorGraph::UpdateKind AudioEngine::updateKind() const noexcept
{
    return graphBatchDepth > 0 ? juce::AudioProcessorGraph::UpdateKind::none
                               : juce::AudioProcessorGraph::UpdateKind::async;
}

void AudioEngine::beginGraphBatch()
{
    ++graphBatchDepth;
}

void AudioEngine::endGraphBatch()
{
    if (graphBatchDepth > 0 && --graphBatchDepth == 0)
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        graph.rebuild();
        juce::Logger::writeToLog ("Graph batch rebuilt in "
                                  + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - start))
                                  + " ms (" + juce::String (graph.getNumNodes()) + " nodes)");
    }
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

    // The master bus: every channel feeds it, it feeds the device (the mixer's master strip). Its
    // connection to the output is made once the graph has its channels (connectMasterOutput)
    masterNode = graph.addNode (std::make_unique<AudioChannelProcessor>())->nodeID;
    midiInNode   = graph.addNode (std::make_unique<IOProcessor> (IOProcessor::midiInputNode))->nodeID;

    // Permanent tap on the live MIDI input for recording.
    auto recorderNodePtr = graph.addNode (std::make_unique<MidiRecorderProcessor> (transport));
    recorderNode = recorderNodePtr->nodeID;
    graph.addConnection ({ { midiInNode, midiChannelIndex }, { recorderNode, midiChannelIndex } });
    recorder = std::make_unique<MidiRecorder> (static_cast<MidiRecorderProcessor&> (*recorderNodePtr->getProcessor()));

    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&ioCallback);
    connectMasterOutput();
    controllerDevices = juce::StringArray::fromLines (settings.getValue ("midiControllers"));
    controllerDevices.removeEmptyStrings();
    deviceManager.addMidiInputDeviceCallback ({}, &inputRouter);
}

AudioEngine::~AudioEngine()
{
    saveSettings();

    deviceManager.removeMidiInputDeviceCallback ({}, &inputRouter);
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

//==============================================================================
// MIDI controllers (control surfaces)
void AudioEngine::InputRouter::handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message)
{
    bool isController = false;

    if (source != nullptr)
    {
        const juce::ScopedLock lock (engine.controllerLock);
        isController = engine.controllerDevices.contains (source->getIdentifier());
    }

    if (! isController)
    {
        engine.player.handleIncomingMidiMessage (source, message);

        if (engine.noteInputListening.load() && message.isNoteOn())
            juce::MessageManager::callAsync ([&e = engine, token = std::weak_ptr<int> (engine.lifetimeToken), message,
                                              received = juce::Time::getMillisecondCounterHiRes()]
            {
                if (! token.expired() && e.onNoteInput)
                    e.onNoteInput (message, received);
            });

        return;
    }

    if (message.isNoteOn() || message.isController() || message.isProgramChange())
        juce::MessageManager::callAsync ([&e = engine, token = std::weak_ptr<int> (engine.lifetimeToken), message]
        {
            if (! token.expired() && e.onControllerMidi)
                e.onControllerMidi (message);
        });
}

void AudioEngine::setControllers (const juce::StringArray& deviceIdentifiers)
{
    {
        const juce::ScopedLock lock (controllerLock);
        controllerDevices = deviceIdentifiers;
    }

    settings.setValue ("midiControllers", deviceIdentifiers.joinIntoString (juce::newLine));
    settings.saveIfNeeded();
}

juce::StringArray AudioEngine::getControllers() const
{
    const juce::ScopedLock lock (controllerLock);
    return controllerDevices;
}

void AudioEngine::sendLiveArticulation (TrackId trackId, const std::vector<ExpressionMap::Output>& outputs)
{
    const auto sourceOf = [this] (TrackId id) -> MidiSourceProcessor*
    {
        if (auto* track = findTrack (id))
            if (auto* node = graph.getNodeForId (track->midiSourceNode))
                return dynamic_cast<MidiSourceProcessor*> (node->getProcessor());

        return nullptr;
    };

    auto* source = sourceOf (trackId);

    if (source == nullptr)
        return;

    // Channel 1: the track's route rewrites it to the output's channel
    for (auto& output : outputs)
    {
        switch (output.type)
        {
            case ExpressionMap::Output::Type::programChange:
                if (output.bank >= 0)
                {
                    source->injectLive (juce::MidiMessage::controllerEvent (1, 0, (output.bank >> 7) & 127));
                    source->injectLive (juce::MidiMessage::controllerEvent (1, 32, output.bank & 127));
                }

                source->injectLive (juce::MidiMessage::programChange (1, output.number));
                break;

            case ExpressionMap::Output::Type::controller:
                source->injectLive (juce::MidiMessage::controllerEvent (1, output.number, output.value));
                break;

            case ExpressionMap::Output::Type::keyswitch:
                source->injectLive (juce::MidiMessage::noteOn (1, output.number, (juce::uint8) juce::jlimit (1, 127, output.value)));
                juce::Timer::callAfterDelay (30, [token = std::weak_ptr<int> (lifetimeToken), trackId, sourceOf, key = output.number]
                {
                    if (! token.expired())
                        if (auto* s = sourceOf (trackId))
                            s->injectLive (juce::MidiMessage::noteOff (1, key));
                });
                break;
        }
    }
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
              #if JUCE_WINDOWS
               .getSiblingFile ("OrchestralDAWScanner.exe");
              #else
               .getSiblingFile ("OrchestralDAWScanner");
              #endif
}

juce::PluginDescription AudioEngine::resolveKnownPlugin (const juce::PluginDescription& saved) const
{
    for (const auto& type : knownPlugins.getTypes())
        if (type.pluginFormatName == saved.pluginFormatName && type.name == saved.name
            && (type.uniqueId == saved.uniqueId || type.deprecatedUid == saved.deprecatedUid))
            return type;

    return saved;
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

    syncFolderGroups();    // things moved: the folder groups' routing follows

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

    // Nests inside a project load / VE Pro sync, which own the title and detail
    busyStatus.begin ("Loading " + description.name);

    formatManager.createPluginInstanceAsync (description, sampleRate, blockSize,
        [this, alive, callback, startMs, name = description.name]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            if (alive.expired())
                return;

            // Closes the overlay on every exit below (failure, success, throw)
            struct BusyEnd
            {
                BusyStatus& status;
                ~BusyEnd()   { status.end(); }
            } busyEnd { busyStatus };

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
            // Outside a batch the plugin is added SYNC: it gets prepared right away,
            // before callers apply saved state - a plugin prepared after its state
            // was restored can lose it (seen: a reloaded parameter reading 0)
            instrument.pluginNode = graph.addNode (std::move (instance), std::nullopt,
                                                   graphBatchDepth > 0 ? updateKind()
                                                                       : juce::AudioProcessorGraph::UpdateKind::sync)->nodeID;

            // Give the instrument its audio channel strip.
            AudioChannel channel;
            channel.name = name;
            channel.node = graph.addNode (std::make_unique<AudioChannelProcessor>(), std::nullopt, updateKind())->nodeID;

            for (int ch = 0; ch < 2; ++ch)
                graph.addConnection ({ { channel.node, ch }, { masterNode, ch } }, updateKind());

            connectMasterOutput();   // in case the device started (or changed) after the engine

            // Only the first stereo pair for now; multi-output routing comes later.
            if (numOuts == 1)
            {
                graph.addConnection ({ { instrument.pluginNode, 0 }, { channel.node, 0 } }, updateKind());
                graph.addConnection ({ { instrument.pluginNode, 0 }, { channel.node, 1 } }, updateKind());
            }
            else
            {
                for (int ch = 0; ch < juce::jmin (2, numOuts); ++ch)
                    graph.addConnection ({ { instrument.pluginNode, ch }, { channel.node, ch } }, updateKind());
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

int AudioEngine::getInstrumentMidiPortCount (InstrumentId id) const
{
    if (auto* plugin = getInstrumentPlugin (id))
        if (auto* vst3 = plugin->getVST3Client())
            if (auto* component = vst3->getIComponentPtr())
                return juce::jmax (1, (int) component->getBusCount (Steinberg::Vst::kEvent, Steinberg::Vst::kInput));

    return 1;
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
                graph.removeNode (it->routeNode, updateKind());
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
        for (auto& [slot, insert] : channelIt->second.inserts)
            graph.removeNode (insert.node);

        graph.removeNode (channelIt->second.node);
        audioChannels.erase (channelIt);
    }

    instruments.erase (id);

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    refreshAllPlayback();   // tracks that played it lose their channel
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

void AudioEngine::setInstrumentName (InstrumentId id, const juce::String& name)
{
    auto* instrument = findInstrument (id);

    if (instrument == nullptr || name.isEmpty() || instrument->name == name)
        return;

    // (Its audio outputs keep their own names: they may be different things - and its group's bus
    // follows the instrument, in syncFolderGroups)
    instrument->name = name;

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    data->setProperty ("name", name);
    data->setProperty ("change", "renamed");
    emitEvent ("instrumentChanged", data);
}

bool AudioEngine::setInstrumentChannelName (InstrumentId id, int midiChannel, const juce::String& name, int midiPort)
{
    auto* instrument = findInstrument (id);

    if (instrument == nullptr)
        return false;

    for (auto it = instrument->midiChannels.begin(); it != instrument->midiChannels.end(); ++it)
    {
        if (it->midiPort == midiPort && it->midiChannel == midiChannel)
        {
            if (it->synced)
                return false;   // inherited from the VE Pro server: immutable

            // An unnamed channel that has no expression map either holds nothing
            if (name.isEmpty() && it->expressionMap.isEmpty())
                instrument->midiChannels.erase (it);
            else
                it->name = name;

            return true;
        }
    }

    if (name.isNotEmpty())
        instrument->midiChannels.push_back ({ midiPort, midiChannel, name, false });

    return true;
}

juce::String AudioEngine::getInstrumentChannelName (InstrumentId id, int midiChannel, int midiPort) const
{
    if (auto* instrument = findInstrument (id))
        for (auto& channel : instrument->midiChannels)
            if (channel.midiPort == midiPort && channel.midiChannel == midiChannel)
                return channel.name;

    return {};
}

std::vector<AudioEngine::MidiChannelInfo> AudioEngine::getInstrumentMidiChannels (InstrumentId id) const
{
    if (auto* instrument = findInstrument (id))
        return instrument->midiChannels;

    return {};
}

void AudioEngine::setSyncedInstrumentChannels (InstrumentId id, std::vector<MidiChannelInfo> channels)
{
    auto* instrument = findInstrument (id);

    if (instrument == nullptr)
        return;

    // Replace the synced set wholesale; manual entries survive. A fetched key
    // range stays while the same server channel sits on the same port/channel.
    std::vector<MidiChannelInfo> previous;

    for (auto& c : instrument->midiChannels)
        if (c.synced)
            previous.push_back (c);

    std::erase_if (instrument->midiChannels, [] (const MidiChannelInfo& c) { return c.synced; });

    for (auto& channel : channels)
    {
        for (auto& old : previous)
            if (old.midiPort == channel.midiPort && old.midiChannel == channel.midiChannel
                 && old.veproInstanceId == channel.veproInstanceId
                 && old.veproChannelAddress == channel.veproChannelAddress
                 && old.veproPluginId == channel.veproPluginId)
            {
                channel.keyLow = old.keyLow;
                channel.keyHigh = old.keyHigh;
                channel.expressionMap = old.expressionMap;   // the one thing sync doesn't own
            }

        channel.synced = true;
        instrument->midiChannels.push_back (channel);
    }

    std::sort (instrument->midiChannels.begin(), instrument->midiChannels.end(),
               [] (const MidiChannelInfo& a, const MidiChannelInfo& b)
               { return a.midiPort != b.midiPort ? a.midiPort < b.midiPort : a.midiChannel < b.midiChannel; });

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    data->setProperty ("change", "channels");
    refreshAllPlayback();   // a re-sync can change which channel a track plays
    emitEvent ("instrumentChanged", data);
}

void AudioEngine::setInstrumentChannelKeyRange (InstrumentId id, int midiPort, int midiChannel, int low, int high)
{
    auto* instrument = findInstrument (id);

    if (instrument == nullptr)
        return;

    for (auto& channel : instrument->midiChannels)
    {
        if (channel.midiPort == midiPort && channel.midiChannel == midiChannel)
        {
            if (channel.keyLow == low && channel.keyHigh == high)
                return;

            channel.keyLow = low;
            channel.keyHigh = high;

            auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
            data->setProperty ("id", id);
            data->setProperty ("change", "keyRange");
            emitEvent ("instrumentChanged", data);
            return;
        }
    }
}

std::optional<AudioEngine::MidiChannelInfo> AudioEngine::getTrackChannelInfo (TrackId trackId) const
{
    const auto outputs = getTrackOutputs (trackId);

    if (outputs.empty())
        return std::nullopt;

    const auto& output = outputs.front();

    if (auto* instrument = findInstrument (output.instrument))
        for (auto& channel : instrument->midiChannels)
            if (channel.midiPort == output.midiPort && channel.midiChannel == output.midiChannel)
                return channel;

    return std::nullopt;
}

//==============================================================================
// Expression maps
std::vector<ExpressionMap> AudioEngine::getExpressionMaps() const
{
    return expressionMaps;
}

std::optional<ExpressionMap> AudioEngine::getExpressionMap (const juce::String& name) const
{
    for (auto& map : expressionMaps)
        if (ExpressionMap::sameName (map.name, name))
            return map;

    return std::nullopt;
}

namespace
{
    juce::String existingMapNames (const std::vector<ExpressionMap>& maps)
    {
        juce::StringArray names;

        for (auto& map : maps)
            names.add (map.name);

        return names.isEmpty() ? "none exist yet" : "existing: " + names.joinIntoString (", ");
    }
}

juce::String AudioEngine::setExpressionMap (ExpressionMap map)
{
    map.name = map.name.trim();

    if (const auto problems = map.validate(); ! problems.isEmpty())
        return "expression map '" + map.name + "' is not valid: " + problems.joinIntoString ("; ");

    const auto existing = std::find_if (expressionMaps.begin(), expressionMaps.end(),
                                        [&] (const ExpressionMap& m) { return ExpressionMap::sameName (m.name, map.name); });

    if (existing != expressionMaps.end())
        *existing = map;
    else
        expressionMaps.push_back (map);

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("name", map.name);
    refreshAllPlayback();   // the tracks playing a channel that uses it
    emitEvent ("expressionMapChanged", data);
    return {};
}

juce::String AudioEngine::removeExpressionMap (const juce::String& name)
{
    const auto before = expressionMaps.size();
    const auto listing = existingMapNames (expressionMaps);

    std::erase_if (expressionMaps, [&] (const ExpressionMap& m) { return ExpressionMap::sameName (m.name, name); });

    if (expressionMaps.size() == before)
        return "no expression map '" + name + "' (" + listing + ")";

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("name", name);
    refreshAllPlayback();
    emitEvent ("expressionMapRemoved", data);
    return {};
}

juce::String AudioEngine::renameExpressionMap (const juce::String& name, const juce::String& newName)
{
    const auto clean = newName.trim();

    if (clean.isEmpty())
        return "an expression map needs a name";

    ExpressionMap* target = nullptr;

    for (auto& map : expressionMaps)
    {
        if (ExpressionMap::sameName (map.name, name))
            target = &map;
        else if (ExpressionMap::sameName (map.name, clean))
            return "there is already an expression map '" + map.name + "' (names ignore case)";
    }

    if (target == nullptr)
        return "no expression map '" + name + "' (" + existingMapNames (expressionMaps) + ")";

    target->name = clean;

    for (auto& [id, instrument] : instruments)
        for (auto& channel : instrument.midiChannels)
            if (ExpressionMap::sameName (channel.expressionMap, name))
                channel.expressionMap = clean;

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("name", clean);
    data->setProperty ("oldName", name);
    refreshAllPlayback();   // the tracks playing a channel that uses it
    emitEvent ("expressionMapChanged", data);
    return {};
}

juce::String AudioEngine::renameExpressionMapItem (const juce::String& mapName, const juce::String& groupName,
                                                   const juce::String& articulationName, const juce::String& newName,
                                                   int* notesChanged)
{
    if (notesChanged != nullptr)
        *notesChanged = 0;

    const auto stored = std::find_if (expressionMaps.begin(), expressionMaps.end(),
                                      [&] (const ExpressionMap& m) { return ExpressionMap::sameName (m.name, mapName); });

    if (stored == expressionMaps.end())
        return "no expression map '" + mapName + "' (" + existingMapNames (expressionMaps) + ")";

    // Work on a copy; nothing changes unless everything is accepted
    auto edited = *stored;
    const auto* group = edited.findGroup (groupName);
    const auto isRootGroup = group != nullptr && group == edited.rootGroup();
    const auto canonicalGroup = group != nullptr ? group->name : groupName;
    const auto renamingGroup = articulationName.trim().isEmpty();
    juce::String oldName = canonicalGroup;

    if (! renamingGroup && group != nullptr)
        if (const auto* articulation = ExpressionMap::findArticulation (*group, articulationName))
            oldName = articulation->name;

    const auto error = renamingGroup ? edited.renameGroup (groupName, newName)
                                     : edited.renameArticulation (groupName, articulationName, newName);

    if (error.isNotEmpty())
        return error;

    const auto clean = newName.trim();

    // The notes that name it: tracks whose channel uses this map (as getTrackExpressionMap sees it)
    struct Rewrite { TrackId id; std::vector<MidiSequence::Note> notes; std::vector<MidiSequence::Control> controls; };
    std::vector<Rewrite> rewrites;
    int total = 0;

    for (auto& [trackId, track] : tracks)
    {
        const auto info = getTrackChannelInfo (trackId);

        if (track.sequence == nullptr || ! info.has_value() || ! ExpressionMap::sameName (info->expressionMap, mapName))
            continue;

        Rewrite rewrite { trackId, track.sequence->getNotes(), track.sequence->getControls() };
        int changed = 0;

        for (auto& note : rewrite.notes)
        {
            auto& selection = note.articulation;
            bool touched = false;

            if (renamingGroup)
                touched = ! isRootGroup && selection.renameModifierGroup (oldName, clean);   // the root group's name isn't in notes
            else if (isRootGroup)
                touched = selection.renameRoot (oldName, clean);
            else
                touched = selection.renameModifier (canonicalGroup, oldName, clean);

            if (touched)
                ++changed;
        }

        if (changed > 0)
        {
            total += changed;
            rewrites.push_back (std::move (rewrite));
        }
    }

    *stored = std::move (edited);

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("name", stored->name);
    data->setProperty ("renamedFrom", oldName);
    data->setProperty ("renamedTo", clean);
    refreshAllPlayback();   // the tracks playing a channel that uses it
    emitEvent ("expressionMapChanged", data);

    for (auto& rewrite : rewrites)
        setTrackSequence (rewrite.id, MidiSequence::create (std::move (rewrite.notes), std::move (rewrite.controls)));

    if (notesChanged != nullptr)
        *notesChanged = total;

    return {};
}

juce::String AudioEngine::setInstrumentChannelMap (InstrumentId id, int midiPort, int midiChannel, const juce::String& mapName)
{
    auto* instrument = findInstrument (id);

    if (instrument == nullptr)
        return "no instrument with id " + juce::String (id);

    juce::String canonical;

    if (mapName.trim().isNotEmpty())
    {
        const auto map = getExpressionMap (mapName);

        if (! map.has_value())
            return "no expression map '" + mapName + "' (" + existingMapNames (expressionMaps) + ")";

        canonical = map->name;
    }

    auto it = std::find_if (instrument->midiChannels.begin(), instrument->midiChannels.end(),
                            [&] (const MidiChannelInfo& c) { return c.midiPort == midiPort && c.midiChannel == midiChannel; });

    if (it == instrument->midiChannels.end())
    {
        if (canonical.isEmpty())
            return {};   // nothing to clear

        MidiChannelInfo channel;
        channel.midiPort = midiPort;
        channel.midiChannel = midiChannel;
        instrument->midiChannels.push_back (channel);
        it = std::prev (instrument->midiChannels.end());
    }

    if (it->expressionMap == canonical)
        return {};

    it->expressionMap = canonical;

    // A manual channel that holds nothing any more is not kept
    if (! it->synced && it->name.isEmpty() && it->expressionMap.isEmpty())
        instrument->midiChannels.erase (it);

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    data->setProperty ("change", "expressionMap");
    refreshAllPlayback();
    emitEvent ("instrumentChanged", data);
    return {};
}

std::optional<ExpressionMap> AudioEngine::getTrackExpressionMap (TrackId trackId) const
{
    if (const auto info = getTrackChannelInfo (trackId); info.has_value() && info->expressionMap.isNotEmpty())
        return getExpressionMap (info->expressionMap);

    return std::nullopt;
}

int AudioEngine::getNumLoadedInstruments() const
{
    return (int) instruments.size();
}

//==============================================================================
//==============================================================================
juce::Array<juce::PluginDescription> AudioEngine::getEffectTypes() const
{
    juce::Array<juce::PluginDescription> effects;

    for (const auto& type : knownPlugins.getTypes())
        if (! type.isInstrument)
            effects.add (type);

    std::sort (effects.begin(), effects.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return effects;
}

void AudioEngine::addInsert (AudioChannelId channelId, int slot, const juce::PluginDescription& description, InsertCallback callback)
{
    if (audioChannels.find (channelId) == audioChannels.end() || slot < 0 || slot >= insertSlots)
    {
        if (callback) callback (false, "No such channel or slot");
        return;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    const auto sampleRate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
    const auto blockSize  = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;
    std::weak_ptr<int> alive = lifetimeToken;
    juce::Logger::writeToLog ("Loading insert: " + description.name + " (" + description.fileOrIdentifier + ")");

    formatManager.createPluginInstanceAsync (description, sampleRate, blockSize,
        [this, alive, channelId, slot, callback, name = description.name]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            if (alive.expired())
                return;

            auto it = audioChannels.find (channelId);

            if (instance == nullptr || it == audioChannels.end())
            {
                juce::Logger::writeToLog ("FAILED to load insert: " + name + (error.isNotEmpty() ? " - " + error : juce::String()));
                if (callback) callback (false, error.isNotEmpty() ? error : juce::String ("The channel is gone"));
                return;
            }

            if (auto old = it->second.inserts.find (slot); old != it->second.inserts.end())
                graph.removeNode (old->second.node, updateKind());

            if (it->second.inserts.empty())   // the first effect switches the section on
                it->second.insertsOn = true;

            Insert insert;
            insert.name = name;
            insert.node = graph.addNode (std::move (instance), std::nullopt, updateKind())->nodeID;
            it->second.inserts[slot] = insert;
            rewireChannelInputs (channelId);
            applyInsertBypass (channelId);   // a new effect in a switched-off section is off too
            emitChannelChanged (channelId, "inserts");

            if (callback) callback (true, {});
        });
}

void AudioEngine::removeInsert (AudioChannelId channelId, int slot)
{
    if (auto it = audioChannels.find (channelId); it != audioChannels.end())
        if (auto insert = it->second.inserts.find (slot); insert != it->second.inserts.end())
        {
            graph.removeNode (insert->second.node, updateKind());
            it->second.inserts.erase (insert);
            rewireChannelInputs (channelId);
            emitChannelChanged (channelId, "inserts");
        }
}

void AudioEngine::setInsertBypassed (AudioChannelId channelId, int slot, bool bypassed)
{
    if (auto it = audioChannels.find (channelId); it != audioChannels.end())
        if (auto insert = it->second.inserts.find (slot); insert != it->second.inserts.end())
        {
            insert->second.bypassed = bypassed;
            applyInsertBypass (channelId);
            emitChannelChanged (channelId, "inserts");
        }
}

void AudioEngine::setInsertsEnabled (AudioChannelId channelId, bool enabled)
{
    if (auto it = audioChannels.find (channelId); it != audioChannels.end() && it->second.insertsOn != enabled)
    {
        it->second.insertsOn = enabled;
        applyInsertBypass (channelId);
        emitChannelChanged (channelId, "inserts");
    }
}

bool AudioEngine::areInsertsEnabled (AudioChannelId channelId) const
{
    const auto it = audioChannels.find (channelId);
    return it == audioChannels.end() || it->second.insertsOn;
}

void AudioEngine::applyInsertBypass (AudioChannelId channelId)
{
    if (auto it = audioChannels.find (channelId); it != audioChannels.end())
        for (auto& [slot, insert] : it->second.inserts)
            if (auto* node = graph.getNodeForId (insert.node))
                node->setBypassed (insert.bypassed || ! it->second.insertsOn);
}

std::vector<AudioEngine::InsertInfo> AudioEngine::getInserts (AudioChannelId channelId) const
{
    std::vector<InsertInfo> result;

    if (auto it = audioChannels.find (channelId); it != audioChannels.end())
        for (auto& [slot, insert] : it->second.inserts)
            result.push_back ({ slot, insert.name, insert.bypassed });

    return result;
}

juce::AudioPluginInstance* AudioEngine::getInsertPlugin (AudioChannelId channelId, int slot) const
{
    if (auto it = audioChannels.find (channelId); it != audioChannels.end())
        if (auto insert = it->second.inserts.find (slot); insert != it->second.inserts.end())
            if (auto* node = graph.getNodeForId (insert->second.node))
                return dynamic_cast<juce::AudioPluginInstance*> (node->getProcessor());

    return nullptr;
}

// A channel's signal chain: its instrument, then each insert in slot order, then the strip. The
// audio connections out of the instrument and the inserts are made afresh (MIDI ones stay).
void AudioEngine::rewireChannelInputs (AudioChannelId channelId)
{
    auto it = audioChannels.find (channelId);

    if (it == audioChannels.end())
        return;

    std::vector<NodeID> chain;

    if (auto* instrument = findInstrument (it->second.input))
        chain.push_back (instrument->pluginNode);

    for (auto& [slot, insert] : it->second.inserts)
        chain.push_back (insert.node);

    for (auto& connection : graph.getConnections())
        if (! connection.source.isMIDI() && std::find (chain.begin(), chain.end(), connection.source.nodeID) != chain.end())
            graph.removeConnection (connection, updateKind());

    if (chain.empty())
        return;

    chain.push_back (it->second.node);

    for (size_t i = 0; i + 1 < chain.size(); ++i)
    {
        auto* from = graph.getNodeForId (chain[i]);
        auto* to = graph.getNodeForId (chain[i + 1]);

        if (from == nullptr || to == nullptr)
            continue;

        const auto outs = from->getProcessor()->getTotalNumOutputChannels();
        const auto ins = juce::jmax (1, juce::jmin (2, to->getProcessor()->getTotalNumInputChannels()));

        for (int ch = 0; ch < ins; ++ch)   // a mono source feeds both sides
            if (outs > 0)
                graph.addConnection ({ { chain[i], juce::jmin (ch, outs - 1) }, { chain[i + 1], ch } }, updateKind());
    }
}

// A loaded project's inserts for a channel, one after another (each loads asynchronously)
void AudioEngine::restoreInserts (AudioChannelId channelId, const juce::XmlElement* audioChannelXml, std::function<void()> done)
{
    auto elements = std::make_shared<std::vector<const juce::XmlElement*>>();

    if (audioChannelXml != nullptr)
        for (auto* i : audioChannelXml->getChildWithTagNameIterator ("INSERT"))
            elements->push_back (i);

    auto next = std::make_shared<std::function<void (size_t)>>();
    const auto savedOn = audioChannelXml != nullptr && audioChannelXml->getBoolAttribute ("insertsOn", false);
    *next = [this, channelId, elements, next, done, savedOn] (size_t index)
    {
        if (index >= elements->size())
        {
            // The section as it was saved (adding the first insert switched it on)
            setInsertsEnabled (channelId, savedOn);
            done();
            juce::MessageManager::callAsync ([next] { *next = nullptr; });   // break the self-reference (not from inside it)
            return;
        }

        const auto* element = (*elements)[index];
        juce::PluginDescription description;
        auto* pluginXml = element->getChildByName ("PLUGIN");

        if (pluginXml == nullptr || ! description.loadFromXml (*pluginXml))
        {
            (*next) (index + 1);
            return;
        }

        description = resolveKnownPlugin (description);

        const auto slot = element->getIntAttribute ("slot");
        addInsert (channelId, slot, description, [this, channelId, slot, element, next, index] (bool ok, const juce::String&)
        {
            if (ok)
            {
                if (auto* stateElement = element->getChildByName ("STATE"))
                {
                    juce::MemoryBlock block;

                    if (block.fromBase64Encoding (stateElement->getAllSubText().trim()) && block.getSize() > 0)
                        if (auto* plugin = getInsertPlugin (channelId, slot))
                            plugin->setStateInformation (block.getData(), (int) block.getSize());
                }

                if (element->getBoolAttribute ("bypassed"))
                    setInsertBypassed (channelId, slot, true);
            }

            (*next) (index + 1);
        });
    };

    (*next) (0);
}

//==============================================================================
AudioEngine::AudioChannelId AudioEngine::addBus (const juce::String& name)
{
    AudioChannel bus;
    bus.node = graph.addNode (std::make_unique<AudioChannelProcessor>(), std::nullopt, updateKind())->nodeID;
    bus.name = name.trim().isNotEmpty() ? name.trim() : "Bus " + juce::String ((int) buses.size() + 1);
    bus.named = true;
    bus.position = (int) buses.size();
    routeStrip (bus.node, 0);

    const auto id = nextAudioChannelId++;
    buses[id] = bus;

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    data->setProperty ("name", bus.name);
    emitEvent ("busAdded", data);
    return id;
}

void AudioEngine::removeBus (AudioChannelId id)
{
    auto it = buses.find (id);

    if (it == buses.end())
        return;

    for (auto& [channelId, channel] : audioChannels)   // its sources go to the master
        if (channel.output == id)
            setAudioChannelOutput (channelId, 0);

    for (auto& [busId, bus] : buses)
        if (bus.output == id)
            setAudioChannelOutput (busId, 0);

    for (auto& [folderId, folder] : folders)
        if (folder.groupBus == id)
            folder.groupBus = 0;

    for (auto& [instrumentId, instrument] : instruments)
        if (instrument.groupBus == id)
            instrument.groupBus = 0;

    graph.removeNode (it->second.node, updateKind());
    buses.erase (it);

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    emitEvent ("busRemoved", data);
}

std::vector<AudioEngine::AudioChannelId> AudioEngine::getBusIds() const
{
    std::vector<std::pair<int, AudioChannelId>> ordered;

    for (auto& [id, bus] : buses)
        ordered.push_back ({ bus.position, id });

    std::sort (ordered.begin(), ordered.end());
    std::vector<AudioChannelId> result;

    for (auto& [position, id] : ordered)
        result.push_back (id);

    return result;
}

bool AudioEngine::isBus (AudioChannelId id) const   { return buses.count (id) > 0; }

//==============================================================================
AudioEngine::AudioChannelId AudioEngine::addAudioTrack (const juce::String& name, bool stereo, FolderId folder)
{
    AudioChannel channel;
    channel.node = graph.addNode (std::make_unique<AudioChannelProcessor>(), std::nullopt, updateKind())->nodeID;
    channel.audioTrack = true;
    channel.stereo = stereo;
    channel.folder = folderExists (folder) ? folder : 0;
    channel.position = nextChildPosition (true, channel.folder);

    int count = 1;

    for (auto& [id, other] : audioChannels)
        count += other.audioTrack ? 1 : 0;

    channel.name = name.trim().isNotEmpty() ? name.trim() : "Audio " + juce::String (count);
    channel.named = true;
    routeStrip (channel.node, 0);

    const auto id = nextAudioChannelId++;
    audioChannels[id] = channel;

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    data->setProperty ("name", channel.name);
    data->setProperty ("stereo", stereo);
    emitEvent ("audioTrackAdded", data);
    return id;
}

void AudioEngine::removeAudioTrack (AudioChannelId id)
{
    auto it = audioChannels.find (id);

    if (it == audioChannels.end() || ! it->second.audioTrack)
        return;

    for (auto& [slot, insert] : it->second.inserts)
        graph.removeNode (insert.node, updateKind());

    graph.removeNode (it->second.node, updateKind());
    audioChannels.erase (it);

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    emitEvent ("audioTrackRemoved", data);
}

bool AudioEngine::isAudioTrack (AudioChannelId id) const
{
    const auto it = audioChannels.find (id);
    return it != audioChannels.end() && it->second.audioTrack;
}

bool AudioEngine::isAudioTrackStereo (AudioChannelId id) const
{
    const auto it = audioChannels.find (id);
    return it != audioChannels.end() && it->second.stereo;
}

//==============================================================================
void AudioEngine::setFolderGrouped (FolderId id, bool grouped)
{
    auto it = folders.find (id);

    if (it == folders.end() || (it->second.groupBus != 0) == grouped)
        return;

    if (grouped)
    {
        const auto bus = addBus (it->second.name);
        buses[bus].folder = id;   // (a bus's folder: the folder it sums)
        folders[id].groupBus = bus;

        // Its colour: a copy of the first instrument colour inside it (then its own) - unless it has one
        // already (chosen before: kept through ungrouping and grouping again)
        int folderDepth = folders[id].colour.isNotEmpty() ? std::numeric_limits<int>::max() : -1;

        for (auto& item : getSidebarItems (true, false))
        {
            if (folderDepth < 0)
            {
                if (item.folder == id)
                    folderDepth = item.depth;

                continue;
            }

            if (item.depth <= folderDepth)
                break;

            if (item.instrument != 0 && getInstrumentColour (item.instrument).isNotEmpty())
            {
                folders[id].colour = getInstrumentColour (item.instrument);
                break;
            }
        }
    }
    else
    {
        const auto bus = it->second.groupBus;
        it->second.groupBus = 0;

        for (auto& [channelId, channel] : audioChannels)   // its audio back to the master
            if (channel.output == bus)
                setAudioChannelOutput (channelId, 0);

        removeBus (bus);
    }

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("folderId", id);
    data->setProperty ("grouped", grouped);
    emitEvent ("folderChanged", data);
}

bool AudioEngine::isFolderGrouped (FolderId id) const   { return getFolderGroupBus (id) != 0; }

AudioEngine::FolderContents AudioEngine::getFolderContents (FolderId id) const
{
    FolderContents contents;

    if (! folderExists (id))
        return contents;

    const auto midiDomain = isFolderMidiDomain (id);

    for (auto& child : getChildrenOf (midiDomain, id))
    {
        if (child.isFolder)
            continue;

        if (child.audioTrack || ! midiDomain)   // an audio channel: a bus (not a group's), else audio
        {
            if (isGroupBus (child.id))
                continue;

            if (isBus (child.id)) contents.bus = true;
            else                  contents.audio = true;
        }
        else if (getTrackInstrument (child.id) != 0)   // an instrument's tracks are the instrument
            contents.instrument = true;
        else
            contents.midi = true;
    }

    return contents;
}

int AudioEngine::getInstrumentOutputCount (InstrumentId id) const
{
    return getAudioChannelForInstrument (id) != 0 ? 1 : 0;
}

void AudioEngine::setInstrumentGrouped (InstrumentId id, bool grouped)
{
    auto it = instruments.find (id);

    if (it == instruments.end() || (it->second.groupBus != 0) == grouped || (grouped && isSingleOutputInstrument (id)))
        return;   // (one output: nothing to group - the instrument is its channel)

    if (grouped)
    {
        const auto bus = addBus (it->second.name);
        buses[bus].input = id;   // (a bus's input: the instrument it sums)
        instruments[id].groupBus = bus;
    }
    else
    {
        const auto bus = it->second.groupBus;
        it->second.groupBus = 0;

        for (auto& [channelId, channel] : audioChannels)
            if (channel.output == bus)
                setAudioChannelOutput (channelId, 0);

        removeBus (bus);
    }

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    data->setProperty ("change", "grouped");
    emitEvent ("instrumentChanged", data);
}

bool AudioEngine::isInstrumentGrouped (InstrumentId id) const   { return getInstrumentGroupBus (id) != 0; }

AudioEngine::AudioChannelId AudioEngine::getInstrumentGroupBus (InstrumentId id) const
{
    const auto it = instruments.find (id);
    return it != instruments.end() ? it->second.groupBus : 0;
}

AudioEngine::AudioChannelId AudioEngine::getFolderGroupBus (FolderId id) const
{
    const auto it = folders.find (id);
    return it != folders.end() ? it->second.groupBus : 0;
}

juce::String AudioEngine::getChannelTagColour (AudioChannelId id) const
{
    if (auto bus = buses.find (id); bus != buses.end())
    {
        if (bus->second.folder != 0)
            return getFolderColour (bus->second.folder);

        return getInstrumentColour (bus->second.input);   // an instrument's group (or "" for a plain bus)
    }

    if (isAudioTrack (id))
        return getAudioTrackColour (id);

    return getInstrumentColour (getAudioChannelInput (id));
}

void AudioEngine::setAudioTrackColour (AudioChannelId id, const juce::String& hex)
{
    const auto it = audioChannels.find (id);

    if (it == audioChannels.end() || ! it->second.audioTrack || it->second.colour == hex)
        return;

    it->second.colour = hex;

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    data->setProperty ("change", "colour");
    emitEvent ("audioTrackChanged", data);
}

juce::String AudioEngine::getAudioTrackColour (AudioChannelId id) const
{
    const auto it = audioChannels.find (id);
    return it != audioChannels.end() && it->second.audioTrack ? it->second.colour : juce::String();
}

bool AudioEngine::isGroupBus (AudioChannelId id) const
{
    const auto it = buses.find (id);
    return it != buses.end() && (it->second.folder != 0 || it->second.input != 0);   // a folder's or an instrument's
}

AudioEngine::FolderId AudioEngine::getGroupBusFolder (AudioChannelId id) const
{
    const auto it = buses.find (id);
    return it != buses.end() ? it->second.folder : 0;
}

// Every audio channel to its innermost grouped folder's bus (or back to the master when it left the
// group); channels routed to another bus by hand stay where they are. Group buses take their folder's name.
void AudioEngine::syncFolderGroups()
{
    if (syncingGroups || (std::none_of (folders.begin(), folders.end(), [] (const auto& f) { return f.second.groupBus != 0; })
                            && std::none_of (instruments.begin(), instruments.end(), [] (const auto& i) { return i.second.groupBus != 0; })))
        return;

    const juce::ScopedValueSetter<bool> guard (syncingGroups, true);

    for (auto& [id, folder] : folders)
        if (auto bus = buses.find (folder.groupBus); bus != buses.end())
            bus->second.name = folder.name;

    for (auto& [id, instrument] : instruments)
        if (auto bus = buses.find (instrument.groupBus); bus != buses.end())
            bus->second.name = instrument.name;

    std::vector<std::pair<int, FolderId>> enclosing;   // (depth, folder) of the folders around an item

    for (auto& item : getSidebarItems (true, false))
    {
        while (! enclosing.empty() && enclosing.back().first >= item.depth)
            enclosing.pop_back();

        // The innermost group around this item (a grouped folder enclosing it)
        const auto groupAround = [&]
        {
            AudioChannelId bus = 0;

            for (auto around = enclosing.rbegin(); around != enclosing.rend() && bus == 0; ++around)
                bus = getFolderGroupBus (around->second);

            return bus;
        };

        const auto route = [this] (AudioChannelId strip, AudioChannelId wanted)
        {
            const auto current = getAudioChannelOutput (strip);

            if (strip != 0 && current != wanted && (current == 0 || isGroupBus (current)))
                setAudioChannelOutput (strip, wanted);
        };

        if (item.folder != 0)   // a group inside a group feeds it
        {
            route (getFolderGroupBus (item.folder), groupAround());
            enclosing.push_back ({ item.depth, item.folder });
        }

        if (item.instrument != 0)
            route (getInstrumentGroupBus (item.instrument), groupAround());

        if (item.channel == 0 || isBus (item.channel))
            continue;

        const auto instrumentGroup = getInstrumentGroupBus (getAudioChannelInput (item.channel));   // its instrument's group first
        route (item.channel, instrumentGroup != 0 ? instrumentGroup : groupAround());
    }
}

// A strip's audio out: its connections to the master and the buses go, one to the target is made
void AudioEngine::routeStrip (NodeID node, AudioChannelId target)
{
    auto targetNode = masterNode;

    if (auto it = buses.find (target); it != buses.end())
        targetNode = it->second.node;

    std::set<NodeID> outputs { masterNode };

    for (auto& [id, bus] : buses)
        outputs.insert (bus.node);

    for (auto& connection : graph.getConnections())
        if (connection.source.nodeID == node && outputs.count (connection.destination.nodeID) > 0)
            graph.removeConnection (connection, updateKind());

    for (int ch = 0; ch < 2; ++ch)
        graph.addConnection ({ { node, ch }, { targetNode, ch } }, updateKind());
}

bool AudioEngine::setAudioChannelOutput (AudioChannelId id, AudioChannelId target)
{
    if (target != 0 && buses.count (target) == 0)
        return false;

    auto* channel = audioChannels.count (id) > 0 ? &audioChannels[id] : (buses.count (id) > 0 ? &buses[id] : nullptr);

    if (channel == nullptr)
        return false;

    for (auto along = target; along != 0; along = getAudioChannelOutput (along))   // a bus may go to a bus - never round in a loop
        if (along == id)
            return false;

    channel->output = target;
    routeStrip (channel->node, target);
    emitChannelChanged (id, "output");
    return true;
}

AudioEngine::AudioChannelId AudioEngine::getAudioChannelOutput (AudioChannelId id) const
{
    if (auto it = audioChannels.find (id); it != audioChannels.end())
        return it->second.output;

    if (auto it = buses.find (id); it != buses.end())
        return it->second.output;

    return 0;
}

// The master bus to the device's first two outputs. The output node has no channels until the
// graph is configured for a device - a connection asked for earlier is refused - so this is called
// once the device runs, and again when instruments are added (it does nothing when connected)
void AudioEngine::connectMasterOutput()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        const juce::AudioProcessorGraph::Connection connection { { masterNode, ch }, { audioOutNode, ch } };

        if (! graph.isConnected (connection) && graph.canConnect (connection))
            graph.addConnection (connection);
    }
}

AudioChannelProcessor* AudioEngine::getMasterChannel() const
{
    if (auto* node = graph.getNodeForId (masterNode))
        return dynamic_cast<AudioChannelProcessor*> (node->getProcessor());

    return nullptr;
}

void AudioEngine::setAudioChannelName (AudioChannelId id, const juce::String& name)
{
    if (auto it = audioChannels.find (id); it != audioChannels.end() && name.trim().isNotEmpty())
    {
        it->second.name = name.trim();
        it->second.named = true;
        emitChannelChanged (id, "name");
    }

    if (auto it = buses.find (id); it != buses.end() && name.trim().isNotEmpty())
    {
        it->second.name = name.trim();
        emitChannelChanged (id, "name");
    }
}

void AudioEngine::setAudioChannelSoloed (AudioChannelId id, bool soloed)
{
    if (auto it = audioChannels.find (id); it != audioChannels.end() && it->second.soloed != soloed)
    {
        it->second.soloed = soloed;
        applySolo();
        emitChannelChanged (id, "solo");
    }
}

bool AudioEngine::isAudioChannelSoloed (AudioChannelId id) const
{
    const auto it = audioChannels.find (id);
    return it != audioChannels.end() && it->second.soloed;
}

void AudioEngine::setAudioChannelPan (AudioChannelId id, float pan)
{
    if (auto* processor = getAudioChannel (id))
    {
        processor->setPan (pan);
        projectDirty = true;
    }
}

void AudioEngine::emitChannelChanged (AudioChannelId id, const juce::String& change)
{
    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("id", id);
    data->setProperty ("change", change);
    emitEvent ("channelChanged", data);
}

// Solo in place: while any channel is soloed, every other one is silent
void AudioEngine::applySolo()
{
    const auto anySoloed = std::any_of (audioChannels.begin(), audioChannels.end(),
                                        [] (const auto& entry) { return entry.second.soloed; });

    for (auto& [id, channel] : audioChannels)
        if (auto* processor = getAudioChannel (id))
            processor->setSoloSilenced (anySoloed && ! channel.soloed);
}

AudioChannelProcessor* AudioEngine::getAudioChannel (AudioChannelId id) const
{
    if (auto it = audioChannels.find (id); it != audioChannels.end())
        if (auto* node = graph.getNodeForId (it->second.node))
            return dynamic_cast<AudioChannelProcessor*> (node->getProcessor());

    if (auto it = buses.find (id); it != buses.end())
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
    track.midiSourceNode = graph.addNode (std::make_unique<MidiSourceProcessor> (transport),
                                          std::nullopt, updateKind())->nodeID;
    // Live input is wired permanently; the source gates it by arming (no graph changes on arm)
    graph.addConnection ({ { midiInNode, midiChannelIndex }, { track.midiSourceNode, midiChannelIndex } },
                         updateKind());
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
        graph.removeNode (output.routeNode, updateKind());

    graph.removeNode (track->midiSourceNode, updateKind());
    tracks.erase (id);
    armedTracks.erase (id);

    if (armedTrack == id)
    {
        if (! armedTracks.empty())
            setArmedTracks (armedTracks, *armedTracks.begin());
        else
            setArmedTrack (tracks.empty() ? 0 : tracks.begin()->first);
    }

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

    if (auto it = buses.find (id); it != buses.end())
        return it->second.name;

    return {};
}

AudioEngine::InstrumentId AudioEngine::getAudioChannelInput (AudioChannelId id) const
{
    if (auto it = audioChannels.find (id); it != audioChannels.end())
        return it->second.input;

    return 0;
}

void AudioEngine::addTrackOutput (TrackId trackId, InstrumentId instrumentId, int midiChannel, int midiPort)
{
    auto* track = findTrack (trackId);
    auto* instrument = findInstrument (instrumentId);

    if (track == nullptr || instrument == nullptr)
        return;

    Output output;
    output.instrument = instrumentId;
    output.midiChannel = juce::jlimit (1, 16, midiChannel);
    output.midiPort = juce::jmax (1, midiPort);
    // The route node tags port >= 2 traffic for the plugin's VST3 event bus
    // 'port - 1' (see MidiRouteProcessor::wrapForPort) - like Cubase, the
    // plugin's own MIDI ports are addressed directly, no helper plugins.
    auto routeProcessor = std::make_unique<MidiRouteProcessor> (output.midiChannel, output.midiPort);
    routeProcessor->setMonitor (&midiMonitor, [this] { return transport.getPositionTicks(); }, trackId, instrumentId);
    output.routeNode = graph.addNode (std::move (routeProcessor), std::nullopt, updateKind())->nodeID;

    graph.addConnection ({ { track->midiSourceNode, midiChannelIndex }, { output.routeNode, midiChannelIndex } },
                         updateKind());
    graph.addConnection ({ { output.routeNode, midiChannelIndex }, { instrument->pluginNode, midiChannelIndex } },
                         updateKind());

    track->outputs.push_back (output);

    // Only the new route needs the mute/solo gate (re-applying it to every route
    // in the project per added output made big syncs quadratic)
    if (auto* route = getRoute (output))
    {
        const auto anySolo = std::any_of (tracks.begin(), tracks.end(),
                                          [] (const auto& entry) { return entry.second.soloed; });
        route->setRouteEnabled (! track->muted && (! anySolo || track->soloed));
    }

    juce::Logger::writeToLog ("Track " + juce::String (trackId) + " output -> " + instrument->name
                              + " port " + juce::String (output.midiPort)
                              + " ch " + juce::String (output.midiChannel));
    refreshPlayback (*track);   // its channel may carry an expression map
    updatePreRoll();
    emitTrackChanged (trackId, "outputs");
}

void AudioEngine::clearTrackOutputs (TrackId id)
{
    if (auto* track = findTrack (id))
    {
        for (auto& output : track->outputs)
            graph.removeNode (output.routeNode, updateKind());

        track->outputs.clear();
        refreshPlayback (*track);
        updatePreRoll();
        emitTrackChanged (id, "outputs");
    }
}

std::vector<AudioEngine::TrackOutput> AudioEngine::getTrackOutputs (TrackId id) const
{
    std::vector<TrackOutput> result;

    if (auto* track = findTrack (id))
        for (auto& output : track->outputs)
            result.push_back ({ output.instrument, output.midiChannel, output.midiPort });

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

void AudioEngine::setTrackEditorLanes (TrackId id, const juce::StringArray& lanes)
{
    if (auto* track = findTrack (id); track != nullptr && track->editorLanes != lanes)
    {
        track->editorLanes = lanes;
        projectDirty = true;   // saved with the project; a view choice, not an edit (no history entry)
    }
}

juce::StringArray AudioEngine::getTrackEditorLanes (TrackId id) const
{
    auto* track = findTrack (id);
    return track != nullptr ? track->editorLanes : juce::StringArray();
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
    track.sequence = std::move (sequence);
    refreshPlayback (track);
    updatePreRoll();
}

std::optional<ExpressionMap> AudioEngine::mapForTrack (const Track& track) const
{
    if (track.outputs.empty())
        return std::nullopt;

    const auto& output = track.outputs.front();

    if (auto* instrument = findInstrument (output.instrument))
        for (auto& channel : instrument->midiChannels)
            if (channel.midiPort == output.midiPort && channel.midiChannel == output.midiChannel)
                return channel.expressionMap.isNotEmpty() ? getExpressionMap (channel.expressionMap) : std::nullopt;

    return std::nullopt;
}

// The audio thread plays the written sequence shifted by the articulations' timing offsets, with their
// switch events inserted (see model/PlaybackSequence.h); the written one stays what the user edits.
void AudioEngine::refreshPlayback (Track& track)
{
    const auto map = mapForTrack (track);
    // Overlapping regions: the overlapped notes are cut for playback only (Settings > Editor)
    const auto overlapsCut = editorSettings::cutOverlappedNotes (settings) ? MidiSequence::withRegionOverlapsCut (track.sequence)
                                                                           : track.sequence;
    const auto written = MidiSequence::withRampsRendered (overlapsCut);   // CC ramps become messages
    const auto built = playback::build (written, map.has_value() ? &*map : nullptr, *masterTempoMap,
                                        editorSettings::firstRootIsDefault (settings));
    track.preRollNeededMs = -built.earliestOffsetMs;

    if (auto* node = graph.getNodeForId (track.midiSourceNode))
        if (auto* source = dynamic_cast<MidiSourceProcessor*> (node->getProcessor()))
            source->setSequence (built.sequence);
}

// The pre-roll covers the earliest any note of any track is scheduled before where it is written
void AudioEngine::updatePreRoll()
{
    auto needed = 0.0;

    for (auto& entry : tracks)
        needed = juce::jmax (needed, entry.second.preRollNeededMs);

    transport.setPreRollMs (needed);
}

void AudioEngine::refreshAllPlayback()
{
    for (auto& entry : tracks)
        refreshPlayback (entry.second);

    updatePreRoll();
}

MidiSequence::Ptr AudioEngine::getTrackPlaybackSequence (TrackId id) const
{
    if (auto* track = findTrack (id))
        if (auto* node = graph.getNodeForId (track->midiSourceNode))
            if (auto* source = dynamic_cast<MidiSourceProcessor*> (node->getProcessor()))
                return source->getSequence();

    return nullptr;
}

void AudioEngine::setTrackSequence (TrackId id, MidiSequence::Ptr sequence)
{
    if (auto* track = findTrack (id))
    {
        constexpr size_t maxHistory = 200;

        track->undoStack.push_back (track->sequence);
        track->undoGroups.push_back (currentUndoGroup);

        if (track->undoStack.size() > maxHistory)
        {
            track->undoStack.erase (track->undoStack.begin());
            track->undoGroups.erase (track->undoGroups.begin());
        }

        track->redoStack.clear();
        track->redoGroups.clear();

        juce::Logger::writeToLog ("Track " + juce::String (id)
                                  + (sequence != nullptr ? ": sequence set (" + juce::String ((int) sequence->getNotes().size()) + " notes)"
                                                         : ": sequence cleared"));
        applySequence (*track, std::move (sequence));
        emitClipChanged (id);
    }
}

bool AudioEngine::undoOne (Track& track, TrackId id)
{
    if (track.undoStack.empty())
        return false;

    track.redoStack.push_back (track.sequence);
    track.redoGroups.push_back (track.undoGroups.back());
    applySequence (track, track.undoStack.back());
    track.undoStack.pop_back();
    track.undoGroups.pop_back();
    emitClipChanged (id);
    return true;
}

bool AudioEngine::redoOne (Track& track, TrackId id)
{
    if (track.redoStack.empty())
        return false;

    track.undoStack.push_back (track.sequence);
    track.undoGroups.push_back (track.redoGroups.back());
    applySequence (track, track.redoStack.back());
    track.redoStack.pop_back();
    track.redoGroups.pop_back();
    emitClipChanged (id);
    return true;
}

bool AudioEngine::undoTrackSequence (TrackId id)
{
    auto* track = findTrack (id);

    if (track == nullptr || track->undoStack.empty())
        return false;

    // Part of a multi-track edit: undo it on every track where it is the latest edit
    if (const auto group = track->undoGroups.back(); group != 0)
    {
        for (auto otherId : getTrackIds())
            if (auto* other = findTrack (otherId); other != nullptr && ! other->undoGroups.empty()
                                                    && other->undoGroups.back() == group)
                undoOne (*other, otherId);

        return true;
    }

    return undoOne (*track, id);
}

bool AudioEngine::redoTrackSequence (TrackId id)
{
    auto* track = findTrack (id);

    if (track == nullptr || track->redoStack.empty())
        return false;

    if (const auto group = track->redoGroups.back(); group != 0)
    {
        for (auto otherId : getTrackIds())
            if (auto* other = findTrack (otherId); other != nullptr && ! other->redoGroups.empty()
                                                    && other->redoGroups.back() == group)
                redoOne (*other, otherId);

        return true;
    }

    return redoOne (*track, id);
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
    refreshAllPlayback();   // the offsets are milliseconds: their length in ticks follows the tempo
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
                                     getTrackOutputs (id), track.sequence, track.folder, track.position,
                                     track.colour });

    snapshot.armedTrack = armedTrack;
    snapshot.tempoMap = masterTempoMap;
    snapshot.markers = markers;

    for (auto& [id, channel] : audioChannels)
        if (auto* processor = getAudioChannel (id))
            snapshot.channels.push_back ({ id, processor->getGain(), processor->isMuted(),
                                           channel.folder, channel.position, processor->getPan(),
                                           channel.soloed, channel.name, channel.named });

    for (auto& [id, folder] : folders)
        snapshot.folders.push_back ({ id, folder.name, folder.midiDomain, folder.parent,
                                      folder.collapsed, folder.position, folder.colour });

    snapshot.expressionMaps = expressionMaps;

    for (auto& [id, instrument] : instruments)
        for (auto& channel : instrument.midiChannels)
            if (channel.expressionMap.isNotEmpty())
                snapshot.channelMaps.push_back ({ id, channel.midiPort, channel.midiChannel, channel.expressionMap });

    return snapshot;
}

void AudioEngine::applyHistorySnapshot (const HistorySnapshot& snapshot)
{
    historySuppress = true;

    // Expression maps and which channel uses which restore wholesale; the channels' own
    // data (names, key ranges, sync state) is not history material
    expressionMaps = snapshot.expressionMaps;

    for (auto& [id, instrument] : instruments)
    {
        for (auto& channel : instrument.midiChannels)
            channel.expressionMap.clear();

        // A manual channel that only existed to hold a map is gone again
        std::erase_if (instrument.midiChannels, [] (const MidiChannelInfo& c) { return ! c.synced && c.name.isEmpty(); });
    }

    for (auto& state : snapshot.channelMaps)
    {
        auto* instrument = findInstrument (state.instrument);

        if (instrument == nullptr)
            continue;   // the instrument is gone

        auto it = std::find_if (instrument->midiChannels.begin(), instrument->midiChannels.end(),
                                [&] (const MidiChannelInfo& c) { return c.midiPort == state.midiPort && c.midiChannel == state.midiChannel; });

        if (it == instrument->midiChannels.end())
        {
            MidiChannelInfo channel;
            channel.midiPort = state.midiPort;
            channel.midiChannel = state.midiChannel;
            instrument->midiChannels.push_back (channel);
            it = std::prev (instrument->midiChannels.end());
        }

        it->expressionMap = state.map;
    }

    // Folders restore wholesale (ids are stable, nothing in the graph references them)
    folders.clear();

    for (auto& state : snapshot.folders)
    {
        folders[state.id] = { state.name, state.midiDomain, state.parent, state.collapsed,
                              state.position, state.colour };
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
            track.midiSourceNode = graph.addNode (std::make_unique<MidiSourceProcessor> (transport),
                                                  std::nullopt, updateKind())->nodeID;
            graph.addConnection ({ { midiInNode, midiChannelIndex }, { track.midiSourceNode, midiChannelIndex } },
                                 updateKind());
            tracks[state.id] = std::move (track);
            nextTrackId = juce::jmax (nextTrackId, state.id + 1);
        }

        auto* track = findTrack (state.id);
        track->name = state.name;
        track->recordReplace = state.recordReplace;
        track->folder = folderExists (state.folder) ? state.folder : 0;
        track->position = state.position;
        track->colour = state.colour;
        setTrackMuted (state.id, state.muted);
        setTrackSoloed (state.id, state.soloed);

        clearTrackOutputs (state.id);
        for (auto& output : state.outputs)
            addTrackOutput (state.id, output.instrument, output.midiChannel, output.midiPort);   // gone instruments: no-op

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
            processor->setPan (channel.pan);
        }

        if (auto it = audioChannels.find (channel.id); it != audioChannels.end())
        {
            it->second.folder = folderExists (channel.folder) ? channel.folder : 0;
            it->second.position = channel.position;
            it->second.soloed = channel.soloed;

            if (channel.named)
            {
                it->second.name = channel.name;
                it->second.named = true;
            }
        }
    }

    applySolo();

    refreshAllPlayback();   // the maps, the assignments and the tempo came back
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
void AudioEngine::setTrackColour (TrackId id, const juce::String& hex)
{
    auto* track = findTrack (id);

    if (track == nullptr || track->colour == hex)
        return;

    track->colour = hex;
    emitTrackChanged (id, "colour");
}

juce::String AudioEngine::getTrackColour (TrackId id) const
{
    // Inside an instrument folder: the instrument's colour (the folder's grey without one), 20% darker
    if (const auto instrument = getTrackInstrument (id); instrument != 0)
        return "#" + colourFromHex (getInstrumentColour (instrument), juce::Colour (0xff8a8f98))
                       .withMultipliedBrightness (0.8f).toDisplayString (false).toLowerCase();

    auto* track = findTrack (id);
    return track != nullptr ? track->colour : juce::String();
}

juce::String AudioEngine::getInstrumentColour (InstrumentId id) const
{
    const auto it = instruments.find (id);
    return it != instruments.end() ? it->second.colour : juce::String();
}

void AudioEngine::setInstrumentColour (InstrumentId id, const juce::String& hex)
{
    if (auto it = instruments.find (id); it != instruments.end() && it->second.colour != hex)
    {
        it->second.colour = hex;
        auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
        data->setProperty ("id", id);
        data->setProperty ("change", "colour");
        emitEvent ("instrumentChanged", data);
    }
}

void AudioEngine::setFolderColour (FolderId id, const juce::String& hex)
{
    const auto it = folders.find (id);

    if (it == folders.end() || it->second.colour == hex)
        return;

    it->second.colour = hex;

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("folderId", id);
    data->setProperty ("change", "colour");
    emitEvent ("folderChanged", data);
}

juce::String AudioEngine::getFolderColour (FolderId id) const
{
    const auto it = folders.find (id);
    return it != folders.end() ? it->second.colour : juce::String();
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

    if (it->second.groupBus != 0)
        setFolderGrouped (id, false);

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

        for (auto& [id, channel] : audioChannels)   // the audio tracks stand among the tracks
            if (channel.audioTrack && effectiveFolder (channel.folder) == parent)
                children.push_back ({ false, id, channel.position, true });
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
    // One pass for the max - no list building or sorting (called per created
    // track, so big syncs made the sort version quadratic)
    int maxPosition = -1;

    for (auto& [id, folder] : folders)
        if (folder.midiDomain == midiDomain && folder.parent == parent)
            maxPosition = juce::jmax (maxPosition, folder.position);

    const auto effectiveFolder = [this] (FolderId f) { return folderExists (f) ? f : 0; };

    if (midiDomain)
    {
        for (auto& [id, track] : tracks)
            if (effectiveFolder (track.folder) == parent)
                maxPosition = juce::jmax (maxPosition, track.position);

        for (auto& [id, channel] : audioChannels)
            if (channel.audioTrack && effectiveFolder (channel.folder) == parent)
                maxPosition = juce::jmax (maxPosition, channel.position);
    }
    else
    {
        for (auto& [id, channel] : audioChannels)
            if (effectiveFolder (channel.folder) == parent)
                maxPosition = juce::jmax (maxPosition, channel.position);
    }

    return maxPosition + 1;
}

void AudioEngine::setChildPosition (bool midiDomain, const ChildRef& child, int position)
{
    if (child.isFolder)
    {
        if (auto it = folders.find (child.id); it != folders.end())
            it->second.position = position;
    }
    else if (midiDomain && ! child.audioTrack)
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

    // Group every child by parent in ONE pass (per-folder rescans of all tracks
    // made this quadratic, and both sidebar and arrangement call it per frame)
    std::map<FolderId, std::vector<ChildRef>> childrenByParent;
    const auto effectiveFolder = [this] (FolderId f) { return folderExists (f) ? f : 0; };

    for (auto& [id, folder] : folders)
        if (folder.midiDomain == midiDomain)
            childrenByParent[folder.parent].push_back ({ true, id, folder.position });

    if (midiDomain)
    {
        for (auto& [id, track] : tracks)
            childrenByParent[effectiveFolder (track.folder)].push_back ({ false, id, track.position });

        for (auto& [id, channel] : audioChannels)   // the audio tracks, where they stand
            if (channel.audioTrack)
                childrenByParent[effectiveFolder (channel.folder)].push_back ({ false, id, channel.position, true });
    }
    else
    {
        for (auto& [id, channel] : audioChannels)
            childrenByParent[effectiveFolder (channel.folder)].push_back ({ false, id, channel.position });
    }

    for (auto& [parent, children] : childrenByParent)
        std::sort (children.begin(), children.end(), [] (const ChildRef& a, const ChildRef& b)
        {
            if (a.position != b.position)   return a.position < b.position;
            if (a.isFolder != b.isFolder)   return a.isFolder;
            return a.id < b.id;
        });

    // The MIDI tree's instrument folders: each instrument's tracks in tree order (collapsed folders' too)
    std::map<InstrumentId, std::vector<TrackId>> tracksOfInstrument;

    if (midiDomain)
    {
        const std::function<void (FolderId)> collect = [&] (FolderId parent)
        {
            if (const auto found = childrenByParent.find (parent); found != childrenByParent.end())
                for (auto& child : found->second)
                {
                    if (child.isFolder)
                        collect (child.id);
                    else if (child.audioTrack)
                        continue;
                    else if (const auto instrument = getTrackInstrument (child.id); instrument != 0)
                        tracksOfInstrument[instrument].push_back (child.id);
                }
        };

        collect (0);
    }

    std::set<InstrumentId> placed;

    const auto pushInstrument = [&] (InstrumentId instrument, int depth, FolderId parent)
    {
        placed.insert (instrument);
        // Its tag: one output - its channel's (unless a group sums it); several - its group's (if grouped)
        const auto strip = isSingleOutputInstrument (instrument) ? getAudioChannelForInstrument (instrument) : getInstrumentGroupBus (instrument);
        items.push_back ({ 0, 0, depth, parent, instrument, 0, strip != 0 && ! isGroupBus (getAudioChannelOutput (strip)) });

        if (skipCollapsed && ! isInstrumentExpanded (instrument))
            return;

        for (auto trackId : tracksOfInstrument[instrument])   // its MIDI tracks, then its audio
            items.push_back ({ 0, trackId, depth + 1, parent, 0, 0 });

        if (const auto channel = getAudioChannelForInstrument (instrument); channel != 0)   // tagged unless a group sums it
            items.push_back ({ 0, 0, depth + 1, parent, 0, channel, ! isGroupBus (getAudioChannelOutput (channel)) });
    };

    const std::function<void (FolderId, int)> visit = [&] (FolderId parent, int depth)
    {
        const auto found = childrenByParent.find (parent);

        if (found == childrenByParent.end())
            return;

        for (auto& child : found->second)
        {
            if (child.isFolder)
            {
                items.push_back ({ child.id, 0, depth, parent, 0, 0,
                                   midiDomain && isFolderGrouped (child.id) && ! isGroupBus (getAudioChannelOutput (getFolderGroupBus (child.id))) });

                if (! (skipCollapsed && isFolderCollapsed (child.id)))
                    visit (child.id, depth + 1);
            }
            else if (child.audioTrack)   // an audio track's row (tagged unless a group sums it)
            {
                items.push_back ({ 0, 0, depth, parent, 0, child.id, ! isGroupBus (getAudioChannelOutput (child.id)) });
            }
            else if (const auto instrument = midiDomain ? getTrackInstrument (child.id) : 0; instrument != 0)
            {
                if (placed.count (instrument) == 0)   // its folder stands where its first track would
                    pushInstrument (instrument, depth, parent);
            }
            else
            {
                items.push_back ({ 0, child.id, depth, parent });
            }
        }
    };

    visit (0, 0);

    if (midiDomain)   // instruments no track plays yet: at the end (not those whose tracks are in a collapsed folder)
        for (auto& [id, instrument] : instruments)
            if (placed.count (id) == 0 && tracksOfInstrument[id].empty())
                pushInstrument (id, 0, 0);

    if (midiDomain)   // the buses, last (audio rows of their own)
        for (auto id : getBusIds())
            if (! isGroupBus (id))
                items.push_back ({ 0, 0, 0, 0, 0, id, true });

    return items;
}

bool AudioEngine::isTreeSlot (const SidebarItem& item) const
{
    return item.folder != 0 || item.instrument != 0
            || (item.member != 0 && getTrackInstrument (item.member) == 0)
            || (item.channel != 0 && isAudioTrack (item.channel));
}

bool AudioEngine::moveTreeNodes (const std::vector<TreeNode>& nodes, FolderId parent, int index)
{
    using Kind = TreeNode::Kind;

    if (parent != 0 && (! folderExists (parent) || ! isFolderMidiDomain (parent)))
        return false;

    // The valid nodes (no folder into itself or its own subtree; instruments with tracks)
    std::vector<TreeNode> moved;

    for (auto& node : nodes)
    {
        auto ok = false;

        switch (node.kind)
        {
            case Kind::folder:
                if (const auto it = folders.find (node.id); it != folders.end() && it->second.midiDomain)
                {
                    ok = true;

                    for (auto walk = parent; walk != 0 && ok; walk = folders.at (walk).parent)
                        ok = walk != node.id;

                    if (! ok)
                        return false;
                }
                break;
            case Kind::track:       ok = findTrack (node.id) != nullptr; break;
            case Kind::audioTrack:  ok = isAudioTrack (node.id); break;
            case Kind::instrument:  ok = instruments.count (node.id) > 0 && ! getInstrumentTracks (node.id).empty(); break;
        }

        if (ok && std::find (moved.begin(), moved.end(), node) == moved.end())
            moved.push_back (node);
    }

    if (moved.empty())
        return false;

    const auto isMoved = [&] (Kind kind, int id) { return std::find (moved.begin(), moved.end(), TreeNode { kind, id }) != moved.end(); };

    // The parent's slots now, the moved ones left out
    std::vector<TreeNode> slots;

    for (auto& child : getChildrenOf (true, parent))
    {
        TreeNode node;

        if (child.isFolder)           node = { Kind::folder, child.id };
        else if (child.audioTrack)    node = { Kind::audioTrack, child.id };
        else if (const auto instrument = getTrackInstrument (child.id); instrument != 0 && ! isMoved (Kind::track, child.id))
            node = { Kind::instrument, instrument };
        else                          node = { Kind::track, child.id };

        if (! isMoved (node.kind, node.id) && std::find (slots.begin(), slots.end(), node) == slots.end())
            slots.push_back (node);
    }

    index = juce::jlimit (0, (int) slots.size(), index);
    slots.insert (slots.begin() + index, moved.begin(), moved.end());

    // The slots as children, in order: an instrument's tracks together (all of them when it moves;
    // else those already here, not moved on their own)
    std::vector<ChildRef> children;

    for (auto& slot : slots)
    {
        switch (slot.kind)
        {
            case Kind::folder:      children.push_back ({ true, slot.id, 0 }); break;
            case Kind::track:       children.push_back ({ false, slot.id, 0 }); break;
            case Kind::audioTrack:  children.push_back ({ false, slot.id, 0, true }); break;
            case Kind::instrument:
            {
                const auto whole = isMoved (Kind::instrument, slot.id);

                for (auto track : getInstrumentTracks (slot.id))
                    if (! isMoved (Kind::track, track) && (whole || getTrackFolder (track) == parent))
                        children.push_back ({ false, track, 0 });
                break;
            }
        }
    }

    // Re-parent, renumber
    for (int i = 0; i < (int) children.size(); ++i)
    {
        const auto& child = children[(size_t) i];

        if (child.isFolder)
            folders[child.id].parent = parent;
        else if (child.audioTrack)
            audioChannels[child.id].folder = parent;
        else if (auto* track = findTrack (child.id))
            track->folder = parent;

        setChildPosition (true, child, i);
    }

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("domain", "midi");
    data->setProperty ("parent", parent);
    data->setProperty ("count", (int) moved.size());
    emitEvent ("sidebarMoved", data);
    return true;
}

void AudioEngine::reorderInstrumentTracks (InstrumentId instrument, const std::vector<TrackId>& order)
{
    const auto current = getInstrumentTracks (instrument);

    if (order.size() != current.size() || ! std::is_permutation (order.begin(), order.end(), current.begin()))
        return;

    std::vector<std::pair<FolderId, int>> places;   // (folder, position) of each, in the old order

    for (auto track : current)
        places.push_back ({ findTrack (track)->folder, findTrack (track)->position });

    for (size_t i = 0; i < order.size(); ++i)
    {
        auto* track = findTrack (order[i]);
        track->folder = places[i].first;
        track->position = places[i].second;
    }

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("domain", "midi");
    data->setProperty ("instrument", instrument);
    emitEvent ("sidebarMoved", data);
}

bool AudioEngine::moveSidebarItems (bool midiDomain, const std::vector<FolderId>& folderIds,
                                    const std::vector<int>& memberIds, FolderId parent, int index)
{
    if (folderIds.empty() && memberIds.empty())
        return false;

    if (midiDomain)
    {
        std::vector<TreeNode> nodes;

        for (auto folder : folderIds)
            nodes.push_back ({ TreeNode::Kind::folder, folder });

        for (auto member : memberIds)
            nodes.push_back ({ TreeNode::Kind::track, member });

        return moveTreeNodes (nodes, parent, index);
    }

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

AudioEngine::InstrumentId AudioEngine::getTrackInstrument (TrackId id) const
{
    if (auto* track = findTrack (id); track != nullptr && ! track->outputs.empty())
        if (instruments.count (track->outputs.front().instrument) > 0)
            return track->outputs.front().instrument;

    return 0;
}

std::vector<AudioEngine::TrackId> AudioEngine::getInstrumentTracks (InstrumentId id) const
{
    std::vector<TrackId> result;

    for (auto& item : getSidebarItems (true, false))
        if (item.member != 0 && getTrackInstrument (item.member) == id)
            result.push_back (item.member);

    return result;
}

float AudioEngine::takeTrackMidiActivity (TrackId id)
{
    if (auto* track = findTrack (id))
        if (auto* node = graph.getNodeForId (track->midiSourceNode))
            if (auto* source = dynamic_cast<MidiSourceProcessor*> (node->getProcessor()))
                return source->takeActivity();

    return 0.0f;
}

bool AudioEngine::isInstrumentExpanded (InstrumentId id) const
{
    const auto it = instruments.find (id);
    return it != instruments.end() && it->second.expanded;
}

void AudioEngine::setInstrumentExpanded (InstrumentId id, bool expanded)
{
    if (auto it = instruments.find (id); it != instruments.end() && it->second.expanded != expanded)
    {
        it->second.expanded = expanded;
        emitEvent ("folderViewChanged");   // a view state, like a folder's (not history)
    }
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
bool AudioEngine::anyReplaceTarget() const
{
    return std::any_of (takeTargets.begin(), takeTargets.end(), [] (const TakeTarget& t) { return t.replace; });
}

bool AudioEngine::startRecording()
{
    if (findTrack (armedTrack) == nullptr || isRecording())
        return false;

    // Every armed track receives the take, each by its own record mode
    takeTargets.clear();
    takeStash = {};
    replaceFromTick = -1;

    for (auto trackId : armedTracks)
    {
        if (auto* track = findTrack (trackId))
        {
            takeTargets.push_back ({ trackId, track->recordReplace, track->sequence });

            // Replace mode: the track's own material is silent for the whole take.
            if (track->recordReplace)
                if (auto* source = getSource (trackId))
                    source->setSuppressed (true);
        }
    }

    recordingSawPlayback = false;
    recorder->start (armedTrack);
    juce::Logger::writeToLog ("Recording started on " + juce::String ((int) takeTargets.size()) + " track(s)");

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("trackId", armedTrack);
    data->setProperty ("trackCount", (int) takeTargets.size());
    emitEvent ("recordingStarted", data);

    if (! transport.isPlaying())
        transport.play();

    return true;
}

void AudioEngine::stopRecording()
{
    if (! isRecording())
        return;

    const auto stopTick = transport.getPositionTicks();
    const auto result = recorder->finish (stopTick);

    for (auto& target : takeTargets)
        if (auto* source = getSource (target.trackId))
            source->setSuppressed (false);

    // finish() drains the FIFO, so the first input may only be known now.
    if (replaceFromTick < 0 && recorder->getFirstEventTick() >= 0)
        replaceFromTick = recorder->getFirstEventTick();

    juce::Logger::writeToLog ("Recording stopped on " + juce::String ((int) takeTargets.size()) + " track(s): "
                              + juce::String ((int) (takeStash.notes.size() + result.notes.size())) + " notes, "
                              + juce::String ((int) (takeStash.controls.size() + result.controls.size()))
                              + " control events");

    for (auto& target : takeTargets)
    {
        if (target.replace && replaceFromTick >= 0)
        {
            // One undoable step: pre-take material erased from first input to stop,
            // plus the WHOLE take (loop-pass commits stashed + the final part).
            auto base = eraseRangeFrom (target.preTakeSequence, replaceFromTick,
                                        juce::jmax (replaceFromTick + 1, stopTick));

            auto notes = takeStash.notes;
            auto controls = takeStash.controls;
            notes.insert (notes.end(), result.notes.begin(), result.notes.end());
            controls.insert (controls.end(), result.controls.begin(), result.controls.end());

            if (base != nullptr)
                MidiSequence::joinRegions (notes, base->getNotes());   // the take joins the regions it lands in

            if (base != nullptr)
            {
                notes.insert (notes.end(), base->getNotes().begin(), base->getNotes().end());
                controls.insert (controls.end(), base->getControls().begin(), base->getControls().end());
            }

            if (auto* track = findTrack (target.trackId))
                applySequence (*track, target.preTakeSequence);   // so the undo snapshot is the pre-take state

            setTrackSequence (target.trackId, notes.empty() && controls.empty()
                                                  ? nullptr
                                                  : MidiSequence::create (std::move (notes), std::move (controls)));
        }
        else if (! target.replace)
        {
            mergeIntoTrack (target.trackId, result);   // earlier loop passes were committed already
        }
    }

    auto data = juce::DynamicObject::Ptr (new juce::DynamicObject());
    data->setProperty ("trackId", armedTrack);
    data->setProperty ("trackCount", (int) takeTargets.size());
    data->setProperty ("notes", (int) (takeStash.notes.size() + result.notes.size()));
    data->setProperty ("controls", (int) (takeStash.controls.size() + result.controls.size()));

    takeTargets.clear();
    takeStash = {};

    emitEvent ("recordingFinished", data);
}

void AudioEngine::pollRecording()
{
    if (! isRecording())
        return;

    recorder->poll();

    if (replaceFromTick < 0 && recorder->getFirstEventTick() >= 0)
        replaceFromTick = recorder->getFirstEventTick();

    if (recorder->consumeWrapFlag())
    {
        // Add-mode targets commit each loop pass so it's audible on the next one;
        // replace-mode targets commit once at stop, so stash what passes here.
        const auto pass = recorder->takePending();

        for (auto& target : takeTargets)
            if (! target.replace)
                mergeIntoTrack (target.trackId, pass);

        if (anyReplaceTarget())
        {
            takeStash.notes.insert (takeStash.notes.end(), pass.notes.begin(), pass.notes.end());
            takeStash.controls.insert (takeStash.controls.end(), pass.controls.begin(), pass.controls.end());
        }
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
        MidiSequence::joinRegions (notes, existing.getNotes());   // added inside a region: part of it
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

    for (auto& map : expressionMaps)
        root.addChildElement (map.toXml().release());

    for (auto& [id, folder] : folders)
    {
        auto* f = root.createNewChildElement ("FOLDER");
        f->setAttribute ("id", id);
        f->setAttribute ("name", folder.name);
        f->setAttribute ("midi", folder.midiDomain);
        f->setAttribute ("parent", folder.parent);
        f->setAttribute ("collapsed", folder.collapsed);
        f->setAttribute ("position", folder.position);
        f->setAttribute ("colour", folder.colour);
    }

    if (! viewState.isEmpty())   // the project's view (the UI's values)
    {
        auto* view = root.createNewChildElement ("VIEW");

        for (auto& value : viewState)
            view->setAttribute (value.name, value.value.toString());
    }

    for (auto id : getBusIds())
    {
        auto& bus = buses[id];
        auto* b = root.createNewChildElement ("BUS");
        b->setAttribute ("id", id);
        b->setAttribute ("name", bus.name);

        if (bus.folder != 0)
            b->setAttribute ("groupOf", bus.folder);   // a folder's group bus

        if (auto* processor = getAudioChannel (id))
        {
            b->setAttribute ("gain", processor->getGain());
            b->setAttribute ("muted", processor->isMuted());
            b->setAttribute ("pan", processor->getPan());

            for (int param = 0; param < AnalogStrip::numParams; ++param)
                if (const auto value = processor->getStrip().get (param); value != AnalogStrip::info (param).initial)
                    b->setAttribute (AnalogStrip::info (param).name, value);
        }
    }

    for (auto& [id, channel] : audioChannels)
    {
        if (! channel.audioTrack)
            continue;

        auto* a = root.createNewChildElement ("AUDIOTRACK");
        a->setAttribute ("id", id);
        a->setAttribute ("name", channel.name);
        a->setAttribute ("stereo", channel.stereo);
        a->setAttribute ("firstInput", channel.firstInput);
        a->setAttribute ("folder", channel.folder);
        a->setAttribute ("position", channel.position);
        a->setAttribute ("output", channel.output);
        a->setAttribute ("soloed", channel.soloed);
        a->setAttribute ("colour", channel.colour);

        if (auto* processor = getAudioChannel (id))
        {
            a->setAttribute ("gain", processor->getGain());
            a->setAttribute ("muted", processor->isMuted());
            a->setAttribute ("pan", processor->getPan());

            for (int param = 0; param < AnalogStrip::numParams; ++param)
                if (const auto value = processor->getStrip().get (param); value != AnalogStrip::info (param).initial)
                    a->setAttribute (AnalogStrip::info (param).name, value);
        }
    }

    for (auto& [id, instrument] : instruments)
    {
        auto* e = root.createNewChildElement ("INSTRUMENT");
        e->setAttribute ("id", id);
        e->setAttribute ("name", instrument.name);
        e->setAttribute ("expanded", instrument.expanded);

        if (instrument.groupBus != 0)
            e->setAttribute ("groupBus", instrument.groupBus);   // its group's bus (the BUS element)
        e->setAttribute ("colour", instrument.colour);

        if (auto* plugin = getInstrumentPlugin (id))
        {
            e->addChildElement (plugin->getPluginDescription().createXml().release());

            juce::MemoryBlock state;
            plugin->getStateInformation (state);

            if (state.getSize() > 0)
                e->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
        }

        for (auto& channel : instrument.midiChannels)
        {
            auto* c = e->createNewChildElement ("MIDICHANNEL");
            c->setAttribute ("port", channel.midiPort);
            c->setAttribute ("channel", channel.midiChannel);
            c->setAttribute ("name", channel.name);
            c->setAttribute ("synced", channel.synced);

            if (channel.expressionMap.isNotEmpty())
                c->setAttribute ("expressionMap", channel.expressionMap);

            if (channel.veproChannelAddress.isNotEmpty())
            {
                c->setAttribute ("veproInstance", channel.veproInstanceId);
                c->setAttribute ("veproChannel", channel.veproChannelAddress);
                c->setAttribute ("veproPlugin", channel.veproPluginId);
            }

            if (channel.keyLow >= 0)
            {
                c->setAttribute ("slotKeyLow", channel.keyLow);   // "keyLow" held the old union over all slots
                c->setAttribute ("slotKeyHigh", channel.keyHigh);
            }
        }

        if (auto* audioChannel = getAudioChannel (instrument.audioChannel))
        {
            auto* a = e->createNewChildElement ("AUDIOCHANNEL");
            a->setAttribute ("gain", audioChannel->getGain());
            a->setAttribute ("muted", audioChannel->isMuted());
            a->setAttribute ("pan", audioChannel->getPan());

            for (int param = 0; param < AnalogStrip::numParams; ++param)   // the console strip: what isn't at its default
                if (const auto value = audioChannel->getStrip().get (param); value != AnalogStrip::info (param).initial)
                    a->setAttribute (AnalogStrip::info (param).name, value);
            a->setAttribute ("folder", getAudioChannelFolder (instrument.audioChannel));

            if (auto it = audioChannels.find (instrument.audioChannel); it != audioChannels.end())
            {
                a->setAttribute ("position", it->second.position);
                a->setAttribute ("soloed", it->second.soloed);
                a->setAttribute ("insertsOn", it->second.insertsOn);

                if (it->second.output != 0)
                    a->setAttribute ("output", it->second.output);

                if (it->second.named)
                    a->setAttribute ("name", it->second.name);

                for (auto& [slot, insert] : it->second.inserts)
                {
                    auto* i = a->createNewChildElement ("INSERT");
                    i->setAttribute ("slot", slot);
                    i->setAttribute ("name", insert.name);
                    i->setAttribute ("bypassed", insert.bypassed);

                    if (auto* plugin = getInsertPlugin (instrument.audioChannel, slot))
                    {
                        i->addChildElement (plugin->getPluginDescription().createXml().release());
                        juce::MemoryBlock pluginState;
                        plugin->getStateInformation (pluginState);

                        if (pluginState.getSize() > 0)
                            i->createNewChildElement ("STATE")->addTextElement (pluginState.toBase64Encoding());
                    }
                }
            }
        }
    }

    for (auto& [id, track] : tracks)
    {
        auto* e = root.createNewChildElement ("TRACK");
        e->setAttribute ("name", track.name);
        e->setAttribute ("muted", track.muted);
        e->setAttribute ("soloed", track.soloed);
        e->setAttribute ("armed", isTrackArmed (id));
        e->setAttribute ("primary", id == armedTrack);
        e->setAttribute ("recordReplace", track.recordReplace);

        if (! track.editorLanes.isEmpty())
            e->setAttribute ("editorLanes", track.editorLanes.joinIntoString (","));
        e->setAttribute ("folder", getTrackFolder (id));
        e->setAttribute ("position", track.position);
        e->setAttribute ("colour", track.colour);

        for (auto& output : track.outputs)
        {
            auto* o = e->createNewChildElement ("OUTPUT");
            o->setAttribute ("instrument", output.instrument);
            o->setAttribute ("channel", output.midiChannel);
            o->setAttribute ("port", output.midiPort);
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
            graph.removeNode (output.routeNode, updateKind());

        graph.removeNode (track.midiSourceNode, updateKind());
    }

    tracks.clear();

    for (auto& [id, instrument] : instruments)
        graph.removeNode (instrument.pluginNode);

    instruments.clear();

    for (auto& [id, channel] : audioChannels)
        graph.removeNode (channel.node);

    audioChannels.clear();
    viewState.clear();

    for (auto& [id, bus] : buses)
        graph.removeNode (bus.node);

    buses.clear();

    armedTrack = 0;
    armedTracks.clear();
    markers.clear();
    expressionMaps.clear();
    folders.clear();
    masterTempoMap = TempoMap::create (120.0);
    transport.setTempoMap (masterTempoMap);

    refreshAllPlayback();
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
    busyStatus.begin ("Loading " + file.getFileNameWithoutExtension());

    if (auto* tempoXml = xml->getChildByName ("TEMPOMAP"))
    {
        masterTempoMap = TempoMap::fromXml (*tempoXml);
        transport.setTempoMap (masterTempoMap);
    }

    for (auto* m : xml->getChildWithTagNameIterator ("EXPRESSIONMAP"))
        expressionMaps.push_back (ExpressionMap::fromXml (*m));   // as saved: validity is reported on demand, never a load failure

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

        folders[id].colour = f->getStringAttribute ("colour");
    }

    if (auto* view = xml->getChildByName ("VIEW"))   // the project's view, before the UI hears of the load
        for (int i = 0; i < view->getNumAttributes(); ++i)
            viewState.set (view->getAttributeName (i), view->getAttributeValue (i));

    // Buses: made now (they have no plugins), so channels can be routed to them as they load
    std::map<int, AudioChannelId> busIdMap;

    for (auto* b : xml->getChildWithTagNameIterator ("BUS"))
    {
        const auto id = addBus (b->getStringAttribute ("name"));
        busIdMap[b->getIntAttribute ("id")] = id;

        if (auto folder = folderIdMap.find (b->getIntAttribute ("groupOf")); folder != folderIdMap.end())
        {
            buses[id].folder = folder->second;
            folders[folder->second].groupBus = id;
        }

        if (auto* processor = getAudioChannel (id))
        {
            processor->setGain ((float) b->getDoubleAttribute ("gain", 1.0));
            processor->setMuted (b->getBoolAttribute ("muted"));
            processor->setPan ((float) b->getDoubleAttribute ("pan", 0.0));

            for (int param = 0; param < AnalogStrip::numParams; ++param)
                if (b->hasAttribute (AnalogStrip::info (param).name))
                    processor->getStrip().set (param, (float) b->getDoubleAttribute (AnalogStrip::info (param).name));
        }
    }

    for (auto* a : xml->getChildWithTagNameIterator ("AUDIOTRACK"))   // the audio tracks (no plugins: now)
    {
        const auto folder = folderIdMap.count (a->getIntAttribute ("folder")) > 0 ? folderIdMap[a->getIntAttribute ("folder")] : 0;
        const auto id = addAudioTrack (a->getStringAttribute ("name"), a->getBoolAttribute ("stereo", true), folder);
        audioChannels[id].firstInput = a->getIntAttribute ("firstInput");
        audioChannels[id].colour = a->getStringAttribute ("colour");

        if (a->hasAttribute ("position"))
            audioChannels[id].position = a->getIntAttribute ("position");

        if (auto bus = busIdMap.find (a->getIntAttribute ("output")); bus != busIdMap.end())
            setAudioChannelOutput (id, bus->second);

        if (a->getBoolAttribute ("soloed"))
            setAudioChannelSoloed (id, true);

        if (auto* processor = getAudioChannel (id))
        {
            processor->setGain ((float) a->getDoubleAttribute ("gain", 1.0));
            processor->setMuted (a->getBoolAttribute ("muted"));
            processor->setPan ((float) a->getDoubleAttribute ("pan", 0.0));

            for (int param = 0; param < AnalogStrip::numParams; ++param)
                if (a->hasAttribute (AnalogStrip::info (param).name))
                    processor->getStrip().set (param, (float) a->getDoubleAttribute (AnalogStrip::info (param).name));
        }
    }

    struct LoadState
    {
        std::unique_ptr<juce::XmlElement> xml;
        std::map<int, AudioChannelId> busIdMap;
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
    state->busIdMap = std::move (busIdMap);
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
            busyStatus.update ("Restoring tracks and building the audio graph...", 1.0);

            // Batch only the track phase: instruments above load into a still-small
            // graph synchronously (prepared before their state is applied); the
            // many track nodes then cost ONE rebuild
            beginGraphBatch();
            restoreProjectTracks (*state->xml, state->idMap, state->folderIdMap, state->warnings);
            endGraphBatch();
            refreshAllPlayback();   // maps, assignments and sequences are all in place now
            busyStatus.end();

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

        busyStatus.update ("Instrument " + juce::String ((int) state->next) + " of "
                             + juce::String ((int) state->instrumentElements.size()) + ": " + savedName,
                           (double) (state->next - 1) / (double) juce::jmax ((size_t) 1, state->instrumentElements.size()));

        juce::PluginDescription description;
        auto* pluginXml = element->getChildByName ("PLUGIN");

        if (pluginXml == nullptr || ! description.loadFromXml (*pluginXml))
        {
            state->warnings.add (savedName + ": missing plugin description");
            (*step)();
            return;
        }

        description = resolveKnownPlugin (description);

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
                setInstrumentName (newId, element->getStringAttribute ("name"));   // the user's name, not the plugin's
                setInstrumentExpanded (newId, element->getBoolAttribute ("expanded", false));

                if (auto bus = state->busIdMap.find (element->getIntAttribute ("groupBus")); bus != state->busIdMap.end())
                {
                    buses[bus->second].input = newId;   // its group's bus (made before the instruments)
                    instruments[newId].groupBus = bus->second;
                }
                setInstrumentColour (newId, element->getStringAttribute ("colour"));

                if (auto* stateElement = element->getChildByName ("STATE"))
                {
                    juce::MemoryBlock block;

                    if (block.fromBase64Encoding (stateElement->getAllSubText().trim()) && block.getSize() > 0)
                        if (auto* plugin = getInstrumentPlugin (newId))
                            plugin->setStateInformation (block.getData(), (int) block.getSize());
                }

                // Legacy projects stored CHANNELNAME (port 1, manual)
                for (auto* c : element->getChildWithTagNameIterator ("CHANNELNAME"))
                    setInstrumentChannelName (newId, c->getIntAttribute ("channel"), c->getStringAttribute ("name"));

                if (auto* loadedInstrument = findInstrument (newId))
                    for (auto* c : element->getChildWithTagNameIterator ("MIDICHANNEL"))
                    {
                        MidiChannelInfo channel;
                        channel.midiPort = c->getIntAttribute ("port", 1);
                        channel.midiChannel = c->getIntAttribute ("channel", 1);
                        channel.name = c->getStringAttribute ("name");
                        channel.synced = c->getBoolAttribute ("synced");
                        channel.veproInstanceId = c->getStringAttribute ("veproInstance");
                        channel.veproChannelAddress = c->getStringAttribute ("veproChannel");
                        channel.veproPluginId = c->getStringAttribute ("veproPlugin");
                        channel.keyLow = c->getIntAttribute ("slotKeyLow", -1);
                        channel.keyHigh = c->getIntAttribute ("slotKeyHigh", -1);
                        channel.expressionMap = c->getStringAttribute ("expressionMap");
                        loadedInstrument->midiChannels.push_back (channel);
                    }

                if (auto* a = element->getChildByName ("AUDIOCHANNEL"))
                {
                    const auto channelId = getAudioChannelForInstrument (newId);

                    if (auto* audioChannel = getAudioChannel (channelId))
                    {
                        audioChannel->setGain ((float) a->getDoubleAttribute ("gain", 1.0));
                        audioChannel->setMuted (a->getBoolAttribute ("muted"));
                        audioChannel->setPan ((float) a->getDoubleAttribute ("pan", 0.0));

                        for (int param = 0; param < AnalogStrip::numParams; ++param)
                            if (a->hasAttribute (AnalogStrip::info (param).name))
                                audioChannel->getStrip().set (param, (float) a->getDoubleAttribute (AnalogStrip::info (param).name));
                    }

                    if (a->getBoolAttribute ("soloed"))
                        setAudioChannelSoloed (channelId, true);

                    if (auto bus = state->busIdMap.find (a->getIntAttribute ("output")); bus != state->busIdMap.end())
                        setAudioChannelOutput (channelId, bus->second);

                    if (a->hasAttribute ("name"))
                        setAudioChannelName (channelId, a->getStringAttribute ("name"));

                    if (auto it = state->folderIdMap.find (a->getIntAttribute ("folder"));
                        it != state->folderIdMap.end())
                        setAudioChannelFolder (channelId, it->second);

                    if (a->hasAttribute ("position"))
                        if (auto channelIt = audioChannels.find (channelId); channelIt != audioChannels.end())
                            channelIt->second.position = a->getIntAttribute ("position");
                }

                // Its inserts (loaded one after another), then the next instrument
                if (auto* a = element->getChildByName ("AUDIOCHANNEL"))
                    setInsertsEnabled (getAudioChannelForInstrument (newId), a->getBoolAttribute ("insertsOn", true));

                restoreInserts (getAudioChannelForInstrument (newId), element->getChildByName ("AUDIOCHANNEL"), [step] { (*step)(); });
            });
    };

    (*step)();
}

void AudioEngine::restoreProjectTracks (const juce::XmlElement& root, const std::map<int, InstrumentId>& instrumentIds,
                                        const std::map<int, FolderId>& folderIds, juce::StringArray& warnings)
{
    std::set<TrackId> loadedArmed;
    TrackId loadedPrimary = 0;

    for (auto* e : root.getChildWithTagNameIterator ("TRACK"))
    {
        const auto trackId = addTrack();
        setTrackName (trackId, e->getStringAttribute ("name"));
        setTrackMuted (trackId, e->getBoolAttribute ("muted"));
        setTrackSoloed (trackId, e->getBoolAttribute ("soloed"));
        setTrackRecordReplace (trackId, e->getBoolAttribute ("recordReplace"));
        setTrackEditorLanes (trackId, juce::StringArray::fromTokens (e->getStringAttribute ("editorLanes"), ",", {}));

        if (auto it = folderIds.find (e->getIntAttribute ("folder")); it != folderIds.end())
            setTrackFolder (trackId, it->second);

        if (e->hasAttribute ("position"))
            findTrack (trackId)->position = e->getIntAttribute ("position");

        findTrack (trackId)->colour = e->getStringAttribute ("colour");

        for (auto* o : e->getChildWithTagNameIterator ("OUTPUT"))
        {
            const auto savedInstrument = o->getIntAttribute ("instrument");

            if (auto it = instrumentIds.find (savedInstrument); it != instrumentIds.end())
                addTrackOutput (trackId, it->second, o->getIntAttribute ("channel", 1), o->getIntAttribute ("port", 1));
            else
                warnings.add (getTrackName (trackId) + ": output skipped (its instrument didn't load)");
        }

        if (auto* sequenceXml = e->getChildByName ("SEQUENCE"))
            setTrackSequence (trackId, MidiSequence::fromXml (*sequenceXml));

        if (e->getBoolAttribute ("armed"))
        {
            loadedArmed.insert (trackId);

            // Older projects have no "primary": the (single) armed track is it
            if (e->getBoolAttribute ("primary", true))
                loadedPrimary = trackId;
        }
    }

    if (! loadedArmed.empty())
        setArmedTracks (loadedArmed, loadedPrimary);

    // A freshly loaded project starts with clean clip histories.
    for (auto& [id, track] : tracks)
    {
        track.undoStack.clear();
        track.redoStack.clear();
        track.undoGroups.clear();
        track.redoGroups.clear();
    }
}

//==============================================================================
void AudioEngine::setArmedTrack (TrackId id)
{
    setArmedTracks (id != 0 ? std::set<TrackId> { id } : std::set<TrackId>(), id);
}

void AudioEngine::setArmedTracks (const std::set<TrackId>& ids, TrackId primary)
{
    std::set<TrackId> next;

    for (auto id : ids)
        if (findTrack (id) != nullptr)
            next.insert (id);

    if (primary != 0 && findTrack (primary) != nullptr)
        next.insert (primary);
    else
        primary = next.empty() ? 0 : *next.begin();

    // Un-arming must not leave live-played notes ringing (ISSUES.md "Sidebar"):
    // release whatever the outputs of tracks LEAVING the armed set still hold.
    for (auto id : armedTracks)
        if (next.count (id) == 0)
            if (auto* previous = findTrack (id))
                for (auto& output : previous->outputs)
                    if (auto* route = getRoute (output))
                        route->killHeldNotes();

    const auto changed = next != armedTracks || primary != armedTrack;
    armedTracks = std::move (next);
    armedTrack = primary;
    updateMidiRouting();

    if (changed && primary != 0)
        emitTrackChanged (primary, "armed");
}

void AudioEngine::updateMidiRouting()
{
    // Live input reaches every track's source permanently; armed ones pass it
    // on. Flags only - no graph change, no render-sequence rebuild.
    for (auto& [id, track] : tracks)
        if (auto* source = getSource (id))
            source->setLiveEnabled (armedTracks.count (id) > 0);
}
