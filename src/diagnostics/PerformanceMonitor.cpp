#include "PerformanceMonitor.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
 #include <psapi.h>
 #include <tlhelp32.h>
 #pragma comment (lib, "psapi.lib")
#endif

#include <map>

namespace
{
   #if JUCE_WINDOWS
    juce::uint64 toUInt64 (const FILETIME& ft)
    {
        return ((juce::uint64) ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }

    struct ProcessTree
    {
        std::multimap<DWORD, DWORD> children;   // parent -> child

        ProcessTree()
        {
            auto snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);

            if (snapshot == INVALID_HANDLE_VALUE)
                return;

            PROCESSENTRY32W entry {};
            entry.dwSize = sizeof (entry);

            for (auto ok = Process32FirstW (snapshot, &entry); ok; ok = Process32NextW (snapshot, &entry))
                if (entry.th32ProcessID != entry.th32ParentProcessID)
                    children.emplace (entry.th32ParentProcessID, entry.th32ProcessID);

            CloseHandle (snapshot);
        }

        std::vector<DWORD> descendantsOf (DWORD root) const
        {
            std::vector<DWORD> result, stack { root };

            // Bounded walk: PIDs get reused, so guard against cycles and absurd sizes.
            while (! stack.empty() && result.size() < 100000)
            {
                const auto pid = stack.back();
                stack.pop_back();

                auto [begin, end] = children.equal_range (pid);

                for (auto it = begin; it != end; ++it)
                {
                    if (it->second == root || std::find (result.begin(), result.end(), it->second) != result.end())
                        continue;

                    result.push_back (it->second);
                    stack.push_back (it->second);
                }
            }

            return result;
        }
    };
   #endif
}

//==============================================================================
struct PerformanceMonitor::Impl
{
   #if JUCE_WINDOWS
    juce::uint64 lastProcessTime = 0, lastSystemIdle = 0, lastSystemTotal = 0, lastWallTime = 0;
   #endif
};

PerformanceMonitor::PerformanceMonitor() : impl (std::make_unique<Impl>())
{
    sample();   // prime the CPU deltas
}

PerformanceMonitor::~PerformanceMonitor() = default;

PerformanceMonitor::Snapshot PerformanceMonitor::sample()
{
    Snapshot s;
    s.time = juce::Time::getCurrentTime();

   #if JUCE_WINDOWS
    const auto process = GetCurrentProcess();

    // CPU: this process
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes (process, &created, &exited, &kernel, &user);
    GetSystemTimeAsFileTime (&now);

    const auto processTime = toUInt64 (kernel) + toUInt64 (user);
    const auto wallTime = toUInt64 (now);

    if (impl->lastWallTime != 0 && wallTime > impl->lastWallTime)
    {
        const auto numCpus = (double) juce::jmax (1, juce::SystemStats::getNumCpus());
        s.processCpuPercent = 100.0 * (double) (processTime - impl->lastProcessTime)
                                / ((double) (wallTime - impl->lastWallTime) * numCpus);
    }

    impl->lastProcessTime = processTime;
    impl->lastWallTime = wallTime;

    // CPU: whole system
    FILETIME idle, sysKernel, sysUser;

    if (GetSystemTimes (&idle, &sysKernel, &sysUser))
    {
        const auto idleTime = toUInt64 (idle);
        const auto totalTime = toUInt64 (sysKernel) + toUInt64 (sysUser);   // kernel time includes idle

        if (impl->lastSystemTotal != 0 && totalTime > impl->lastSystemTotal)
        {
            const auto totalDelta = (double) (totalTime - impl->lastSystemTotal);
            const auto idleDelta = (double) (idleTime - impl->lastSystemIdle);
            s.systemCpuPercent = 100.0 * (1.0 - idleDelta / totalDelta);
        }

        impl->lastSystemIdle = idleTime;
        impl->lastSystemTotal = totalTime;
    }

    // Memory
    PROCESS_MEMORY_COUNTERS_EX counters {};
    if (GetProcessMemoryInfo (process, (PROCESS_MEMORY_COUNTERS*) &counters, sizeof (counters)))
    {
        s.workingSetBytes = (juce::int64) counters.WorkingSetSize;
        s.privateBytes = (juce::int64) counters.PrivateUsage;
    }

    MEMORYSTATUSEX memStatus {};
    memStatus.dwLength = sizeof (memStatus);
    if (GlobalMemoryStatusEx (&memStatus))
    {
        s.systemAvailableBytes = (juce::int64) memStatus.ullAvailPhys;
        s.systemTotalBytes = (juce::int64) memStatus.ullTotalPhys;
    }

    DWORD handles = 0;
    if (GetProcessHandleCount (process, &handles))
        s.handleCount = (int) handles;

    // Threads + descendant processes
    const ProcessTree tree;
    const auto self = GetCurrentProcessId();
    const auto descendants = tree.descendantsOf (self);
    s.descendantProcessCount = (int) descendants.size();

    for (auto pid : descendants)
    {
        if (auto h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            PROCESS_MEMORY_COUNTERS c {};
            if (GetProcessMemoryInfo (h, &c, sizeof (c)))
                s.descendantWorkingSetBytes += (juce::int64) c.WorkingSetSize;

            CloseHandle (h);
        }
    }

    auto threadSnapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
    if (threadSnapshot != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W entry {};
        entry.dwSize = sizeof (entry);

        for (auto ok = Process32FirstW (threadSnapshot, &entry); ok; ok = Process32NextW (threadSnapshot, &entry))
        {
            if (entry.th32ProcessID == self)
            {
                s.threadCount = (int) entry.cntThreads;
                break;
            }
        }

        CloseHandle (threadSnapshot);
    }
   #endif

    return s;
}

int PerformanceMonitor::countDescendantProcesses()
{
   #if JUCE_WINDOWS
    return (int) ProcessTree().descendantsOf (GetCurrentProcessId()).size();
   #else
    return 0;
   #endif
}

juce::String PerformanceMonitor::formatBytes (juce::int64 bytes)
{
    if (bytes >= (juce::int64) 1 << 30)
        return juce::String ((double) bytes / (double) (1 << 30), 2) + " GB";

    return juce::String ((bytes + (1 << 19)) >> 20) + " MB";
}

//==============================================================================
juce::String PerformanceMonitor::Snapshot::csvHeader()
{
    return "time,process_cpu_pct,system_cpu_pct,working_set_mb,private_mb,system_avail_mb,system_total_mb,"
           "threads,handles,descendant_processes,descendant_working_set_mb";
}

juce::String PerformanceMonitor::Snapshot::toCsv() const
{
    // Note: juce::String (double, 0) means "default precision", not "no decimals".
    const auto mb = [] (juce::int64 bytes) { return juce::String ((bytes + (1 << 19)) >> 20); };

    return juce::StringArray {
        time.formatted ("%H:%M:%S"),
        juce::String (processCpuPercent, 1),
        juce::String (systemCpuPercent, 1),
        mb (workingSetBytes),
        mb (privateBytes),
        mb (systemAvailableBytes),
        mb (systemTotalBytes),
        juce::String (threadCount),
        juce::String (handleCount),
        juce::String (descendantProcessCount),
        mb (descendantWorkingSetBytes)
    }.joinIntoString (",");
}

juce::String PerformanceMonitor::Snapshot::toSummary() const
{
    return "CPU " + juce::String (processCpuPercent, 1) + "% (system " + juce::String (juce::roundToInt (systemCpuPercent)) + "%)"
         + "  RAM " + formatBytes (workingSetBytes)
         + "  free " + formatBytes (systemAvailableBytes)
         + "  threads " + juce::String (threadCount)
         + "  children " + juce::String (descendantProcessCount);
}

//==============================================================================
PerformanceCsvLog::PerformanceCsvLog (const juce::File& file, const juce::StringArray& extraColumns)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    stream = std::make_unique<juce::FileOutputStream> (file);

    if (stream->openedOk())
    {
        auto header = PerformanceMonitor::Snapshot::csvHeader();

        if (! extraColumns.isEmpty())
            header << "," << extraColumns.joinIntoString (",");

        stream->writeText (header + "\n", false, false, nullptr);
        stream->flush();
    }
}

void PerformanceCsvLog::write (const PerformanceMonitor::Snapshot& snapshot, const juce::StringArray& extraValues)
{
    if (stream == nullptr || ! stream->openedOk())
        return;

    auto line = snapshot.toCsv();

    if (! extraValues.isEmpty())
        line << "," << extraValues.joinIntoString (",");

    stream->writeText (line + "\n", false, false, nullptr);
    stream->flush();
}
