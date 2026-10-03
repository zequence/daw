#pragma once

#include <juce_core/juce_core.h>

// Talking to the Vienna Ensemble Pro SERVER (instances, players) goes through
// VSL's own CLI, which ships with VE Pro and speaks their service protocol:
//   <ProgramData>/VSL/Vienna Ensemble Pro/mcp/vepro-api-cli.exe
// We spawn it per request (the same pattern as the plugin scanner and the MCP
// adapter) rather than reimplementing a proprietary wire protocol.
// All calls block - run them off the message thread.
namespace vepro
{
    constexpr auto serverHostKey = "veproServerHost";
    constexpr auto serverPortKey = "veproServerPort";
    constexpr auto cliPathKey    = "veproCliPath";

    inline juce::String defaultServerHost()  { return "127.0.0.1"; }
    constexpr int defaultServerPort = 7200;

    inline juce::File defaultCliPath()
    {
        return juce::File ("C:\\ProgramData\\VSL\\Vienna Ensemble Pro\\mcp\\vepro-api-cli.exe");
    }

    // One request -> the response's 'data'; void var + 'error' set on failure.
    inline juce::var serverCall (const juce::File& cli, const juce::String& host, int port,
                                 const juce::var& payload, juce::String& error)
    {
        if (! cli.existsAsFile())
        {
            error = "VE Pro CLI not found: " + cli.getFullPathName()
                      + " (install VE Pro 8.1+, or set its path in settings '" + juce::String (cliPathKey) + "')";
            return {};
        }

        juce::ChildProcess child;
        const juce::StringArray args { cli.getFullPathName(), "call",
                                       "--host", host, "--port", juce::String (port),
                                       "--payload-json", juce::JSON::toString (payload, true) };

        if (! child.start (args, juce::ChildProcess::wantStdOut))
        {
            error = "couldn't start the VE Pro CLI";
            return {};
        }

        // Hard timeout: a CLI aimed at a dead or wedged service must never hang us
        if (! child.waitForProcessToFinish (15000))
        {
            child.kill();
            error = "the VE Pro CLI timed out talking to " + host + ":" + juce::String (port)
                      + " (is the server running and the address right?)";
            return {};
        }

        const auto output = child.readAllProcessOutput();

        const auto jsonStart = output.indexOf ("{");

        if (jsonStart < 0)
        {
            error = "no JSON from the VE Pro CLI: " + output.substring (0, 200).trim();
            return {};
        }

        const auto parsed = juce::JSON::parse (output.substring (jsonStart));
        const auto response = parsed.getProperty ("response", juce::var());

        if (! response.isObject())
        {
            error = "unexpected VE Pro CLI output";
            return {};
        }

        if (response.getProperty ("status", {}).toString() != "success")
        {
            error = response.getProperty ("error", "VE Pro server error").toString();
            return {};
        }

        return response.getProperty ("data", juce::var());
    }

    //==========================================================================
    // What "Sync to VE Pro Server" works from
    struct SyncPlayer
    {
        int midiPort = 1;
        int midiChannel = 1;
        juce::String name;
    };

    struct SyncInstance
    {
        juce::String id, name;
        bool connected = false;
        std::vector<SyncPlayer> players;
    };

    // Blocking: instance list + each instance's MIDI routing (player names per
    // port/channel). Empty + 'error' set on failure.
    inline std::vector<SyncInstance> fetchInstances (const juce::File& cli, const juce::String& host, int port,
                                                     juce::String& error)
    {
        auto listPayload = juce::DynamicObject::Ptr (new juce::DynamicObject());
        listPayload->setProperty ("cmd", "instance/list");
        const auto list = serverCall (cli, host, port, juce::var (listPayload.get()), error);

        if (error.isNotEmpty() || list.getArray() == nullptr)
        {
            if (error.isEmpty())
                error = "instance/list returned no instances array";

            return {};
        }

        std::vector<SyncInstance> instances;

        for (auto& entry : *list.getArray())
        {
            SyncInstance instance;
            instance.id = entry.getProperty ("id", {}).toString();
            instance.name = entry.getProperty ("name", {}).toString();
            instance.connected = entry.getProperty ("connected", false);

            auto routingPayload = juce::DynamicObject::Ptr (new juce::DynamicObject());
            routingPayload->setProperty ("cmd", "instance/midirouting/summary");
            routingPayload->setProperty ("instanceId", instance.id);

            juce::String routingError;
            const auto routing = serverCall (cli, host, port, juce::var (routingPayload.get()), routingError);

            if (routingError.isEmpty())
            {
                if (auto* summaries = routing.getProperty ("instances", {}).getArray())
                {
                    for (auto& summary : *summaries)
                    {
                        if (auto* pairs = summary.getProperty ("occupiedPairs", {}).getArray())
                        {
                            for (auto& pair : *pairs)
                            {
                                SyncPlayer player;
                                player.midiPort = (int) pair.getProperty ("midiPort", 1);
                                player.midiChannel = (int) pair.getProperty ("midiChannel", 1);

                                if (auto* channels = pair.getProperty ("channels", {}).getArray())
                                    if (! channels->isEmpty())
                                        player.name = channels->getFirst().getProperty ("title", {}).toString();

                                if (player.name.isEmpty())
                                    player.name = instance.name + " " + juce::String (player.midiPort)
                                                    + "." + juce::String (player.midiChannel);

                                instance.players.push_back (player);
                            }
                        }
                    }
                }
            }

            instances.push_back (std::move (instance));
        }

        return instances;
    }
}
