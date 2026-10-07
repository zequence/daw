#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <map>
#include "../UserData.h"

// Theming (MILESTONES.md "Theming", plan in THEMING.md).
//
// Every themable thing is a Token: either a colour or a number (a modifier such
// as an opacity). A colour token may name a parent token; while it is not
// overridden it follows the parent, so one change can retheme many items or just
// one. The built-in "Dark" theme is the registry's defaults (= the colours the
// app always had). Custom themes are files of overrides in the user's Themes
// folder; edits that are not saved yet are kept in a cache file and restored at
// the next start.
namespace theme
{
    // (name, id, group, label, parent, argb, hasAlpha)
    // (name, id, group, label, default, min, max)
    // A parent must be listed before the tokens that use it.
   #define THEME_TOKENS(C, N)                                                                                        \
    /* General: the window's surfaces, lines and selection - other settings follow these until changed */           \
    C (surfaceWindow,    "surface.window",    "General",  "Window background",         -1,                      0xff1d1f23, false) \
    C (surfaceContent,   "surface.content",   "General",  "Content area background",   -1,                      0xff1a1c1f, false) \
    C (surfacePanel,     "surface.panel",     "General",  "Panel background",          -1,                      0xff232529, false) \
    C (borderSubtle,     "border.subtle",     "General",  "Subtle border / grid line", -1,                      0xff2e3136, false) \
    C (selectionBg,      "selection.bg",      "General",  "Selection background (lists, menus)", -1,            0xff39404d, false) \
    C (selectionBorder,  "selection.border",  "General",  "Selection outline",         -1,                      0xff6c87b5, false) \
    /* The sidebar and its tracks */                                                                                  \
    C (sidebarBg,        "sidebar.bg",        "Sidebar",  "Sidebar background",        (int) Token::surfacePanel, 0xff232529, false) \
    C (folderBg,         "folder.bg",         "Tracks",   "Folder (instrument) background", -1,                 0xff2e3038, false) \
    C (trackMidiBg,      "track.midi.bg",     "Tracks",   "MIDI track background",     -1,                      0xff3a3e46, false) \
    C (trackAudioBg,     "track.audio.bg",    "Tracks",   "Audio track background",    -1,                      0xff45393b, false) \
    C (folderBorder,     "folder.border",     "Tracks",   "Folder border",             -1,                      0x00000000, true)  \
    C (channelBorder,    "channel.border",    "Tracks",   "Track border",              -1,                      0x00000000, true)  \
    N (rowSelectedBrightness, "row.selected.brightness", "Tracks", "Selected: brighter by (1 = white)",          0.16f, 0.0f, 1.0f) \
    N (rowSubselectedBrightness, "row.subselected.brightness", "Tracks", "Subselected (inside a selected folder or instrument): brighter by", 0.07f, 0.0f, 1.0f) \
    C (channelEdited,    "channel.edited",    "Tracks",   "Shown in the MIDI editor (right edge)", -1,           0xff3aa6c4, false) \
    C (trackArmOn,       "track.arm.on",      "Track buttons", "Record arm (armed)",   -1,                      0xffd50000, false) \
    C (trackSoloOn,      "track.solo.on",     "Track buttons", "Solo (soloed)",        -1,                      0xffdaa520, false) \
    C (trackMuteOn,      "track.mute.on",     "Track buttons", "Mute (muted)",         -1,                      0xffc47f00, false) \
    /* The arrangement and its regions */                                                                             \
    C (arrangeBg,        "arrange.bg",        "Arrangement", "Background",             (int) Token::surfaceContent, 0xff1a1c1f, false) \
    C (arrangeLaneEven,  "arrange.lane.even", "Arrangement", "Track lane (even)",      -1,                      0xff202327, false) \
    C (arrangeLaneOdd,   "arrange.lane.odd",  "Arrangement", "Track lane (odd)",       -1,                      0xff24272c, false) \
    C (arrangeLaneFolder,"arrange.lane.folder","Arrangement", "Folder lane",           -1,                      0xff1d2024, false) \
    C (arrangeBarline,   "arrange.barline",   "Arrangement", "Bar line",               (int) Token::borderSubtle, 0xff2e3136, false) \
    C (transportLine,    "transport.line",    "Arrangement", "Playhead",               -1,                      0xb3ffffff, true)  \
    C (arrangeMarkerLine,"arrange.markerline","Arrangement", "Marker line",            -1,                      0x59ffd700, true)  \
    C (arrangeGutterBg,  "arrange.gutter.bg", "Arrangement", "Name gutter background", (int) Token::surfaceWindow, 0xff1d1f23, false) \
    C (arrangeGutterBorder, "arrange.gutter.border", "Arrangement", "Name gutter border", (int) Token::borderSubtle, 0xff2e3136, false) \
    N (regionBgOpacity,       "region.bg.opacity",        "MIDI regions", "Background opacity",                   0.6f,  0.0f, 1.0f) \
    N (regionBgBrightness,    "region.bg.brightness",     "MIDI regions", "Background brightness",                1.2f,  0.3f, 2.0f) \
    N (regionBorderOpacity,   "region.border.opacity",    "MIDI regions", "Border opacity",                       1.0f,  0.0f, 1.0f) \
    N (regionBorderBrightness,"region.border.brightness", "MIDI regions", "Border brightness",                    1.0f,  0.3f, 2.0f) \
    N (regionBgSelectedOpacity,"region.bg.selopacity",    "MIDI regions", "Selected: background opacity",         0.95f, 0.0f, 1.0f) \
    N (regionBgSelectedBrightness,"region.bg.selbrightness","MIDI regions","Selected: background brightness",     1.2f,  0.3f, 2.0f) \
    N (regionBorderSelectedOpacity,"region.border.selopacity","MIDI regions","Selected: border opacity",          1.0f,  0.0f, 1.0f) \
    N (regionBorderSelectedBrightness,"region.border.selbrightness","MIDI regions","Selected: border brightness", 1.6f,  0.3f, 2.0f) \
    /* The transport bar */                                                                                           \
    C (transportPlayBg,  "transport.play.bg", "Transport", "Play",                       -1,                      0xff2b4634, false) \
    C (transportPlayOn,  "transport.play.on", "Transport", "Play (playing)",             -1,                      0xff006400, false) \
    C (transportRecordBg,"transport.record.bg","Transport","Record",                     -1,                      0xff4a2e2e, false) \
    C (transportRecordOn,"transport.record.on","Transport","Record (recording)",         -1,                      0xff8b0000, false) \
    C (transportLoopBg,  "transport.loop.bg", "Transport", "Loop",                       -1,                      0xff2e3b4a, false) \
    C (transportLoopOn,  "transport.loop.on", "Transport", "Loop (looping)",             -1,                      0xff4682b4, false) \
    C (transportRtzBg,   "transport.rtz.bg",  "Transport", "Return to start",            -1,                      0xff3a3e46, false) \
    /* Controls everywhere */                                                                                         \
    C (buttonBg,         "button.bg",         "Buttons",  "Button background",         -1,                      0xff263238, false) \
    C (buttonOn,         "button.on",         "Buttons",  "Button background (on / selected)", -1,              0xff181f22, false) \
    C (buttonAccentOn,   "button.accent.on",  "Buttons",  "Toggle button (on)",        -1,                      0xff4682b4, false) \
    C (buttonBorder,     "button.border",     "Buttons",  "Button and field border",   -1,                      0xff8e989b, false) \
    C (buttonText,       "button.text",       "Buttons",  "Button text",               -1,                      0xffffffff, false) \
    C (topbarButtonBg,   "topbar.button.bg",  "Buttons",  "Bar button (menu, views)",  (int) Token::buttonBg,   0xff263238, false) \
    C (topbarButtonOn,   "topbar.button.on",  "Buttons",  "Bar button (on / selected)", (int) Token::buttonOn,  0xff181f22, false) \
    C (menuBg,           "menu.bg",           "Menus",    "Menu background",           (int) Token::surfacePanel, 0xff232529, false) \
    C (menuText,         "menu.text",         "Menus",    "Menu text",                 -1,                      0xffe6e8eb, false) \
    C (menuHeaderText,   "menu.header.text",  "Menus",    "Section header text",       -1,                      0xff8e959e, false) \
    C (menuHighlightBg,  "menu.highlight.bg", "Menus",    "Highlighted item background", (int) Token::selectionBg, 0xff39404d, false) \
    C (menuHighlightText,"menu.highlight.text","Menus",   "Highlighted item text",     -1,                      0xffffffff, false) \
    C (menuBorder,       "menu.border",       "Menus",    "Menu border",               -1,                      0xff3a3e45, false)

    enum class Token : int
    {
       #define THEME_ENUM(name, ...) name,
        THEME_TOKENS (THEME_ENUM, THEME_ENUM)
       #undef THEME_ENUM
        count
    };

    constexpr int tokenCount = (int) Token::count;

    struct Def
    {
        const char* id;
        const char* group;
        const char* label;
        bool isNumber;
        int parent;           // colours only; -1 = none
        juce::uint32 argb;    // colours only
        bool hasAlpha;        // colours only: the picker offers alpha
        float number, min, max;
    };

    inline const std::array<Def, (size_t) tokenCount>& defs()
    {
        static const std::array<Def, (size_t) tokenCount> table {
           #define THEME_COLOUR(name, id, group, label, parent, argb, alpha) \
            Def { id, group, label, false, parent, (juce::uint32) argb, alpha, 0.0f, 0.0f, 0.0f },
           #define THEME_NUMBER(name, id, group, label, def, lo, hi) \
            Def { id, group, label, true, -1, 0, false, def, lo, hi },
            THEME_TOKENS (THEME_COLOUR, THEME_NUMBER)
           #undef THEME_COLOUR
           #undef THEME_NUMBER
        };

        return table;
    }

    inline const Def& def (Token t)   { return defs()[(size_t) t]; }

    inline juce::String hexOf (juce::Colour c, bool withAlpha)
    {
        return "#" + (withAlpha ? c.toString() : c.toDisplayString (false)).toLowerCase();
    }

    // "#rrggbb", "rrggbb", "#aarrggbb" ... Returns false when it is not a colour.
    inline bool parseHex (juce::String text, juce::Colour& out)
    {
        text = text.trim().trimCharactersAtStart ("#").toLowerCase();

        if ((text.length() != 6 && text.length() != 8) || ! text.containsOnly ("0123456789abcdef"))
            return false;

        out = juce::Colour ((juce::uint32) text.getHexValue64() | (text.length() == 6 ? 0xff000000u : 0u));
        return true;
    }

    //==========================================================================
    class Manager final : public juce::ChangeBroadcaster
    {
    public:
        static constexpr auto builtInName = "Dark";

        static Manager& get()
        {
            static Manager instance;
            return instance;
        }

        //----- reading (message thread; cheap enough for paint()) -----
        juce::Colour colour (Token t) const noexcept   { return colours[(size_t) t]; }
        float number (Token t) const noexcept          { return numbers[(size_t) t]; }
        int getRevision() const noexcept               { return revision; }

        bool isOverridden (Token t) const              { return overrides.count ((int) t) != 0; }
        juce::String getActiveName() const             { return activeName; }
        bool isModified() const noexcept               { return modified; }
        bool isBuiltIn (const juce::String& name) const { return name == builtInName; }

        juce::StringArray listThemes() const
        {
            juce::StringArray names { builtInName };
            juce::StringArray custom;

            for (auto& file : UserData::getThemesDir().findChildFiles (juce::File::findFiles, false, "*.xml"))
                if (auto xml = juce::XmlDocument::parse (file))
                    if (xml->hasTagName ("Theme"))
                        custom.add (xml->getStringAttribute ("name", file.getFileNameWithoutExtension()));

            custom.sortNatural();
            names.addArray (custom);
            return names;
        }

        //----- editing (works on the working copy) -----
        void setColour (Token t, juce::Colour c)
        {
            overrides[(int) t] = { c.getARGB(), 0.0f };
            changed (true);
        }

        void setNumber (Token t, float value)
        {
            const auto& d = def (t);
            overrides[(int) t] = { 0, juce::jlimit (d.min, d.max, value) };
            changed (true);
        }

        void reset (Token t)
        {
            if (overrides.erase ((int) t) > 0)
                changed (true);
        }

        //----- themes -----
        bool use (const juce::String& name)
        {
            std::map<int, Override> loaded;

            if (! isBuiltIn (name) && ! read (fileFor (name), loaded))
                return false;

            overrides = std::move (loaded);
            activeName = name;
            changed (false);
            return true;
        }

        // Throws away the unsaved edits.
        void revert()   { use (activeName); }

        // Saves the working copy under `name` (a new name makes a new theme).
        juce::String saveAs (const juce::String& name)
        {
            const auto clean = name.trim();

            if (clean.isEmpty())                      return "Give the theme a name";
            if (isBuiltIn (clean))                    return "'" + clean + "' is the built-in theme; pick another name";

            if (! write (fileFor (clean), clean))     return "Could not write " + fileFor (clean).getFullPathName();

            activeName = clean;
            changed (false);
            return {};
        }

        juce::String save()   { return saveAs (activeName); }

        // The built-in theme can't be deleted; deleting the active one falls back to it.
        bool remove (const juce::String& name)
        {
            if (isBuiltIn (name))
                return false;

            const auto deleted = fileFor (name).deleteFile();

            if (name == activeName)
                use (builtInName);
            else
                sendChangeMessage();

            return deleted;
        }

        // Restores the unsaved working copy from the last session (call once at start).
        void restoreSession()
        {
            std::map<int, Override> loaded;
            juce::String name = builtInName;
            auto isModified = false;

            if (auto xml = juce::XmlDocument::parse (UserData::getThemeWorkingCopyFile()))
                if (xml->hasTagName ("Working"))
                {
                    name = xml->getStringAttribute ("theme", builtInName);
                    isModified = xml->getBoolAttribute ("modified");
                    parse (*xml, loaded);
                }

            // A clean working copy is simply the saved theme (it may have been edited on disk).
            if (! isModified && ! isBuiltIn (name))
            {
                if (! read (fileFor (name), loaded))
                    name = builtInName;
            }

            overrides = std::move (loaded);
            activeName = name;
            modified = isModified;
            rebuild();
        }

    private:
        struct Override
        {
            juce::uint32 argb;
            float number;
        };

        Manager() { rebuild(); }

        static juce::File fileFor (const juce::String& name)
        {
            return UserData::getThemesDir().getChildFile (juce::File::createLegalFileName (name) + ".xml");
        }

        void rebuild()
        {
            for (int i = 0; i < tokenCount; ++i)
            {
                const auto& d = defs()[(size_t) i];
                const auto found = overrides.find (i);

                if (d.isNumber)
                {
                    numbers[(size_t) i] = found != overrides.end() ? found->second.number : d.number;
                }
                else if (found != overrides.end())
                {
                    colours[(size_t) i] = juce::Colour (found->second.argb);
                }
                else
                {
                    // Parents are listed before their children, so they are already resolved.
                    colours[(size_t) i] = d.parent >= 0 ? colours[(size_t) d.parent] : juce::Colour (d.argb);
                }
            }
        }

        void changed (bool isEdit)
        {
            modified = isEdit;
            ++revision;
            rebuild();
            writeSession();
            sendChangeMessage();
        }

        void fill (juce::XmlElement& xml) const
        {
            for (auto& [index, value] : overrides)
            {
                const auto& d = defs()[(size_t) index];
                auto* e = xml.createNewChildElement (d.isNumber ? "Number" : "Color");
                e->setAttribute ("token", d.id);
                e->setAttribute ("value", d.isNumber ? juce::String (value.number)
                                                     : juce::Colour (value.argb).toString());
            }
        }

        // Unknown tokens (a theme from a newer version) are ignored.
        static void parse (const juce::XmlElement& xml, std::map<int, Override>& into)
        {
            for (auto* e : xml.getChildIterator())
            {
                const auto id = e->getStringAttribute ("token");

                for (int i = 0; i < tokenCount; ++i)
                {
                    const auto& d = defs()[(size_t) i];

                    if (id != d.id || d.isNumber != e->hasTagName ("Number"))
                        continue;

                    if (d.isNumber)
                        into[i] = { 0, juce::jlimit (d.min, d.max, (float) e->getDoubleAttribute ("value")) };
                    else
                        into[i] = { (juce::uint32) e->getStringAttribute ("value").getHexValue64(), 0.0f };
                }
            }
        }

        static bool read (const juce::File& file, std::map<int, Override>& into)
        {
            if (auto xml = juce::XmlDocument::parse (file))
                if (xml->hasTagName ("Theme"))
                {
                    parse (*xml, into);
                    return true;
                }

            return false;
        }

        bool write (const juce::File& file, const juce::String& name) const
        {
            juce::XmlElement xml ("Theme");
            xml.setAttribute ("name", name);
            xml.setAttribute ("base", builtInName);
            xml.setAttribute ("version", 1);
            fill (xml);
            return xml.writeTo (file);
        }

        void writeSession() const
        {
            juce::XmlElement xml ("Working");
            xml.setAttribute ("theme", activeName);
            xml.setAttribute ("modified", modified);
            fill (xml);
            xml.writeTo (UserData::getThemeWorkingCopyFile());
        }

        std::map<int, Override> overrides;
        std::array<juce::Colour, (size_t) tokenCount> colours;
        std::array<float, (size_t) tokenCount> numbers {};
        juce::String activeName { builtInName };
        bool modified = false;
        int revision = 0;
    };

    inline juce::Colour colour (Token t)   { return Manager::get().colour (t); }
    inline float number (Token t)          { return Manager::get().number (t); }

    //==========================================================================
    // MIDI regions inherit the colour of their track (project data), so the
    // theme only holds modifiers: opacity and brightness (ISSUES.md "Colored
    // tracks"). Shared by the arrangement view and the settings preview.
    struct RegionStyle
    {
        juce::Colour fill, border;
    };

    inline RegionStyle regionStyle (juce::Colour trackColour, bool emphasised)
    {
        using T = Token;
        auto& m = Manager::get();

        // Selected regions have their own opacity and brightness for both parts.
        // The box is less colorful than the border (the track colour stays the hero).
        const auto fill = trackColour.withMultipliedSaturation (0.45f)
                                     .withMultipliedBrightness (m.number (emphasised ? T::regionBgSelectedBrightness
                                                                                     : T::regionBgBrightness))
                                     .withAlpha (m.number (emphasised ? T::regionBgSelectedOpacity
                                                                      : T::regionBgOpacity));

        const auto border = trackColour.withMultipliedBrightness (m.number (emphasised ? T::regionBorderSelectedBrightness
                                                                                      : T::regionBorderBrightness))
                                       .withAlpha (m.number (emphasised ? T::regionBorderSelectedOpacity
                                                                        : T::regionBorderOpacity));

        return { fill, border };
    }

    // Corners of buttons, rows, clips and other selectables: only ever so slightly rounded
    constexpr float corner = 2.0f;

    // The rounded box of a channel row or a folder row in the sidebar lists:
    // background, then the always-on border, then the selection outline.
    // A row's background when selected - brighter, towards white by the theme's amount - or subselected
    // (inside a selected folder or instrument: brighter, by less)
    inline juce::Colour rowColour (juce::Colour base, bool selected, bool subselected)
    {
        const auto amount = selected ? number (Token::rowSelectedBrightness) : subselected ? number (Token::rowSubselectedBrightness) : 0.0f;
        return base.interpolatedWith (juce::Colours::white, juce::jlimit (0.0f, 1.0f, amount));
    }

    // A track row's box in its kind's background (a Tracks token), brighter when (sub)selected
    inline void paintTrackBox (juce::Graphics& g, juce::Rectangle<float> bounds, Token background, bool selected, bool subselected = false)
    {
        g.setColour (rowColour (colour (background), selected, subselected));
        g.fillRoundedRectangle (bounds, corner);
        g.setColour (colour (background == Token::folderBg ? Token::folderBorder : Token::channelBorder));
        g.drawRoundedRectangle (bounds, corner, 1.0f);
    }

}
