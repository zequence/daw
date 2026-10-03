#pragma once

#include <juce_core/juce_core.h>

// Samples process- and system-level resource usage. Shared by the app and the plugin scanner.
// Call sample() periodically (e.g. once per second); CPU figures are averages since the previous call.
class PerformanceMonitor
{
public:
    struct Snapshot
    {
        juce::Time time;
        double processCpuPercent = 0.0;     // this process, % of the whole machine
        double systemCpuPercent  = 0.0;     // whole machine
        juce::int64 workingSetBytes = 0;    // this process, physical RAM in use
        juce::int64 privateBytes    = 0;    // this process, committed memory
        juce::int64 systemAvailableBytes = 0;
        juce::int64 systemTotalBytes     = 0;
        int threadCount = 0;
        int handleCount = 0;
        int descendantProcessCount = 0;     // children, grandchildren, ...
        juce::int64 descendantWorkingSetBytes = 0;

        static juce::String csvHeader();
        juce::String toCsv() const;
        juce::String toSummary() const;
    };

    PerformanceMonitor();
    ~PerformanceMonitor();

    Snapshot sample();

    // Cheap check without a full sample: number of live descendant processes.
    static int countDescendantProcesses();

    static juce::String formatBytes (juce::int64 bytes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (PerformanceMonitor)
};

//==============================================================================
// Appends snapshots (plus caller-supplied extra columns) to a CSV file.
class PerformanceCsvLog
{
public:
    PerformanceCsvLog (const juce::File& file, const juce::StringArray& extraColumns = {});

    void write (const PerformanceMonitor::Snapshot&, const juce::StringArray& extraValues = {});

private:
    std::unique_ptr<juce::FileOutputStream> stream;

    JUCE_DECLARE_NON_COPYABLE (PerformanceCsvLog)
};
