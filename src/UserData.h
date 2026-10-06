#pragma once

#include <juce_core/juce_core.h>

// Per-user data shared by every build of the app (Debug, Release, installed).
// Default: %APPDATA%\DawPlus. Override with the DAWPLUS_DATA_DIR environment variable (the older
// ORCHESTRAL_DAW_DATA_DIR still works). The folder used to be %APPDATA%\OrchestralDAW: the first
// start after the rename moves it to the new place (settings, maps, themes, startup project, logs).
namespace UserData
{
    inline juce::File getDir()
    {
        auto overridePath = juce::SystemStats::getEnvironmentVariable ("DAWPLUS_DATA_DIR", {});

        if (overridePath.isEmpty())
            overridePath = juce::SystemStats::getEnvironmentVariable ("ORCHESTRAL_DAW_DATA_DIR", {});

        if (overridePath.isNotEmpty())
        {
            juce::File dir (overridePath);
            dir.createDirectory();
            return dir;
        }

        const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
        auto dir = appData.getChildFile ("DawPlus");

        // Once: the old folder moves to the new name (or is copied, if it can't be moved)
        if (const auto old = appData.getChildFile ("OrchestralDAW"); ! dir.exists() && old.isDirectory())
        {
            if (! old.moveFileTo (dir))
                old.copyDirectoryTo (dir);
        }

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
    inline juce::File getAppLog()           { return getLogsDir().getChildFile ("DawPlus.log"); }
    inline juce::File getAppPerfLog()       { return getLogsDir().getChildFile ("perf-app.csv"); }
    inline juce::File getScannerPerfLog()   { return getLogsDir().getChildFile ("perf-scanner.csv"); }
}
