#pragma once

#include <juce_core/juce_core.h>

// Per-user data shared by every build of the app (Debug, Release, installed).
// Default: %APPDATA%\OrchestralDAW. Override with the ORCHESTRAL_DAW_DATA_DIR environment variable.
namespace UserData
{
    inline juce::File getDir()
    {
        const auto overridePath = juce::SystemStats::getEnvironmentVariable ("ORCHESTRAL_DAW_DATA_DIR", {});

        auto dir = overridePath.isNotEmpty()
                       ? juce::File (overridePath)
                       : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("OrchestralDAW");

        dir.createDirectory();
        return dir;
    }

    inline juce::File getSettingsFile()     { return getDir().getChildFile ("Settings.xml"); }
    inline juce::File getPluginCacheFile()  { return getDir().getChildFile ("PluginCache.xml"); }
    inline juce::File getThemesDir()
    {
        auto dir = getDir().getChildFile ("Themes");
        dir.createDirectory();
        return dir;
    }

    // Unsaved theme edits survive a restart here (see ui/Theme.h)
    inline juce::File getThemeWorkingCopyFile()
    {
        auto dir = getDir().getChildFile ("Cache");
        dir.createDirectory();
        return dir.getChildFile ("ThemeWorkingCopy.xml");
    }

    inline juce::File getLogsDir()
    {
        auto dir = getDir().getChildFile ("Logs");
        dir.createDirectory();
        return dir;
    }

    inline juce::File getPluginScanLog()    { return getLogsDir().getChildFile ("PluginScan.log"); }
    inline juce::File getAppLog()           { return getLogsDir().getChildFile ("OrchestralDAW.log"); }
    inline juce::File getAppPerfLog()       { return getLogsDir().getChildFile ("perf-app.csv"); }
    inline juce::File getScannerPerfLog()   { return getLogsDir().getChildFile ("perf-scanner.csv"); }
}
