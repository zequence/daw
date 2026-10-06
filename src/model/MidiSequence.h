#pragma once

#include <limits>
#include <map>
#include "ExpressionMap.h"
#include "TempoMap.h"

// An immutable, sorted collection of MIDI notes and control events, in ticks.
// Like TempoMap: build one with create(), share it as a Ptr, read it from any thread.
class MidiSequence
{
public:
    using Ptr = std::shared_ptr<const MidiSequence>;

    static constexpr juce::int64 noSource = std::numeric_limits<juce::int64>::min();

    struct Note
    {
        juce::int64 startTick = 0;
        juce::int64 lengthTicks = Ticks::perQuarterNote;
        int channel = 1;        // 1..16
        int key = 60;           // 0..127
        int velocity = 100;     // 1..127
        ExpressionMap::Selection articulation;   // empty = none (see ExpressionMap.h)

        // Playback sequences only (MILESTONES.md "Timing offset"): where the event was WRITTEN,
        // while startTick is when it is SCHEDULED (shifted by an articulation's timing offset).
        // Decides whether it belongs to the part being played when the transport pre-rolls or
        // the loop wraps. noSource = not shifted: written == scheduled.
        juce::int64 sourceTick = noSource;

        juce::int64 written() const noexcept    { return sourceTick == noSource ? startTick : sourceTick; }

        // A keyswitch inserted by an articulation (playback sequences only). When playback starts or a loop
        // wraps mid-piece, the keyswitch of the articulation in effect is re-sent (the chase).
        bool isKeyswitch = false;

        // The arrangement region the note belongs to (PhraseBlocks.h). 0 = the track's ordinary
        // material, divided by silence. A moved or copied region gets its own id, so it stays a
        // region of its own - touching or overlapping others - until glued.
        int region = 0;
    };

    enum class ControlType { controller, pitchBend, programChange };

    struct Control
    {
        juce::int64 tick = 0;
        ControlType type = ControlType::controller;
        int channel = 1;
        int number = 0;         // controller number; unused for bend/program
        int value = 0;          // 0..127, or 0..16383 for pitch bend

        juce::int64 sourceTick = noSource;   // as for Note: where it was written (switch events inherit their note's)

        juce::int64 written() const noexcept    { return sourceTick == noSource ? tick : sourceTick; }
    };

    // allowNegativeTimes: a PLAYBACK sequence may hold events before tick 0 (shifted earlier than
    // the start of the piece); the written sequence never does.
    static Ptr create (std::vector<Note> notes, std::vector<Control> controls, bool allowNegativeTimes = false)
    {
        auto seq = std::shared_ptr<MidiSequence> (new MidiSequence());

        for (auto& n : notes)
        {
            if (! allowNegativeTimes)
                n.startTick = juce::jmax ((juce::int64) 0, n.startTick);

            n.lengthTicks = juce::jmax ((juce::int64) 1, n.lengthTicks);   // zero-length notes break retrigger ordering
            n.channel = juce::jlimit (1, 16, n.channel);
            n.key = juce::jlimit (0, 127, n.key);
            n.velocity = juce::jlimit (1, 127, n.velocity);
        }

        for (auto& c : controls)
        {
            if (! allowNegativeTimes)
                c.tick = juce::jmax ((juce::int64) 0, c.tick);

            c.channel = juce::jlimit (1, 16, c.channel);
            c.number = juce::jlimit (0, 127, c.number);
            c.value = juce::jlimit (0, c.type == ControlType::pitchBend ? 16383 : 127, c.value);
        }

        std::stable_sort (notes.begin(), notes.end(),
                          [] (const Note& a, const Note& b) { return a.startTick < b.startTick; });

        for (auto& n : notes)    seq->lengthTicks = juce::jmax (seq->lengthTicks, n.startTick + n.lengthTicks);
        for (auto& c : controls) seq->lengthTicks = juce::jmax (seq->lengthTicks, c.tick + 1);

        seq->notes = std::move (notes);
        seq->controls = std::move (controls);
        return seq;
    }

    // Notes being ADDED (drawn, recorded, entered) inside a region's span join that region;
    // the existing notes keep theirs (a moved region may overlap ordinary material)
    static void joinRegions (std::vector<Note>& added, const std::vector<Note>& existing)
    {
        std::map<int, std::pair<juce::int64, juce::int64>> spans;

        for (auto& n : existing)
            if (n.region != 0)
            {
                auto [it, fresh] = spans.try_emplace (n.region, n.startTick, n.startTick + n.lengthTicks);

                if (! fresh)
                {
                    it->second.first = juce::jmin (it->second.first, n.startTick);
                    it->second.second = juce::jmax (it->second.second, n.startTick + n.lengthTicks);
                }
            }

        for (auto& n : added)
            if (n.region == 0)
                for (auto& [region, span] : spans)
                    if (n.startTick >= span.first && n.startTick < span.second)
                    {
                        n.region = region;
                        break;
                    }
    }

    const std::vector<Note>& getNotes() const noexcept       { return notes; }
    const std::vector<Control>& getControls() const noexcept { return controls; }
    juce::int64 getLengthTicks() const noexcept              { return lengthTicks; }

    //==============================================================================
    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("SEQUENCE");

        for (auto& n : notes)
        {
            auto* e = xml->createNewChildElement ("NOTE");
            e->setAttribute ("start", juce::String (n.startTick));
            e->setAttribute ("length", juce::String (n.lengthTicks));
            e->setAttribute ("channel", n.channel);
            e->setAttribute ("key", n.key);
            e->setAttribute ("velocity", n.velocity);

            if (n.region != 0)
                e->setAttribute ("region", n.region);

            if (! n.articulation.isEmpty())
                e->addChildElement (n.articulation.toXml().release());
        }

        for (auto& c : controls)
        {
            auto* e = xml->createNewChildElement ("CONTROL");
            e->setAttribute ("tick", juce::String (c.tick));
            e->setAttribute ("type", (int) c.type);
            e->setAttribute ("channel", c.channel);
            e->setAttribute ("number", c.number);
            e->setAttribute ("value", c.value);
        }

        return xml;
    }

    static Ptr fromXml (const juce::XmlElement& xml)
    {
        std::vector<Note> notes;
        std::vector<Control> controls;

        for (auto* e : xml.getChildWithTagNameIterator ("NOTE"))
        {
            Note note { e->getStringAttribute ("start").getLargeIntValue(),
                        e->getStringAttribute ("length").getLargeIntValue(),
                        e->getIntAttribute ("channel", 1),
                        e->getIntAttribute ("key", 60),
                        e->getIntAttribute ("velocity", 100) };

            note.region = e->getIntAttribute ("region", 0);

            if (auto* articulation = e->getChildByName ("ARTICULATION"))   // absent in older projects
                note.articulation = ExpressionMap::Selection::fromXml (*articulation);

            notes.push_back (std::move (note));
        }

        for (auto* e : xml.getChildWithTagNameIterator ("CONTROL"))
            controls.push_back ({ e->getStringAttribute ("tick").getLargeIntValue(),
                                  (ControlType) juce::jlimit (0, 2, e->getIntAttribute ("type")),
                                  e->getIntAttribute ("channel", 1),
                                  e->getIntAttribute ("number"),
                                  e->getIntAttribute ("value") });

        return create (std::move (notes), std::move (controls));
    }

private:
    MidiSequence() = default;

    std::vector<Note> notes;
    std::vector<Control> controls;
    juce::int64 lengthTicks = 0;

    JUCE_LEAK_DETECTOR (MidiSequence)
};
