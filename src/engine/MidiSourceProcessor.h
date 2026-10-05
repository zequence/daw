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
        liveIn.ensureSize (4096);
    }

    // Live MIDI (the graph's MIDI input, connected to EVERY source permanently)
    // passes through only while this track is armed. Arming therefore flips two
    // flags instead of changing graph connections - which on big projects meant
    // a full render-sequence rebuild (>1 s with 1000+ tracks) per selection.
    void setLiveEnabled (bool shouldPass)        { liveEnabled.store (shouldPass); }

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
        // 'midi' arrives holding the live input; keep it only while armed
        liveIn.clear();

        if (liveEnabled.load())
            liveIn.addEvents (midi, 0, -1, 0);

        renderSequence (midi);
        midi.addEvents (liveIn, 0, -1, 0);
    }

private:
    void renderSequence (juce::MidiBuffer& midi)
    {
        midi.clear();

        const auto& b = transport.getBlock();
        const auto seq = sequence.load();
        const auto isSuppressed = suppressed.load();

        // Notes begun early for the NEXT lap (the loop look-ahead) survive a loop wrap, and only that;
        // a stop, a locate or a kill-all ends them like any other note.
        const auto stopsEverything = killAllRequest.exchange (false) || (isSuppressed && ! activeNotes.empty())
                                       || (b.killAtStart && ! b.wrappedAtStart);

        if (stopsEverything)
            emitAllNotesOff (midi, 0, false);
        else if (b.killAtStart)
            emitAllNotesOff (midi, 0, true);

        if (! b.playing || b.numSegments == 0 || isSuppressed)
            return;

        // The loop was switched off (or moved) before the look-ahead notes got their lap: that lap will
        // not happen, so the notes belong to nothing and end now
        if (! b.hasAhead && ! b.wrappedAtStart)
        {
            for (auto it = activeNotes.begin(); it != activeNotes.end();)
            {
                if (it->ahead)
                {
                    midi.addEvent (juce::MidiMessage::noteOff (it->channel, it->key), 0);
                    it = activeNotes.erase (it);
                }
                else
                {
                    ++it;
                }
            }
        }

        if (b.chaseAtStart && seq != nullptr)
            chase (midi, *seq, b.segments[0].gateTick, b.segments[0].offset, b.segments[0].startTick);

        for (int i = 0; i < b.numSegments; ++i)
        {
            if (i > 0)   // crossing the loop point
            {
                emitAllNotesOff (midi, b.segments[i].offset, true);

                if (seq != nullptr)
                    chase (midi, *seq, b.segments[i].gateTick, b.segments[i].offset, b.segments[i].startTick);
            }

            if (seq != nullptr)
                renderSegment (midi, *seq, b, i);

            // The tail of the lap also plays the next lap's early events (after the segment's own
            // notes, so the ordering within a sample stays: offs, controls, ons)
            if (i == 0 && b.hasAhead && seq != nullptr)
                renderAhead (midi, *seq, b);
        }
    }

    //==============================================================================
public:
    const juce::String getName() const override              { return "MIDI Source"; }
    bool acceptsMidi() const override                        { return true; }    // live input, passed while armed
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
    // 'ahead': begun in the loop look-ahead for the next lap; endTick is in that lap's time
    struct ActiveNote { int channel, key; juce::int64 endTick; bool ahead = false; };

    // keepAhead: notes begun early for the next lap carry over a loop wrap (and become ordinary notes)
    void emitAllNotesOff (juce::MidiBuffer& midi, int offset, bool keepAhead)
    {
        std::vector<ActiveNote> carried;

        for (const auto& note : activeNotes)
        {
            if (keepAhead && note.ahead)
            {
                carried.push_back (note);
                carried.back().ahead = false;
            }
            else
            {
                midi.addEvent (juce::MidiMessage::noteOff (note.channel, note.key), offset);
            }
        }

        activeNotes = std::move (carried);

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
            if (it->ahead)   // ends in the next lap's time: renderAhead owns it until the wrap
            {
                ++it;
                continue;
            }

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
            if (plays (it->written(), it->tick, seg.gateTick, b.regionEndTick))
                emitControl (midi, *it, b.offsetFor (it->tick, segmentIndex));
        }

        const auto& notes = seq.getNotes();

        for (auto it = std::lower_bound (notes.begin(), notes.end(), seg.startTick,
                                         [] (const MidiSequence::Note& n, juce::int64 t) { return n.startTick < t; });
             it != notes.end() && it->startTick < seg.endTick; ++it)
        {
            if (! plays (it->written(), it->startTick, seg.gateTick, b.regionEndTick))
                continue;

            midi.addEvent (juce::MidiMessage::noteOn (it->channel, it->key, (juce::uint8) it->velocity),
                           b.offsetFor (it->startTick, segmentIndex));
            activeNotes.push_back ({ it->channel, it->key, it->startTick + it->lengthTicks });
        }

        // Notes short enough to start and end inside this same segment.
        emitDueNoteOffs (midi, b, segmentIndex);
    }

    // Does an event belong to the part being played? It was written before the region ends (the loop
    // end), and if it is scheduled before the gate (the pre-roll, or the time just before a lap) it was
    // written at or after the gate - an ordinary event that lies before the start stays silent.
    static bool plays (juce::int64 written, juce::int64 scheduled, juce::int64 gate, juce::int64 regionEnd) noexcept
    {
        return written < regionEnd && ! (scheduled < gate && written < gate);
    }

    // The loop look-ahead: the tail of a lap plays the next lap's early events (scheduled just before the
    // loop start, written inside the loop), at the same samples one loop length earlier.
    void renderAhead (juce::MidiBuffer& midi, const MidiSequence& seq, const Transport::Block& b)
    {
        const auto& ahead = b.ahead;

        // Offs for look-ahead notes begun in earlier blocks
        for (auto it = activeNotes.begin(); it != activeNotes.end();)
        {
            if (it->ahead && it->endTick < ahead.endTick)
            {
                midi.addEvent (juce::MidiMessage::noteOff (it->channel, it->key),
                               b.offsetFor (juce::jmax (it->endTick, ahead.startTick), ahead));
                it = activeNotes.erase (it);
            }
            else
            {
                ++it;
            }
        }

        const auto& controls = seq.getControls();

        for (auto it = std::lower_bound (controls.begin(), controls.end(), ahead.startTick,
                                         [] (const MidiSequence::Control& c, juce::int64 t) { return c.tick < t; });
             it != controls.end() && it->tick < ahead.endTick; ++it)
            if (plays (it->written(), it->tick, ahead.gateTick, b.regionEndTick))
                emitControl (midi, *it, b.offsetFor (it->tick, ahead));

        const auto& notes = seq.getNotes();

        for (auto it = std::lower_bound (notes.begin(), notes.end(), ahead.startTick,
                                         [] (const MidiSequence::Note& n, juce::int64 t) { return n.startTick < t; });
             it != notes.end() && it->startTick < ahead.endTick; ++it)
        {
            if (! plays (it->written(), it->startTick, ahead.gateTick, b.regionEndTick))
                continue;

            midi.addEvent (juce::MidiMessage::noteOn (it->channel, it->key, (juce::uint8) it->velocity),
                           b.offsetFor (it->startTick, ahead));
            activeNotes.push_back ({ it->channel, it->key, it->startTick + it->lengthTicks, true });
        }

        // A short look-ahead note can be over before the lap ends
        for (auto it = activeNotes.begin(); it != activeNotes.end();)
        {
            if (it->ahead && it->endTick < ahead.endTick)
            {
                midi.addEvent (juce::MidiMessage::noteOff (it->channel, it->key),
                               b.offsetFor (juce::jmax (it->endTick, ahead.startTick), ahead));
                it = activeNotes.erase (it);
            }
            else
            {
                ++it;
            }
        }
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

    void chase (juce::MidiBuffer& midi, const MidiSequence& seq, juce::int64 chaseTick, int offset, juce::int64 emitTick)
    {
        std::memset (ccState, -1, sizeof (ccState));
        std::memset (bendState, -1, sizeof (bendState));
        std::memset (programState, -1, sizeof (programState));

        // The state just before 'chaseTick': events both scheduled and written before it (an event written
        // later but scheduled earlier belongs to the pre-roll and is played, not chased)
        for (const auto& c : seq.getControls())
        {
            if (c.tick >= chaseTick)
                break;

            if (c.written() >= chaseTick)
                continue;

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

        chaseKeyswitch (midi, seq, chaseTick, offset, emitTick);
    }

    // A keyswitch is a note, so the controller chase above doesn't cover it: starting mid-piece
    // (or wrapping a loop) the articulation in effect is re-sent. That is the last group of
    // keyswitches written before 'chaseTick' (and scheduled before it - one scheduled later is
    // played in due course). A tapped one is replayed for its own length; a held one that is still
    // down at 'chaseTick' goes down again until its original end.
    void chaseKeyswitch (juce::MidiBuffer& midi, const MidiSequence& seq, juce::int64 chaseTick, int offset, juce::int64 emitTick)
    {
        constexpr auto none = std::numeric_limits<juce::int64>::min();
        auto lastWritten = none;

        for (const auto& n : seq.getNotes())
        {
            if (n.startTick >= chaseTick)
                break;

            if (n.isKeyswitch && n.written() < chaseTick)
                lastWritten = juce::jmax (lastWritten, n.written());
        }

        if (lastWritten == none)
            return;

        for (const auto& n : seq.getNotes())
        {
            if (n.startTick >= chaseTick)
                break;

            if (! n.isKeyswitch || n.written() != lastWritten)
                continue;

            const auto originalEnd = n.startTick + n.lengthTicks;
            midi.addEvent (juce::MidiMessage::noteOn (n.channel, n.key, (juce::uint8) n.velocity), offset);
            activeNotes.push_back ({ n.channel, n.key, originalEnd > chaseTick ? originalEnd : emitTick + n.lengthTicks });
        }
    }

    const Transport& transport;
    std::atomic<MidiSequence::Ptr> sequence;
    std::atomic<bool> killAllRequest { false }, suppressed { false }, liveEnabled { false };
    juce::MidiBuffer liveIn;   // audio-thread scratch for the passed-through live input

    std::vector<ActiveNote> activeNotes;
    bool sustainDown[16] = {};

    // Chase scratch space (kept as members to stay off the audio-thread stack)
    juce::int16 ccState[16][128];
    int bendState[16], programState[16];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiSourceProcessor)
};
