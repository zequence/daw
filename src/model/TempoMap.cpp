#include "TempoMap.h"

namespace
{
    double secondsPerTick (double bpm) noexcept
    {
        return 60.0 / (bpm * (double) Ticks::perQuarterNote);
    }

    bool isValidDenominator (int den) noexcept
    {
        return den >= 1 && den <= 64 && juce::isPowerOfTwo (den);
    }
}

//==============================================================================
TempoMap::Ptr TempoMap::create (double initialBpm, int numerator, int denominator)
{
    auto map = std::shared_ptr<TempoMap> (new TempoMap());
    map->tempos.push_back ({ 0, juce::jlimit (minBpm, maxBpm, initialBpm), 0.0 });
    map->meters.push_back ({ 0, juce::jmax (1, numerator), isValidDenominator (denominator) ? denominator : 4, 1 });
    map->rebuild();
    return map;
}

//==============================================================================
const TempoMap::TempoChange& TempoMap::tempoSegmentFor (juce::int64 tick) const noexcept
{
    auto it = std::upper_bound (tempos.begin(), tempos.end(), tick,
                                [] (juce::int64 t, const TempoChange& c) { return t < c.tick; });
    return *std::prev (it == tempos.begin() ? std::next (it) : it);
}

const TempoMap::TempoChange& TempoMap::tempoSegmentForSeconds (double seconds) const noexcept
{
    auto it = std::upper_bound (tempos.begin(), tempos.end(), seconds,
                                [] (double s, const TempoChange& c) { return s < c.startSeconds; });
    return *std::prev (it == tempos.begin() ? std::next (it) : it);
}

double TempoMap::getTempoAt (juce::int64 tick) const noexcept
{
    return tempoSegmentFor (tick).bpm;
}

const TempoMap::MeterChange& TempoMap::getMeterAt (juce::int64 tick) const noexcept
{
    auto it = std::upper_bound (meters.begin(), meters.end(), tick,
                                [] (juce::int64 t, const MeterChange& c) { return t < c.tick; });
    return *std::prev (it == meters.begin() ? std::next (it) : it);
}

//==============================================================================
double TempoMap::ticksToSeconds (juce::int64 tick) const noexcept
{
    tick = juce::jmax ((juce::int64) 0, tick);
    const auto& seg = tempoSegmentFor (tick);
    return seg.startSeconds + (double) (tick - seg.tick) * secondsPerTick (seg.bpm);
}

juce::int64 TempoMap::secondsToTicks (double seconds) const noexcept
{
    seconds = juce::jmax (0.0, seconds);
    const auto& seg = tempoSegmentForSeconds (seconds);
    return seg.tick + juce::int64 (std::llround ((seconds - seg.startSeconds) / secondsPerTick (seg.bpm)));
}

juce::int64 TempoMap::ticksToSamples (juce::int64 tick, double sampleRate) const noexcept
{
    return juce::int64 (std::llround (ticksToSeconds (tick) * sampleRate));
}

juce::int64 TempoMap::samplesToTicks (juce::int64 samples, double sampleRate) const noexcept
{
    return sampleRate > 0 ? secondsToTicks ((double) samples / sampleRate) : 0;
}

//==============================================================================
juce::int64 TempoMap::getTicksPerBeat (juce::int64 tick) const noexcept
{
    return Ticks::perQuarterNote * 4 / getMeterAt (tick).denominator;
}

juce::int64 TempoMap::getTicksPerBar (juce::int64 tick) const noexcept
{
    const auto& meter = getMeterAt (tick);
    return (juce::int64) meter.numerator * (Ticks::perQuarterNote * 4 / meter.denominator);
}

juce::int64 TempoMap::getBarStart (juce::int64 tick) const noexcept
{
    tick = juce::jmax ((juce::int64) 0, tick);
    const auto& meter = getMeterAt (tick);
    const auto ticksPerBar = (juce::int64) meter.numerator * (Ticks::perQuarterNote * 4 / meter.denominator);
    return meter.tick + ((tick - meter.tick) / ticksPerBar) * ticksPerBar;
}

TempoMap::BarsBeats TempoMap::ticksToBarsBeats (juce::int64 tick) const noexcept
{
    tick = juce::jmax ((juce::int64) 0, tick);
    const auto& meter = getMeterAt (tick);
    const auto ticksPerBeat = Ticks::perQuarterNote * 4 / meter.denominator;
    const auto ticksPerBar = (juce::int64) meter.numerator * ticksPerBeat;

    const auto fromMeter = tick - meter.tick;
    const auto inBar = fromMeter % ticksPerBar;

    return { meter.bar + (int) (fromMeter / ticksPerBar),
             (int) (inBar / ticksPerBeat) + 1,
             inBar % ticksPerBeat };
}

juce::int64 TempoMap::barsBeatsToTicks (const BarsBeats& position) const noexcept
{
    const auto bar = juce::jmax (1, position.bar);

    // Find the last meter change at or before the requested bar.
    auto it = std::upper_bound (meters.begin(), meters.end(), bar,
                                [] (int b, const MeterChange& c) { return b < c.bar; });
    const auto& meter = *std::prev (it == meters.begin() ? std::next (it) : it);

    const auto ticksPerBeat = Ticks::perQuarterNote * 4 / meter.denominator;
    const auto ticksPerBar = (juce::int64) meter.numerator * ticksPerBeat;

    return meter.tick + (juce::int64) (bar - meter.bar) * ticksPerBar
                      + (juce::int64) (juce::jmax (1, position.beat) - 1) * ticksPerBeat
                      + juce::jmax ((juce::int64) 0, position.tickInBeat);
}

juce::String TempoMap::BarsBeats::toString() const
{
    return juce::String (bar) + "." + juce::String (beat) + "." + juce::String (tickInBeat);
}

//==============================================================================
TempoMap::Ptr TempoMap::withTempoChange (juce::int64 tick, double bpm) const
{
    auto map = std::shared_ptr<TempoMap> (new TempoMap (*this));
    map->tempos.push_back ({ juce::jmax ((juce::int64) 0, tick), juce::jlimit (minBpm, maxBpm, bpm), 0.0 });
    map->rebuild();
    return map;
}

TempoMap::Ptr TempoMap::withoutTempoChange (juce::int64 tick) const
{
    auto map = std::shared_ptr<TempoMap> (new TempoMap (*this));
    std::erase_if (map->tempos, [tick] (const TempoChange& c) { return c.tick == tick; });
    map->rebuild();
    return map;
}

TempoMap::Ptr TempoMap::withMeterChange (juce::int64 tick, int numerator, int denominator) const
{
    auto map = std::shared_ptr<TempoMap> (new TempoMap (*this));
    map->meters.push_back ({ getBarStart (tick), juce::jlimit (1, 99, numerator),
                             isValidDenominator (denominator) ? denominator : 4, 0 });
    map->rebuild();
    return map;
}

TempoMap::Ptr TempoMap::withoutMeterChange (juce::int64 tick) const
{
    auto map = std::shared_ptr<TempoMap> (new TempoMap (*this));
    std::erase_if (map->meters, [tick] (const MeterChange& c) { return c.tick == tick; });
    map->rebuild();
    return map;
}

//==============================================================================
std::unique_ptr<juce::XmlElement> TempoMap::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("TEMPOMAP");

    for (auto& tempo : tempos)
    {
        auto* e = xml->createNewChildElement ("TEMPO");
        e->setAttribute ("tick", juce::String (tempo.tick));
        e->setAttribute ("bpm", tempo.bpm);
    }

    for (auto& meter : meters)
    {
        auto* e = xml->createNewChildElement ("METER");
        e->setAttribute ("tick", juce::String (meter.tick));
        e->setAttribute ("numerator", meter.numerator);
        e->setAttribute ("denominator", meter.denominator);
    }

    return xml;
}

TempoMap::Ptr TempoMap::fromXml (const juce::XmlElement& xml)
{
    auto map = std::shared_ptr<TempoMap> (new TempoMap());

    for (auto* e : xml.getChildWithTagNameIterator ("TEMPO"))
        map->tempos.push_back ({ e->getStringAttribute ("tick").getLargeIntValue(),
                                 juce::jlimit (minBpm, maxBpm, e->getDoubleAttribute ("bpm", 120.0)), 0.0 });

    for (auto* e : xml.getChildWithTagNameIterator ("METER"))
        map->meters.push_back ({ e->getStringAttribute ("tick").getLargeIntValue(),
                                 juce::jlimit (1, 99, e->getIntAttribute ("numerator", 4)),
                                 isValidDenominator (e->getIntAttribute ("denominator", 4))
                                     ? e->getIntAttribute ("denominator", 4) : 4,
                                 0 });

    map->rebuild();
    return map;
}

//==============================================================================
void TempoMap::rebuild()
{
    // --- tempos: sort, keep the later addition where ticks collide, ensure an event at 0 ---
    std::stable_sort (tempos.begin(), tempos.end(),
                      [] (const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });

    for (auto it = tempos.begin(); it != tempos.end();)
    {
        auto next = std::next (it);
        if (next != tempos.end() && next->tick == it->tick) it = tempos.erase (it);
        else                                                ++it;
    }

    if (tempos.empty())
        tempos.push_back ({ 0, 120.0, 0.0 });

    tempos.front().tick = 0;
    tempos.front().startSeconds = 0.0;

    for (size_t i = 1; i < tempos.size(); ++i)
        tempos[i].startSeconds = tempos[i - 1].startSeconds
                                   + (double) (tempos[i].tick - tempos[i - 1].tick)
                                       * secondsPerTick (tempos[i - 1].bpm);

    // --- meters: sort, dedupe, snap each change onto the bar grid of the previous one ---
    std::stable_sort (meters.begin(), meters.end(),
                      [] (const MeterChange& a, const MeterChange& b) { return a.tick < b.tick; });

    for (auto it = meters.begin(); it != meters.end();)
    {
        auto next = std::next (it);
        if (next != meters.end() && next->tick == it->tick) it = meters.erase (it);
        else                                                ++it;
    }

    if (meters.empty())
        meters.push_back ({ 0, 4, 4, 1 });

    meters.front().tick = 0;
    meters.front().bar = 1;

    for (size_t i = 1; i < meters.size();)
    {
        auto& prev = meters[i - 1];
        const auto ticksPerBar = (juce::int64) prev.numerator * (Ticks::perQuarterNote * 4 / prev.denominator);
        const auto bars = juce::jmax ((juce::int64) 1,
                                      (meters[i].tick - prev.tick + ticksPerBar / 2) / ticksPerBar);

        meters[i].tick = prev.tick + bars * ticksPerBar;
        meters[i].bar = prev.bar + (int) bars;

        // Snapping can push an event onto its successor; keep the later one.
        if (i + 1 < meters.size() && meters[i + 1].tick <= meters[i].tick)
            meters.erase (meters.begin() + (long) i);
        else
            ++i;
    }
}
