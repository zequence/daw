#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <cstring>

#include "Transport.h"
#include "../model/MidiSequence.h"

// A MIDI-only graph node that plays one track's sequence, driven by the Transport's
// current Block. Responsibilities beyond rendering notes and controllers:
//
//  - note tracking: remembers which note-ons it has sent and guarantees matching
//    note-offs on stop, locate and loop wrap - no stuck notes
//  - controller chasing: when playback (re)starts mid-piece, sends the most recent
//    value of every controller, pitch bend and program change before the first note,
//    so modwheel dynamics and keyswitches are correct wherever you drop the playhead
//    (sounding notes are not restarted; only controllers are chased)
class MidiSourceProcessor final : public juce::AudioProcessor
{
public:
    explicit MidiSourceProcessor (const Transport& t) : transport (t)
    {
        activeNotes.reserve (128);
    }

    // Any thread; the audio thread picks the new sequence up at the next block.
    void setSequence (MidiSequence::Ptr s)       { sequence.store (std::move (s)); }
    MidiSequence::Ptr getSequence() const        { return sequence.load(); }

    // Release everything sounding at the next block.
    void requestKillAllNotes()                   { killAllRequest.store (true); }

    // While suppressed (replace-recording a take on this track), the sequencer plays
    // nothing from this source; live input through the routes is unaffected.
    void setSuppressed (bool shouldSuppress)     { suppressed.store (shouldSuppress); }

    //==============================================================================
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& midi) override
    {
        midi.clear();

        const auto& b = transport.getBlock();
        const auto seq = sequence.load();
        const auto isSuppressed = suppressed.load();

        if (b.killAtStart || killAllRequest.exchange (false) || (isSuppressed && ! activeNotes.empty()))
            emitAllNotesOff (midi, 0);

        if (! b.playing || b.numSegments == 0 || isSuppressed)
            return;

        if (b.chaseAtStart && seq != nullptr)
            chase (midi, *seq, b.segments[0].startTick, b.segments[0].offset);

        for (int i = 0; i < b.numSegments; ++i)
        {
            if (i > 0)   // crossing the loop point
            {
                emitAllNotesOff (midi, b.segments[i].offset);

                if (seq != nullptr)
                    chase (midi, *seq, b.segments[i].startTick, b.segments[i].offset);
            }

            if (seq != nullptr)
                renderSegment (midi, *seq, b, i);
        }
    }

    //==============================================================================
    const juce::String getName() const override              { return "MIDI Source"; }
    bool acceptsMidi() const override                        { return false; }
    bool producesMidi() const override                       { return true; }
    void prepareToPlay (double, int) override                {}
    void releaseResources() override                         {}
    double getTailLengthSeconds() const override             { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override      { return nullptr; }
    bool hasEditor() const override                          { return false; }
    int getNumPrograms() override                            { return 1; }
    int getCurrentProgram() override                         { return 0; }
    void setCurrentProgram (int) override                    {}
    const juce::String getProgramName (int) override         { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override   {}
    void setStateInformation (const void*, int) override     {}

private:
    struct ActiveNote { int channel, key; juce::int64 endTick; };

    void emitAllNotesOff (juce::MidiBuffer& midi, int offset)
    {
        for (const auto& note : activeNotes)
            midi.addEvent (juce::MidiMessage::noteOff (note.channel, note.key), offset);

        activeNotes.clear();

        for (int ch = 1; ch <= 16; ++ch)
        {
            if (sustainDown[ch - 1])
            {
                midi.addEvent (juce::MidiMessage::controllerEvent (ch, 64, 0), offset);
                sustainDown[ch - 1] = false;
            }
        }
    }

    void emitDueNoteOffs (juce::MidiBuffer& midi, const Transport::Block& b, int segmentIndex)
    {
        const auto& seg = b.segments[segmentIndex];

        for (auto it = activeNotes.begin(); it != activeNotes.end();)
        {
            if (it->endTick < seg.endTick)   // due in this segment (or overdue after an edit)
            {
                midi.addEvent (juce::MidiMessage::noteOff (it->channel, it->key),
                               b.offsetFor (juce::jmax (it->endTick, seg.startTick), segmentIndex));
                it = activeNotes.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    void renderSegment (juce::MidiBuffer& midi, const MidiSequence& seq, const Transport::Block& b, int segmentIndex)
    {
        const auto& seg = b.segments[segmentIndex];

        // Offs for notes started in earlier blocks come first, so a retriggered note
        // (off and on at the same sample) arrives in the right order.
        emitDueNoteOffs (midi, b, segmentIndex);

        const auto& controls = seq.getControls();

        for (auto it = std::lower_bound (controls.begin(), controls.end(), seg.startTick,
                                         [] (const MidiSequence::Control& c, juce::int64 t) { return c.tick < t; });
             it != controls.end() && it->tick < seg.endTick; ++it)
        {
            emitControl (midi, *it, b.offsetFor (it->tick, segmentIndex));
        }

        const auto& notes = seq.getNotes();

        for (auto it = std::lower_bound (notes.begin(), notes.end(), seg.startTick,
                                         [] (const MidiSequence::Note& n, juce::int64 t) { return n.startTick < t; });
             it != notes.end() && it->startTick < seg.endTick; ++it)
        {
            midi.addEvent (juce::MidiMessage::noteOn (it->channel, it->key, (juce::uint8) it->velocity),
                           b.offsetFor (it->startTick, segmentIndex));
            activeNotes.push_back ({ it->channel, it->key, it->startTick + it->lengthTicks });
        }

        // Notes short enough to start and end inside this same segment.
        emitDueNoteOffs (midi, b, segmentIndex);
    }

    void emitControl (juce::MidiBuffer& midi, const MidiSequence::Control& c, int offset)
    {
        using Type = MidiSequence::ControlType;

        switch (c.type)
        {
            case Type::controller:
                midi.addEvent (juce::MidiMessage::controllerEvent (c.channel, c.number, c.value), offset);
                if (c.number == 64)
                    sustainDown[c.channel - 1] = c.value >= 64;
                break;

            case Type::pitchBend:
                midi.addEvent (juce::MidiMessage::pitchWheel (c.channel, c.value), offset);
                break;

            case Type::programChange:
                midi.addEvent (juce::MidiMessage::programChange (c.channel, c.value), offset);
                break;
        }
    }

    void chase (juce::MidiBuffer& midi, const MidiSequence& seq, juce::int64 chaseTick, int offset)
    {
        std::memset (ccState, -1, sizeof (ccState));
        std::memset (bendState, -1, sizeof (bendState));
        std::memset (programState, -1, sizeof (programState));

        for (const auto& c : seq.getControls())
        {
            if (c.tick >= chaseTick)
                break;

            using Type = MidiSequence::ControlType;
            switch (c.type)
            {
                case Type::controller:    ccState[c.channel - 1][c.number] = (juce::int16) c.value; break;
                case Type::pitchBend:     bendState[c.channel - 1] = c.value; break;
                case Type::programChange: programState[c.channel - 1] = c.value; break;
            }
        }

        for (int ch = 1; ch <= 16; ++ch)
        {
            if (programState[ch - 1] >= 0)
                midi.addEvent (juce::MidiMessage::programChange (ch, programState[ch - 1]), offset);

            for (int cc = 0; cc < 128; ++cc)
            {
                if (ccState[ch - 1][cc] >= 0)
                {
                    midi.addEvent (juce::MidiMessage::controllerEvent (ch, cc, ccState[ch - 1][cc]), offset);
                    if (cc == 64)
                        sustainDown[ch - 1] = ccState[ch - 1][cc] >= 64;
                }
            }

            if (bendState[ch - 1] >= 0)
                midi.addEvent (juce::MidiMessage::pitchWheel (ch, bendState[ch - 1]), offset);
        }
    }

    const Transport& transport;
    std::atomic<MidiSequence::Ptr> sequence;
    std::atomic<bool> killAllRequest { false }, suppressed { false };

    std::vector<ActiveNote> activeNotes;
    bool sustainDown[16] = {};

    // Chase scratch space (kept as members to stay off the audio-thread stack)
    juce::int16 ccState[16][128];
    int bendState[16], programState[16];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiSourceProcessor)
};
