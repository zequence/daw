#pragma once

#include "MidiSequence.h"
#include "TempoMap.h"

// Meta-regions (DESIGN.md): a track's stream is visually divided into phrase blocks
// wherever there's enough silence. Blocks are computed, never stored - they can't
// desynchronize from the content. Notes define phrases; CC data rides along.
struct PhraseBlock
{
    juce::int64 startTick = 0, endTick = 0;
    int noteCount = 0;
};

inline std::vector<PhraseBlock> computePhraseBlocks (const MidiSequence& sequence, const TempoMap& map,
                                                     double gapBars = 1.0)
{
    std::vector<PhraseBlock> blocks;
    const auto& notes = sequence.getNotes();

    if (notes.empty())
    {
        // A controller-only stream (e.g. a CC pass) still deserves a visible block.
        if (! sequence.getControls().empty())
            blocks.push_back ({ sequence.getControls().front().tick, sequence.getLengthTicks(), 0 });

        return blocks;
    }

    PhraseBlock current { notes[0].startTick, notes[0].startTick + notes[0].lengthTicks, 1 };

    for (size_t i = 1; i < notes.size(); ++i)
    {
        const auto& note = notes[i];
        const auto gapTicks = (juce::int64) (gapBars * (double) map.getTicksPerBar (current.endTick));

        if (note.startTick - current.endTick >= gapTicks)
        {
            blocks.push_back (current);
            current = { note.startTick, note.startTick + note.lengthTicks, 1 };
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
