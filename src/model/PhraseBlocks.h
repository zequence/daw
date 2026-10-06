#pragma once

#include "MidiSequence.h"
#include "TempoMap.h"

// Meta-regions (DESIGN.md): a track's stream is visually divided into phrase blocks
// wherever there's enough silence. Blocks are computed, never stored - they can't
// desynchronize from the content. Notes define phrases; CC data rides along.
// A block starts at the beginning of the bar its first note is in; two bars or
// more of silence separate blocks. Notes of different regions (Note::region: a
// moved or copied region keeps its own id until glued) never share a block - such
// blocks may touch or overlap; overlapping ones are stacked in layers.
struct PhraseBlock
{
    juce::int64 startTick = 0, endTick = 0;
    int noteCount = 0;
    int region = 0;           // the notes' Note::region
    int layer = 0, layers = 1;   // stacking among overlapping blocks (0 = top)
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

    // Each region on its own, divided by silence (the notes are sorted by start)
    std::map<int, PhraseBlock> open;

    for (auto& note : notes)
    {
        const auto end = note.startTick + note.lengthTicks;
        auto it = open.find (note.region);

        if (it != open.end())
        {
            auto& current = it->second;
            const auto gapTicks = (juce::int64) (gapBars * (double) map.getTicksPerBar (current.endTick));

            if (note.startTick - current.endTick < gapTicks)
            {
                current.endTick = std::max (current.endTick, end);
                ++current.noteCount;
                continue;
            }

            blocks.push_back (current);
        }

        open[note.region] = { barStart (note.startTick), end, 1, note.region };
    }

    for (auto& [region, block] : open)
        blocks.push_back (block);

    std::sort (blocks.begin(), blocks.end(), [] (const PhraseBlock& a, const PhraseBlock& b)
               { return a.startTick != b.startTick ? a.startTick < b.startTick : a.region < b.region; });

    // Overlapping blocks stack: each cluster of overlaps gets layers, the first free from the top
    for (size_t first = 0; first < blocks.size();)
    {
        auto clusterEnd = blocks[first].endTick;
        auto last = first + 1;

        while (last < blocks.size() && blocks[last].startTick < clusterEnd)
            clusterEnd = std::max (clusterEnd, blocks[last++].endTick);

        std::vector<juce::int64> layerEnds;

        for (auto i = first; i < last; ++i)
        {
            size_t layer = 0;

            while (layer < layerEnds.size() && layerEnds[layer] > blocks[i].startTick)
                ++layer;

            if (layer == layerEnds.size())
                layerEnds.push_back (0);

            layerEnds[layer] = blocks[i].endTick;
            blocks[i].layer = (int) layer;
        }

        for (auto i = first; i < last; ++i)
            blocks[i].layers = (int) layerEnds.size();

        first = last;
    }

    return blocks;
}
