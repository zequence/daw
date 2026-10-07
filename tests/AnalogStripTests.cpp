#include "../src/engine/AnalogStrip.h"

// The console strip's processing (AnalogStrip.h): neutral is transparent, the EQ and filters do what
// their knobs say, the compressor reduces by its ratio, drive is clean at low levels
class AnalogStripTests final : public juce::UnitTest
{
public:
    AnalogStripTests() : UnitTest ("Analog strip") {}

    static constexpr double rate = 48000.0;

    // A sine through the strip; the output's RMS over the last half (after it has settled), in dB
    // relative to the input's
    static double gainDb (AnalogStrip& strip, double frequency, float amplitude = 0.25f, int seconds = 1)
    {
        strip.prepare (rate);
        const auto samples = (int) rate * seconds;
        juce::AudioBuffer<float> buffer (2, samples);

        for (int i = 0; i < samples; ++i)
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate));

        for (int start = 0; start < samples; start += 512)   // in blocks, as the device would
        {
            juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), 2, start, juce::jmin (512, samples - start));
            strip.process (block);
        }

        const auto rms = buffer.getRMSLevel (0, samples / 2, samples / 2);
        return juce::Decibels::gainToDecibels ((double) rms) - juce::Decibels::gainToDecibels (amplitude / std::sqrt (2.0));
    }

    void runTest() override
    {
        beginTest ("a neutral strip changes nothing");
        {
            AnalogStrip strip;
            expectWithinAbsoluteError (gainDb (strip, 1000.0), 0.0, 0.01);
            expectWithinAbsoluteError (gainDb (strip, 50.0), 0.0, 0.01);
        }

        beginTest ("a mid band boosts by its gain at its frequency");
        {
            AnalogStrip strip;
            strip.set (AnalogStrip::hmfGain, 12.0f);
            strip.set (AnalogStrip::hmfFreq, 2000.0f);
            expectWithinAbsoluteError (gainDb (strip, 2000.0), 12.0, 0.3);
            expect (gainDb (strip, 200.0) < 1.0, "far below the band: barely touched");
        }

        beginTest ("a shelf lifts above its corner; EQ IN off is flat again");
        {
            AnalogStrip strip;
            strip.set (AnalogStrip::hfGain, 9.0f);
            strip.set (AnalogStrip::hfFreq, 3000.0f);
            expectWithinAbsoluteError (gainDb (strip, 12000.0), 9.0, 1.0);
            expect (std::abs (gainDb (strip, 100.0)) < 0.3);

            strip.set (AnalogStrip::eqIn, 0.0f);
            expectWithinAbsoluteError (gainDb (strip, 12000.0), 0.0, 0.05);
        }

        beginTest ("the high-pass cuts 18 dB/oct below its frequency, the low-pass above");
        {
            AnalogStrip strip;
            strip.set (AnalogStrip::hpf, 120.0f);
            expect (gainDb (strip, 30.0) < -30.0, "two octaves below: over 30 dB down");
            expect (std::abs (gainDb (strip, 1000.0)) < 0.2, "the passband: untouched");

            AnalogStrip low;
            low.set (AnalogStrip::lpf, 4000.0f);
            expect (gainDb (low, 16000.0) < -20.0, "two octaves above: over 20 dB down (12 dB/oct)");
        }

        beginTest ("the compressor reduces by its ratio above the threshold, makeup adds back");
        {
            AnalogStrip strip;
            strip.set (AnalogStrip::threshold, -20.0f);
            strip.set (AnalogStrip::ratio, 4.0f);
            strip.set (AnalogStrip::attack, 1.0f);
            // A 0 dBFS-peak sine is 20 dB over: 4:1 leaves 5 of them - 15 dB of reduction (the detector
            // follows the peaks, so a little less on the RMS)
            const auto reduced = gainDb (strip, 200.0, 1.0f);
            expect (reduced < -11.0 && reduced > -16.0, "reduced by about 15 dB: " + juce::String (reduced, 2));
            expect (strip.getGainReduction() < -10.0f);

            strip.set (AnalogStrip::makeup, 10.0f);
            expectWithinAbsoluteError (gainDb (strip, 200.0, 1.0f), reduced + 10.0, 0.5);

            AnalogStrip quiet;   // below the threshold: nothing
            quiet.set (AnalogStrip::threshold, -20.0f);
            quiet.set (AnalogStrip::ratio, 4.0f);
            expect (std::abs (gainDb (quiet, 200.0, 0.01f)) < 0.1);
        }

        beginTest ("drive is clean and level at low levels, rounds the peaks when pushed");
        {
            AnalogStrip strip;
            strip.set (AnalogStrip::drive, 10.0f);
            expect (std::abs (gainDb (strip, 1000.0, 0.005f)) < 0.5, "quiet: the same level");
            expect (gainDb (strip, 1000.0, 0.8f) < -3.0, "loud: the peaks are rounded off");
        }
    }
};

static AnalogStripTests analogStripTests;
