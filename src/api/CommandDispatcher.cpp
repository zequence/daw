#include "CommandDispatcher.h"
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
    add ("track.list", "All MIDI tracks with routing and clip summary", "",
         [this] (const juce::var&, Respond respond)
         {
             juce::Array<juce::var> list;

             for (auto id : engine.getTrackIds())
             {
                 auto t = object();
                 t->setProperty ("id", id);
                 t->setProperty ("name", engine.getTrackName (id));
                 t->setProperty ("muted", engine.isTrackMuted (id));
                 t->setProperty ("soloed", engine.isTrackSoloed (id));
                 t->setProperty ("armed", id == engine.getArmedTrack());
                 t->setProperty ("recordMode", engine.isTrackRecordReplace (id) ? "replace" : "add");

                 juce::Array<juce::var> outputs;

                 for (auto& output : engine.getTrackOutputs (id))
                 {
                     auto o = object();
                     o->setProperty ("instrument", output.instrument);
                     o->setProperty ("instrumentName", engine.getInstrumentName (output.instrument));
                     o->setProperty ("channel", output.midiChannel);
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

    add ("track.arm", "Arm a track for live input and recording", "trackId:int",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;
             engine.setArmedTrack (id);
             respond (ok());
         });

    add ("track.setOutput", "Replace a track's outputs with one (instrument, channel)", "trackId:int instrumentId:int channel:int(1-16)",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const int instrumentId = (int) params.getProperty ("instrumentId", 0);

             if (engine.getInstrumentName (instrumentId).isEmpty())
                 return respond (fail ("no instrument with id " + juce::String (instrumentId)));

             engine.clearTrackOutputs (id);
             engine.addTrackOutput (id, instrumentId, (int) params.getProperty ("channel", 1));
             respond (ok());
         });

    add ("track.addOutput", "Add an output to a track (keeps existing ones)", "trackId:int instrumentId:int channel:int(1-16)",
         [this, requireTrack] (const juce::var& params, Respond respond)
         {
             int id = 0;
             if (! requireTrack (params, respond, id)) return;

             const int instrumentId = (int) params.getProperty ("instrumentId", 0);

             if (engine.getInstrumentName (instrumentId).isEmpty())
                 return respond (fail ("no instrument with id " + juce::String (instrumentId)));

             engine.addTrackOutput (id, instrumentId, (int) params.getProperty ("channel", 1));
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

                 auto channels = object();

                 for (int ch = 1; ch <= 16; ++ch)
                     if (auto channelName = engine.getInstrumentChannelName (id, ch); channelName.isNotEmpty())
                         channels->setProperty (juce::Identifier (juce::String (ch)), channelName);

                 o->setProperty ("channelNames", juce::var (channels.get()));
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

    add ("instrument.setChannelName", "Name one of an instrument's MIDI channels", "instrumentId:int channel:int(1-16) name:string",
         [this] (const juce::var& params, Respond respond)
         {
             const int id = (int) params.getProperty ("instrumentId", 0);

             if (engine.getInstrumentName (id).isEmpty())
                 return respond (fail ("no instrument with id " + juce::String (id)));

             engine.setInstrumentChannelName (id, (int) params.getProperty ("channel", 1),
                                              params.getProperty ("name", {}).toString());
             respond (ok());
         });

    //==========================================================================
    add ("channel.list", "Audio channels with levels", "",
         [this] (const juce::var&, Respond respond)
         {
             juce::Array<juce::var> list;

             for (auto id : engine.getAudioChannelIds())
             {
                 auto o = object();
                 o->setProperty ("id", id);
                 o->setProperty ("name", engine.getAudioChannelName (id));
                 o->setProperty ("inputInstrument", engine.getAudioChannelInput (id));

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
