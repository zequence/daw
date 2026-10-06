#pragma once

#include <juce_core/juce_core.h>
#include <future>

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

    // "auto" = let the CLI discover the server on the network (ZeroConf announce).
    // Under Wine the server can't announce itself (no DNS-SD), so Linux defaults to
    // a server on this machine.
   #if JUCE_WINDOWS
    inline juce::String defaultServerHost()  { return "auto"; }
   #else
    inline juce::String defaultServerHost()  { return "127.0.0.1"; }
   #endif
    constexpr int defaultServerPort = 7200;

    inline bool isAutoHost (const juce::String& host)
    {
        return host.trim().isEmpty() || host.trim().equalsIgnoreCase ("auto");
    }

    inline juce::File defaultCliPath()
    {
       #if JUCE_WINDOWS
        return juce::File ("C:\\ProgramData\\VSL\\Vienna Ensemble Pro\\mcp\\vepro-api-cli.exe");
       #else
        // VE Pro runs under Wine; the CLI lives in the Wine prefix ($WINEPREFIX, else ~/.wine)
        auto prefix = juce::SystemStats::getEnvironmentVariable ("WINEPREFIX", {});

        if (prefix.isEmpty())
            prefix = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".wine").getFullPathName();

        return juce::File (prefix).getChildFile ("drive_c/ProgramData/VSL/Vienna Ensemble Pro/mcp/vepro-api-cli.exe");
       #endif
    }

    // Runs the CLI with 'args' under a hard timeout, capturing stdout through a
    // temp file. A pipe would deadlock: we must not wait for exit before reading,
    // but blocking reads can't honour a timeout when the CLI hangs. The file does
    // both - the CLI writes freely, and a hung process gets killed on deadline.
    inline juce::String runCliCapture (const juce::File& cli, const juce::StringArray& args, int timeoutMs, juce::String& error)
    {
        juce::TemporaryFile temp ("vepro-cli");
        juce::ChildProcess child;

       #if JUCE_WINDOWS
        // Build the command line ourselves: JUCE doesn't escape embedded quotes on
        // Windows, which silently mangles the JSON payload (CRT rules: \" inside "...").
        auto commandLine = cli.getFullPathName().quoted();

        for (auto& arg : args)
            commandLine << " " << (arg.containsAnyOf (" \"") ? "\"" + arg.replace ("\\", "\\\\").replace ("\"", "\\\"") + "\""
                                                              : arg);

        const auto started = child.start ("cmd.exe /s /c \"" + commandLine + " > \""
                                            + temp.getFile().getFullPathName() + "\"\"", 0);
       #else
        // The CLI is a Windows program: run it through Wine. The shell only does the
        // redirect; arguments go through "$@" untouched (no quoting of the JSON).
        juce::StringArray argv { "/bin/sh", "-c", "WINEDEBUG=-all exec \"$@\" > \"$0\" 2>/dev/null",
                                 temp.getFile().getFullPathName(), "wine", cli.getFullPathName() };
        argv.addArray (args);
        const auto started = child.start (argv, 0);
       #endif

        if (! started)
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

        const auto output = runCliCapture (cli, { "discover" }, 15000, error);

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

        const auto json = juce::JSON::toString (payload, true);
        const auto output = runCliCapture (cli, { "call", "--host", host, "--port", juce::String (port),
                                                  "--payload-json", json }, 15000, error);

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
        juce::String channelAddress;   // the server's channel address (for state export)
        juce::String pluginId;         // e.g. "Vienna Synchron Player"
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

        // Each instance needs two independent queries; instances are fetched up
        // to 8 at a time (sequential fetching dominated the server phase)
        const auto fetchOne = [&cli, &host, port] (const juce::var& entry, juce::String& warning)
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
                warning = instance.name + ": couldn't read its MIDI routing - " + routingError;

            // All channel colors of the instance in ONE call (per-player
            // channel/color/get made big projects spawn a CLI per player).
            std::map<juce::String, juce::String> colourByChannelId;
            {
                auto dataPayload = juce::DynamicObject::Ptr (new juce::DynamicObject());
                dataPayload->setProperty ("cmd", "instance/channelsdata");
                dataPayload->setProperty ("instanceId", instance.id);

                juce::String dataError;
                const auto channelsData = serverCall (cli, host, port, juce::var (dataPayload.get()), dataError);

                if (auto* rows = channelsData.getArray())
                    for (auto& row : *rows)
                        colourByChannelId[row.getProperty ("id", {}).toString()] = row.getProperty ("color", {}).toString();
            }

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
                                {
                                    if (! channels->isEmpty())
                                    {
                                        player.name = channels->getFirst().getProperty ("title", {}).toString();
                                        player.channelAddress = channels->getFirst().getProperty ("channelAddress", {}).toString();
                                        player.pluginId = channels->getFirst().getProperty ("instrument", {})
                                                                              .getProperty ("pluginId", {}).toString();

                                        const auto channelId = channels->getFirst().getProperty ("id", {}).toString();

                                        if (auto it = colourByChannelId.find (channelId); it != colourByChannelId.end())
                                            player.colour = it->second;
                                    }
                                }

                                if (player.name.isEmpty())
                                    player.name = instance.name + " " + juce::String (player.midiPort)
                                                    + "." + juce::String (player.midiChannel);

                                instance.players.push_back (player);
                            }
                        }
                    }
                }
            }

            return instance;
        };

        const auto& entries = *list.getArray();
        std::vector<SyncInstance> instances ((size_t) entries.size());
        std::vector<juce::String> instanceWarnings ((size_t) entries.size());
        constexpr int maxParallel = 8;

        for (int first = 0; first < entries.size(); first += maxParallel)
        {
            std::vector<std::future<void>> batch;

            for (int i = first; i < juce::jmin (entries.size(), first + maxParallel); ++i)
                batch.push_back (std::async (std::launch::async, [&, i]
                {
                    instances[(size_t) i] = fetchOne (entries.getReference (i), instanceWarnings[(size_t) i]);
                }));

            for (auto& done : batch)
                done.get();
        }

        for (auto& warning : instanceWarnings)
            if (warning.isNotEmpty())
                warnings.add (warning);

        return instances;   // server order preserved
    }
}
