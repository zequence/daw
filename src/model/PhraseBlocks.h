#pragma once

#include "MidiSequence.h"
#include "TempoMap.h"

// Meta-regions (DESIGN.md): a track's stream is visually divided into phrase blocks
// wherever there's enough silence. Blocks are computed, never stored - they can't
// desynchronize from the content. Notes define phrases; CC data rides along.
// A block starts at the beginning of the bar its first note is in; two bars or
// more of silence separate blocks, and so does a cut (MidiSequence::getCuts) lying
// between one note's end and the next note's start - there the block starts at the cut.
struct PhraseBlock
{
    juce::int64 startTick = 0, endTick = 0;
    int noteCount = 0;
};

inline std::vector<PhraseBlock> computePhraseBlocks (const MidiSequence& sequence, const TempoMap& map,
                                                     double gapBars = 2.0)
{
    std::vector<PhraseBlock> blocks;
    const auto& notes = sequence.getNotes();

    if (notes.empty())
    {
        // A controller-only stream (e.g. a CC pass) still deserves a visible block.
        if (! sequence.getControls().empty())
            blocks.push_back ({ map.getBarStart (sequence.getControls().front().tick), sequence.getLengthTicks(), 0 });

        return blocks;
    }

    const auto barStart = [&map] (juce::int64 tick) { return map.getBarStart (juce::jmax ((juce::int64) 0, tick)); };
    const auto& cuts = sequence.getCuts();

    // A cut between the block so far and this note (the latest such cut)
    const auto cutBetween = [&cuts] (juce::int64 from, juce::int64 to) -> juce::int64
    {
        juce::int64 found = -1;

        for (auto cut : cuts)
            if (cut >= from && cut <= to)
                found = cut;

        return found;
    };

    // A block starts at its first note's bar - or at a cut inside that bar, before the note
    const auto blockStart = [&] (juce::int64 noteStart)
    {
        const auto bar = barStart (noteStart);
        return juce::jmax (bar, cutBetween (bar, noteStart));
    };

    PhraseBlock current { blockStart (notes[0].startTick), notes[0].startTick + notes[0].lengthTicks, 1 };

    for (size_t i = 1; i < notes.size(); ++i)
    {
        const auto& note = notes[i];
        const auto gapTicks = (juce::int64) (gapBars * (double) map.getTicksPerBar (current.endTick));

        if (const auto cut = cutBetween (current.endTick, note.startTick); cut >= 0)
        {
            blocks.push_back (current);
            current = { juce::jmax (cut, blockStart (note.startTick)), note.startTick + note.lengthTicks, 1 };
        }
        else if (note.startTick - current.endTick >= gapTicks)
        {
            blocks.push_back (current);
            current = { blockStart (note.startTick), note.startTick + note.lengthTicks, 1 };
        }
        else
        {
            current.endTick = std::max (current.endTick, note.startTick + note.lengthTicks);
            ++current.noteCount;
        }
    }

    blocks.push_back (current);
    return blocks;
}
