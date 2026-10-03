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

    // "auto" = let the CLI discover the server on the network (ZeroConf announce)
    inline juce::String defaultServerHost()  { return "auto"; }
    constexpr int defaultServerPort = 7200;

    inline bool isAutoHost (const juce::String& host)
    {
        return host.trim().isEmpty() || host.trim().equalsIgnoreCase ("auto");
    }

    inline juce::File defaultCliPath()
    {
        return juce::File ("C:\\ProgramData\\VSL\\Vienna Ensemble Pro\\mcp\\vepro-api-cli.exe");
    }

    // Runs one CLI command line with a hard timeout, capturing stdout through a
    // temp file. A pipe would deadlock: we must not wait for exit before reading,
    // but blocking reads can't honour a timeout when the CLI hangs. The file does
    // both - the CLI writes freely, and a hung process gets killed on deadline.
    inline juce::String runCliCapture (const juce::String& commandLine, int timeoutMs, juce::String& error)
    {
        juce::TemporaryFile temp ("vepro-cli");
        const auto wrapped = "cmd.exe /s /c \"" + commandLine + " > \""
                               + temp.getFile().getFullPathName() + "\"\"";

        juce::ChildProcess child;

        if (! child.start (wrapped, 0))
        {
            error = "couldn't start the VE Pro CLI";
            return {};
        }

        if (! child.waitForProcessToFinish (timeoutMs))
        {
            child.kill();
            error = "the VE Pro CLI timed out";
            return {};
        }

        return temp.getFile().loadFileAsString();
    }

    // ZeroConf discovery through the CLI: fills host/port with the announced
    // server. Blocking; false + 'error' set when nothing announces itself.
    inline bool discoverServer (const juce::File& cli, juce::String& host, int& port, juce::String& error)
    {
        if (! cli.existsAsFile())
        {
            error = "VE Pro CLI not found: " + cli.getFullPathName();
            return false;
        }

        const auto output = runCliCapture (cli.getFullPathName().quoted() + " discover", 15000, error);

        if (error.isNotEmpty())
        {
            error = "VE Pro server discovery failed: " + error;
            return false;
        }

        const auto jsonStart = output.indexOf ("{");
        const auto parsed = jsonStart >= 0 ? juce::JSON::parse (output.substring (jsonStart)) : juce::var();

        if (! parsed.isObject() || parsed.getProperty ("host", {}).toString().isEmpty())
        {
            error = "no VE Pro server announced itself on the network (is the server running?)";
            return false;
        }

        host = parsed.getProperty ("host", {}).toString();
        port = (int) parsed.getProperty ("port", defaultServerPort);
        return true;
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

        // Build the command line ourselves: JUCE doesn't escape embedded quotes on
        // Windows, which silently mangles the JSON payload (CRT rules: \" inside "...").
        const auto json = juce::JSON::toString (payload, true);
        const auto command = cli.getFullPathName().quoted()
                               + " call --host " + host + " --port " + juce::String (port)
                               + " --payload-json \"" + json.replace ("\\", "\\\\").replace ("\"", "\\\"") + "\"";

        const auto output = runCliCapture (command, 15000, error);

        if (error.isNotEmpty())
        {
            error += " talking to " + host + ":" + juce::String (port)
                       + " (is the server running and the address right?)";
            return {};
        }

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
        juce::String colour;           // "#rrggbb" from channel/color/get
    };

    struct SyncInstance
    {
        juce::String id, name;
        bool connected = false;
        juce::String colour;           // "#rrggbb" from instance/list
        std::vector<SyncPlayer> players;
    };

    // Blocking: instance list + each instance's MIDI routing (player names per
    // port/channel). Empty + 'error' set on failure; per-instance routing
    // failures land in 'warnings' instead of being swallowed.
    inline std::vector<SyncInstance> fetchInstances (const juce::File& cli, const juce::String& host, int port,
                                                     juce::String& error, juce::StringArray& warnings)
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
            instance.colour = entry.getProperty ("color", {}).toString();

            auto routingPayload = juce::DynamicObject::Ptr (new juce::DynamicObject());
            routingPayload->setProperty ("cmd", "instance/midirouting/summary");
            routingPayload->setProperty ("instanceId", instance.id);

            juce::String routingError;
            const auto routing = serverCall (cli, host, port, juce::var (routingPayload.get()), routingError);

            if (routingError.isNotEmpty())
                warnings.add (instance.name + ": couldn't read its MIDI routing - " + routingError);

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
                                juce::String channelAddress;

                                if (auto* channels = pair.getProperty ("channels", {}).getArray())
                                {
                                    if (! channels->isEmpty())
                                    {
                                        player.name = channels->getFirst().getProperty ("title", {}).toString();
                                        channelAddress = channels->getFirst().getProperty ("channelAddress", {}).toString();
                                    }
                                }

                                if (player.name.isEmpty())
                                    player.name = instance.name + " " + juce::String (player.midiPort)
                                                    + "." + juce::String (player.midiChannel);

                                // The player's color (instance colors come with instance/list)
                                if (channelAddress.isNotEmpty())
                                {
                                    auto colorPayload = juce::DynamicObject::Ptr (new juce::DynamicObject());
                                    colorPayload->setProperty ("cmd", "channel/color/get");
                                    colorPayload->setProperty ("instanceId", instance.id);
                                    colorPayload->setProperty ("channelAddress", channelAddress);

                                    juce::String colorError;
                                    const auto color = serverCall (cli, host, port,
                                                                   juce::var (colorPayload.get()), colorError);

                                    if (colorError.isEmpty())
                                        player.colour = color.toString();
                                }

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
