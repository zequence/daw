#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <cstdio>

#if JUCE_WINDOWS && JUCE_DEBUG
 #include <crtdbg.h>
 #include <windows.h>
#endif

#include "../src/model/TempoMap.h"

namespace
{
    constexpr auto Q = Ticks::perQuarterNote;
}

class TempoMapTests final : public juce::UnitTest
{
public:
    TempoMapTests() : UnitTest ("TempoMap") {}

    void runTest() override
    {
        beginTest ("default map: 120 bpm, 4/4");
        {
            auto map = TempoMap::create();

            expectEquals (map->ticksToSeconds (0), 0.0);
            expectEquals (map->ticksToSeconds (Q), 0.5);          // one quarter at 120 bpm
            expectEquals (map->ticksToSeconds (4 * Q), 2.0);      // one bar
            expectEquals (map->secondsToTicks (2.0), 4 * Q);
            expectEquals (map->getTempoAt (999 * Q), 120.0);
            expectEquals (map->getTicksPerBar (0), 4 * Q);
            expectEquals (map->getTicksPerBeat (0), Q);
        }

        beginTest ("tick <-> seconds round trip across tempo changes");
        {
            auto map = TempoMap::create (120.0)
                           ->withTempoChange (8 * Q, 60.0)        // bar 3: half speed
                           ->withTempoChange (16 * Q, 180.0);     // bar 5: faster

            // 8 quarters at 120 = 4 s, then 8 quarters at 60 = 8 s
            expectEquals (map->ticksToSeconds (8 * Q), 4.0);
            expectEquals (map->ticksToSeconds (16 * Q), 12.0);
            expectEquals (map->ticksToSeconds (19 * Q), 13.0);    // 3 quarters at 180 = 1 s

            juce::Random random (42);

            for (int i = 0; i < 1000; ++i)
            {
                const auto tick = (juce::int64) random.nextInt (1 << 30) * 977;   // up to ~10^12
                expectEquals (map->secondsToTicks (map->ticksToSeconds (tick)), tick);
            }
        }

        beginTest ("tick <-> samples round trip at 48 kHz and 44.1 kHz");
        {
            auto map = TempoMap::create (97.3)->withTempoChange (5 * Q, 140.0);
            juce::Random random (7);

            for (auto rate : { 48000.0, 44100.0 })
            {
                for (int i = 0; i < 500; ++i)
                {
                    const auto tick = (juce::int64) random.nextInt (1 << 30) * 31;
                    const auto samples = map->ticksToSamples (tick, rate);

                    // One sample spans ~30 ticks at these settings; round trip must stay within that.
                    expectWithinAbsoluteError ((double) map->samplesToTicks (samples, rate), (double) tick, 40.0);
                }
            }
        }

        beginTest ("bars and beats in 4/4 then 7/8");
        {
            auto map = TempoMap::create();
            map = map->withMeterChange (8 * Q, 7, 8);             // bar 3 becomes 7/8

            expectEquals (map->getMeterAt (8 * Q).numerator, 7);
            expectEquals (map->getTicksPerBar (8 * Q), 7 * Q / 2);
            expectEquals (map->getTicksPerBeat (8 * Q), Q / 2);   // beat = eighth note

            expect (bb (map, 0) == "1.1.0");
            expect (bb (map, 4 * Q) == "2.1.0");
            expect (bb (map, 8 * Q) == "3.1.0");
            expect (bb (map, 8 * Q + Q / 2) == "3.2.0");          // second eighth of the 7/8 bar
            expect (bb (map, 8 * Q + 7 * Q / 2) == "4.1.0");      // next bar after seven eighths
            expect (bb (map, 8 * Q + 3 * Q + 1234) == "3.7.1234");

            // Inverse direction
            for (auto tick : { (juce::int64) 0, 4 * Q, 8 * Q + Q / 2, 8 * Q + 7 * Q, 8 * Q + 12345 })
                expectEquals (map->barsBeatsToTicks (map->ticksToBarsBeats (tick)), tick);
        }

        beginTest ("meter changes snap to bar lines");
        {
            auto map = TempoMap::create();
            map = map->withMeterChange (6 * Q, 3, 4);             // middle of bar 2 -> snaps to its start

            expectEquals (map->getMeterChanges().back().tick, 4 * Q);
            expectEquals (map->getMeterChanges().back().bar, 2);

            // A later meter change stays bar-aligned on the new 3/4 grid
            map = map->withMeterChange (4 * Q + 7 * Q, 4, 4);     // mid-bar again -> start of that 3/4 bar
            expectEquals (map->getMeterChanges().back().tick, 4 * Q + 6 * Q);

            expectEquals (map->getBarStart (4 * Q + 8 * Q), 4 * Q + 6 * Q);       // within the first 4/4 bar
            expectEquals (map->getBarStart (4 * Q + 11 * Q), 4 * Q + 6 * Q + 4 * Q);
        }

        beginTest ("edits are immutable and removable");
        {
            auto original = TempoMap::create (120.0);
            auto edited = original->withTempoChange (4 * Q, 60.0);

            expectEquals ((int) original->getTempoChanges().size(), 1);
            expectEquals ((int) edited->getTempoChanges().size(), 2);
            expectEquals (original->ticksToSeconds (8 * Q), 4.0);
            expectEquals (edited->ticksToSeconds (8 * Q), 6.0);

            auto removed = edited->withoutTempoChange (4 * Q);
            expectEquals ((int) removed->getTempoChanges().size(), 1);
            expectEquals (removed->ticksToSeconds (8 * Q), 4.0);

            // Replacing the tempo at an existing tick keeps one event
            auto replaced = edited->withTempoChange (4 * Q, 90.0);
            expectEquals ((int) replaced->getTempoChanges().size(), 2);
            expectEquals (replaced->getTempoAt (5 * Q), 90.0);

            // Removing the event at tick 0 falls back to a sane map
            auto bare = edited->withoutTempoChange (0);
            expectEquals (bare->getTempoChanges().front().tick, (juce::int64) 0);
            expectEquals (bare->getTempoAt (0), 60.0);
        }

        beginTest ("time before the start continues backwards at the first tempo; bars, beats and bad input still clamp");
        {
            auto map = TempoMap::create();   // 120 bpm: a quarter note is 0.5 s

            // A pre-roll, or an event shifted earlier than bar 1, lives before tick 0
            expectWithinAbsoluteError (map->ticksToSeconds (-Q), -0.5, 1e-9);
            expectEquals (map->secondsToTicks (-2.0), -4 * Q);
            expectEquals (map->samplesToTicks (-3360, 48000.0), (juce::int64) -134400);   // 70 ms at 40 ticks per sample
            expectEquals (map->ticksToSamples (-134400, 48000.0), (juce::int64) -3360);

            // ...and it stays continuous across tick 0 and a tempo change later on
            auto changed = map->withTempoChange (4 * Q, 60.0);
            expectWithinAbsoluteError (changed->ticksToSeconds (-Q), -0.5, 1e-9);   // the FIRST tempo, not a later one
            for (const juce::int64 tick : std::initializer_list<juce::int64> { -3 * Q, -Q, -1000, 0, Q })
                expectEquals (changed->secondsToTicks (changed->ticksToSeconds (tick)), tick);

            expectEquals (map->getBarStart (-1), (juce::int64) 0);
            expectEquals (map->ticksToBarsBeats (-1).bar, 1);
            expectEquals (map->samplesToTicks (480, 0.0), (juce::int64) 0);   // no device yet

            auto clamped = map->withTempoChange (4 * Q, 10000.0);
            expectEquals (clamped->getTempoAt (4 * Q), TempoMap::maxBpm);
        }
    }

private:
    static juce::String bb (const TempoMap::Ptr& map, juce::int64 tick)
    {
        return map->ticksToBarsBeats (tick).toString();
    }
};

static TempoMapTests tempoMapTests;

//==============================================================================
#include "TestFlags.h"

int main (int argc, char* argv[])
{
   #if JUCE_WINDOWS && JUCE_DEBUG
    // Debug checks of the C runtime (asserts, "vector subscript out of range", abort()) report to
    // stderr instead of a modal dialog, and a crash ends the run instead of opening an error box:
    // test runs are unattended
    for (auto type : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT })
    {
        _CrtSetReportMode (type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile (type, _CRTDBG_FILE_STDERR);
    }

    _set_abort_behavior (0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
   #endif

    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--quiet")
            skipAudibleTests = true;

    juce::ScopedJuceInitialiser_GUI juceInit;   // the graph tests need a message manager

    struct ConsoleRunner final : juce::UnitTestRunner
    {
        void logMessage (const juce::String& message) override
        {
            std::printf ("%s\n", message.toRawUTF8());
            std::fflush (stdout);
        }
    };

    ConsoleRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;

    std::printf (failures == 0 ? "\nAll tests passed.\n" : "\n%d FAILURE(S).\n", failures);
    return failures == 0 ? 0 : 1;
}
