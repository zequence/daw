#pragma once

#include "ExpressionMap.h"

// The user's library of expression maps (MILESTONES.md "Articulation / expression maps" > phase 5):
// one XML file per map in a folder (the user data folder's Maps), so a map built once can be copied
// into any project, and files can be exchanged. A project always holds its own copies; using a
// library map copies it in, and changing the library later does not touch projects.
//
// Everything takes the folder as an argument (the engine owns which one), so tests use a temp folder.
namespace expressionMapLibrary
{
    inline juce::File fileFor (const juce::File& dir, const juce::String& mapName)
    {
        return dir.getChildFile (juce::File::createLegalFileName (mapName.trim()) + ".xml");
    }

    // Reads one file; empty optional + 'error' says why not (not XML, not one of our maps, invalid)
    inline std::optional<ExpressionMap> readFile (const juce::File& file, juce::String& error)
    {
        if (! file.existsAsFile())
        {
            error = "no such file: " + file.getFullPathName();
            return std::nullopt;
        }

        const auto xml = juce::XmlDocument::parse (file);

        if (xml == nullptr)
            return error = file.getFileName() + " is not an XML file", std::nullopt;

        if (! xml->hasTagName ("EXPRESSIONMAP"))
            return error = file.getFileName() + " is not one of this application's expression maps (its root is '"
                             + xml->getTagName() + "'; Cubase .expressionmap files are not supported yet)", std::nullopt;

        auto map = ExpressionMap::fromXml (*xml);

        if (const auto problems = map.validate(); ! problems.isEmpty())
            return error = file.getFileName() + " holds an invalid expression map: " + problems.joinIntoString ("; "), std::nullopt;

        return map;
    }

    // Built-in starting points, listed with the library but not stored in it
    inline std::vector<ExpressionMap> templates()
    {
        using Out = ExpressionMap::Output;

        ExpressionMap starter;
        starter.name = "Keyswitch starter (template)";
        starter.description = "A starting point for a library that switches articulations with keyswitches. "
                              "The keys are PLACEHOLDERS (C0 upwards): set them to your library's keyswitch keys.";

        ExpressionMap::Group root;
        root.name = "Articulation";
        int key = 24;

        for (const auto* name : { "Sustain", "Staccato", "Legato", "Pizzicato", "Tremolo", "Marcato" })
        {
            ExpressionMap::Articulation articulation;
            articulation.name = name;
            articulation.outputs.push_back ({ Out::Type::keyswitch, key++, 100, false, -1 });
            root.articulations.push_back (articulation);
        }

        starter.groups.push_back (root);
        starter.convertToSlots();   // one sound slot per articulation
        return { starter };
    }

    inline bool isTemplate (const juce::String& name)
    {
        for (auto& t : templates())
            if (ExpressionMap::sameName (t.name, name))
                return true;

        return false;
    }

    // The library's maps, by name (files that can't be read are skipped)
    inline std::vector<ExpressionMap> list (const juce::File& dir)
    {
        std::vector<ExpressionMap> maps;

        for (auto& file : dir.findChildFiles (juce::File::findFiles, false, "*.xml"))
        {
            juce::String ignored;

            if (auto map = readFile (file, ignored))
                maps.push_back (std::move (*map));
        }

        std::sort (maps.begin(), maps.end(), [] (const ExpressionMap& a, const ExpressionMap& b)
                   { return a.name.compareNatural (b.name, false) < 0; });
        return maps;
    }

    // A stored map or a template, by name (ignoring case)
    inline std::optional<ExpressionMap> find (const juce::File& dir, const juce::String& name)
    {
        for (auto& t : templates())
            if (ExpressionMap::sameName (t.name, name))
                return t;

        for (auto& map : list (dir))
            if (ExpressionMap::sameName (map.name, name))
                return map;

        return std::nullopt;
    }

    // Saves (or replaces) a map in the library. Returns an error sentence (empty = saved).
    inline juce::String save (const juce::File& dir, const ExpressionMap& map)
    {
        if (const auto problems = map.validate(); ! problems.isEmpty())
            return "'" + map.name + "' is not valid and was not saved: " + problems.joinIntoString ("; ");

        if (isTemplate (map.name))
            return "'" + map.name + "' is the name of a built-in template; pick another name";

        if (! dir.createDirectory())
            return "can't create the library folder " + dir.getFullPathName();

        // A map of this name may sit in a differently-named file (the name is what counts)
        for (auto& file : dir.findChildFiles (juce::File::findFiles, false, "*.xml"))
        {
            juce::String ignored;

            if (auto existing = readFile (file, ignored); existing.has_value() && ExpressionMap::sameName (existing->name, map.name))
                file.deleteFile();
        }

        if (! map.toXml()->writeTo (fileFor (dir, map.name)))
            return "can't write " + fileFor (dir, map.name).getFullPathName();

        return {};
    }

    inline juce::String remove (const juce::File& dir, const juce::String& name)
    {
        if (isTemplate (name))
            return "'" + name + "' is a built-in template and can't be deleted";

        auto removed = false;

        for (auto& file : dir.findChildFiles (juce::File::findFiles, false, "*.xml"))
        {
            juce::String ignored;

            if (auto existing = readFile (file, ignored); existing.has_value() && ExpressionMap::sameName (existing->name, name))
                removed = file.deleteFile() || removed;
        }

        if (! removed)
        {
            juce::StringArray names;

            for (auto& map : list (dir))
                names.add (map.name);

            return "no library map '" + name + "' (" + (names.isEmpty() ? juce::String ("the library is empty") : "existing: " + names.joinIntoString (", ")) + ")";
        }

        return {};
    }

    inline juce::String exportTo (const juce::File& file, const ExpressionMap& map)
    {
        if (const auto problems = map.validate(); ! problems.isEmpty())
            return "'" + map.name + "' is not valid and was not exported: " + problems.joinIntoString ("; ");

        if (! file.getParentDirectory().createDirectory() || ! map.toXml()->writeTo (file))
            return "can't write " + file.getFullPathName();

        return {};
    }
}
