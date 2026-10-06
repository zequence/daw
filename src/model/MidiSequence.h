#pragma once

#include <limits>
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
        std::stable_sort (controls.begin(), controls.end(),
                          [] (const Control& a, const Control& b) { return a.tick < b.tick; });

        for (auto& n : notes)    seq->lengthTicks = juce::jmax (seq->lengthTicks, n.startTick + n.lengthTicks);
        for (auto& c : controls) seq->lengthTicks = juce::jmax (seq->lengthTicks, c.tick + 1);

        seq->notes = std::move (notes);
        seq->controls = std::move (controls);
        return seq;
    }

    // Region cuts (the arrangement's phrase blocks): ticks where a region boundary is kept even
    // without silence - a region moved next to another stays separate until glued. A sequence
    // built by create() has its cuts UNSET: storing it on a track keeps the track's cuts.
    const std::vector<juce::int64>& getCuts() const noexcept { return cuts; }
    bool areCutsSet() const noexcept                         { return cutsSet; }

    Ptr withCuts (std::vector<juce::int64> newCuts) const
    {
        auto seq = std::shared_ptr<MidiSequence> (new MidiSequence (*this));
        std::sort (newCuts.begin(), newCuts.end());
        newCuts.erase (std::unique (newCuts.begin(), newCuts.end()), newCuts.end());
        std::erase_if (newCuts, [] (juce::int64 tick) { return tick <= 0; });
        seq->cuts = std::move (newCuts);
        seq->cutsSet = true;
        return seq;
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

        for (auto cut : cuts)
            xml->createNewChildElement ("CUT")->setAttribute ("tick", juce::String (cut));

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

        std::vector<juce::int64> cutTicks;

        for (auto* e : xml.getChildWithTagNameIterator ("CUT"))
            cutTicks.push_back (e->getStringAttribute ("tick").getLargeIntValue());

        return create (std::move (notes), std::move (controls))->withCuts (std::move (cutTicks));
    }

private:
    MidiSequence() = default;

    std::vector<Note> notes;
    std::vector<Control> controls;
    juce::int64 lengthTicks = 0;
    std::vector<juce::int64> cuts;
    bool cutsSet = false;

    JUCE_LEAK_DETECTOR (MidiSequence)
};
