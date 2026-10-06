#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../model/MidiSequence.h"
#include <array>
#include <cmath>

// Controller lanes (Settings > Controller lanes): what the MIDI editor's lower pane can show -
// velocity, pitch bend, aftertouch and every CC. Each has a name (standard ones for the common
// CCs, all changeable) and is available or not; the available ones can be added to a track's
// lanes (right-click the lane pane). Lane ids: "velocity", "pitchBend", "aftertouch", "cc0".."cc127".
namespace lanes
{
    enum class Kind { velocity, pitchBend, aftertouch, controller };

    struct Lane
    {
        Kind kind = Kind::velocity;
        int cc = 0;   // controller lanes

        bool isValid() const noexcept { return kind != Kind::controller || (cc >= 0 && cc < 128); }
        int maxValue() const noexcept { return kind == Kind::pitchBend ? 16383 : 127; }

        // The control events this lane shows (none for velocity)
        bool shows (const MidiSequence::Control& c) const noexcept
        {
            switch (kind)
            {
                case Kind::pitchBend:  return c.type == MidiSequence::ControlType::pitchBend;
                case Kind::aftertouch: return c.type == MidiSequence::ControlType::aftertouch;
                case Kind::controller: return c.type == MidiSequence::ControlType::controller && c.number == cc;
                case Kind::velocity:   break;
            }

            return false;
        }

        int controlType() const noexcept   // for clip.setControlRange
        {
            return kind == Kind::pitchBend ? 1 : kind == Kind::aftertouch ? 3 : 0;
        }
    };

    inline Lane parse (const juce::String& id)
    {
        if (id == "pitchBend")  return { Kind::pitchBend };
        if (id == "aftertouch") return { Kind::aftertouch };
        if (id.startsWith ("cc") && id.substring (2).containsOnly ("0123456789") && id.length() > 2)
            return { Kind::controller, id.substring (2).getIntValue() };
        return { Kind::velocity };
    }

    // Every lane, in the order the settings list them
    inline const juce::StringArray& allIds()
    {
        static const auto ids = []
        {
            juce::StringArray list { "velocity", "pitchBend", "aftertouch" };

            for (int cc = 0; cc < 128; ++cc)
                list.add ("cc" + juce::String (cc));

            return list;
        }();

        return ids;
    }

    // The common names (MIDI 1.0); a CC without one has an empty name until named
    inline juce::String defaultName (const juce::String& id)
    {
        const auto lane = parse (id);

        if (lane.kind == Kind::velocity)   return "Velocity";
        if (lane.kind == Kind::pitchBend)  return "Pitch bend";
        if (lane.kind == Kind::aftertouch) return "Aftertouch";

        static const std::map<int, const char*> names {
            { 0, "Bank select" }, { 1, "Modulation" }, { 2, "Breath" }, { 4, "Foot" }, { 5, "Portamento time" },
            { 6, "Data entry" }, { 7, "Volume" }, { 8, "Balance" }, { 10, "Pan" }, { 11, "Expression" },
            { 12, "Effect 1" }, { 13, "Effect 2" }, { 32, "Bank select LSB" }, { 64, "Sustain" },
            { 65, "Portamento" }, { 66, "Sostenuto" }, { 67, "Soft pedal" }, { 68, "Legato" }, { 69, "Hold 2" },
            { 71, "Resonance" }, { 72, "Release time" }, { 73, "Attack time" }, { 74, "Brightness" },
            { 84, "Portamento control" }, { 91, "Reverb" }, { 92, "Tremolo" }, { 93, "Chorus" }, { 94, "Detune" },
            { 95, "Phaser" }, { 120, "All sound off" }, { 121, "Reset controllers" }, { 123, "All notes off" },
        };

        const auto it = names.find (lane.cc);
        return it != names.end() ? juce::String (it->second) : juce::String();
    }

    // Available on a fresh install (and a track's lanes before any choice)
    inline bool availableByDefault (const juce::String& id)
    {
        return id == "velocity" || id == "pitchBend" || id == "aftertouch" || id == "cc1" || id == "cc7"
                 || id == "cc11" || id == "cc64";
    }

    inline juce::StringArray defaultTrackLanes()   { return { "velocity", "cc1" }; }

    // Availability and names, stored in the settings file (only what differs from the defaults)
    class Settings
    {
    public:
        static Settings& get()
        {
            static Settings instance;
            return instance;
        }

        static constexpr auto settingsKey = "controllerLanes";

        void load (juce::PropertySet& settings)
        {
            file = &settings;
            available.clear();
            names.clear();
            const auto parsed = juce::JSON::parse (settings.getValue (settingsKey));

            if (auto* object = parsed.getDynamicObject())
                for (auto& property : object->getProperties())
                {
                    const auto id = property.name.toString();

                    if (! allIds().contains (id))
                        continue;

                    if (property.value.hasProperty ("available"))
                        available[id] = (bool) property.value["available"];

                    if (property.value.hasProperty ("name"))
                        names[id] = property.value["name"].toString();
                }
        }

        bool isAvailable (const juce::String& id) const
        {
            const auto it = available.find (id);
            return it != available.end() ? it->second : availableByDefault (id);
        }

        juce::String name (const juce::String& id) const
        {
            const auto it = names.find (id);
            return it != names.end() && it->second.isNotEmpty() ? it->second : defaultName (id);
        }

        // As the lanes show it: "CC7 Volume", "CC21" (no name yet), "Velocity"
        juce::String displayName (const juce::String& id) const
        {
            const auto lane = parse (id);

            if (lane.kind != Kind::controller)
                return name (id);

            const auto own = name (id);
            return "CC" + juce::String (lane.cc) + (own.isNotEmpty() ? " " + own : juce::String());
        }

        void setAvailable (const juce::String& id, bool shouldBe)
        {
            if (shouldBe == availableByDefault (id)) available.erase (id);
            else                                     available[id] = shouldBe;
            save();
        }

        void setName (const juce::String& id, const juce::String& newName)
        {
            if (newName.trim().isEmpty() || newName.trim() == defaultName (id)) names.erase (id);
            else                                                                 names[id] = newName.trim();
            save();
        }

        juce::StringArray availableIds() const
        {
            juce::StringArray ids;

            for (auto& id : allIds())
                if (isAvailable (id))
                    ids.add (id);

            return ids;
        }

        std::function<void()> onChanged;

    private:
        void save()
        {
            if (file != nullptr)
            {
                auto object = juce::DynamicObject::Ptr (new juce::DynamicObject());

                for (auto& id : allIds())
                {
                    const auto a = available.find (id);
                    const auto n = names.find (id);

                    if (a == available.end() && n == names.end())
                        continue;

                    auto entry = juce::DynamicObject::Ptr (new juce::DynamicObject());

                    if (a != available.end()) entry->setProperty ("available", a->second);
                    if (n != names.end())     entry->setProperty ("name", n->second);

                    object->setProperty (id, juce::var (entry.get()));
                }

                file->setValue (settingsKey, juce::JSON::toString (juce::var (object.get()), true));

                if (auto* properties = dynamic_cast<juce::PropertiesFile*> (file))
                    properties->saveIfNeeded();
            }

            if (onChanged)
                onChanged();
        }

        juce::PropertySet* file = nullptr;
        std::map<juce::String, bool> available;
        std::map<juce::String, juce::String> names;
    };

    // The colour of a value: velocity violet, dark (soft) -> cyan, light (loud), evenly, as the notes; controllers dark purple,
    // almost blue (low) -> fairly bright magenta (high)
    // How bright a colour looks (relative luminance of sRGB, 0..1): blue looks dark, cyan bright
    inline float perceivedLuminance (juce::Colour c)
    {
        const auto linear = [] (float v) { return v <= 0.04045f ? v / 12.92f : std::pow ((v + 0.055f) / 1.055f, 2.4f); };
        return 0.2126f * linear (c.getFloatRed()) + 0.7152f * linear (c.getFloatGreen()) + 0.0722f * linear (c.getFloatBlue());
    }

    // The hue and saturation, at the brightness that LOOKS like 'luminance' (as near as the hue allows)
    inline juce::Colour withLuminance (float hue, float saturation, float luminance)
    {
        float low = 0.0f, high = 1.0f;

        for (int i = 0; i < 20; ++i)
        {
            const auto mid = 0.5f * (low + high);
            (perceivedLuminance (juce::Colour::fromHSV (hue, saturation, mid, 1.0f)) < luminance ? low : high) = mid;
        }

        return juce::Colour::fromHSV (hue, saturation, high, 1.0f);
    }

    inline juce::Colour valueColour (Kind kind, float normalized)
    {
        normalized = juce::jlimit (0.0f, 1.0f, normalized);

        if (kind == Kind::velocity)
        {
            // Violet 262deg (soft) -> cyan with a hint of blue 186deg (loud; 180 looks greenish), the hue moving more near the top (cyans look
            // alike). The brightness is set by how bright each hue LOOKS, so the ramp goes evenly
            // from dark to light - purple doesn't outshine the blue after it, nor cyan jump out
            static const auto table = []
            {
                std::array<juce::Colour, 128> colours;

                for (int v = 0; v < 128; ++v)
                {
                    const auto n = (float) v / 127.0f;
                    const auto t = std::pow (n, 1.4f);
                    colours[(size_t) v] = withLuminance (0.728f - 0.211f * t, 0.62f - 0.1f * n, 0.07f + 0.43f * n);
                }

                return colours;
            }();

            return table[(size_t) juce::roundToInt (normalized * 127.0f)];
        }

        // dark purple, almost blue (250deg, dim) -> bright magenta (305deg), not too saturated
        return juce::Colour::fromHSV (0.695f + 0.152f * normalized, 0.62f - 0.07f * normalized,
                                      0.55f + 0.45f * normalized, 1.0f);
    }
}
