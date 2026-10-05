#include "../src/api/CommandDispatcher.h"

namespace
{
    constexpr auto Q = Ticks::perQuarterNote;
    using Map = ExpressionMap;
    using Out = ExpressionMap::Output;

    juce::var params (std::initializer_list<std::pair<juce::String, juce::var>> pairs)
    {
        auto o = new juce::DynamicObject();
        for (auto& [key, value] : pairs)
            o->setProperty (juce::Identifier (key), value);
        return juce::var (o);
    }

    juce::var note (juce::int64 start, int key)
    {
        return params ({ { "start", start }, { "length", Q }, { "key", key } });
    }

    Map::Articulation art (const juce::String& name, int cc, int value, std::initializer_list<const char*> appliesTo = {})
    {
        Map::Articulation a;
        a.name = name;
        a.outputs.push_back ({ Out::Type::controller, cc, value, false, -1 });

        for (auto* root : appliesTo)
            a.appliesTo.add (root);

        return a;
    }

    // Roots Staccato/Legato; Release (Short: Staccato only, Long: Legato only); Mute (all)
    Map makeMap (const juce::String& name)
    {
        Map map;
        map.name = name;
        map.groups.push_back ({ "Articulation", "", { art ("Staccato", 32, 10), art ("Legato", 32, 20) } });
        map.groups.push_back ({ "Release", "", { art ("Short", 33, 10, { "Staccato" }), art ("Long", 33, 90, { "Legato" }) } });
        map.groups.push_back ({ "Mute", "", { art ("Con sord", 34, 127) } });
        return map;
    }

    juce::String errorOf (const juce::var& reply)    { return reply["error"].toString(); }

    template <typename Condition>
    void pumpUntil (Condition&& isDone, int timeoutMs = 15000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! isDone() && juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    }
}

class ExpressionMapCommandTests final : public juce::UnitTest
{
public:
    ExpressionMapCommandTests() : UnitTest ("Expression map commands") {}

    void runTest() override
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);
        AudioEngine engine (settings);
        CommandDispatcher api (engine);

        beginTest ("expressionmap.set creates and replaces; list and get report it");
        {
            auto reply = api.run ("expressionmap.set", params ({ { "map", makeMap ("Strings").toVar() } }));
            expect (reply["ok"], errorOf (reply));
            expect ((bool) reply["result"]["created"]);

            reply = api.run ("expressionmap.set", params ({ { "map", makeMap ("strings").toVar() } }));
            expect (reply["ok"] && ! (bool) reply["result"]["created"], errorOf (reply));   // same name ignoring case: replaced

            const auto list = api.run ("expressionmap.list")["result"];
            expectEquals ((int) list.size(), 1);
            expectEquals ((int) list[0]["groups"], 3);
            expectEquals ((int) list[0]["articulations"], 5);
            expect ((bool) list[0]["valid"]);
            expectEquals ((int) list[0]["usedBy"].size(), 0);

            const auto get = api.run ("expressionmap.get", params ({ { "name", "STRINGS" } }));
            expect (get["ok"], errorOf (get));
            expectEquals (get["result"]["groups"][0]["name"].toString(), juce::String ("Articulation"));   // the root group first
            expectEquals ((int) get["result"]["groups"][1]["articulations"][0]["outputs"][0]["number"], 33);

            // The JSON round trips
            Map back;
            expectEquals (Map::fromVar (get["result"], back), juce::String());
            expect (back.isValid());
        }

        beginTest ("expressionmap.set explains what is wrong, structurally and by validation");
        {
            auto reply = api.run ("expressionmap.set", params ({ { "map", params ({ { "name", "X" } }) } }));
            expect (! reply["ok"] && errorOf (reply).contains ("needs 'groups'"), errorOf (reply));

            auto bad = makeMap ("Bad").toVar();
            bad["groups"][0]["articulations"][0]["outputs"][0].getDynamicObject()->setProperty ("type", "sysex");
            reply = api.run ("expressionmap.set", params ({ { "map", bad } }));
            expect (! reply["ok"] && errorOf (reply).contains ("must be one of keyswitch, controller, programChange (got 'sysex')"),
                    errorOf (reply));

            auto invalid = makeMap ("Invalid").toVar();
            invalid["groups"][1]["articulations"][0].getDynamicObject()->setProperty ("appliesTo", juce::Array<juce::var> { "Pizzicato" });
            reply = api.run ("expressionmap.set", params ({ { "map", invalid } }));
            expect (! reply["ok"] && errorOf (reply).contains ("'Pizzicato', which is not a root articulation"), errorOf (reply));
            expect (! engine.getExpressionMap ("Invalid").has_value());

            reply = api.run ("expressionmap.get", params ({ { "name", "Nope" } }));
            expect (! reply["ok"] && errorOf (reply).contains ("existing: strings"), errorOf (reply));
        }

        beginTest ("expressionmap.validate checks a stored map or an inline one");
        {
            auto reply = api.run ("expressionmap.validate", params ({ { "name", "Strings" } }));
            expect (reply["ok"] && (bool) reply["result"]["valid"], errorOf (reply));

            auto inline_ = makeMap ("Inline").toVar();
            inline_["groups"][0]["articulations"][1].getDynamicObject()->setProperty ("name", "STACCATO");   // duplicate ignoring case
            reply = api.run ("expressionmap.validate", params ({ { "map", inline_ } }));
            expect (reply["ok"] && ! (bool) reply["result"]["valid"]);
            expect (reply["result"]["problems"][0].toString().contains ("two articulations named"));

            reply = api.run ("expressionmap.validate");
            expect (! reply["ok"] && errorOf (reply).contains ("existing: strings"), errorOf (reply));
        }

        beginTest ("expressionmap.choose is the menu's rules as a query; nothing changes");
        {
            // From nothing: a root
            auto reply = api.run ("expressionmap.choose", params ({ { "name", "Strings" }, { "group", "articulation" },
                                                                    { "articulation", "staccato" } }));
            expect (reply["ok"] && (bool) reply["result"]["ok"], errorOf (reply));
            expectEquals (reply["result"]["selection"]["root"].toString(), juce::String ("Staccato"));

            // Then a modifier of it, via the selection it just returned
            reply = api.run ("expressionmap.choose", params ({ { "name", "Strings" }, { "selection", reply["result"]["selection"] },
                                                               { "group", "Release" }, { "articulation", "Short" } }));
            expect ((bool) reply["result"]["ok"], reply["result"]["error"].toString());
            expectEquals (reply["result"]["selection"]["modifiers"][0]["name"].toString(), juce::String ("Short"));

            // Switching the root reports what it would drop
            const auto withShort = reply["result"]["selection"];
            reply = api.run ("expressionmap.choose", params ({ { "name", "Strings" }, { "selection", withShort },
                                                               { "group", "Articulation" }, { "articulation", "Legato" } }));
            expect ((bool) reply["result"]["ok"]);
            expectEquals ((int) reply["result"]["dropped"].size(), 1);
            expectEquals (reply["result"]["dropped"][0]["name"].toString(), juce::String ("Short"));

            // A refused choice is an answer, with the reason; the call itself succeeded
            reply = api.run ("expressionmap.choose", params ({ { "name", "Strings" }, { "group", "Release" }, { "articulation", "Short" } }));
            expect (reply["ok"] && ! (bool) reply["result"]["ok"] && reply["result"]["error"].toString().contains ("need a root"));

            reply = api.run ("expressionmap.choose", params ({ { "name", "Nope" }, { "group", "A" }, { "articulation", "B" } }));
            expect (! reply["ok"] && errorOf (reply).contains ("existing: strings"));
        }

        beginTest ("notes carry articulations through clip.addNotes, clip.updateNotes and clip.get");
        {
            const auto trackId = (int) api.run ("track.create")["result"]["id"];
            const auto tid = juce::var (trackId);

            auto withArticulation = note (0, 60);
            withArticulation.getDynamicObject()->setProperty ("articulation",
                params ({ { "root", "Legato" },
                          { "modifiers", juce::Array<juce::var> { params ({ { "group", "Release" }, { "name", "Long" } }) } } }));

            auto reply = api.run ("clip.set", params ({ { "trackId", tid },
                                                        { "notes", juce::Array<juce::var> { withArticulation, note (Q, 62) } } }));
            expect (reply["ok"], errorOf (reply));

            auto notes = engine.getTrackSequence (trackId)->getNotes();
            expectEquals (notes[0].articulation.root, juce::String ("Legato"));
            expectEquals (notes[0].articulation.modifiers[0].second, juce::String ("Long"));
            expect (notes[1].articulation.isEmpty());

            // clip.get shows it only where there is one
            const auto got = api.run ("clip.get", params ({ { "trackId", tid } }))["result"]["notes"];
            expectEquals (got[0]["articulation"]["root"].toString(), juce::String ("Legato"));
            expect (! got[1].hasProperty ("articulation"));

            // updateNotes replaces the whole choice, and null clears it
            reply = api.run ("clip.updateNotes", params ({ { "trackId", tid }, { "notes", juce::Array<juce::var> {
                params ({ { "index", 1 }, { "articulation", params ({ { "root", "Staccato" } }) } }),
                params ({ { "index", 0 }, { "articulation", juce::var() } }) } } }));
            expect (reply["ok"], errorOf (reply));
            notes = engine.getTrackSequence (trackId)->getNotes();
            expect (notes[0].articulation.isEmpty());
            expectEquals (notes[1].articulation.root, juce::String ("Staccato"));

            reply = api.run ("clip.updateNotes", params ({ { "trackId", tid }, { "notes", juce::Array<juce::var> {
                params ({ { "index", 0 }, { "articulation", 5 } }) } } }));
            expect (! reply["ok"] && errorOf (reply).contains ("articulation must be an object"), errorOf (reply));

            // Orphan names are stored as given: a visible error later, never refused or erased
            reply = api.run ("clip.updateNotes", params ({ { "trackId", tid }, { "notes", juce::Array<juce::var> {
                params ({ { "index", 0 }, { "articulation", params ({ { "root", "No such" } }) } }) } } }));
            expect (reply["ok"], errorOf (reply));
            expectEquals (engine.getTrackSequence (trackId)->getNotes()[0].articulation.root, juce::String ("No such"));

            // Without a map on the track, choosing is refused with the way out; clearing needs none
            reply = api.run ("clip.setArticulation", params ({ { "trackId", tid }, { "indices", juce::Array<juce::var> { 0 } },
                                                               { "group", "Articulation" }, { "articulation", "Legato" } }));
            expect (! reply["ok"] && errorOf (reply).contains ("instrument.setChannelMap"), errorOf (reply));

            reply = api.run ("clip.setArticulation", params ({ { "trackId", tid }, { "indices", juce::Array<juce::var> { 0, 1 } },
                                                               { "clear", true } }));
            expect (reply["ok"], errorOf (reply));
            expect (engine.getTrackSequence (trackId)->getNotes()[0].articulation.isEmpty());
            expect (engine.getTrackSequence (trackId)->getNotes()[1].articulation.isEmpty());
        }

        runChannelTests (engine, api);
    }

private:
    void runChannelTests (AudioEngine& engine, CommandDispatcher& api)
    {
        beginTest ("instrument.setChannelMap and clip.setArticulation on a track playing that channel");

        juce::PluginDescription description;
        bool found = false;

        for (const auto* wanted : { "TAL-NoiseMaker", "Twin 3" })
        {
            for (auto& type : engine.getInstrumentTypes())
                if (type.name == wanted)
                {
                    description = type;
                    found = true;
                    break;
                }

            if (found)
                break;
        }

        if (! found)
        {
            logMessage ("!!! no TAL-NoiseMaker or Twin 3 in the plugin cache - skipping the channel command tests");
            return;
        }

        std::atomic<int> instrumentId { -1 };
        engine.addInstrument (description, [&instrumentId] (auto id, const juce::String&) { instrumentId = id; });
        pumpUntil ([&instrumentId] { return instrumentId.load() != -1; });
        expect (instrumentId > 0, "instrument failed to load");

        if (instrumentId <= 0)
            return;

        const auto iid = juce::var (instrumentId.load());

        // Helpful failures
        auto reply = api.run ("instrument.setChannelMap", params ({ { "instrumentId", 9999 }, { "channel", 2 }, { "map", "Strings" } }));
        expect (! reply["ok"] && errorOf (reply).contains ("no instrument with id 9999"), errorOf (reply));
        reply = api.run ("instrument.setChannelMap", params ({ { "instrumentId", iid }, { "channel", 2 }, { "map", "Brass" } }));
        expect (! reply["ok"] && errorOf (reply).contains ("existing: strings"), errorOf (reply));

        reply = api.run ("instrument.setChannelMap", params ({ { "instrumentId", iid }, { "channel", 2 }, { "map", "strings" } }));
        expect (reply["ok"], errorOf (reply));

        // It shows in instrument.list and in expressionmap.list
        bool listed = false;

        const auto instrumentList = api.run ("instrument.list")["result"];   // kept alive: getArray() points into it

        for (auto& instrument : *instrumentList.getArray())
        {
            const auto channelList = instrument["midiChannels"];

            for (auto& channel : *channelList.getArray())
                if ((int) channel["channel"] == 2)
                    listed = channel["expressionMap"].toString() == "strings";
        }

        expect (listed, "instrument.list does not show the channel's map");

        const auto used = api.run ("expressionmap.list")["result"][0]["usedBy"];
        expectEquals ((int) used.size(), 1);
        expectEquals ((int) used[0]["channel"], 2);

        // A track on that channel, with three notes
        const auto trackId = (int) api.run ("track.create")["result"]["id"];
        const auto tid = juce::var (trackId);
        engine.addTrackOutput (trackId, instrumentId, 2);
        api.run ("clip.set", params ({ { "trackId", tid }, { "notes", juce::Array<juce::var> { note (0, 60), note (Q, 62), note (2 * Q, 64) } } }));

        const auto choose = [&] (juce::Array<juce::var> indices, const char* group, const char* articulation, bool drop = false)
        {
            return api.run ("clip.setArticulation", params ({ { "trackId", tid }, { "indices", indices },
                                                              { "group", group }, { "articulation", articulation },
                                                              { "dropIncompatible", drop } }));
        };

        const auto articulationOf = [&engine, trackId] (size_t i) { return engine.getTrackSequence (trackId)->getNotes()[i].articulation; };

        reply = choose ({ 0, 1 }, "Articulation", "Staccato");
        expect (reply["ok"], errorOf (reply));
        expectEquals (articulationOf (0).root, juce::String ("Staccato"));
        expectEquals (articulationOf (1).root, juce::String ("Staccato"));
        expect (articulationOf (2).isEmpty());

        reply = choose ({ 0 }, "Release", "Short");
        expect (reply["ok"], errorOf (reply));
        expectEquals ((int) articulationOf (0).modifiers.size(), 1);

        // The modifier doesn't apply to the other roots, and modifiers need a root
        reply = choose ({ 1 }, "Release", "Long");
        expect (! reply["ok"] && errorOf (reply).contains ("note 1:") && errorOf (reply).contains ("doesn't apply"), errorOf (reply));
        reply = choose ({ 2 }, "Mute", "Con sord");
        expect (! reply["ok"] && errorOf (reply).contains ("need a root"), errorOf (reply));

        // Switching the root drops Short: refused until accepted; all or nothing
        reply = choose ({ 0, 1 }, "Articulation", "Legato");
        expect (! reply["ok"] && errorOf (reply).contains ("would drop 'Short' (Release)")
                  && errorOf (reply).contains ("dropIncompatible=true"), errorOf (reply));
        expectEquals (articulationOf (0).root, juce::String ("Staccato"));   // nothing changed, note 1 included
        expectEquals (articulationOf (1).root, juce::String ("Staccato"));

        reply = choose ({ 0, 1 }, "Articulation", "Legato", true);
        expect (reply["ok"], errorOf (reply));
        expectEquals (articulationOf (0).root, juce::String ("Legato"));
        expect (articulationOf (0).modifiers.empty());

        // Toggling the root off
        reply = choose ({ 0, 1 }, "Articulation", "Legato", true);
        expect (reply["ok"] && articulationOf (0).isEmpty() && articulationOf (1).isEmpty(), errorOf (reply));

        reply = choose ({ 7 }, "Articulation", "Legato");
        expect (! reply["ok"] && errorOf (reply).contains ("out of range"), errorOf (reply));

        //======================================================================
        beginTest ("renaming inside a map rewrites the notes of the tracks that play a channel using it");

        const auto selectionVar = [] (const char* root, const char* group = nullptr, const char* modifierName = nullptr)
        {
            juce::Array<juce::var> modifiers;

            if (group != nullptr)
                modifiers.add (params ({ { "group", group }, { "name", modifierName } }));

            return params ({ { "root", root }, { "modifiers", modifiers } });
        };

        // The track above plays channel 2 (map "strings"): Staccato+Short, Legato+Long, Staccato
        reply = api.run ("clip.updateNotes", params ({ { "trackId", tid }, { "notes", juce::Array<juce::var> {
            params ({ { "index", 0 }, { "articulation", selectionVar ("Staccato", "Release", "Short") } }),
            params ({ { "index", 1 }, { "articulation", selectionVar ("Legato", "Release", "Long") } }),
            params ({ { "index", 2 }, { "articulation", selectionVar ("Staccato") } }) } } }));
        expect (reply["ok"], errorOf (reply));

        // A track on channel 3, which has no map: it uses the same words but is not governed by this map
        const auto otherId = (int) api.run ("track.create")["result"]["id"];
        engine.addTrackOutput (otherId, instrumentId, 3);
        api.run ("clip.set", params ({ { "trackId", otherId }, { "notes", juce::Array<juce::var> { note (0, 60) } } }));
        api.run ("clip.updateNotes", params ({ { "trackId", otherId }, { "notes", juce::Array<juce::var> {
            params ({ { "index", 0 }, { "articulation", selectionVar ("Staccato") } }) } } }));

        const auto notesOf = [&engine] (int track) { return engine.getTrackSequence (track)->getNotes(); };
        const auto rename = [&] (const char* what, juce::var args) { return api.run (what, args); };

        // A root articulation: notes and the modifiers' applies-to lists follow
        reply = rename ("expressionmap.renameArticulation", params ({ { "name", "STRINGS" }, { "group", "articulation" },
                                                                      { "articulation", "staccato" }, { "newName", "Spiccato" } }));
        expect (reply["ok"], errorOf (reply));
        expectEquals ((int) reply["result"]["notesChanged"], 2);
        expectEquals (notesOf (trackId)[0].articulation.root, juce::String ("Spiccato"));
        expectEquals (notesOf (trackId)[1].articulation.root, juce::String ("Legato"));
        expectEquals (notesOf (trackId)[2].articulation.root, juce::String ("Spiccato"));
        expectEquals (notesOf (otherId)[0].articulation.root, juce::String ("Staccato"));   // another map's world: untouched
        expectEquals (engine.getExpressionMap ("strings")->groups[1].articulations[0].appliesTo.joinIntoString (","), juce::String ("Spiccato"));
        expect (engine.getExpressionMap ("strings")->isValid());

        // A modifier group
        reply = rename ("expressionmap.renameGroup", params ({ { "name", "strings" }, { "group", "Release" }, { "newName", "Tail" } }));
        expect (reply["ok"], errorOf (reply));
        expectEquals ((int) reply["result"]["notesChanged"], 2);
        expectEquals (notesOf (trackId)[0].articulation.modifiers[0].first, juce::String ("Tail"));
        expectEquals (notesOf (trackId)[1].articulation.modifiers[0].first, juce::String ("Tail"));

        // A modifier articulation
        reply = rename ("expressionmap.renameArticulation", params ({ { "name", "strings" }, { "group", "Tail" },
                                                                      { "articulation", "Short" }, { "newName", "Brief" } }));
        expect (reply["ok"], errorOf (reply));
        expectEquals ((int) reply["result"]["notesChanged"], 1);
        expectEquals (notesOf (trackId)[0].articulation.modifiers[0].second, juce::String ("Brief"));
        expectEquals (notesOf (trackId)[1].articulation.modifiers[0].second, juce::String ("Long"));

        // Everything still resolves against the map: no note is an error
        const auto map = *engine.getExpressionMap ("strings");

        for (auto& n : notesOf (trackId))
            expect (map.problemsOf (n.articulation).isEmpty(), map.problemsOf (n.articulation).joinIntoString ("; "));

        // Renaming the root group changes no note; a case-only change counts as no note either
        reply = rename ("expressionmap.renameGroup", params ({ { "name", "strings" }, { "group", "Articulation" }, { "newName", "Main" } }));
        expect (reply["ok"] && (int) reply["result"]["notesChanged"] == 0, errorOf (reply));
        reply = rename ("expressionmap.renameGroup", params ({ { "name", "strings" }, { "group", "Mute" }, { "newName", "MUTE" } }));
        expect (reply["ok"], errorOf (reply));

        // Refusals change nothing, and say why
        reply = rename ("expressionmap.renameArticulation", params ({ { "name", "strings" }, { "group", "Main" },
                                                                      { "articulation", "Legato" }, { "newName", "spiccato" } }));
        expect (! reply["ok"] && errorOf (reply).contains ("already has an articulation 'Spiccato'"), errorOf (reply));
        expectEquals (notesOf (trackId)[1].articulation.root, juce::String ("Legato"));

        reply = rename ("expressionmap.renameGroup", params ({ { "name", "Nope" }, { "group", "Main" }, { "newName", "X" } }));
        expect (! reply["ok"] && errorOf (reply).contains ("no expression map 'Nope'"), errorOf (reply));

        reply = rename ("expressionmap.renameArticulation", params ({ { "name", "strings" }, { "group", "Main" }, { "newName", "X" } }));
        expect (! reply["ok"] && errorOf (reply).contains ("give the 'articulation'"), errorOf (reply));

        beginTest ("sound slots: setSlot, slots, offered, choose and removeSlot");
        {
            // Color (root) Regular -> default Main Long; Main Long / Rep. (-> Tempo 120); Tempo 120 / 130
            Map slotMap;
            slotMap.name = "Duality";
            Map::Articulation regular, longNotes, rep, t120, t130;
            regular.name = "Regular";
            regular.defaults = { { "Main", "Long" } };
            longNotes.name = "Long";
            rep.name = "Rep.";
            rep.defaults = { { "Tempo", "120" } };
            t120.name = "120";
            t130.name = "130";
            slotMap.groups.push_back ({ "Color", "", { regular } });
            slotMap.groups.push_back ({ "Main", "", { longNotes, rep } });
            slotMap.groups.push_back ({ "Tempo", "", { t120, t130 } });

            Map::Slot first;
            first.selection.root = "Regular";
            first.selection.modifiers = { { "Main", "Long" } };
            first.outputs = { { Out::Type::programChange, 112, 0, false, -1 }, { Out::Type::programChange, 0, 0, false, -1 } };
            slotMap.slots = { first };

            auto slotReply = api.run ("expressionmap.set", params ({ { "map", slotMap.toVar() } }));
            expect (slotReply["ok"], errorOf (slotReply));

            const auto slotJson = [] (const char* main, const char* tempo, int program)
            {
                juce::Array<juce::var> modifiers { params ({ { "group", "Main" }, { "name", main } }) };

                if (tempo != nullptr)
                    modifiers.add (params ({ { "group", "Tempo" }, { "name", tempo } }));

                return params ({ { "articulation", params ({ { "root", "Regular" }, { "modifiers", modifiers } }) },
                                 { "outputs", juce::Array<juce::var> { params ({ { "type", "programChange" }, { "number", program } }) } },
                                 { "keyLow", 55 }, { "keyHigh", 98 } });
            };

            slotReply = api.run ("expressionmap.setSlot", params ({ { "name", "duality" }, { "slot", slotJson ("Rep.", "120", 4) } }));
            expect (slotReply["ok"] && ! (bool) slotReply["result"]["replaced"], errorOf (slotReply));
            slotReply = api.run ("expressionmap.setSlot", params ({ { "name", "duality" }, { "slot", slotJson ("Rep.", "130", 5) } }));
            expect (slotReply["ok"], errorOf (slotReply));
            slotReply = api.run ("expressionmap.setSlot", params ({ { "name", "duality" }, { "slot", slotJson ("Rep.", "130", 6) } }));
            expect (slotReply["ok"] && (bool) slotReply["result"]["replaced"] && (int) slotReply["result"]["slots"] == 3, errorOf (slotReply));

            slotReply = api.run ("expressionmap.slots", params ({ { "name", "duality" },
                                                               { "selection", params ({ { "root", "Regular" }, { "modifiers",
                                                                 juce::Array<juce::var> { params ({ { "group", "Main" }, { "name", "Rep." } }) } } }) } }));
            expect (slotReply["ok"] && slotReply["result"]["slots"].size() == 2, errorOf (slotReply));

            // Offered: no Tempo with Long, both tempos with Rep.
            const auto offered = [&] (const char* main)
            {
                return api.run ("expressionmap.offered", params ({ { "name", "duality" }, { "selection", params ({ { "root", "Regular" },
                    { "modifiers", juce::Array<juce::var> { params ({ { "group", "Main" }, { "name", main } }) } } }) } }))["result"];
            };

            expectEquals ((int) offered ("Long")["groups"].size(), 1);   // Main only
            expectEquals ((int) offered ("Rep.")["groups"][1]["articulations"].size(), 2);

            // Choosing Rep. fills in its default tempo
            slotReply = api.run ("expressionmap.choose", params ({ { "name", "duality" }, { "selection", params ({ { "root", "Regular" },
                { "modifiers", juce::Array<juce::var> { params ({ { "group", "Main" }, { "name", "Long" } }) } } }) },
                { "group", "Main" }, { "articulation", "Rep." } }));
            expect (slotReply["ok"] && (bool) slotReply["result"]["ok"], errorOf (slotReply));
            expectEquals (slotReply["result"]["selection"]["modifiers"][1]["name"].toString(), juce::String ("120"));

            // Removing the only slot of the root's default would leave the root without a whole slot: refused
            slotReply = api.run ("expressionmap.removeSlot", params ({ { "name", "duality" }, { "articulation", params ({ { "root", "Regular" },
                { "modifiers", juce::Array<juce::var> { params ({ { "group", "Main" }, { "name", "Long" } }) } } }) } }));
            expect (! slotReply["ok"] && errorOf (slotReply).contains ("must give a sound slot"), errorOf (slotReply));

            slotReply = api.run ("expressionmap.removeSlot", params ({ { "name", "duality" }, { "articulation", params ({ { "root", "Regular" },
                { "modifiers", juce::Array<juce::var> { params ({ { "group", "Main" }, { "name", "Rep." } }),
                                                        params ({ { "group", "Tempo" }, { "name", "130" } }) } } }) } }));
            expect (slotReply["ok"], errorOf (slotReply));
        }

        beginTest ("instrument.remove: tracks that play only it go with removeTracks, the others lose that output");
        {
            std::atomic<int> first { -1 }, second { -1 };
            engine.addInstrument (description, [&first] (auto id, const juce::String&) { first = id; });
            engine.addInstrument (description, [&second] (auto id, const juce::String&) { second = id; });
            pumpUntil ([&] { return first.load() != -1 && second.load() != -1; });
            expect (first > 0 && second > 0);

            const auto only = engine.addTrack ("Only");
            engine.addTrackOutput (only, first, 1);
            const auto both = engine.addTrack ("Both");
            engine.addTrackOutput (both, first, 2);
            engine.addTrackOutput (both, second, 1);

            auto removeReply = api.run ("instrument.remove", params ({ { "instrumentId", 9999 } }));
            expect (! removeReply["ok"] && errorOf (removeReply).contains ("existing:"), errorOf (removeReply));

            removeReply = api.run ("instrument.remove", params ({ { "instrumentId", first.load() }, { "removeTracks", true } }));
            expect (removeReply["ok"], errorOf (removeReply));
            expect (removeReply["result"]["removedTracks"].size() == 1 && (int) removeReply["result"]["removedTracks"][0] == only);
            expect (removeReply["result"]["unroutedTracks"].size() == 1 && (int) removeReply["result"]["unroutedTracks"][0] == both);

            const auto trackIds = engine.getTrackIds();
            expect (std::find (trackIds.begin(), trackIds.end(), only) == trackIds.end(), "the track that played only it is gone");
            expect (engine.getTrackOutputs (both).size() == 1 && engine.getTrackOutputs (both)[0].instrument == second);
            expect (engine.getInstrumentPlugin (first) == nullptr);

            removeReply = api.run ("instrument.remove", params ({ { "instrumentId", second.load() } }));
            expect (removeReply["ok"] && engine.getTrackOutputs (both).empty(), "without removeTracks the track stays, unrouted");
            const auto remaining = engine.getTrackIds();
            expect (std::find (remaining.begin(), remaining.end(), both) != remaining.end());
        }
    }
};

static ExpressionMapCommandTests expressionMapCommandTests;
