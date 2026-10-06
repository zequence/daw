// OrchestralDAWScanner: scans VST3 plugins into the shared plugin cache.
//
// Each plugin is scanned in its own child process (this same exe with --scan-one),
// several in parallel, so a plugin that crashes or hangs can't take the scan down.
// Plugins already in the cache with an unchanged file are skipped, so re-running is cheap.
//
// Usage:
//   OrchestralDAWScanner --scan [--jobs N] [--timeout SECONDS] [--rescan-all] [--retry-failed]
//   OrchestralDAWScanner --scan-one <plugin path> <output xml>      (internal: worker mode)
//
// Safety: worker mode never spawns processes; a process started by a scan (marked through an
// inherited environment variable) refuses to run a scan itself; only one scan runs at a time;
// and the scan aborts if its process tree ever grows beyond what it started.

#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdio>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

#include "../UserData.h"
#include "../diagnostics/PerformanceMonitor.h"

namespace
{
    constexpr auto workerEnvVar = "ORCHESTRAL_DAW_SCAN_WORKER";

    enum ExitCode
    {
        exitOk = 0,
        exitUsage = 1,
        exitNoPlugins = 2,
        exitWriteFailed = 3,
        exitRecursionRefused = 4,
        exitAlreadyRunning = 5,
        exitRunawayAborted = 6
    };

    void lowerOwnPriority()
    {
       #if JUCE_WINDOWS
        SetPriorityClass (GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
       #endif
    }

    void markChildrenAsWorkers()
    {
       #if JUCE_WINDOWS
        SetEnvironmentVariableW (L"ORCHESTRAL_DAW_SCAN_WORKER", L"1");
       #endif
    }

    //==============================================================================
    struct Logger
    {
        explicit Logger (const juce::File& file)
        {
            file.deleteFile();
            stream = std::make_unique<juce::FileOutputStream> (file);
        }

        void operator() (const juce::String& line)
        {
            std::printf ("%s\n", line.toRawUTF8());
            std::fflush (stdout);

            if (stream != nullptr && stream->openedOk())
            {
                stream->writeText (juce::Time::getCurrentTime().formatted ("%H:%M:%S  ") + line + "\n", false, false, nullptr);
                stream->flush();
            }
        }

        std::unique_ptr<juce::FileOutputStream> stream;
    };

    // Discovery returns bundle paths ("X.vst3"), but scanned descriptions point inside the bundle
    // ("X.vst3\Contents\x86_64-win\X.vst3"). Map both to the outermost .vst3 so they can be matched.
    juce::String bundleRootOf (const juce::String& path)
    {
        juce::File result (path);

        // A hidden folder named ".vst3" (~/.vst3 on Linux) is a search root, not a bundle
        for (auto f = result.getParentDirectory(); f != f.getParentDirectory(); f = f.getParentDirectory())
            if (f.hasFileExtension ("vst3") && ! f.getFileName().startsWithChar ('.'))
                result = f;

        return result.getFullPathName().toLowerCase();
    }

    // Cache entries grouped by bundle, so "is this file already scanned and unchanged?" works.
    std::map<juce::String, juce::Array<juce::PluginDescription>> groupByBundle (const juce::KnownPluginList& list)
    {
        std::map<juce::String, juce::Array<juce::PluginDescription>> result;

        for (auto& type : list.getTypes())
            result[bundleRootOf (type.fileOrIdentifier)].add (type);

        return result;
    }

    bool isUpToDate (const juce::Array<juce::PluginDescription>& types, juce::AudioPluginFormat& format)
    {
        for (auto& type : types)
            if (format.pluginNeedsRescanning (type))
                return false;

        return ! types.isEmpty();
    }

    bool saveList (const juce::KnownPluginList& list, const juce::File& file)
    {
        auto xml = list.createXml();

        if (xml == nullptr)
            return false;

        juce::TemporaryFile temp (file);
        return xml->writeTo (temp.getFile()) && temp.overwriteTargetFileWithTemporary();
    }

    //==============================================================================
    // Worker mode: scan one plugin file and write the descriptions it contains as XML.
    // Must never start other processes.
    int scanOne (const juce::String& path, const juce::File& outFile)
    {
        lowerOwnPriority();

        juce::VST3PluginFormatHeadless format;
        juce::OwnedArray<juce::PluginDescription> found;
        format.findAllTypesForFile (found, path);

        if (found.isEmpty())
            return exitNoPlugins;

        juce::XmlElement root ("PLUGINS");

        for (auto* desc : found)
        {
            // Must match what AudioPluginFormat::pluginNeedsRescanning() compares against.
            desc->lastFileModTime = juce::File (desc->fileOrIdentifier).getLastModificationTime();
            desc->lastInfoUpdateTime = juce::Time::getCurrentTime();
            root.addChildElement (desc->createXml().release());
        }

        return root.writeTo (outFile) ? exitOk : exitWriteFailed;
    }

    //==============================================================================
    struct Job
    {
        juce::String path;
        juce::File outFile;
        std::unique_ptr<juce::ChildProcess> process;
        juce::uint32 startedMs = 0;
    };

    struct Options
    {
        int jobs = juce::jlimit (1, 4, juce::SystemStats::getNumCpus() / 4);
        int timeoutSeconds = 120;
        int limit = 0;   // 0 = no limit; for testing
        bool rescanAll = false, retryFailed = false;
    };

    Options parseOptions (const juce::StringArray& args)
    {
        Options options;

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--jobs")              options.jobs = juce::jlimit (1, 16, args[++i].getIntValue());
            else if (args[i] == "--timeout")      options.timeoutSeconds = juce::jmax (5, args[++i].getIntValue());
            else if (args[i] == "--limit")        options.limit = juce::jmax (0, args[++i].getIntValue());
            else if (args[i] == "--rescan-all")   options.rescanAll = true;
            else if (args[i] == "--retry-failed") options.retryFailed = true;
        }

        return options;
    }

    int scanAll (const Options& options)
    {
        if (juce::SystemStats::getEnvironmentVariable (workerEnvVar, {}).isNotEmpty())
        {
            std::printf ("Refusing to start a scan from inside a scan worker.\n");
            return exitRecursionRefused;
        }

        juce::InterProcessLock scanLock ("OrchestralDAW_PluginScan");

        if (! scanLock.enter (0))
        {
            std::printf ("Another plugin scan is already running.\n");
            return exitAlreadyRunning;
        }

        lowerOwnPriority();
        markChildrenAsWorkers();

        Logger log (UserData::getPluginScanLog());
        PerformanceMonitor perf;
        PerformanceCsvLog perfLog (UserData::getScannerPerfLog(), { "running_jobs", "done", "total" });

        const auto cacheFile = UserData::getPluginCacheFile();

        juce::VST3PluginFormatHeadless format;
        juce::KnownPluginList list;

        if (auto xml = juce::parseXML (cacheFile))
            list.recreateFromXml (*xml);

        if (options.retryFailed)
            list.clearBlacklistedFiles();

        // Forget plugins whose files have been removed.
        for (auto& type : list.getTypes())
            if (! format.doesPluginStillExist (type))
                list.removeType (type);

        const auto allFiles = format.searchPathsForPlugins (format.getDefaultLocationsToSearch(), true, true);
        const auto blacklist = list.getBlacklistedFiles();

        const auto cachedByBundle = groupByBundle (list);
        juce::StringArray queue;

        for (auto& file : allFiles)
        {
            if (blacklist.contains (file))
                continue;

            const auto cached = cachedByBundle.find (bundleRootOf (file));

            if (options.rescanAll || cached == cachedByBundle.end() || ! isUpToDate (cached->second, format))
                queue.add (file);
        }

        if (options.limit > 0)
            queue.removeRange (options.limit, queue.size());

        log ("Plugin cache: " + cacheFile.getFullPathName());
        log (juce::String (allFiles.size()) + " VST3 files found, " + juce::String (queue.size()) + " to scan, "
             + juce::String (blacklist.size()) + " previously failed (skipped), "
             + juce::String (options.jobs) + " parallel jobs, " + juce::String (options.timeoutSeconds) + " s timeout");

        const auto selfExe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
        const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("OrchestralDAWScan");
        tempDir.deleteRecursively();
        tempDir.createDirectory();

        // A worker may briefly have helper processes of its own, but never many.
        const auto maxDescendants = options.jobs * 3 + 4;

        std::vector<std::unique_ptr<Job>> running;
        int nextIndex = 0, done = 0, failed = 0, added = 0;
        const auto total = queue.size();
        const auto startTime = juce::Time::getMillisecondCounter();
        auto lastPerfSample = startTime;
        bool throttled = false;

        auto finish = [&] (Job& job, const juce::String& failure)
        {
            ++done;
            const auto progress = "[" + juce::String (done) + "/" + juce::String (total) + "] ";
            const auto name = juce::File (job.path).getFileNameWithoutExtension();

            std::unique_ptr<juce::XmlElement> xml;
            if (failure.isEmpty())
                xml = juce::parseXML (job.outFile);

            if (xml == nullptr)
            {
                ++failed;
                list.addToBlacklist (job.path);
                log (progress + "FAILED  " + name + "  (" + (failure.isNotEmpty() ? failure : "no plugins found") + ")");
            }
            else
            {
                // Replace any stale entries for this file (shell plugins can contain many types).
                const auto bundle = bundleRootOf (job.path);

                for (auto& old : list.getTypes())
                    if (bundleRootOf (old.fileOrIdentifier) == bundle)
                        list.removeType (old);

                juce::StringArray names;

                for (auto* child : xml->getChildIterator())
                {
                    juce::PluginDescription desc;

                    if (desc.loadFromXml (*child))
                    {
                        list.addType (desc);
                        names.add (desc.name + (desc.isInstrument ? " (instrument)" : ""));
                        ++added;
                    }
                }

                log (progress + "OK      " + names.joinIntoString (", "));
            }

            job.outFile.deleteFile();
            saveList (list, cacheFile);
        };

        auto killAll = [&]
        {
            for (auto& job : running)
                job->process->kill();

            running.clear();
        };

        while (nextIndex < total || ! running.empty())
        {
            // Once a second: record resource usage, check for runaway process growth, decide on throttling.
            if (juce::Time::getMillisecondCounter() - lastPerfSample >= 1000)
            {
                lastPerfSample = juce::Time::getMillisecondCounter();
                const auto snapshot = perf.sample();
                perfLog.write (snapshot, { juce::String ((int) running.size()), juce::String (done), juce::String (total) });

                if (snapshot.descendantProcessCount > maxDescendants)
                {
                    log ("ABORTING: " + juce::String (snapshot.descendantProcessCount) + " child processes (limit "
                         + juce::String (maxDescendants) + "). Something is spawning processes unexpectedly.");
                    killAll();
                    saveList (list, cacheFile);
                    return exitRunawayAborted;
                }

                // Memory figures are 0 where PerformanceMonitor can't measure them (non-Windows)
                const auto lowMemory = snapshot.systemTotalBytes > 0
                                       && snapshot.systemAvailableBytes < juce::jmax ((juce::int64) 2 << 30,
                                                                                  snapshot.systemTotalBytes / 10);
                const auto busyCpu = snapshot.systemCpuPercent > 90.0;
                const auto shouldThrottle = lowMemory || busyCpu;

                if (shouldThrottle != throttled)
                {
                    throttled = shouldThrottle;
                    log (throttled ? "Throttling: " + snapshot.toSummary() : "Resuming full speed");
                }
            }

            const auto maxRunning = throttled ? 1 : options.jobs;

            while (nextIndex < total && (int) running.size() < maxRunning)
            {
                auto job = std::make_unique<Job>();
                job->path = queue[nextIndex];
                job->outFile = tempDir.getChildFile ("scan_" + juce::String (nextIndex) + ".xml");
                job->process = std::make_unique<juce::ChildProcess>();
                job->startedMs = juce::Time::getMillisecondCounter();
                ++nextIndex;

                if (job->process->start (juce::StringArray { selfExe, "--scan-one", job->path,
                                                             job->outFile.getFullPathName() }, 0))
                    running.push_back (std::move (job));
                else
                    finish (*job, "couldn't start scanner process");
            }

            for (auto it = running.begin(); it != running.end();)
            {
                auto& job = **it;

                if (! job.process->isRunning())
                {
                    const auto exitCode = (int) job.process->getExitCode();
                    finish (job, exitCode == exitOk          ? juce::String()
                               : exitCode == exitNoPlugins   ? juce::String ("no plugins found")
                                                             : "scanner exited with code " + juce::String (exitCode));
                    it = running.erase (it);
                }
                else if (juce::Time::getMillisecondCounter() - job.startedMs > (juce::uint32) options.timeoutSeconds * 1000)
                {
                    job.process->kill();
                    finish (job, "timed out after " + juce::String (options.timeoutSeconds) + " s");
                    it = running.erase (it);
                }
                else
                {
                    ++it;
                }
            }

            juce::Thread::sleep (50);
        }

        saveList (list, cacheFile);
        tempDir.deleteRecursively();

        int instruments = 0;
        for (auto& type : list.getTypes())
            if (type.isInstrument)
                ++instruments;

        log ("Done in " + juce::String ((juce::Time::getMillisecondCounter() - startTime) / 1000.0, 1) + " s: "
             + juce::String (added) + " plugins added, " + juce::String (failed) + " failed. Cache now has "
             + juce::String (list.getNumTypes()) + " plugins (" + juce::String (instruments) + " instruments).");

        if (failed > 0)
            log ("Failed plugins are skipped next time. Use --retry-failed to try them again.");

        return exitOk;
    }

    int run (const juce::StringArray& args)
    {
        juce::ScopedJuceInitialiser_GUI juceInit;

        if (args[0] == "--scan-one")
        {
            if (args.size() != 3)
                return exitUsage;

            return scanOne (args[1], juce::File (args[2]));
        }

        if (args[0] == "--scan")
            return scanAll (parseOptions (args));

        std::printf ("Usage:\n"
                     "  OrchestralDAWScanner --scan [--jobs N] [--timeout SECONDS] [--rescan-all] [--retry-failed] [--limit N]\n");
        return exitUsage;
    }
}

#if JUCE_WINDOWS
int wmain (int argc, wchar_t* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String (argv[i]));

    return run (args);
}
#else
int main (int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    return run (args);
}
#endif
