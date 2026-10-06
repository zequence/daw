#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <map>

// Every key command of the app in one registry: its context, what it does and its
// default keys. The user's own keys (Settings > Key commands) are stored as
// overrides in the settings file; code asks keys::matches (id, keyPress).
//
// KEY_COMMANDS.md documents this list - a test fails when a command is missing
// there. Expression maps' articulation key commands are per map (not listed here).
namespace keys
{
    struct Command
    {
        const char* id;
        const char* context;    // "Global", "MIDI editor", "Note input"
        const char* label;
        std::vector<juce::KeyPress> defaults;
    };

    inline const std::vector<Command>& commands()
    {
        using K = juce::KeyPress;
        using M = juce::ModifierKeys;
        const auto ctrl = M::ctrlModifier, shift = M::shiftModifier, alt = M::altModifier;

        static const std::vector<Command> list {
            { "transport.playStop",   "Global", "Start / stop playback",                       { K (K::spaceKey) } },
            { "transport.home",       "Global", "Back to the beginning",                       { K (K::homeKey) } },
            { "view.back",            "Global", "Close the open view / go back (deselects first in the MIDI editor and arrangement)", { K (K::escapeKey) } },
            { "view.edit",            "Global", "MIDI editor with the select pointer (toggle)", { K ('e') } },
            { "view.draw",            "Global", "MIDI editor with the pen (toggle)",           { K ('d') } },
            { "view.performance",     "Global", "Performance monitor",                         { K (K::F12Key) } },
            { "view.history",         "Global", "History pane on / off",                       { K ('h') } },
            { "view.instruments",     "Global", "Instruments pane on / off",                   { K ('i') } },
            { "track.solo",           "Global", "Solo the selected track",                     { K ('s') } },
            { "track.mute",           "Global", "Mute the selected track",                     { K ('m') } },
            { "track.instrumentGui",  "Global", "Open / close the selected track's instrument GUI", { K ('g') } },
            { "edit.undo",            "Global", "Undo (the editor's own, else the selected track's)", { K ('z', ctrl, 0) } },
            { "edit.redo",            "Global", "Redo",                                        { K ('y', ctrl, 0), K ('z', ctrl | shift, 0) } },

            { "editor.noteInput",     "MIDI editor", "Note input on / off",                    { K ('n') } },
            { "editor.delete",        "MIDI editor", "Delete the selected notes (or CC points)", { K (K::deleteKey), K (K::backspaceKey) } },
            { "editor.selectAll",     "MIDI editor", "Select all notes",                       { K ('a', ctrl, 0) } },
            { "editor.copy",          "MIDI editor", "Copy the selected notes",                { K ('c', ctrl, 0) } },
            { "editor.cut",           "MIDI editor", "Cut the selected notes",                 { K ('x', ctrl, 0) } },
            { "editor.paste",         "MIDI editor", "Paste at the transport line",            { K ('v', ctrl, 0) } },
            { "editor.playheadLeft",  "MIDI editor", "Transport line to the previous note start (else the next grid line)", { K (K::leftKey) } },
            { "editor.playheadRight", "MIDI editor", "Transport line to the next note end (else the next grid line)",       { K (K::rightKey) } },
            { "editor.nudgeLeft",     "MIDI editor", "Move the selected notes a grid step earlier", { K (K::leftKey, alt, 0) } },
            { "editor.nudgeRight",    "MIDI editor", "Move the selected notes a grid step later",   { K (K::rightKey, alt, 0) } },
            { "editor.transposeUp",   "MIDI editor", "Transpose the selected notes a half step up",   { K (K::upKey) } },
            { "editor.transposeDown", "MIDI editor", "Transpose the selected notes a half step down", { K (K::downKey) } },
            { "editor.octaveUp",      "MIDI editor", "Transpose the selected notes an octave up",     { K (K::upKey, ctrl, 0) } },
            { "editor.octaveDown",    "MIDI editor", "Transpose the selected notes an octave down",   { K (K::downKey, ctrl, 0) } },

            { "input.length1",        "Note input", "Note length 1/1",   { K ('1'), K (K::numberPad1) } },
            { "input.length2",        "Note input", "Note length 1/2",   { K ('2'), K (K::numberPad2) } },
            { "input.length3",        "Note input", "Note length 1/4",   { K ('3'), K (K::numberPad3) } },
            { "input.length4",        "Note input", "Note length 1/8",   { K ('4'), K (K::numberPad4) } },
            { "input.length5",        "Note input", "Note length 1/16",  { K ('5'), K (K::numberPad5) } },
            { "input.length6",        "Note input", "Note length 1/32",  { K ('6'), K (K::numberPad6) } },
            { "input.length7",        "Note input", "Note length 1/64",  { K ('7'), K (K::numberPad7) } },
            { "input.length8",        "Note input", "Note length 1/128", { K ('8'), K (K::numberPad8) } },
            { "input.length9",        "Note input", "Note length 1/256", { K ('9'), K (K::numberPad9) } },
            { "input.rest",           "Note input", "Rest (move on by the note length)", { K ('0'), K (K::numberPad0) } },
            { "input.dot",            "Note input", "Dotted note (toggle)",        { K ('.') } },
            { "input.doubleDot",      "Note input", "Double-dotted note (toggle)", { K ('.', shift, 0) } },
        };

        return list;
    }

    inline const Command* find (const juce::String& id)
    {
        for (auto& command : commands())
            if (id == command.id)
                return &command;

        return nullptr;
    }

    // The keys in force: the defaults, or the user's own (stored as text descriptions)
    class Bindings
    {
    public:
        static Bindings& get()
        {
            static Bindings instance;
            return instance;
        }

        static constexpr auto settingsKey = "keyCommands";

        void load (juce::PropertySet& settings)   // the settings file (any property set in tests)
        {
            file = &settings;
            overrides.clear();

            const auto parsed = juce::JSON::parse (settings.getValue (settingsKey));   // keeps the object alive

            if (auto* object = parsed.getDynamicObject())
                for (auto& property : object->getProperties())
                    if (find (property.name.toString()) != nullptr)
                    {
                        std::vector<juce::KeyPress> list;

                        if (auto* array = property.value.getArray())
                            for (auto& description : *array)
                                if (auto press = juce::KeyPress::createFromDescription (description.toString()); press.isValid())
                                    list.push_back (press);

                        overrides[property.name.toString()] = list;
                    }
        }

        std::vector<juce::KeyPress> keysFor (const juce::String& id) const
        {
            if (const auto it = overrides.find (id); it != overrides.end())
                return it->second;

            const auto* command = find (id);
            return command != nullptr ? command->defaults : std::vector<juce::KeyPress>();
        }

        bool matches (const juce::String& id, const juce::KeyPress& press) const
        {
            for (auto& bound : keysFor (id))
                if (bound == press)
                    return true;

            return false;
        }

        bool isDefault (const juce::String& id) const   { return overrides.find (id) == overrides.end(); }

        void set (const juce::String& id, std::vector<juce::KeyPress> list)
        {
            const auto* command = find (id);

            if (command == nullptr)
                return;

            if (sameKeys (list, command->defaults))
                overrides.erase (id);
            else
                overrides[id] = std::move (list);

            save();
        }

        void reset (const juce::String& id)   { overrides.erase (id); save(); }
        void resetAll()                       { overrides.clear(); save(); }

        // Another command in the same context (or Global vs. any) already using this key
        juce::String conflictFor (const juce::String& id, const juce::KeyPress& press) const
        {
            const auto* self = find (id);

            for (auto& command : commands())
                if (id != command.id && self != nullptr
                     && (juce::String (command.context) == self->context || juce::String (command.context) == "Global"
                          || juce::String (self->context) == "Global")
                     && matches (command.id, press))
                    return command.label;

            return {};
        }

        static juce::String describe (const std::vector<juce::KeyPress>& list)
        {
            juce::StringArray names;

            for (auto& press : list)
                names.add (press.getTextDescriptionWithIcons());

            return names.isEmpty() ? juce::String ("(none)") : names.joinIntoString (", ");
        }

        std::function<void()> onChanged;

    private:
        static bool sameKeys (const std::vector<juce::KeyPress>& a, const std::vector<juce::KeyPress>& b)
        {
            if (a.size() != b.size())
                return false;

            for (size_t i = 0; i < a.size(); ++i)
                if (! (a[i] == b[i]))
                    return false;

            return true;
        }

        void save()
        {
            if (file != nullptr)
            {
                auto object = juce::DynamicObject::Ptr (new juce::DynamicObject());

                for (auto& [id, list] : overrides)
                {
                    juce::Array<juce::var> descriptions;

                    for (auto& press : list)
                        descriptions.add (press.getTextDescription());

                    object->setProperty (id, descriptions);
                }

                file->setValue (settingsKey, juce::JSON::toString (juce::var (object.get()), true));

                if (auto* properties = dynamic_cast<juce::PropertiesFile*> (file))
                    properties->saveIfNeeded();
            }

            if (onChanged)
                onChanged();
        }

        juce::PropertySet* file = nullptr;
        std::map<juce::String, std::vector<juce::KeyPress>> overrides;
    };

    inline bool matches (const char* id, const juce::KeyPress& press)   { return Bindings::get().matches (id, press); }
}
