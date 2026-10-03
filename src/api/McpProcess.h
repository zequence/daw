#pragma once

#include <juce_core/juce_core.h>

// Runs the MCP adapter (tools/mcp/orchestral_daw_mcp.py) as a child process in HTTP
// mode, so agents can connect whenever the app is running. Toggled from Settings;
// when enabled it starts with the app.
class McpProcess
{
public:
    McpProcess() = default;
    ~McpProcess() { stop(); }

    static juce::File findScript()
    {
        // Next to the executable (installed layout), else walk up to the repo's tools/mcp.
        auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        for (int i = 0; i < 8; ++i)
        {
            const auto candidate = dir.getChildFile ("tools").getChildFile ("mcp")
                                      .getChildFile ("orchestral_daw_mcp.py");

            if (candidate.existsAsFile())
                return candidate;

            dir = dir.getParentDirectory();
        }

        return {};
    }

    bool start (int portToUse)
    {
        stop();

        const auto script = findScript();

        if (script == juce::File())
        {
            juce::Logger::writeToLog ("MCP: adapter script not found (tools/mcp/orchestral_daw_mcp.py)");
            return false;
        }

        port = portToUse;
        process = std::make_unique<juce::ChildProcess>();

        const juce::StringArray command { "python", script.getFullPathName(), "--http", juce::String (port) };

        if (! process->start (command, 0))
        {
            process.reset();
            juce::Logger::writeToLog ("MCP: couldn't start python (is it on PATH?)");
            return false;
        }

        juce::Logger::writeToLog ("MCP server started: " + getUrl());
        return true;
    }

    void stop()
    {
        if (process != nullptr)
        {
            process->kill();
            process.reset();
            juce::Logger::writeToLog ("MCP server stopped");
        }
    }

    bool isRunning() const      { return process != nullptr && process->isRunning(); }
    juce::String getUrl() const { return "http://127.0.0.1:" + juce::String (port) + "/mcp"; }

private:
    std::unique_ptr<juce::ChildProcess> process;
    int port = 53218;

    JUCE_DECLARE_NON_COPYABLE (McpProcess)
};
