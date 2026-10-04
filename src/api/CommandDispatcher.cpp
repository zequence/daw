#include "CommandDispatcher.h"
#include "../integrations/VeproState.h"
#include "../integrations/VeproServer.h"
#include "../integrations/VeproKeyRange.h"
#include <AppVersion.h>
#include "../engine/AudioChannelProcessor.h"
#include "../engine/HistoryManager.h"

namespace
{
    juce::DynamicObject::Ptr object()
    {
        return new juce::DynamicObject();
    }

    juce::var ok (const juce::var& result = {})
    {
        auto o = object();
        o->setProperty ("ok", true);

        if (! result.isVoid())
            o->setProperty ("result", result);

        return juce::var (o.get());
    }

    juce::var fail (const juce::String& error)
    {
        auto o = object();
        o->setProperty ("ok", false);
        o->setProperty ("error", error);
        return juce::var (o.get());
    }

    juce::int64 tickParam (const juce::var& params, const juce::String& name, juce::int64 fallback = 0)
    {
        return params.hasProperty (juce::Identifier (name)) ? (juce::int64) params[juce::Identifier (name)] : fallback;
    }
}

//==============================================================================
CommandDispatcher::CommandDispatcher (AudioEngine& e) : engine (e)
{
    registerCommands();
}

void CommandDispatcher::add (const juce::String& name, const juce::String& description,
                             const juce::String& params, std::function<void (const juce::var&, Respond)> run)
{
    commands[name] = { description, params, std::move (run) };
}

void CommandDispatcher::dispatch (const juce::String& message, Respond out)
{
    dispatchParsed (juce::JSON::parse (message), std::move (out));
}

void CommandDispatcher::dispatchParsed (const juce::var& parsed, Respond out)
{
    const auto id = parsed.getProperty ("id", {});

    auto respond = [out = std::move (out), id] (juce::var reply)
    {
        if (auto* o = reply.getDynamicObject(); o != nullptr && ! id.isVoid())
            o->setProperty ("id", id);

        if (out)
            out (reply);
    };

    if (! parsed.isObject())
    {
        respond (fail ("invalid JSON (expected one object per line)"));
        return;
    }

    const auto name = parsed.getProperty ("cmd", {}).toString();
    const auto it = commands.find (name);

    if (it == commands.end())
    {
        juce::StringArray names;
        for (auto& [commandName, command] : commands)
            names.add (commandName);

        respond (fail ("unknown command '" + name + "'. Commands: " + names.joinIntoString (", ")));
        return;
    }

    it->second.run (parsed.getProperty ("params", {}), std::move (respond));
}

juce::var CommandDispatcher::run (const juce::String& cmd, const juce::var& params)
{
    juce::var reply;

    if (const auto it = commands.find (cmd); it != commands.end())
        it->second.run (params, [&reply] (const juce::var& r) { reply = r; });
    else
        reply = fail ("unknown command '" + cmd + "'");

    return reply;
}

//==============================================================================
void CommandDispatcher::registerCommands()
{
    //==========================================================================
    add ("describe", "List every command with its parameters", "",
         [this] (const juce::var&, Respond respond)
         {
             juce::Array<juce::var> list;

             for (auto& [name, command] : commands)
             {
                 auto o = object();
                 o->setProperty ("name", name);
                 o->setProperty ("description", command.description);
                 o->setProperty ("params", command.params);
                 list.add (juce::var (o.get()));
             }

             auto result = object();
             result->setProperty ("ticksPerQuarterNote", Ticks::perQuarterNote);
             result->setProperty ("commands", list);
             respond (ok (juce::var (result.get())));
         });

    add ("app.status", "Application and audio device status", "",
         [this] (const juce::var&, Respond respond)
         {
             auto result = object();
             result->setProperty ("version", ORCHESTRAL_DAW_VERSION);
             result->setProperty ("ticksPerQuarterNote", Ticks::perQuarterNote);
             result->setProperty ("tracks", (int) engine.getTrackIds().size());
             result->setProperty ("instruments", engine.getNumLoadedInstruments());

             if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
             {
                 auto d = object();
                 d->setProperty ("type", device->getTypeName());
                 d->setProperty ("name", device->getName());
                 d->setProperty ("sampleRate", device->getCurrentSampleRate());
                 d->setProperty ("bufferSize", device->getCurrentBufferSizeSamples());
                 result->setProperty ("device", juce::var (d.get()));
             }

             respond (ok (juce::var (result.get())));
         });

    //==========================================================================
    add ("transport.status", "Playback state and position", "",
         [this] (const juce::var&, Respond respond)
         {
             auto& transport = engine.getTransport();
             const auto map = transport.getTempoMap();
             const auto ticks = transport.getPositionTicks();
             const auto position = map->ticksToBarsBeats (ticks);

             auto result = object();
             result->setProperty ("playing", transport.isPlaying());
             result->setProperty ("recording", engine.isRecording());
             result->setProperty ("looping", transport.isLooping());
             result->setProperty ("loopStartTick", transport.getLoopStart());
             result->setProperty ("loopEndTick", transport.getLoopEnd());
             result->setProperty ("positionTicks", ticks);
             result->setProperty ("bar", position.bar);
             result->setProperty ("beat", position.beat);
             result->setProperty ("seconds", transport.getPositionSeconds());
             result->setProperty ("bpm", engine.getTempoBpm());
             respond (ok (juce::var (result.get())));
         });

    add ("transport.play", "Start playback", "", [this] (const juce::var&, Respond respond)
         { engine.getTransport().play(); respond (ok()); });

    add ("transport.stop", "Stop playback", "", [this] (const juce::var&, Respond respond)
         { engine.getTransport().stop(); respond (ok()); });

    add ("transport.locate", "Move the playhead", "tick:int64 | bar:int [beat:int]",
         [this] (const juce::var& params, Respond respond)
         {
             auto& transport = engine.getTransport();

             if (params.hasProperty ("bar"))
                 transport.locate (transport.getTempoMap()->barsBeatsToTicks (
                     { (int) params["bar"], (int) params.getProperty ("beat", 1), 0 }));
             else
                 transport.locate (tickParam (params, "tick"));

             respond (ok());
         });

    add ("transport.setLoop", "Enable/disable looping", "enabled:bool [startTick:int64 endTick:int64] (default: all content)",
         [this] (const juce::var& params, Respond respond)
         {
             auto& transport = engine.getTransport();
             const bool enabled = params.getProperty ("enabled", true);

             if (enabled)
                 transport.setLoopRegion (tickParam (params, "startTick"),
                                          tickParam (params, "endTick", engine.getLoopEndTicks()));

             transport.setLooping (enabled);
             respond (ok());
         });

    add ("tempo.set", "Set the tempo at the start of the piece", "bpm:number",
         [this] (const juce::var& params, Respond respond)
         {
             const double bpm = params.getProperty ("bpm", 0.0);

             if (bpm <= 0)
                 return respond (fail ("'bpm' must be a positive number"));

             engine.setTempoBpm (bpm);
             respond (ok());
         });

    //==========================================================================
    add ("track.list", "All MIDI tracks with routing and clip summary, in sidebar display order", "",
         [this] (const juce::var&, Respond respond)
         {
             juce::Array<juce::var> list;

             // Display order (the folder tree), collapsed folders included
             std::vector<AudioEngine::TrackId> ordered;

             for (auto& item : engine.getSidebarItems (true, false))
                 if (item.member != 0)
                     ordered.push_back (item.member);

             for (auto id : ordered)
             {
                 auto t = object();
                 t->setProperty ("id", id);
                 t->setProperty ("name", engine.getTrackName (id));
                 t->setProperty ("muted", engine.isTrackMuted (id));
                 t->setProperty ("soloed", engine.isTrackSoloed (id));
                 t->setProperty ("armed", engine.isTrackArmed (id));
                 t->setProperty ("primaryArmed", id == engine.getArmedTrack());
                 t->setProperty ("recordMode", engine.isTrackRecordReplace (id) ? "replace" : "add");
                 t->setProperty ("folderId", engine.getTrackFolder (id));
                 t->setProperty ("color", engine.getTrackColour (id));

                 juce::Array<juce::var> outputs;

                 for (auto& output : engine.getTrackOutputs (id))
                 {
                     auto o = object();
                     o->setProperty ("instrument", output.instrument);
                     o->setProperty ("instrumentName", engine.getInstrumentName (output.instrument));
                     o->setProperty ("channel", output.midiChannel);
                     o->setProperty ("port", output.midiPort);
                     outputs.add (juce::var (o.get()));
                 }

                 t->setProperty ("outputs", outputs);

                 if (auto sequence = engine.getTrackSequence (id))
                 {
                     auto c = object();
                     c->setProperty ("notes", (int) sequence->getNotes().size());
                     c->setProperty ("controls", (int) sequence->getControls().size());
                     c->setProperty ("lengthTicks", sequence->getLengthTicks());
                     t->setProperty ("clip", juce::var (c.get()));
                 }

                 list.add (juce::var (t.get()));
             }

             respond (ok (list));
         });

    add ("track.create", "Create a MIDI track", "[name:string]",
         [this] (const juce::var& params, Respond respond)
         {
             const auto id = engine.addTrack (params.getProperty ("name", {}).toString());

             auto result = object();
             result->setProperty ("id", id);
             result->setProperty ("name", engine.getTrackName (id));
             respond (ok (juce::var (result.get())));
         });

    auto requireTrack = [this] (const juce::var& params, Respond& respond, int& outId) -> bool
    {
        outId = (int) params.getProperty ("trackId", 0);
        const auto trackIds = engine.getTrackIds();

        if (std::find (trackIds.begin(), trackIds.end(), outId) != trackIds.end())
            return true;

        juce::StringArray ids;
        for (auto id : trackIds)
            ids.add (juce::String (id));

        respond (fail ("no track with id " + juce::String (outId)
                       + (ids.isEmpty() ? juce::String (" (no tracks exist)") : " (existing: " + ids.joinIntoString (", ") + ")")));
        return false;
    };

    add ("track.remove", "Remove a track", "trackId:int",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;
             engine.removeTrack (id);
             respond (ok());
         });

    add ("track.rename", "Rename a track", "trackId:int name:string",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;
             engine.setTrackName (id, params.getProperty ("name", {}).toString());
             respond (ok());
         });

    add ("ui.selectTrack", "Select a track exactly like clicking it in the sidebar (arms it when auto-arm is on; "
                           "the open editor follows)", "trackId:int",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             if (onSelectTrack == nullptr)
                 return respond (fail ("no UI attached"));

             const auto start = juce::Time::getMillisecondCounterHiRes();
             onSelectTrack (id);

             auto o = object();
             o->setProperty ("ms", juce::Time::getMillisecondCounterHiRes() - start);
             respond (ok (juce::var (o.get())));
         });

    add ("track.setColor", "Color a track ('#rrggbb', empty = none); shown as the row's left border and the region borders",
         "trackId:int color:string",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const auto color = params.getProperty ("color", {}).toString().trim();

             if (color.isNotEmpty() && ! (color.length() == 7 && color.startsWithChar ('#')))
                 return respond (fail ("'color' must be '#rrggbb' or empty"));

             engine.setTrackColour (id, color);
             respond (ok());
         });

    add ("track.setMuted", "Mute/unmute a track (MIDI)", "trackId:int muted:bool",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;
             engine.setTrackMuted (id, params.getProperty ("muted", true));
             respond (ok());
         });

    add ("track.setSoloed", "Solo/unsolo a track (MIDI)", "trackId:int soloed:bool",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;
             engine.setTrackSoloed (id, params.getProperty ("soloed", true));
             respond (ok());
         });

    add ("track.setRecordMode", "Recording mode: 'add' merges takes; 'replace' erases existing material from the first played event until recording stops",
         "trackId:int mode:'add'|'replace'",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const auto mode = params.getProperty ("mode", {}).toString();

             if (mode != "add" && mode != "replace")
                 return respond (fail ("'mode' must be 'add' or 'replace'"));

             engine.setTrackRecordReplace (id, mode == "replace");
             respond (ok());
         });

    add ("track.arm", "Arm tracks for live input and recording: one (trackId), or several at once "
                      "(trackIds; live input plays through all, a take records into all, each by its own "
                      "record mode). The primary is trackId, or the first of trackIds",
         "trackId:int | trackIds:[int]",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             if (auto* list = params.getProperty ("trackIds", {}).getArray())
             {
                 std::set<AudioEngine::TrackId> ids;
                 const auto existing = engine.getTrackIds();

                 for (auto& value : *list)
                 {
                     const auto id = (int) value;

                     if (std::find (existing.begin(), existing.end(), id) == existing.end())
                         return respond (fail ("no track with id " + juce::String (id) + " (see track.list)"));

                     ids.insert (id);
                 }

                 if (ids.empty())
                     return respond (fail ("'trackIds' is empty"));

                 engine.setArmedTracks (ids, (int) (*list)[0]);
                 return respond (ok());
             }

             int id = 0;
             if (! requireTrack (params, respond, id)) return;
             engine.setArmedTrack (id);
             respond (ok());
         });

    add ("track.setOutput", "Replace a track's outputs with one (instrument, channel; port for multiport instruments)",
         "trackId:int instrumentId:int channel:int(1-16) [port:int=1]",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const int instrumentId = (int) params.getProperty ("instrumentId", 0);

             if (engine.getInstrumentName (instrumentId).isEmpty())
                 return respond (fail ("no instrument with id " + juce::String (instrumentId)));

             engine.clearTrackOutputs (id);
             engine.addTrackOutput (id, instrumentId, (int) params.getProperty ("channel", 1),
                                    (int) params.getProperty ("port", 1));
             respond (ok());
         });

    add ("track.addOutput", "Add an output to a track (keeps existing ones)",
         "trackId:int instrumentId:int channel:int(1-16) [port:int=1]",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const int instrumentId = (int) params.getProperty ("instrumentId", 0);

             if (engine.getInstrumentName (instrumentId).isEmpty())
                 return respond (fail ("no instrument with id " + juce::String (instrumentId)));

             engine.addTrackOutput (id, instrumentId, (int) params.getProperty ("channel", 1),
                                    (int) params.getProperty ("port", 1));
             respond (ok());
         });

    add ("track.clearOutputs", "Remove all outputs from a track", "trackId:int",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;
             engine.clearTrackOutputs (id);
             respond (ok());
         });

    //==========================================================================
    add ("clip.get", "A track's clip contents", "trackId:int [includeControls:bool=false]",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const auto sequence = engine.getTrackSequence (id);

             if (sequence == nullptr)
                 return respond (ok());   // empty result = no clip

             auto result = object();
             result->setProperty ("lengthTicks", sequence->getLengthTicks());

             juce::Array<juce::var> notes;

             for (auto& n : sequence->getNotes())
             {
                 auto o = object();
                 o->setProperty ("start", n.startTick);
                 o->setProperty ("length", n.lengthTicks);
                 o->setProperty ("key", n.key);
                 o->setProperty ("velocity", n.velocity);
                 o->setProperty ("channel", n.channel);
                 notes.add (juce::var (o.get()));
             }

             result->setProperty ("notes", notes);

             if (params.getProperty ("includeControls", false))
             {
                 juce::Array<juce::var> controls;

                 for (auto& c : sequence->getControls())
                 {
                     auto o = object();
                     o->setProperty ("tick", c.tick);
                     o->setProperty ("type", (int) c.type);   // 0=controller 1=pitchBend 2=programChange
                     o->setProperty ("number", c.number);
                     o->setProperty ("value", c.value);
                     o->setProperty ("channel", c.channel);
                     controls.add (juce::var (o.get()));
                 }

                 result->setProperty ("controls", controls);
             }
             else
             {
                 result->setProperty ("controlCount", (int) sequence->getControls().size());
             }

             respond (ok (juce::var (result.get())));
         });

    auto parseNotes = [] (const juce::var& list, std::vector<MidiSequence::Note>& out) -> juce::String
    {
        auto* array = list.getArray();

        if (array == nullptr)
            return "'notes' must be an array of {start,length,key[,velocity,channel]}";

        for (auto& n : *array)
        {
            if (! n.hasProperty ("start") || ! n.hasProperty ("length") || ! n.hasProperty ("key"))
                return "every note needs start, length and key (ticks; 960000 per quarter note)";

            out.push_back ({ (juce::int64) n["start"], (juce::int64) n["length"],
                             (int) n.getProperty ("channel", 1), (int) n["key"],
                             (int) n.getProperty ("velocity", 100) });
        }

        return {};
    };

    auto parseControls = [] (const juce::var& list, std::vector<MidiSequence::Control>& out) -> juce::String
    {
        if (list.isVoid())
            return {};

        auto* array = list.getArray();

        if (array == nullptr)
            return "'controls' must be an array of {tick,type,number,value[,channel]}";

        for (auto& c : *array)
            out.push_back ({ (juce::int64) c["tick"],
                             (MidiSequence::ControlType) juce::jlimit (0, 2, (int) c.getProperty ("type", 0)),
                             (int) c.getProperty ("channel", 1), (int) c.getProperty ("number", 1),
                             (int) c.getProperty ("value", 0) });

        return {};
    };

    add ("clip.addNotes", "Add notes (and optionally controls) to a track's clip",
         "trackId:int notes:[{start,length,key,velocity?,channel?}] [controls:[{tick,type,number,value,channel?}]]",
         [this, requireTrack, parseNotes, parseControls] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             std::vector<MidiSequence::Note> notes;
             std::vector<MidiSequence::Control> controls;

             if (auto error = parseNotes (params["notes"], notes); error.isNotEmpty())
                 return respond (fail (error));

             if (auto error = parseControls (params["controls"], controls); error.isNotEmpty())
                 return respond (fail (error));

             engine.addToTrackSequence (id, std::move (notes), std::move (controls));

             const auto sequence = engine.getTrackSequence (id);
             auto result = object();
             result->setProperty ("noteCount", sequence != nullptr ? (int) sequence->getNotes().size() : 0);
             respond (ok (juce::var (result.get())));
         });

    add ("clip.set", "Replace a track's clip", "trackId:int notes:[...] [controls:[...]] (empty notes clears)",
         [this, requireTrack, parseNotes, parseControls] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             std::vector<MidiSequence::Note> notes;
             std::vector<MidiSequence::Control> controls;

             if (auto error = parseNotes (params["notes"], notes); error.isNotEmpty())
                 return respond (fail (error));

             if (auto error = parseControls (params["controls"], controls); error.isNotEmpty())
                 return respond (fail (error));

             engine.setTrackSequence (id, notes.empty() && controls.empty()
                                              ? nullptr
                                              : MidiSequence::create (std::move (notes), std::move (controls)));
             respond (ok());
         });

    add ("clip.clear", "Delete a track's clip", "trackId:int",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;
             engine.setTrackSequence (id, nullptr);
             respond (ok());
         });

    // Applies 'edit' to copies of the clip's note/control vectors, then commits the
    // result as one undoable step. An empty result clears the clip.
    auto editClip = [this] (int trackId,
                            std::function<juce::String (std::vector<MidiSequence::Note>&,
                                                        std::vector<MidiSequence::Control>&)> edit,
                            Respond& respond)
    {
        std::vector<MidiSequence::Note> notes;
        std::vector<MidiSequence::Control> controls;

        if (auto sequence = engine.getTrackSequence (trackId))
        {
            notes = sequence->getNotes();
            controls = sequence->getControls();
        }

        if (auto error = edit (notes, controls); error.isNotEmpty())
            return respond (fail (error));

        engine.setTrackSequence (trackId, notes.empty() && controls.empty()
                                              ? nullptr
                                              : MidiSequence::create (std::move (notes), std::move (controls)));

        const auto sequence = engine.getTrackSequence (trackId);
        auto result = object();
        result->setProperty ("noteCount", sequence != nullptr ? (int) sequence->getNotes().size() : 0);
        result->setProperty ("controlCount", sequence != nullptr ? (int) sequence->getControls().size() : 0);
        respond (ok (juce::var (result.get())));
    };

    add ("clip.updateNotes", "Modify notes by index (indices refer to the clip before the edit)",
         "trackId:int notes:[{index:int, start?,length?,key?,velocity?,channel?}]",
         [requireTrack, editClip] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             editClip (id, [&params] (auto& notes, auto&) -> juce::String
             {
                 auto* edits = params["notes"].getArray();

                 if (edits == nullptr)
                     return "'notes' must be an array of {index, ...changes}";

                 for (auto& edit : *edits)
                 {
                     const int index = (int) edit.getProperty ("index", -1);

                     if (index < 0 || index >= (int) notes.size())
                         return "note index " + juce::String (index) + " out of range (0.."
                                + juce::String ((int) notes.size() - 1) + ")";

                     auto& note = notes[(size_t) index];
                     if (edit.hasProperty ("start"))    note.startTick = (juce::int64) edit["start"];
                     if (edit.hasProperty ("length"))   note.lengthTicks = (juce::int64) edit["length"];
                     if (edit.hasProperty ("key"))      note.key = (int) edit["key"];
                     if (edit.hasProperty ("velocity")) note.velocity = (int) edit["velocity"];
                     if (edit.hasProperty ("channel"))  note.channel = (int) edit["channel"];
                 }

                 return {};
             }, respond);
         });

    add ("clip.removeNotes", "Remove notes by index", "trackId:int indices:[int]",
         [requireTrack, editClip] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             editClip (id, [&params] (auto& notes, auto&) -> juce::String
             {
                 auto* indices = params["indices"].getArray();

                 if (indices == nullptr)
                     return "'indices' must be an array of note indices";

                 std::vector<bool> remove (notes.size(), false);

                 for (auto& index : *indices)
                 {
                     const int i = (int) index;

                     if (i < 0 || i >= (int) notes.size())
                         return "note index " + juce::String (i) + " out of range";

                     remove[(size_t) i] = true;
                 }

                 size_t kept = 0;
                 for (size_t i = 0; i < notes.size(); ++i)
                     if (! remove[i])
                         notes[kept++] = notes[i];

                 notes.resize (kept);
                 return {};
             }, respond);
         });

    add ("clip.quantize", "Snap note starts to a grid", "trackId:int grid:int64(ticks) [strength:0..1=1] [start:int64 end:int64] ",
         [requireTrack, editClip] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const auto grid = (juce::int64) params.getProperty ("grid", 0);

             if (grid <= 0)
                 return respond (fail ("'grid' must be a positive tick count (e.g. 960000 = quarter note)"));

             const auto strength = juce::jlimit (0.0, 1.0, (double) params.getProperty ("strength", 1.0));
             const auto rangeStart = (juce::int64) params.getProperty ("start", 0);
             const auto rangeEnd = params.hasProperty ("end") ? (juce::int64) params["end"]
                                                              : std::numeric_limits<juce::int64>::max();

             editClip (id, [grid, strength, rangeStart, rangeEnd] (auto& notes, auto&) -> juce::String
             {
                 for (auto& note : notes)
                 {
                     if (note.startTick < rangeStart || note.startTick >= rangeEnd)
                         continue;

                     const auto target = ((note.startTick + grid / 2) / grid) * grid;
                     note.startTick += (juce::int64) std::llround ((double) (target - note.startTick) * strength);
                 }

                 return {};
             }, respond);
         });

    add ("clip.eraseRange", "Erase content in [start,end); notes crossing 'start' are truncated",
         "trackId:int start:int64 end:int64 [includeControls:bool=true]",
         [requireTrack, editClip] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const auto start = (juce::int64) params.getProperty ("start", 0);
             const auto end = (juce::int64) params.getProperty ("end", 0);

             if (end <= start)
                 return respond (fail ("'end' must be greater than 'start'"));

             const bool includeControls = params.getProperty ("includeControls", true);

             editClip (id, [start, end, includeControls] (auto& notes, auto& controls) -> juce::String
             {
                 std::erase_if (notes, [start, end] (const auto& n)
                                { return n.startTick >= start && n.startTick < end; });

                 for (auto& note : notes)
                     if (note.startTick < start && note.startTick + note.lengthTicks > start)
                         note.lengthTicks = start - note.startTick;

                 if (includeControls)
                     std::erase_if (controls, [start, end] (const auto& c)
                                    { return c.tick >= start && c.tick < end; });

                 return {};
             }, respond);
         });

    auto rangeCopier = [requireTrack, editClip] (bool removeSource)
    {
        return [requireTrack, editClip, removeSource] (const juce::var& params, Respond respond)
        {
            int id = 0;
            if (! requireTrack (params, respond, id)) return;

            const auto start = (juce::int64) params.getProperty ("start", 0);
            const auto end = (juce::int64) params.getProperty ("end", 0);
            const auto destStart = (juce::int64) params.getProperty ("destStart", 0);
            const auto times = juce::jlimit (1, 256, (int) params.getProperty ("times", 1));
            const bool includeControls = params.getProperty ("includeControls", true);

            if (end <= start)
                return respond (fail ("'end' must be greater than 'start'"));

            editClip (id, [=] (auto& notes, auto& controls) -> juce::String
            {
                std::vector<MidiSequence::Note> sourceNotes;
                std::vector<MidiSequence::Control> sourceControls;

                for (auto& note : notes)
                    if (note.startTick >= start && note.startTick < end)
                        sourceNotes.push_back (note);

                if (includeControls)
                    for (auto& control : controls)
                        if (control.tick >= start && control.tick < end)
                            sourceControls.push_back (control);

                if (removeSource)
                {
                    std::erase_if (notes, [start, end] (const auto& n)
                                   { return n.startTick >= start && n.startTick < end; });

                    if (includeControls)
                        std::erase_if (controls, [start, end] (const auto& c)
                                       { return c.tick >= start && c.tick < end; });
                }

                const auto span = end - start;

                for (int repeat = 0; repeat < times; ++repeat)
                {
                    const auto offset = destStart + (juce::int64) repeat * span - start;

                    for (auto note : sourceNotes)
                    {
                        note.startTick += offset;
                        notes.push_back (note);
                    }

                    for (auto control : sourceControls)
                    {
                        control.tick += offset;
                        controls.push_back (control);
                    }
                }

                return {};
            }, respond);
        };
    };

    add ("clip.copyRange", "Copy [start,end) to destStart, optionally repeated (ostinato)",
         "trackId:int start:int64 end:int64 destStart:int64 [times:int=1] [includeControls:bool=true]",
         rangeCopier (false));

    add ("clip.moveRange", "Move [start,end) to destStart",
         "trackId:int start:int64 end:int64 destStart:int64 [includeControls:bool=true]",
         rangeCopier (true));

    add ("clip.setControlRange",
         "Replace one controller's events inside [start,end) in one undoable step; empty 'events' erases. "
         "The CC-lane editor draws through this.",
         "trackId:int type:int(0=cc,1=pitchBend,2=program) number:int(cc only) start:int64 end:int64 "
         "events:[{tick,value}] [channel:int=1]",
         [requireTrack, editClip] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const auto type = (MidiSequence::ControlType) juce::jlimit (0, 2, (int) params.getProperty ("type", 0));
             const auto number = (int) params.getProperty ("number", 1);
             const auto start = (juce::int64) params.getProperty ("start", 0);
             const auto end = (juce::int64) params.getProperty ("end", 0);
             const auto channel = (int) params.getProperty ("channel", 1);

             if (end <= start)
                 return respond (fail ("'end' must be greater than 'start'"));

             auto* eventList = params["events"].getArray();

             if (eventList == nullptr && ! params["events"].isVoid())
                 return respond (fail ("'events' must be an array of {tick,value}"));

             editClip (id, [&] (auto&, auto& controls) -> juce::String
             {
                 std::erase_if (controls, [type, number, start, end] (const auto& c)
                 {
                     return c.type == type
                         && (type != MidiSequence::ControlType::controller || c.number == number)
                         && c.tick >= start && c.tick < end;
                 });

                 if (eventList != nullptr)
                     for (auto& event : *eventList)
                         controls.push_back ({ (juce::int64) event.getProperty ("tick", 0), type, channel,
                                               number, (int) event.getProperty ("value", 0) });

                 return {};
             }, respond);
         });

    add ("clip.undo", "Undo the last clip change on a track", "trackId:int",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             if (! engine.undoTrackSequence (id))
                 return respond (fail ("nothing to undo on track " + juce::String (id)));

             respond (ok());
         });

    add ("clip.redo", "Redo the last undone clip change on a track", "trackId:int",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             if (! engine.redoTrackSequence (id))
                 return respond (fail ("nothing to redo on track " + juce::String (id)));

             respond (ok());
         });

    //==========================================================================
    // These two are handled per-connection by the socket server; they exist here so
    // describe documents them (and so in-process callers get a helpful error).
    add ("subscribe", "Receive push events on this socket connection (events are JSON lines with an "
                      "\"event\" field: transport, position, trackAdded/Removed/Changed, clipChanged, "
                      "instrumentAdded/Removed, markerAdded/Removed, tempoChanged, recordingStarted/"
                      "Finished, projectCleared/Loaded/Saved)", "",
         [] (const juce::var&, Respond respond)
         { respond (fail ("subscribe only works on a TCP connection (the socket server handles it)")); });

    add ("unsubscribe", "Stop receiving push events on this socket connection", "",
         [] (const juce::var&, Respond respond)
         { respond (fail ("unsubscribe only works on a TCP connection (the socket server handles it)")); });

    //==========================================================================
    add ("marker.list", "Project markers (named positions dividing the song into parts)", "",
         [this] (const juce::var&, Respond respond)
         {
             const auto map = engine.getTransport().getTempoMap();
             juce::Array<juce::var> list;

             for (auto& marker : engine.getMarkers())
             {
                 auto o = object();
                 o->setProperty ("tick", marker.tick);
                 o->setProperty ("bar", map->ticksToBarsBeats (marker.tick).bar);
                 o->setProperty ("name", marker.name);
                 list.add (juce::var (o.get()));
             }

             respond (ok (list));
         });

    add ("marker.add", "Add (or rename) a marker", "name:string, tick:int64 | bar:int",
         [this] (const juce::var& params, Respond respond)
         {
             const auto map = engine.getTransport().getTempoMap();
             const auto tick = params.hasProperty ("bar")
                                   ? map->barsBeatsToTicks ({ (int) params["bar"], 1, 0 })
                                   : tickParam (params, "tick");

             engine.addMarker (tick, params.getProperty ("name", {}).toString());
             respond (ok());
         });

    add ("marker.remove", "Remove the marker at a tick", "tick:int64",
         [this] (const juce::var& params, Respond respond)
         {
             engine.removeMarker (tickParam (params, "tick"));
             respond (ok());
         });

    //==========================================================================
    add ("plugins.list", "Available instrument plugins from the scan cache", "[filter:string]",
         [this] (const juce::var& params, Respond respond)
         {
             const auto filter = params.getProperty ("filter", {}).toString();
             juce::Array<juce::var> list;

             for (auto& type : engine.getInstrumentTypes())
             {
                 if (filter.isNotEmpty() && ! (type.name + " " + type.manufacturerName).containsIgnoreCase (filter))
                     continue;

                 auto o = object();
                 o->setProperty ("name", type.name);
                 o->setProperty ("manufacturer", type.manufacturerName);
                 list.add (juce::var (o.get()));
             }

             respond (ok (list));
         });

    add ("instrument.list", "Loaded instruments (the rack)", "",
         [this] (const juce::var&, Respond respond)
         {
             juce::Array<juce::var> list;

             for (auto& [id, name] : engine.getInstruments())
             {
                 auto o = object();
                 o->setProperty ("id", id);
                 o->setProperty ("name", name);
                 o->setProperty ("audioChannelId", engine.getAudioChannelForInstrument (id));

                 juce::Array<juce::var> channels;

                 for (auto& channel : engine.getInstrumentMidiChannels (id))
                 {
                     auto c = object();
                     c->setProperty ("port", channel.midiPort);
                     c->setProperty ("channel", channel.midiChannel);
                     c->setProperty ("name", channel.name);
                     c->setProperty ("synced", channel.synced);

                     if (channel.veproPluginId.isNotEmpty())
                         c->setProperty ("veproPlugin", channel.veproPluginId);

                     if (channel.keyLow >= 0)
                     {
                         c->setProperty ("keyLow", channel.keyLow);
                         c->setProperty ("keyHigh", channel.keyHigh);
                     }

                     channels.add (juce::var (c.get()));
                 }

                 o->setProperty ("midiChannels", channels);

                 o->setProperty ("midiPorts", engine.getInstrumentMidiPortCount (id));
                 list.add (juce::var (o.get()));
             }

             respond (ok (list));
         });

    add ("instrument.add", "Load an instrument plugin into the rack (async; replies when loaded)", "name:string (exact or partial match)",
         [this] (const juce::var& params, Respond respond)
         {
             const auto wanted = params.getProperty ("name", {}).toString();

             if (wanted.isEmpty())
                 return respond (fail ("'name' is required (see plugins.list)"));

             const auto types = engine.getInstrumentTypes();
             juce::Array<juce::PluginDescription> matches;

             for (auto& type : types)
                 if (type.name.equalsIgnoreCase (wanted))
                     matches.add (type);

             if (matches.isEmpty())
                 for (auto& type : types)
                     if (type.name.containsIgnoreCase (wanted))
                         matches.add (type);

             if (matches.isEmpty())
                 return respond (fail ("no instrument matching '" + wanted + "' (see plugins.list)"));

             if (matches.size() > 1)
             {
                 juce::StringArray names;
                 for (auto& match : matches)
                     names.add (match.name);

                 return respond (fail ("'" + wanted + "' is ambiguous: " + names.joinIntoString (", ")));
             }

             engine.addInstrument (matches.getReference (0),
                 [respond, name = matches.getReference (0).name] (auto id, const juce::String& error)
                 {
                     if (id == 0)
                         return respond (fail (name + ": " + error));

                     auto result = object();
                     result->setProperty ("id", id);
                     result->setProperty ("name", name);
                     respond (ok (juce::var (result.get())));
                 });
         });

    add ("instrument.setChannelName", "Name one of an instrument's MIDI channels (synced channels are immutable)",
         "instrumentId:int channel:int(1-16) name:string [port:int=1]",
         [this] (const juce::var& params, Respond respond)
         {
             const int id = (int) params.getProperty ("instrumentId", 0);

             if (engine.getInstrumentName (id).isEmpty())
                 return respond (fail ("no instrument with id " + juce::String (id)));

             if (! engine.setInstrumentChannelName (id, (int) params.getProperty ("channel", 1),
                                                    params.getProperty ("name", {}).toString(),
                                                    (int) params.getProperty ("port", 1)))
                 return respond (fail ("that channel is synced from the VE Pro server and its name is immutable"));

             respond (ok());
         });

    // Runtime plugin introspection: everything the loaded instance exposes.
    add ("instrument.describe",
         "What the loaded plugin reports at runtime: version, format, buses, latency, programs, parameter count",
         "instrumentId:int",
         [this] (const juce::var& params, Respond respond)
         {
             const int id = (int) params.getProperty ("instrumentId", 0);
             auto* plugin = engine.getInstrumentPlugin (id);

             if (plugin == nullptr)
                 return respond (fail ("no instrument with id " + juce::String (id) + " (see instrument.list)"));

             const auto description = plugin->getPluginDescription();

             auto o = object();
             o->setProperty ("name", description.name);
             o->setProperty ("manufacturer", description.manufacturerName);
             o->setProperty ("version", description.version);
             o->setProperty ("format", description.pluginFormatName);
             o->setProperty ("category", description.category);
             o->setProperty ("file", description.fileOrIdentifier);
             o->setProperty ("acceptsMidi", plugin->acceptsMidi());
             o->setProperty ("producesMidi", plugin->producesMidi());
             o->setProperty ("latencySamples", plugin->getLatencySamples());
             o->setProperty ("tailLengthSeconds", plugin->getTailLengthSeconds());
             o->setProperty ("parameterCount", plugin->getParameters().size());
             o->setProperty ("midiPorts", engine.getInstrumentMidiPortCount (id));   // VST3 MIDI event input buses

             // Audio buses (VE Pro exposes many outputs; disabled ones report 0 channels)
             for (auto isInput : { true, false })
             {
                 juce::Array<juce::var> buses;

                 for (int i = 0; i < plugin->getBusCount (isInput); ++i)
                 {
                     if (auto* bus = plugin->getBus (isInput, i))
                     {
                         auto b = object();
                         b->setProperty ("name", bus->getName());
                         b->setProperty ("channels", bus->getNumberOfChannels());
                         b->setProperty ("enabled", bus->isEnabled());
                         buses.add (juce::var (b.get()));
                     }
                 }

                 o->setProperty (isInput ? "inputBuses" : "outputBuses", buses);
             }

             // Programs (capped: some samplers report hundreds)
             const auto numPrograms = plugin->getNumPrograms();
             o->setProperty ("programCount", numPrograms);
             o->setProperty ("currentProgram", plugin->getCurrentProgram());

             juce::Array<juce::var> programs;

             for (int i = 0; i < juce::jmin (numPrograms, 64); ++i)
                 programs.add (plugin->getProgramName (i));

             o->setProperty ("programs", programs);

             juce::MemoryBlock state;
             plugin->getStateInformation (state);
             o->setProperty ("stateBytes", (juce::int64) state.getSize());

             respond (ok (juce::var (o.get())));
         });

    add ("instrument.listParameters",
         "The plugin's parameters with live values (paged: some plugins expose thousands)",
         "instrumentId:int start:int? count:int(<=200)?",
         [this] (const juce::var& params, Respond respond)
         {
             const int id = (int) params.getProperty ("instrumentId", 0);
             auto* plugin = engine.getInstrumentPlugin (id);

             if (plugin == nullptr)
                 return respond (fail ("no instrument with id " + juce::String (id) + " (see instrument.list)"));

             const auto& parameters = plugin->getParameters();
             const auto start = juce::jmax (0, (int) params.getProperty ("start", 0));
             const auto count = juce::jlimit (1, 200, (int) params.getProperty ("count", 50));

             juce::Array<juce::var> list;

             for (int i = start; i < juce::jmin (parameters.size(), start + count); ++i)
             {
                 auto* parameter = parameters[i];
                 auto p = object();
                 p->setProperty ("index", i);
                 p->setProperty ("name", parameter->getName (128));
                 p->setProperty ("value", parameter->getValue());           // normalized 0..1
                 p->setProperty ("text", parameter->getCurrentValueAsText());
                 p->setProperty ("label", parameter->getLabel());
                 p->setProperty ("default", parameter->getDefaultValue());
                 p->setProperty ("automatable", parameter->isAutomatable());
                 list.add (juce::var (p.get()));
             }

             auto o = object();
             o->setProperty ("total", parameters.size());
             o->setProperty ("start", start);
             o->setProperty ("parameters", list);
             respond (ok (juce::var (o.get())));
         });

    add ("instrument.getStateStrings",
         "Readable strings fished out of the plugin's opaque state blob (ASCII and UTF-16 runs). "
         "Useful for e.g. seeing which server/instance a Vienna Ensemble Pro plugin is connected to",
         "instrumentId:int minLength:int(default 4)",
         [this] (const juce::var& params, Respond respond)
         {
             const int id = (int) params.getProperty ("instrumentId", 0);
             auto* plugin = engine.getInstrumentPlugin (id);

             if (plugin == nullptr)
                 return respond (fail ("no instrument with id " + juce::String (id) + " (see instrument.list)"));

             juce::MemoryBlock state;
             plugin->getStateInformation (state);

             const auto minLength = juce::jlimit (3, 64, (int) params.getProperty ("minLength", 4));
             juce::StringArray found;

             const auto scanBlock = [&found, minLength] (const juce::uint8* data, int size)
             {
                 const auto isPrintable = [] (juce::uint8 c) { return c >= 0x20 && c < 0x7f; };

                 // ASCII runs
                 for (int i = 0; i < size && found.size() < 400;)
                 {
                     int j = i;
                     while (j < size && isPrintable (data[j]))
                         ++j;

                     if (j - i >= minLength)
                         found.add (juce::String::fromUTF8 ((const char*) data + i, j - i));

                     i = juce::jmax (j, i + 1);
                 }

                 // UTF-16LE runs (printable ASCII char followed by a zero byte)
                 for (int i = 0; i + 1 < size && found.size() < 400;)
                 {
                     int j = i;
                     while (j + 1 < size && isPrintable (data[j]) && data[j + 1] == 0)
                         j += 2;

                     if ((j - i) / 2 >= minLength)
                     {
                         juce::String text;
                         for (int k = i; k < j; k += 2)
                             text += juce::String::charToString ((juce::juce_wchar) data[k]);

                         found.add (text);
                     }

                     i = juce::jmax (j, i + 2);
                 }
             };

             scanBlock (static_cast<const juce::uint8*> (state.getData()), (int) state.getSize());

             // JUCE wraps VST3 states as <VST3PluginState><IComponent>juce-base64...</IComponent>...;
             // the readable content lives inside, so decode the inner blobs and scan those too.
             {
                 juce::MemoryInputStream stream (state, false);
                 const auto text = stream.readEntireStreamAsString();
                 const auto xmlStart = text.indexOf ("<VST3PluginState>");

                 if (xmlStart >= 0)
                 {
                     if (auto xml = juce::parseXML (text.substring (xmlStart)))
                     {
                         for (auto* child : xml->getChildIterator())
                         {
                             juce::MemoryBlock inner;

                             if (inner.fromBase64Encoding (child->getAllSubText().trim()) && inner.getSize() > 0)
                                 scanBlock (static_cast<const juce::uint8*> (inner.getData()), (int) inner.getSize());
                         }
                     }
                 }
             }

             // The base64 payloads themselves aren't useful output
             for (int i = found.size(); --i >= 0;)
                 if (found[i].length() > 300)
                     found.remove (i);

             found.removeDuplicates (false);

             juce::Array<juce::var> list;
             for (auto& text : found)
                 list.add (text);

             auto o = object();
             o->setProperty ("stateBytes", (juce::int64) state.getSize());
             o->setProperty ("strings", list);
             respond (ok (juce::var (o.get())));
         });

    add ("instrument.getState",
         "The plugin's full state as base64. Stored states reconnect network plugins "
         "(e.g. Vienna Ensemble Pro) when set back, so this doubles as a connection template",
         "instrumentId:int",
         [this] (const juce::var& params, Respond respond)
         {
             const int id = (int) params.getProperty ("instrumentId", 0);
             auto* plugin = engine.getInstrumentPlugin (id);

             if (plugin == nullptr)
                 return respond (fail ("no instrument with id " + juce::String (id) + " (see instrument.list)"));

             juce::MemoryBlock state;
             plugin->getStateInformation (state);

             auto o = object();
             o->setProperty ("bytes", (juce::int64) state.getSize());
             o->setProperty ("stateBase64", juce::Base64::toBase64 (state.getData(), state.getSize()));
             respond (ok (juce::var (o.get())));
         });

    add ("instrument.setState",
         "Apply a previously captured base64 state to the plugin (same plugin type!)",
         "instrumentId:int stateBase64:string",
         [this] (const juce::var& params, Respond respond)
         {
             const int id = (int) params.getProperty ("instrumentId", 0);
             auto* plugin = engine.getInstrumentPlugin (id);

             if (plugin == nullptr)
                 return respond (fail ("no instrument with id " + juce::String (id) + " (see instrument.list)"));

             juce::MemoryOutputStream decoded;

             if (! juce::Base64::convertFromBase64 (decoded, params.getProperty ("stateBase64", {}).toString())
                  || decoded.getDataSize() == 0)
                 return respond (fail ("'stateBase64' is not valid base64"));

             plugin->setStateInformation (decoded.getData(), (int) decoded.getDataSize());
             respond (ok());
         });

    add ("instrument.connectVepro",
         "Connect a loaded Vienna Ensemble Pro plugin to a server instance by synthesizing its "
         "connection state. The state format is versioned per Pro Server release; the default "
         "comes from Settings > Integrations",
         "instrumentId:int instance:string [host:string=127.0.0.1] [hostName:string=localhost] "
         "[decoupled:bool=true] [version:string]",
         [this] (const juce::var& params, Respond respond)
         {
             const int id = (int) params.getProperty ("instrumentId", 0);
             auto* plugin = engine.getInstrumentPlugin (id);

             if (plugin == nullptr)
                 return respond (fail ("no instrument with id " + juce::String (id) + " (see instrument.list)"));

             if (! plugin->getPluginDescription().name.containsIgnoreCase ("Vienna Ensemble"))
                 return respond (fail ("instrument " + juce::String (id) + " is '"
                                       + plugin->getPluginDescription().name
                                       + "', not a Vienna Ensemble Pro plugin"));

             const auto instance = params.getProperty ("instance", {}).toString();

             if (instance.isEmpty())
                 return respond (fail ("'instance' (the server instance's name) is required"));

             const auto version = params.hasProperty ("version")
                                      ? params.getProperty ("version", {}).toString()
                                      : engine.getSettingsFile().getValue (vepro::versionSettingsKey,
                                                                           vepro::defaultVersion());

             vepro::ConnectTarget target;
             target.instanceName = instance;
             target.hostAddress = params.getProperty ("host", "127.0.0.1").toString();
             target.hostName = params.getProperty ("hostName", "localhost").toString();
             target.decoupled = params.getProperty ("decoupled", true);

             const auto state = vepro::buildConnectionState (version, target);

             if (state.getSize() == 0)
                 return respond (fail ("unsupported VE Pro Server version '" + version
                                       + "' (supported: " + vepro::supportedVersions().joinIntoString (", ")
                                       + "; select in Settings > Integrations)"));

             plugin->setStateInformation (state.getData(), (int) state.getSize());

             auto o = object();
             o->setProperty ("version", version);
             o->setProperty ("instance", instance);
             respond (ok (juce::var (o.get())));
         });

    add ("vepro.sync",
         "Sync to the VE Pro Server: one connected VE Pro instrument per server instance (named after "
         "it), synced per-player MIDI channels (immutable names), and one track per player. Idempotent; "
         "never deletes tracks - players gone from the server are reported in 'notes'. Server address "
         "and CLI path come from Settings > Integrations unless overridden",
         "[host:string] [port:int] [cliPath:string]",
         [this] (const juce::var& params, Respond respond)
         {
             if (veproSyncRunning)
                 return respond (fail ("a VE Pro sync is already running"));

             veproSyncRunning = true;
             engine.getBusyStatus().begin ("Syncing to the VE Pro Server");
             engine.getBusyStatus().update ("Querying the server for instances and players...");

             respond = [this, inner = std::move (respond)] (const juce::var& reply)
             {
                 veproSyncRunning = false;
                 engine.getBusyStatus().end();
                 inner (reply);
             };

             auto& settings = engine.getSettingsFile();
             const auto host = params.getProperty ("host",
                                   settings.getValue (vepro::serverHostKey, vepro::defaultServerHost())).toString();
             const auto port = (int) params.getProperty ("port",
                                   settings.getIntValue (vepro::serverPortKey, vepro::defaultServerPort));
             const auto cli = juce::File (params.getProperty ("cliPath",
                                   settings.getValue (vepro::cliPathKey,
                                                      vepro::defaultCliPath().getFullPathName())).toString());
             const auto version = settings.getValue (vepro::versionSettingsKey, vepro::defaultVersion());

             // Server queries block -> background thread; everything else -> message
             // thread. Host "auto" resolves via ZeroConf discovery first, so the
             // plugin connection states always get the server's real address.
             juce::Thread::launch ([weak = juce::WeakReference<CommandDispatcher> (this),
                                    host, port, cli, version, respond]
             {
                 juce::String fetchError;
                 juce::StringArray fetchWarnings;
                 auto resolvedHost = host;
                 auto resolvedPort = port;
                 const auto fetchStart = juce::Time::getMillisecondCounterHiRes();

                 if (vepro::isAutoHost (resolvedHost))
                     vepro::discoverServer (cli, resolvedHost, resolvedPort, fetchError);

                 auto fetched = fetchError.isEmpty()
                                    ? vepro::fetchInstances (cli, resolvedHost, resolvedPort, fetchError, fetchWarnings)
                                    : std::vector<vepro::SyncInstance>();

                 const auto fetchMs = juce::Time::getMillisecondCounterHiRes() - fetchStart;

                 juce::MessageManager::callAsync ([weak, resolvedHost, resolvedPort, version, respond, fetchMs,
                                                   fetchError, fetchWarnings, instances = std::move (fetched)]
                 {
                     if (weak == nullptr)
                         return;

                     if (fetchError.isNotEmpty())
                         return respond (fail ("VE Pro server: " + fetchError));

                     weak->veproResolvedHost = resolvedHost;
                     weak->veproResolvedPort = resolvedPort;
                     weak->applyVeproSync (instances, resolvedHost, version, respond, fetchWarnings, fetchMs);
                 });
             });
         });

    add ("vepro.keyRange",
         "Playable key range of the Synchron Player behind a synced track (of its first loaded sound slot), "
         "fetched from the VE Pro server once and cached in the project. Replies {low, high} as MIDI notes "
         "(60 = middle C), or available=false for players that don't expose one",
         "trackId:int [refresh:bool]",
         [this] (const juce::var& params, Respond respond)
         {
             const auto trackId = (int) params.getProperty ("trackId", 0);
             const auto info = engine.getTrackChannelInfo (trackId);

             const auto reply = [] (int low, int high)
             {
                 auto o = juce::DynamicObject::Ptr (new juce::DynamicObject());
                 o->setProperty ("available", low >= 0);

                 if (low >= 0)
                 {
                     o->setProperty ("low", low);
                     o->setProperty ("high", high);
                 }

                 return ok (juce::var (o.get()));
             };

             if (! info.has_value() || ! info->synced || info->veproChannelAddress.isEmpty()
                  || ! vepro::isSynchronPlayer (info->veproPluginId))
                 return respond (reply (-1, -1));

             if (info->keyLow >= 0 && ! (bool) params.getProperty ("refresh", false))
                 return respond (reply (info->keyLow, info->keyHigh));

             const auto key = info->veproInstanceId + "/" + info->veproChannelAddress;

             if (keyRangeFetches.count (key) > 0)
                 return respond (fail ("a key range fetch for that player is already running"));

             keyRangeFetches.insert (key);

             auto& settings = engine.getSettingsFile();
             const auto host = veproResolvedHost.isNotEmpty()
                                   ? veproResolvedHost
                                   : settings.getValue (vepro::serverHostKey, vepro::defaultServerHost());
             const auto port = veproResolvedHost.isNotEmpty()
                                   ? veproResolvedPort
                                   : settings.getIntValue (vepro::serverPortKey, vepro::defaultServerPort);
             const auto cli = juce::File (settings.getValue (vepro::cliPathKey,
                                                             vepro::defaultCliPath().getFullPathName()));
             const auto trackOutputs = engine.getTrackOutputs (trackId);
             const auto output = trackOutputs.front();
             const auto instanceId = info->veproInstanceId;
             const auto address = info->veproChannelAddress;

             juce::Thread::launch ([weak = juce::WeakReference<CommandDispatcher> (this),
                                    host, port, cli, instanceId, address, key, output, respond, reply]
             {
                 juce::String error;
                 auto resolvedHost = host;
                 auto resolvedPort = port;
                 int low = -1, high = -1;

                 if (vepro::isAutoHost (resolvedHost))
                     vepro::discoverServer (cli, resolvedHost, resolvedPort, error);

                 if (error.isEmpty())
                     vepro::fetchKeyRange (cli, resolvedHost, resolvedPort, instanceId, address, low, high, error);

                 juce::MessageManager::callAsync ([weak, key, output, respond, reply, low, high, error,
                                                   resolvedHost, resolvedPort]
                 {
                     if (weak == nullptr)
                         return;

                     weak->keyRangeFetches.erase (key);

                     if (error.isNotEmpty())
                         return respond (fail ("VE Pro server: " + error));

                     weak->veproResolvedHost = resolvedHost;
                     weak->veproResolvedPort = resolvedPort;
                     weak->engine.setInstrumentChannelKeyRange (output.instrument, output.midiPort,
                                                                output.midiChannel, low, high);
                     respond (reply (low, high));
                 });
             });
         });

    //==========================================================================
    add ("channel.list", "Audio channels with levels", "",
         [this] (const juce::var&, Respond respond)
         {
             juce::Array<juce::var> list;

             // Display order (the folder tree), collapsed folders included
             std::vector<AudioEngine::AudioChannelId> ordered;

             for (auto& item : engine.getSidebarItems (false, false))
                 if (item.member != 0)
                     ordered.push_back (item.member);

             for (auto id : ordered)
             {
                 auto o = object();
                 o->setProperty ("id", id);
                 o->setProperty ("name", engine.getAudioChannelName (id));
                 o->setProperty ("inputInstrument", engine.getAudioChannelInput (id));
                 o->setProperty ("folderId", engine.getAudioChannelFolder (id));

                 if (auto* processor = engine.getAudioChannel (id))
                 {
                     o->setProperty ("gainDb", juce::Decibels::gainToDecibels (processor->getGain(), -60.0f));
                     o->setProperty ("muted", processor->isMuted());
                     o->setProperty ("peak", processor->getLastPeak());
                 }

                 list.add (juce::var (o.get()));
             }

             respond (ok (list));
         });

    add ("channel.setGain", "Set an audio channel's gain", "channelId:int db:number (-60..+6)",
         [this] (const juce::var& params, Respond respond)
         {
             auto* processor = engine.getAudioChannel ((int) params.getProperty ("channelId", 0));

             if (processor == nullptr)
                 return respond (fail ("no audio channel with that id (see channel.list)"));

             processor->setGain (juce::Decibels::decibelsToGain (
                 juce::jlimit (-60.0f, 6.0f, (float) (double) params.getProperty ("db", 0.0)), -60.0f));
             respond (ok());
         });

    add ("channel.setMuted", "Mute/unmute an audio channel", "channelId:int muted:bool",
         [this] (const juce::var& params, Respond respond)
         {
             auto* processor = engine.getAudioChannel ((int) params.getProperty ("channelId", 0));

             if (processor == nullptr)
                 return respond (fail ("no audio channel with that id (see channel.list)"));

             processor->setMuted (params.getProperty ("muted", true));
             respond (ok());
         });

    //==========================================================================
    // Folders: Cubase-style grouping of the sidebar lists. Two trees ("midi" for
    // tracks, "audio" for channels); purely organisational, no effect on playback.
    auto requireFolder = [this] (const juce::var& params, Respond respond, int& outId) -> bool
    {
        outId = (int) params.getProperty ("folderId", 0);

        if (engine.folderExists (outId))
            return true;

        respond (fail ("no folder with id " + juce::String (outId) + " (see folder.list)"));
        return false;
    };

    add ("folder.create", "Create a folder for grouping sidebar channels; folders nest",
         "domain:'midi'|'audio' name:string? parent:folderId?",
         [this] (const juce::var& params, Respond respond)
         {
             const auto domain = params.getProperty ("domain", "midi").toString();

             if (domain != "midi" && domain != "audio")
                 return respond (fail ("'domain' must be 'midi' or 'audio'"));

             const auto parent = (int) params.getProperty ("parent", 0);

             if (parent != 0 && ! engine.folderExists (parent))
                 return respond (fail ("no folder with id " + juce::String (parent) + " (see folder.list)"));

             if (parent != 0 && engine.isFolderMidiDomain (parent) != (domain == "midi"))
                 return respond (fail ("parent folder is in the other domain"));

             const auto id = engine.addFolder (domain == "midi", params.getProperty ("name", {}).toString(), parent);

             auto o = object();
             o->setProperty ("folderId", id);
             o->setProperty ("name", engine.getFolderName (id));
             respond (ok (juce::var (o.get())));
         });

    add ("folder.list", "Folders of one or both domains, with parent and collapsed state",
         "domain:'midi'|'audio'?",
         [this] (const juce::var& params, Respond respond)
         {
             const auto domain = params.getProperty ("domain", {}).toString();
             juce::Array<juce::var> list;

             for (auto midi : { true, false })
             {
                 if (domain.isNotEmpty() && (domain == "midi") != midi)
                     continue;

                 for (auto id : engine.getFolderIds (midi))
                 {
                     auto o = object();
                     o->setProperty ("folderId", id);
                     o->setProperty ("name", engine.getFolderName (id));
                     o->setProperty ("domain", midi ? "midi" : "audio");
                     o->setProperty ("parent", engine.getFolderParent (id));
                     o->setProperty ("collapsed", engine.isFolderCollapsed (id));
                     o->setProperty ("color", engine.getFolderColour (id));
                     list.add (juce::var (o.get()));
                 }
             }

             respond (ok (list));
         });

    add ("folder.rename", "Rename a folder", "folderId:int name:string",
         [this, requireFolder] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireFolder (params, respond, id)) return;

             const auto name = params.getProperty ("name", {}).toString();

             if (name.isEmpty())
                 return respond (fail ("'name' must not be empty"));

             engine.setFolderName (id, name);
             respond (ok());
         });

    add ("folder.setColor", "Color a folder ('#rrggbb', empty = none)", "folderId:int color:string",
         [this, requireFolder] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireFolder (params, respond, id)) return;

             const auto color = params.getProperty ("color", {}).toString().trim();

             if (color.isNotEmpty() && ! (color.length() == 7 && color.startsWithChar ('#')))
                 return respond (fail ("'color' must be '#rrggbb' or empty"));

             engine.setFolderColour (id, color);
             respond (ok());
         });

    add ("folder.remove", "Remove a folder; its contents move to its parent", "folderId:int",
         [this, requireFolder] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireFolder (params, respond, id)) return;
             engine.removeFolder (id);
             respond (ok());
         });

    add ("folder.setParent", "Move a folder into another folder (0 = top level)", "folderId:int parent:int",
         [this, requireFolder] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireFolder (params, respond, id)) return;

             if (! engine.setFolderParent (id, (int) params.getProperty ("parent", 0)))
                 return respond (fail ("can't move there (unknown parent, other domain, or it would create a cycle)"));

             respond (ok());
         });

    add ("folder.setCollapsed", "Collapse/expand a folder in the sidebar", "folderId:int collapsed:bool",
         [this, requireFolder] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireFolder (params, respond, id)) return;
             engine.setFolderCollapsed (id, params.getProperty ("collapsed", true));
             respond (ok());
         });

    add ("sidebar.move", "Move folders and/or members (tracks for midi, channels for audio) into a parent at a child index, as one ordered group - the drag operation",
         "domain:'midi'|'audio' folders:[int]? members:[int]? parent:int index:int",
         [this] (const juce::var& params, Respond respond)
         {
             const auto domain = params.getProperty ("domain", "midi").toString();

             if (domain != "midi" && domain != "audio")
                 return respond (fail ("'domain' must be 'midi' or 'audio'"));

             std::vector<AudioEngine::FolderId> folderIds;
             std::vector<int> memberIds;

             if (auto* array = params.getProperty ("folders", {}).getArray())
                 for (auto& value : *array)
                     folderIds.push_back ((int) value);

             if (auto* array = params.getProperty ("members", {}).getArray())
                 for (auto& value : *array)
                     memberIds.push_back ((int) value);

             if (! engine.moveSidebarItems (domain == "midi", folderIds, memberIds,
                                            (int) params.getProperty ("parent", 0),
                                            (int) params.getProperty ("index", 0)))
                 return respond (fail ("couldn't move (unknown ids, a domain mismatch, or a folder into its own subtree)"));

             respond (ok());
         });

    add ("track.setFolder", "Put a track in a folder (folderId 0 = top level)", "trackId:int folderId:int",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const auto folderId = (int) params.getProperty ("folderId", 0);

             if (folderId != 0 && ! engine.folderExists (folderId))
                 return respond (fail ("no folder with id " + juce::String (folderId) + " (see folder.list)"));

             if (folderId != 0 && ! engine.isFolderMidiDomain (folderId))
                 return respond (fail ("that folder is for audio channels"));

             engine.setTrackFolder (id, folderId);
             respond (ok());
         });

    add ("channel.setFolder", "Put an audio channel in a folder (folderId 0 = top level)", "channelId:int folderId:int",
         [this] (const juce::var& params, Respond respond)
         {
             const auto channelId = (int) params.getProperty ("channelId", 0);

             if (engine.getAudioChannel (channelId) == nullptr)
                 return respond (fail ("no audio channel with that id (see channel.list)"));

             const auto folderId = (int) params.getProperty ("folderId", 0);

             if (folderId != 0 && ! engine.folderExists (folderId))
                 return respond (fail ("no folder with id " + juce::String (folderId) + " (see folder.list)"));

             if (folderId != 0 && engine.isFolderMidiDomain (folderId))
                 return respond (fail ("that folder is for midi tracks"));

             engine.setAudioChannelFolder (channelId, folderId);
             respond (ok());
         });

    //==========================================================================
    add ("record.start", "Start recording onto the armed track (starts playback if stopped)", "",
         [this] (const juce::var&, Respond respond)
         {
             if (! engine.startRecording())
                 return respond (fail ("couldn't start recording (is there an armed track?)"));

             respond (ok());
         });

    add ("record.stop", "Stop recording and merge the take into the track's clip", "",
         [this] (const juce::var&, Respond respond)
         {
             engine.stopRecording();
             respond (ok());
         });

    //==========================================================================
    add ("project.save", "Save the project", "path:string (.odaw)",
         [this] (const juce::var& params, Respond respond)
         {
             const auto path = params.getProperty ("path", {}).toString();

             if (path.isEmpty())
                 return respond (fail ("'path' is required"));

             const juce::File file (path);

             if (! engine.saveProject (file))
                 return respond (fail ("couldn't write " + file.getFullPathName()));

             if (onAfterProjectChange)
                 onAfterProjectChange (file);

             respond (ok());
         });

    add ("project.load", "Load a project (async; replies when every instrument is up)", "path:string",
         [this] (const juce::var& params, Respond respond)
         {
             const juce::File file (params.getProperty ("path", {}).toString());

             if (! file.existsAsFile())
                 return respond (fail ("no such file: " + file.getFullPathName()));

             if (onBeforeProjectChange)
                 onBeforeProjectChange();

             engine.loadProject (file, [respond, file, after = onAfterProjectChange] (bool loaded, const juce::String& warnings)
             {
                 if (after)
                     after (loaded ? file : juce::File());

                 if (! loaded)
                     return respond (fail (warnings));

                 auto result = object();

                 if (warnings.isNotEmpty())
                     result->setProperty ("warnings", warnings);

                 respond (ok (juce::var (result.get())));
             });
         });

    add ("history.list", "The global action history (every change, newest last)",
         "[category:string(track|clip|instrument|marker|tempo|recording|project)] [trackId:int]",
         [this] (const juce::var& params, Respond respond)
         {
             if (history == nullptr)
                 return respond (fail ("history is not available"));

             const auto category = params.getProperty ("category", {}).toString();
             const auto trackFilter = (int) params.getProperty ("trackId", 0);
             const auto currentId = history->getCurrentEntryId();
             juce::Array<juce::var> list;

             for (auto& entry : history->getEntries())
             {
                 if (category.isNotEmpty() && entry.category != category)
                     continue;

                 if (trackFilter != 0 && entry.trackId != trackFilter)
                     continue;

                 auto o = object();
                 o->setProperty ("id", entry.id);
                 o->setProperty ("time", entry.time.formatted ("%H:%M:%S"));
                 o->setProperty ("description", entry.description);
                 o->setProperty ("category", entry.category);
                 o->setProperty ("trackId", entry.trackId);
                 o->setProperty ("current", entry.id == currentId);
                 list.add (juce::var (o.get()));
             }

             respond (ok (list));
         });

    add ("history.travel", "Time-travel the project to right after the given history entry", "id:int",
         [this] (const juce::var& params, Respond respond)
         {
             if (history == nullptr)
                 return respond (fail ("history is not available"));

             const auto id = (int) params.getProperty ("id", -1);

             if (! history->travelTo (id))
                 return respond (fail ("no history entry with id " + juce::String (id) + " (see history.list)"));

             respond (ok());
         });

    add ("project.new", "Close the current project and start empty", "",
         [this] (const juce::var&, Respond respond)
         {
             if (onBeforeProjectChange)
                 onBeforeProjectChange();

             engine.clearProject();

             if (onAfterProjectChange)
                 onAfterProjectChange ({});

             respond (ok());
         });
}

//==============================================================================
// vepro.sync, message-thread half. Instances are processed sequentially because
// loading a plugin is asynchronous; each step reuses an instrument named after
// the instance or creates one, (re)connects it, replaces its synced channels and
// creates a track per player that doesn't have one yet.
void CommandDispatcher::applyVeproSync (const std::vector<vepro::SyncInstance>& instances,
                                        const juce::String& host, const juce::String& version, Respond respond,
                                        const juce::StringArray& fetchWarnings, double fetchMs)
{
    struct SyncState
    {
        std::vector<vepro::SyncInstance> instances;
        size_t next = 0;
        juce::String host, version;
        juce::StringArray notes;
        int instrumentsCreated = 0, tracksCreated = 0, channelsSynced = 0;
        Respond respond;

        // Phase timing (reported in the reply): server fetch, plugin loads, building
        double fetchMs = 0, loadMs = 0, buildMs = 0, start = 0, loadStart = 0;
        bool announcedRebuild = false;
    };

    auto state = std::make_shared<SyncState>();
    state->instances = instances;
    state->host = host;
    state->version = version;
    state->notes = fetchWarnings;
    state->respond = std::move (respond);
    state->fetchMs = fetchMs;
    state->start = juce::Time::getMillisecondCounterHiRes();

    // One graph rebuild for the whole sync instead of one per instance/plugin
    engine.beginGraphBatch();

    auto step = std::make_shared<std::function<void()>>();

    auto finishInstance = [this, state, step] (size_t index, AudioEngine::InstrumentId instrumentId)
    {
        const auto buildStart = juce::Time::getMillisecondCounterHiRes();
        const auto& instance = state->instances[index];

        engine.setInstrumentName (instrumentId, instance.name);

        // (Re)connect when the latency fingerprint says we're not connected.
        // Never force-connect to an instance the server reports as taken: VSL can
        // block inside the connect (freezing the app AND the server) when the
        // instance still belongs to another - possibly dead - plugin socket.
        if (auto* plugin = engine.getInstrumentPlugin (instrumentId))
        {
            if (plugin->getLatencySamples() == 0)
            {
                if (instance.connected)
                {
                    state->notes.add (instance.name + ": the server reports it as connected to another plugin"
                                      " - left untouched (disconnect it on the server, then re-sync)");
                }
                else
                {
                    vepro::ConnectTarget target;
                    target.instanceName = instance.name;
                    target.hostAddress = state->host;
                    target.hostName = state->host;

                    const auto blob = vepro::buildConnectionState (state->version, target);

                    if (blob.getSize() > 0)
                    {
                        juce::Logger::writeToLog ("VE Pro sync: connecting '" + instance.name + "' via "
                                                  + state->host + "...");
                        plugin->setStateInformation (blob.getData(), (int) blob.getSize());
                        juce::Logger::writeToLog ("VE Pro sync: '" + instance.name + "' state applied");
                    }
                    else
                    {
                        state->notes.add (instance.name + ": unsupported state-format version '"
                                          + state->version + "' - not connected");
                    }
                }
            }
        }

        // Synced channels: names inherited from the players, immutable
        std::vector<AudioEngine::MidiChannelInfo> channels;

        for (auto& player : instance.players)
        {
            AudioEngine::MidiChannelInfo channel { player.midiPort, player.midiChannel, player.name, true };
            channel.veproInstanceId = instance.id;
            channel.veproChannelAddress = player.channelAddress;
            channel.veproPluginId = player.pluginId;
            channels.push_back (channel);
        }

        engine.setSyncedInstrumentChannels (instrumentId, channels);
        state->channelsSynced += (int) channels.size();

        // One track per player (idempotent: skip players that already have one);
        // report tracks pointing at players that no longer exist - never delete.
        const auto trackIds = engine.getTrackIds();

        // Where do new tracks land? Next to this instrument's existing tracks, so
        // the user's re-organisation is honoured. Only a brand-new instrument gets
        // the created structure: "VE Pro Server" > instance folder (ISSUES.md
        // "Integrations"; these are ordinary folders - renamable, movable).
        AudioEngine::FolderId newTrackFolder = 0;
        bool haveExistingTracks = false;

        for (auto trackId : trackIds)
        {
            for (auto& output : engine.getTrackOutputs (trackId))
            {
                if (output.instrument == instrumentId)
                {
                    newTrackFolder = engine.getTrackFolder (trackId);
                    haveExistingTracks = true;
                    break;
                }
            }

            if (haveExistingTracks)
                break;
        }

        if (! haveExistingTracks && ! instance.players.empty())
        {
            AudioEngine::FolderId serverFolder = 0;

            for (auto folderId : engine.getFolderIds (true))
                if (engine.getFolderParent (folderId) == 0 && engine.getFolderName (folderId) == "VE Pro Server")
                    serverFolder = folderId;

            if (serverFolder == 0)
                serverFolder = engine.addFolder (true, "VE Pro Server");

            AudioEngine::FolderId instanceFolder = 0;

            for (auto folderId : engine.getFolderIds (true))
                if (engine.getFolderParent (folderId) == serverFolder && engine.getFolderName (folderId) == instance.name)
                    instanceFolder = folderId;

            if (instanceFolder == 0)
            {
                instanceFolder = engine.addFolder (true, instance.name, serverFolder);

                // Instance color -> folder color, on creation only (user edits win later)
                if (instance.colour.isNotEmpty())
                    engine.setFolderColour (instanceFolder, instance.colour);
            }

            newTrackFolder = instanceFolder;
        }

        // (port, channel) pairs of this instrument that already have a track - one
        // scan instead of one per player (big projects made this quadratic)
        std::set<std::pair<int, int>> routed;

        for (auto trackId : trackIds)
            for (auto& output : engine.getTrackOutputs (trackId))
                if (output.instrument == instrumentId)
                    routed.insert ({ output.midiPort, output.midiChannel });

        for (auto& player : instance.players)
        {
            const auto exists = routed.count ({ player.midiPort, player.midiChannel }) > 0;

            if (! exists)
            {
                const auto trackId = engine.addTrack (player.name);
                engine.addTrackOutput (trackId, instrumentId, player.midiChannel, player.midiPort);

                if (newTrackFolder != 0)
                    engine.setTrackFolder (trackId, newTrackFolder);

                // Player color -> track color, on creation only (user edits win later)
                if (player.colour.isNotEmpty())
                    engine.setTrackColour (trackId, player.colour);

                ++state->tracksCreated;
            }
        }

        for (auto trackId : trackIds)
            for (auto& output : engine.getTrackOutputs (trackId))
                if (output.instrument == instrumentId
                     && std::none_of (instance.players.begin(), instance.players.end(),
                                      [&output] (const vepro::SyncPlayer& p)
                                      { return p.midiPort == output.midiPort && p.midiChannel == output.midiChannel; }))
                    state->notes.add ("Track '" + engine.getTrackName (trackId) + "' targets "
                                      + instance.name + " port " + juce::String (output.midiPort)
                                      + " ch " + juce::String (output.midiChannel)
                                      + ", which has no player on the server (kept)");

        // Players need their port to exist on the plugin (its VST3 MIDI event
        // buses mirror the server's port setting, e.g. 8 or 16)
        const auto portCount = engine.getInstrumentMidiPortCount (instrumentId);

        for (auto& player : instance.players)
            if (player.midiPort > portCount)
                state->notes.add (player.name + ": port " + juce::String (player.midiPort)
                                  + " but " + instance.name + "'s plugin offers " + juce::String (portCount)
                                  + " MIDI ports (raise the port count on the VE Pro server)");

        state->buildMs += juce::Time::getMillisecondCounterHiRes() - buildStart;

        // Breathe between instances: Windows only delivers paint and timer
        // messages when the queue is EMPTY, so chaining the next step with
        // callAsync starved the overlay (measured: a 5.4 s frozen frame). One
        // short frame of idle per instance lets it animate.
        juce::Timer::callAfterDelay (25, [step] { if (*step) (*step)(); });
    };

    *step = [this, state, step, finishInstance]
    {
        if (state->next >= state->instances.size() && ! state->announcedRebuild)
        {
            // Show the last step on screen BEFORE the one unavoidable block
            state->announcedRebuild = true;
            engine.getBusyStatus().update ("Building the audio graph...", 1.0);
            juce::Timer::callAfterDelay (40, [step] { if (*step) (*step)(); });
            return;
        }

        if (state->next >= state->instances.size())
        {
            const auto rebuildStart = juce::Time::getMillisecondCounterHiRes();
            engine.endGraphBatch();
            const auto rebuildMs = juce::Time::getMillisecondCounterHiRes() - rebuildStart;

            auto o = juce::DynamicObject::Ptr (new juce::DynamicObject());
            o->setProperty ("instances", (int) state->instances.size());
            o->setProperty ("instrumentsCreated", state->instrumentsCreated);
            o->setProperty ("tracksCreated", state->tracksCreated);
            o->setProperty ("channelsSynced", state->channelsSynced);

            juce::Array<juce::var> notes;
            for (auto& note : state->notes)
                notes.add (note);

            o->setProperty ("notes", notes);

            auto timing = juce::DynamicObject::Ptr (new juce::DynamicObject());
            timing->setProperty ("fetchMs", juce::roundToInt (state->fetchMs));
            timing->setProperty ("pluginLoadMs", juce::roundToInt (state->loadMs));
            timing->setProperty ("buildMs", juce::roundToInt (state->buildMs));
            timing->setProperty ("graphRebuildMs", juce::roundToInt (rebuildMs));
            timing->setProperty ("applyTotalMs",
                                 juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - state->start));
            o->setProperty ("timing", juce::var (timing.get()));
            juce::Logger::writeToLog ("VE Pro sync timing: " + juce::JSON::toString (juce::var (timing.get()), true));

            auto reply = juce::DynamicObject::Ptr (new juce::DynamicObject());
            reply->setProperty ("ok", true);
            reply->setProperty ("result", juce::var (o.get()));
            state->respond (juce::var (reply.get()));

            *step = nullptr;   // break the shared_ptr self-reference
            return;
        }

        const auto index = state->next++;
        const auto& instance = state->instances[index];

        engine.getBusyStatus().update ("Instance " + juce::String ((int) index + 1) + " of "
                                         + juce::String ((int) state->instances.size()) + ": " + instance.name
                                         + " (" + juce::String ((int) instance.players.size()) + " players)",
                                       (double) index / (double) juce::jmax ((size_t) 1, state->instances.size()));

        // Reuse the instrument named after the instance, if it's a VE Pro plugin
        for (auto& [instrumentId, name] : engine.getInstruments())
        {
            if (name == instance.name)
            {
                if (auto* plugin = engine.getInstrumentPlugin (instrumentId))
                {
                    if (plugin->getPluginDescription().name.containsIgnoreCase ("Vienna Ensemble"))
                    {
                        finishInstance (index, instrumentId);
                        return;
                    }
                }
            }
        }

        // Otherwise load a fresh VE Pro plugin
        juce::PluginDescription description;
        bool found = false;

        for (auto& type : engine.getInstrumentTypes())
        {
            if (type.name == "Vienna Ensemble Pro")
            {
                description = type;
                found = true;
                break;
            }
        }

        if (! found)
        {
            state->notes.add (instance.name + ": 'Vienna Ensemble Pro' is not in the plugin cache - skipped");
            (*step)();
            return;
        }

        state->loadStart = juce::Time::getMillisecondCounterHiRes();

        engine.addInstrument (description,
            [state, step, finishInstance, index] (AudioEngine::InstrumentId newId, const juce::String& error)
            {
                state->loadMs += juce::Time::getMillisecondCounterHiRes() - state->loadStart;

                if (newId == 0)
                {
                    state->notes.add (state->instances[index].name + ": plugin failed to load: " + error);
                    (*step)();
                    return;
                }

                ++state->instrumentsCreated;
                finishInstance (index, newId);
            });
    };

    (*step)();
}
