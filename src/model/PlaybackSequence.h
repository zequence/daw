#pragma once

#include "ArticulationMenu.h"
#include "MidiSequence.h"
#include "TempoMap.h"

// The PLAYBACK sequence of a track (MILESTONES.md "Articulation / expression maps" > Playback).
//
// The written sequence is what the user edits and saves; the audio thread plays a derived one:
//  - each note is shifted by its articulation's timing offset (milliseconds, converted to ticks at
//    the note's position; its length is kept), and
//  - the articulation's outputs (keyswitches, CCs, program changes, in series) are inserted exactly
//    at the note's own, shifted, tick - just before its note-on - whenever the articulation changes
//    from one note to the next.
// Every generated event records where it was WRITTEN (sourceTick), which the transport's pre-roll and
// the loop look-ahead need to decide what belongs to the part being played.
//
// A note whose articulation is missing from the map (or doesn't fit it) plays as having none: no
// switch, no shift. A note with none sends nothing: the instrument stays as it was.
namespace playback
{
    struct Result
    {
        MidiSequence::Ptr sequence;         // the written one itself when nothing needs generating
        double earliestOffsetMs = 0.0;      // the most negative timing offset any note uses (<= 0): the pre-roll it needs
    };

    constexpr double tappedKeyswitchMs = 30.0;

    // useFirstRootAsDefault: the editor setting - a note with no root counts as having the map's first one.
    inline Result build (const MidiSequence::Ptr& written, const ExpressionMap* map, const TempoMap& tempo,
                         bool useFirstRootAsDefault)
    {
        Result result;
        result.sequence = written;

        if (written == nullptr || map == nullptr || map->groups.empty())
            return result;

        const auto& source = written->getNotes();
        const auto defaultsApply = useFirstRootAsDefault && map->firstRoot() != nullptr;

        if (! defaultsApply && std::none_of (source.begin(), source.end(), [] (const auto& n) { return ! n.articulation.isEmpty(); }))
            return result;   // nothing to generate: play the written sequence as it is

        using Output = ExpressionMap::Output;

        const auto shifted = [&tempo] (juce::int64 tick, double ms)
        {
            if (ms == 0.0)
                return tick;

            return tempo.secondsToTicks (tempo.ticksToSeconds (tick) + ms * 0.001);
        };

        std::vector<MidiSequence::Note> notes;
        std::vector<MidiSequence::Control> controls = written->getControls();
        notes.reserve (source.size());

        struct HeldKeyswitch { size_t index; };
        std::vector<HeldKeyswitch> held;   // keyswitches that stay down until the articulation changes

        juce::String previousKey;
        juce::int64 lastEnd = 0;

        const auto closeHeld = [&] (juce::int64 atTick)
        {
            for (auto& h : held)
                notes[h.index].lengthTicks = juce::jmax ((juce::int64) 1, atTick - notes[h.index].startTick);

            held.clear();
        };

        for (const auto& original : source)
        {
            const auto selection = articulations::effective (*map, original.articulation, useFirstRootAsDefault);
            const auto resolved = ! selection.isEmpty() && articulations::resolves (*map, selection);

            std::vector<const ExpressionMap::Articulation*> chain;   // the root, then the modifiers in the map's group order
            double offsetMs = 0.0;
            juce::String key;

            if (resolved)
            {
                chain.push_back (ExpressionMap::findArticulation (map->groups.front(), selection.root));

                for (size_t g = 1; g < map->groups.size(); ++g)
                    for (auto& [groupName, articulationName] : selection.modifiers)
                        if (ExpressionMap::sameName (groupName, map->groups[g].name))
                            chain.push_back (ExpressionMap::findArticulation (map->groups[g], articulationName));

                for (auto* articulation : chain)
                {
                    offsetMs += articulation->timingOffsetMs;
                    key += articulation->name.toLowerCase() + "|";
                }
            }

            const auto writtenTick = original.startTick;
            const auto scheduled = shifted (writtenTick, offsetMs);
            result.earliestOffsetMs = juce::jmin (result.earliestOffsetMs, offsetMs);

            // The articulation changed: send its outputs, in order, at the note's own tick
            if (resolved && key != previousKey)
            {
                closeHeld (scheduled);

                for (auto* articulation : chain)
                    for (auto& output : articulation->outputs)
                    {
                        switch (output.type)
                        {
                            case Output::Type::keyswitch:
                            {
                                MidiSequence::Note keyswitch;
                                keyswitch.startTick = scheduled;
                                keyswitch.sourceTick = writtenTick;
                                keyswitch.isKeyswitch = true;
                                keyswitch.channel = original.channel;
                                keyswitch.key = output.number;
                                keyswitch.velocity = output.value;
                                keyswitch.lengthTicks = juce::jmax ((juce::int64) 1, shifted (scheduled, tappedKeyswitchMs) - scheduled);
                                notes.push_back (keyswitch);

                                if (output.held)
                                    held.push_back ({ notes.size() - 1 });

                                break;
                            }

                            case Output::Type::controller:
                                controls.push_back ({ scheduled, MidiSequence::ControlType::controller, original.channel,
                                                      output.number, output.value, writtenTick });
                                break;

                            case Output::Type::programChange:
                                if (output.bank >= 0)
                                {
                                    controls.push_back ({ scheduled, MidiSequence::ControlType::controller, original.channel, 0,
                                                          (output.bank >> 7) & 127, writtenTick });
                                    controls.push_back ({ scheduled, MidiSequence::ControlType::controller, original.channel, 32,
                                                          output.bank & 127, writtenTick });
                                }

                                controls.push_back ({ scheduled, MidiSequence::ControlType::programChange, original.channel,
                                                      0, output.number, writtenTick });
                                break;
                        }
                    }

                previousKey = key;
            }

            auto note = original;
            note.startTick = scheduled;
            note.sourceTick = writtenTick;
            notes.push_back (note);
            lastEnd = juce::jmax (lastEnd, note.startTick + note.lengthTicks);
        }

        closeHeld (lastEnd);   // a held keyswitch lasts as long as the articulation does

        result.sequence = MidiSequence::create (std::move (notes), std::move (controls), true);
        return result;
    }
}
