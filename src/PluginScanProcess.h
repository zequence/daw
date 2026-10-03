#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

// Runs OrchestralDAWScanner in the background and reports its output lines on the message thread.
class PluginScanProcess final : private juce::Thread
{
public:
    PluginScanProcess (const juce::File& scannerExe, juce::StringArray extraArgs)
        : Thread ("Plugin scan"), exe (scannerExe), args (std::move (extraArgs))
    {
    }

    ~PluginScanProcess() override
    {
        signalThreadShouldExit();
        process.kill();
        stopThread (5000);
    }

    std::function<void (const juce::String& line)> onOutput;
    std::function<void (bool success)> onFinished;

    void start()                    { startThread(); }
    bool isScanning() const         { return isThreadRunning(); }

private:
    void run() override
    {
        juce::StringArray command { exe.getFullPathName() };
        command.addArray (args);

        const auto started = process.start (command, juce::ChildProcess::wantStdOut);
        juce::String pending;

        while (started && ! threadShouldExit())
        {
            char buffer[512];
            const auto numRead = process.readProcessOutput (buffer, sizeof (buffer));

            if (numRead <= 0)
            {
                if (! process.isRunning())
                    break;

                wait (50);
                continue;
            }

            pending += juce::String::fromUTF8 (buffer, numRead);

            for (auto newline = pending.indexOfChar ('\n'); newline >= 0; newline = pending.indexOfChar ('\n'))
            {
                post (pending.substring (0, newline).trimEnd());
                pending = pending.substring (newline + 1);
            }
        }

        if (pending.isNotEmpty())
            post (pending.trimEnd());

        const auto success = started && ! threadShouldExit() && process.getExitCode() == 0;

        juce::MessageManager::callAsync ([this, success, alive = std::weak_ptr<int> (lifetime)]
        {
            if (! alive.expired() && onFinished)
                onFinished (success);
        });
    }

    void post (const juce::String& line)
    {
        juce::MessageManager::callAsync ([this, line, alive = std::weak_ptr<int> (lifetime)]
        {
            if (! alive.expired() && onOutput)
                onOutput (line);
        });
    }

    const juce::File exe;
    const juce::StringArray args;
    juce::ChildProcess process;
    std::shared_ptr<int> lifetime = std::make_shared<int>();

    JUCE_DECLARE_NON_COPYABLE (PluginScanProcess)
};
