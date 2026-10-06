#pragma once

#include "../AudioEngine.h"

// The color palette for tracks and folders (ISSUES.md "Colored tracks"), and
// the "Color" submenu shared by the right-click menus. Colors are "#rrggbb"
// strings throughout (the VE Pro server's format). How regions use them
// (opacity, brightness) comes from Settings > Theming (ui/Theme.h).
namespace colours
{
    struct Entry
    {
        const char* name;
        const char* hex;
    };

    // Leans on the orchestral section colors (VSL's conventions) plus neutrals.
    inline const std::vector<Entry>& palette()
    {
        static const std::vector<Entry> entries {
            { "Red",        "#dd2c40" },
            { "Orange",     "#f1a56d" },
            { "Yellow",     "#eac13a" },
            { "Green",      "#56b58c" },
            { "Teal",       "#3aa6a6" },
            { "Light blue", "#9dedf0" },
            { "Blue",       "#4368f5" },
            { "Purple",     "#be98f8" },
            { "Pink",       "#e585c0" },
            { "Grey",       "#cdcbcc" },
            { "Brown",      "#a07850" },
            { "Slate",      "#3e688c" },

            // Pastels (the second column of the menu)
            { "Rose",       "#cd6f7a" },
            { "Peach",      "#d09665" },
            { "Apricot",    "#d3a96b" },
            { "Butter",     "#cfbc67" },
            { "Lime",       "#aec66c" },
            { "Mint",       "#86c19d" },
            { "Seafoam",    "#77bcb7" },
            { "Sky",        "#72a7cb" },
            { "Periwinkle", "#838ecd" },
            { "Lavender",   "#a78dcb" },
            { "Lilac",      "#c48ec0" },
            { "Blush",      "#cb96a9" },
            { "Sand",       "#bfab8a" },
            { "Sage",       "#9fb18e" },
            { "Stone",      "#aaafb7" },
        };

        return entries;
    }


    // A submenu of swatches; 'apply' receives the "#rrggbb" string ("" = none).
    inline juce::PopupMenu buildMenu (const juce::String& current, std::function<void (juce::String)> apply)
    {
        juce::PopupMenu menu;

        for (auto& entry : palette())
        {
            if (juce::String (entry.name) == "Rose")
                menu.addColumnBreak();   // vivid | pastel

            juce::PopupMenu::Item item (juce::String::fromUTF8 ("\xE2\x96\xA0 ") + entry.name);   // filled square
            item.colour = AudioEngine::colourFromHex (entry.hex, juce::Colours::white);
            item.isTicked = current == entry.hex;
            item.action = [apply, hex = juce::String (entry.hex)] { apply (hex); };
            menu.addItem (std::move (item));
        }

        menu.addSeparator();
        menu.addItem ("None", true, current.isEmpty(), [apply] { apply ({}); });
        return menu;
    }
}
