#pragma once

#include "TempoMap.h"

// An immutable, sorted collection of MIDI notes and control events, in ticks.
// Like TempoMap: build one with create(), share it as a Ptr, read it from any thread.
class MidiSequence
{
public:
    using Ptr = std::shared_ptr<const MidiSequence>;

    struct Note
    {
        juce::int64 startTick = 0;
        juce::int64 lengthTicks = Ticks::perQuarterNote;
        int channel = 1;        // 1..16
        int key = 60;           // 0..127
        int velocity = 100;     // 1..127
    };

    enum class ControlType { controller, pitchBend, programChange };

    struct Control
    {
        juce::int64 tick = 0;
        ControlType type = ControlType::controller;
        int channel = 1;
        int number = 0;         // controller number; unused for bend/program
        int value = 0;          // 0..127, or 0..16383 for pitch bend
    };

    static Ptr create (std::vector<Note> notes, std::vector<Control> controls)
    {
        auto seq = std::shared_ptr<MidiSequence> (new MidiSequence());

        for (auto& n : notes)
        {
            n.startTick = juce::jmax ((juce::int64) 0, n.startTick);
            n.lengthTicks = juce::jmax ((juce::int64) 1, n.lengthTicks);   // zero-length notes break retrigger ordering
            n.channel = juce::jlimit (1, 16, n.channel);
            n.key = juce::jlimit (0, 127, n.key);
            n.velocity = juce::jlimit (1, 127, n.velocity);
        }

        for (auto& c : controls)
        {
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
            notes.push_back ({ e->getStringAttribute ("start").getLargeIntValue(),
                               e->getStringAttribute ("length").getLargeIntValue(),
                               e->getIntAttribute ("channel", 1),
                               e->getIntAttribute ("key", 60),
                               e->getIntAttribute ("velocity", 100) });

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
