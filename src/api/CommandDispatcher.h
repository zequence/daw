#pragma once

#include "../AudioEngine.h"

class HistoryManager;

// The application's command surface: JSON in, JSON out (see API.md).
//
// Every command runs on the message thread. Protocol:
//   request:  { "id": <any>, "cmd": "track.create", "params": { ... } }
//   reply:    { "id": <echoed>, "ok": true, "result": { ... } }
//          or { "id": <echoed>, "ok": false, "error": "..." }
//
// Some commands (instrument.add, project.load) finish asynchronously; the reply
// arrives when they complete. "describe" lists every command with its parameters,
// so clients - scripts, tests, agents - can discover the surface at runtime.
class CommandDispatcher
{
public:
    explicit CommandDispatcher (AudioEngine&);

    using Respond = std::function<void (const juce::var& reply)>;

    // 'message' is one JSON object (see above). Must be called on the message thread.
    void dispatch (const juce::String& message, Respond);
    void dispatchParsed (const juce::var& message, Respond);

    // In-process clients (the UI) call commands directly; the reply is returned for
    // synchronous commands (clip edits, transport...). Same code path as the socket.
    juce::var run (const juce::String& cmd, const juce::var& params = {});

    // UI hooks, so API-driven project changes keep the window state sane.
    std::function<void()> onBeforeProjectChange;                 // e.g. close plugin editor windows
    std::function<void (const juce::File&)> onAfterProjectChange;   // e.g. refresh labels/title ({} = new project)

    void setHistoryManager (HistoryManager* h)   { history = h; }

private:
    struct Command
    {
        juce::String description, params;   // params: human/agent-readable signature
        std::function<void (const juce::var& params, Respond)> run;
    };

    void add (const juce::String& name, const juce::String& description,
              const juce::String& params, std::function<void (const juce::var&, Respond)> run);
    void registerCommands();

    AudioEngine& engine;
    HistoryManager* history = nullptr;
    std::map<juce::String, Command> commands;

    JUCE_DECLARE_WEAK_REFERENCEABLE (CommandDispatcher)
    JUCE_DECLARE_NON_COPYABLE (CommandDispatcher)
};
