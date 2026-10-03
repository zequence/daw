#pragma once

#include <juce_core/juce_core.h>

// Musical time is counted in integer ticks so positions never accumulate floating-point
// drift over the length of a piece (a design borrowed from Ardour's integer time model,
// implemented independently).
//
// 960'000 ticks per quarter note: an integer number of ticks for halves, thirds, fifths,
// sixths, dotted values and standard-MIDI 960 PPQ positions (x1000). Septuplets round by
// less than a tick, which is far below a microsecond.
namespace Ticks
{
    constexpr juce::int64 perQuarterNote = 960'000;
}

//==============================================================================
// The project's tempo and meter timeline.
//
// A TempoMap is immutable: edits return a new map and never touch the original. Holding a
// TempoMap::Ptr therefore makes it safe to read from any thread, including the audio thread -
// the engine just swaps in a new pointer when the map changes.
//
// Invariants (enforced on construction and after every edit):
//  - there is always a tempo change and a meter change at tick 0
//  - events are sorted by tick, with at most one event of each kind per tick
//  - every meter change sits exactly on a bar line
class TempoMap
{
public:
    using Ptr = std::shared_ptr<const TempoMap>;

    struct TempoChange
    {
        juce::int64 tick = 0;
        double bpm = 120.0;          // quarter notes per minute, constant until the next change
        double startSeconds = 0.0;   // cached wall-clock time of 'tick'
    };

    struct MeterChange
    {
        juce::int64 tick = 0;
        int numerator = 4;           // beats per bar
        int denominator = 4;         // beat note value; powers of two only
        int bar = 1;                 // cached 1-based bar number at 'tick'
    };

    static constexpr double minBpm = 1.0, maxBpm = 990.0;

    static Ptr create (double initialBpm = 120.0, int numerator = 4, int denominator = 4);

    //==============================================================================
    // Queries. All const and allocation-free; safe from any thread while you hold a Ptr.

    double getTempoAt (juce::int64 tick) const noexcept;
    const MeterChange& getMeterAt (juce::int64 tick) const noexcept;

    double ticksToSeconds (juce::int64 tick) const noexcept;
    juce::int64 secondsToTicks (double seconds) const noexcept;

    juce::int64 ticksToSamples (juce::int64 tick, double sampleRate) const noexcept;
    juce::int64 samplesToTicks (juce::int64 samples, double sampleRate) const noexcept;

    struct BarsBeats
    {
        int bar = 1;                     // 1-based
        int beat = 1;                    // 1-based, counts 'numerator' beats per bar
        juce::int64 tickInBeat = 0;

        juce::String toString() const;   // e.g. "5.3.120000"
    };

    BarsBeats ticksToBarsBeats (juce::int64 tick) const noexcept;
    juce::int64 barsBeatsToTicks (const BarsBeats&) const noexcept;

    juce::int64 getBarStart (juce::int64 tick) const noexcept;       // start of the bar containing tick
    juce::int64 getTicksPerBar (juce::int64 tick) const noexcept;
    juce::int64 getTicksPerBeat (juce::int64 tick) const noexcept;

    const std::vector<TempoChange>& getTempoChanges() const noexcept  { return tempos; }
    const std::vector<MeterChange>& getMeterChanges() const noexcept  { return meters; }

    //==============================================================================
    // Edits. Each returns a new map; 'this' is unchanged.

    Ptr withTempoChange (juce::int64 tick, double bpm) const;        // adds, or replaces one at the same tick
    Ptr withoutTempoChange (juce::int64 tick) const;                 // removing tick 0 resets it to the next tempo
    Ptr withMeterChange (juce::int64 tick, int numerator, int denominator) const;  // snaps to the containing bar
    Ptr withoutMeterChange (juce::int64 tick) const;

    //==============================================================================
    std::unique_ptr<juce::XmlElement> toXml() const;
    static Ptr fromXml (const juce::XmlElement&);   // tolerant: missing events fall back to defaults

private:
    TempoMap() = default;

    const TempoChange& tempoSegmentFor (juce::int64 tick) const noexcept;
    const TempoChange& tempoSegmentForSeconds (double seconds) const noexcept;
    void rebuild();   // sorts, dedupes, re-snaps meters to bar lines, refreshes caches

    std::vector<TempoChange> tempos;
    std::vector<MeterChange> meters;

    JUCE_LEAK_DETECTOR (TempoMap)
};
