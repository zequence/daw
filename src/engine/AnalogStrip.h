#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>

// The mixer strip's own processing (MILESTONES.md "Audio mixer", phase 4), after the SSL 4000 channel:
//
//   high-pass (18 dB/oct) -> low-pass (12 dB/oct) -> EQ (LF, LMF, HMF, HF) -> dynamics -> drive
//
// Written from scratch for an "analogue" character:
//  - The shelves have the small bump past the corner frequency that passive / op-amp shelves have
//    (a resonant second-order shelf, not the flat digital one); BELL turns HF and LF into bells.
//  - The mid bands have proportional Q (as on the SSL G): the more boost or cut, the narrower the bell.
//  - The compressor is a feed-forward VCA design with a soft knee, stereo-linked, smoothing in the
//    dB domain (attack and release behave like the console's).
//  - Drive pushes a soft, slightly asymmetric clipper (odd and some even harmonics, like a console
//    channel driven hot); level-compensated, so at low levels it stays clean and the same loudness.
//
// Parameters are atomics set from the message thread; the audio thread smooths them (no zipper
// noise) and recomputes the filters every few samples. EQ IN and DYN IN crossfade. At its neutral
// settings each part is skipped, so a flat strip costs almost nothing and changes nothing.
class AnalogStrip
{
public:
    enum Param
    {
        hpf, lpf,
        hfGain, hfFreq, hfBell,
        hmfGain, hmfFreq, hmfQ,
        lmfGain, lmfFreq, lmfQ,
        lfGain, lfFreq, lfBell,
        eqIn,
        threshold, ratio, attack, release, makeup, dynIn,
        drive,
        numParams
    };

    struct Info { const char* name; float min, max, initial; };

    // Names (for the project file), ranges and defaults - the mixer's knobs use the same
    static const Info& info (int p)
    {
        static const std::array<Info, numParams> table {{
            { "hpf", 16.0f, 350.0f, 16.0f },         { "lpf", 3000.0f, 22000.0f, 22000.0f },
            { "hfGain", -15.0f, 15.0f, 0.0f },       { "hfFreq", 1500.0f, 16000.0f, 8000.0f },   { "hfBell", 0.0f, 1.0f, 0.0f },
            { "hmfGain", -15.0f, 15.0f, 0.0f },      { "hmfFreq", 600.0f, 7000.0f, 2000.0f },    { "hmfQ", 0.5f, 3.0f, 1.0f },
            { "lmfGain", -15.0f, 15.0f, 0.0f },      { "lmfFreq", 200.0f, 2000.0f, 600.0f },     { "lmfQ", 0.5f, 3.0f, 1.0f },
            { "lfGain", -15.0f, 15.0f, 0.0f },       { "lfFreq", 30.0f, 450.0f, 100.0f },        { "lfBell", 0.0f, 1.0f, 0.0f },
            { "eqIn", 0.0f, 1.0f, 1.0f },
            { "threshold", -40.0f, 0.0f, 0.0f },     { "ratio", 1.0f, 20.0f, 1.0f },             { "attack", 0.1f, 100.0f, 10.0f },
            { "release", 50.0f, 2000.0f, 300.0f },   { "makeup", 0.0f, 20.0f, 0.0f },            { "dynIn", 0.0f, 1.0f, 1.0f },
            { "drive", 0.0f, 10.0f, 0.0f }
        }};

        return table[(size_t) p];
    }

    AnalogStrip()
    {
        for (int p = 0; p < numParams; ++p)
            params[(size_t) p].store (info (p).initial);
    }

    void set (int p, float value) noexcept   { params[(size_t) p].store (juce::jlimit (info (p).min, info (p).max, value)); }
    float get (int p) const noexcept         { return params[(size_t) p].load(); }

    // The compressor's gain reduction in dB (<= 0), for a meter
    float getGainReduction() const noexcept  { return gainReductionDb.load(); }

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;

        for (int p = 0; p < numParams; ++p)
            smoothed[(size_t) p] = get (p);

        reset();
        updateFilters (true);
    }

    void reset()
    {
        for (auto& f : filters)
            for (auto& state : f.state)
                state = {};

        envelopeDb = 0.0;
        for (auto& d : dcState) d = {};
    }

    void process (juce::AudioBuffer<float>& buffer)
    {
        const auto channels = juce::jmin (2, buffer.getNumChannels());
        const auto samples = buffer.getNumSamples();

        for (int start = 0; start < samples; start += controlBlock)
        {
            const auto count = juce::jmin (controlBlock, samples - start);
            smoothParams (count);
            updateFilters (false);

            for (int i = start; i < start + count; ++i)
            {
                std::array<float, 2> frame { buffer.getSample (0, i), channels > 1 ? buffer.getSample (1, i) : buffer.getSample (0, i) };
                processFrame (frame);

                for (int ch = 0; ch < channels; ++ch)
                    buffer.setSample (ch, i, frame[(size_t) ch]);
            }
        }

        gainReductionDb.store ((float) lastReductionDb);
    }

private:
    //==========================================================================
    // A biquad (transposed direct form II, double precision: low frequencies at high sample rates)
    struct Coefficients { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    struct Filter
    {
        Coefficients c;
        bool active = false;
        struct State { double z1 = 0, z2 = 0; };
        std::array<State, 2> state {};

        inline double process (double x, int ch) noexcept
        {
            auto& s = state[(size_t) ch];
            const auto y = c.b0 * x + s.z1;
            s.z1 = c.b1 * x - c.a1 * y + s.z2;
            s.z2 = c.b2 * x - c.a2 * y;
            return y;
        }
    };

    enum FilterIndex { hp1, hp2, lp, lf, lmf, hmf, hf, numFilters };

    static Coefficients normalised (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    // The designs (after Robert Bristow-Johnson's cookbook, with the bilinear transform's pre-warping)
    Coefficients highPass1 (double f) const
    {
        const auto k = std::tan (juce::MathConstants<double>::pi * f / sampleRate);
        return normalised (1.0, -1.0, 0.0, 1.0 + k, k - 1.0, 0.0);
    }

    Coefficients highPass2 (double f, double q) const
    {
        const auto w = juce::MathConstants<double>::twoPi * f / sampleRate, alpha = std::sin (w) / (2.0 * q), cw = std::cos (w);
        return normalised ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    Coefficients lowPass2 (double f, double q) const
    {
        const auto w = juce::MathConstants<double>::twoPi * f / sampleRate, alpha = std::sin (w) / (2.0 * q), cw = std::cos (w);
        return normalised ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    Coefficients peak (double f, double gainDb, double q) const
    {
        const auto A = std::pow (10.0, gainDb / 40.0);
        const auto w = juce::MathConstants<double>::twoPi * f / sampleRate, alpha = std::sin (w) / (2.0 * q), cw = std::cos (w);
        return normalised (1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
    }

    Coefficients shelf (double f, double gainDb, double q, bool high) const
    {
        const auto A = std::pow (10.0, gainDb / 40.0);
        const auto w = juce::MathConstants<double>::twoPi * f / sampleRate, cw = std::cos (w);
        const auto beta = 2.0 * std::sqrt (A) * std::sin (w) / (2.0 * q);

        if (high)
            return normalised (A * ((A + 1) + (A - 1) * cw + beta), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - beta),
                               (A + 1) - (A - 1) * cw + beta, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - beta);

        return normalised (A * ((A + 1) - (A - 1) * cw + beta), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - beta),
                           (A + 1) + (A - 1) * cw + beta, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - beta);
    }

    // Proportional Q: broad for gentle settings, narrower the further the band is pushed
    static double proportionalQ (double q, double gainDb)   { return q * (0.6 + 0.9 * std::abs (gainDb) / 15.0); }

    // A shelf's Q: above 0.707 it overshoots a little past the corner - the analogue shelf's bump
    static constexpr double shelfQ = 0.95, bandBellQ = 0.85;

    //==========================================================================
    void smoothParams (int count)
    {
        // About 20 ms to settle, whatever the block size
        const auto k = 1.0 - std::exp (-(double) count / (0.02 * sampleRate));

        for (int p = 0; p < numParams; ++p)
        {
            auto& s = smoothed[(size_t) p];
            const auto target = (double) get (p);

            if (p == hfBell || p == lfBell)   // switches: at once
                s = target;
            else if (p == hpf || p == lpf || p == hfFreq || p == hmfFreq || p == lmfFreq || p == lfFreq)   // frequencies: on a log scale
                s = std::exp (std::log (s) + (std::log (target) - std::log (s)) * k);
            else
                s += (target - s) * k;

            if (std::abs (target - s) < 1.0e-4 * juce::jmax (1.0, std::abs (target)))
                s = target;
        }
    }

    void updateFilters (bool force)
    {
        if (! force && smoothed == lastDesigned)
            return;

        lastDesigned = smoothed;
        const auto& v = smoothed;
        const auto nyquistSafe = [this] (double f) { return juce::jmin (f, sampleRate * 0.45); };

        std::array<bool, numFilters> wasActive {};

        for (int i = 0; i < numFilters; ++i)
            wasActive[(size_t) i] = filters[(size_t) i].active;

        filters[hp1].active = filters[hp2].active = v[hpf] > 16.5;   // at its lowest: off
        filters[hp1].c = highPass1 (v[hpf]);
        filters[hp2].c = highPass2 (v[hpf], 1.0);                    // with the first-order: a 3rd-order Butterworth, 18 dB/oct

        filters[lp].active = v[lpf] < 21900.0;                       // at its highest: off
        filters[lp].c = lowPass2 (nyquistSafe (v[lpf]), juce::MathConstants<double>::sqrt2 * 0.5);

        const auto band = [&] (FilterIndex index, double gain, double frequency, double q, bool bellNotShelf, bool high)
        {
            filters[index].active = std::abs (gain) > 0.01;
            frequency = nyquistSafe (frequency);
            filters[index].c = bellNotShelf ? peak (frequency, gain, q) : shelf (frequency, gain, shelfQ, high);
        };

        band (lf, v[lfGain], v[lfFreq], bandBellQ, v[lfBell] > 0.5, false);
        band (lmf, v[lmfGain], v[lmfFreq], proportionalQ (v[lmfQ], v[lmfGain]), true, false);
        band (hmf, v[hmfGain], v[hmfFreq], proportionalQ (v[hmfQ], v[hmfGain]), true, true);
        band (hf, v[hfGain], v[hfFreq], bandBellQ, v[hfBell] > 0.5, true);

        for (int i = 0; i < numFilters; ++i)   // a filter switching in starts from silence, not old state
            if (filters[(size_t) i].active && ! wasActive[(size_t) i])
                filters[(size_t) i].state = {};

        // The compressor's time constants
        attackCoeff = std::exp (-1.0 / (juce::jmax (0.05, v[attack]) * 0.001 * sampleRate));
        releaseCoeff = std::exp (-1.0 / (juce::jmax (1.0, v[release]) * 0.001 * sampleRate));
    }

    //==========================================================================
    inline void processFrame (std::array<float, 2>& frame) noexcept
    {
        const auto& v = smoothed;
        const auto eqMix = v[eqIn], dynMix = v[dynIn];

        // EQ (with the filters), crossfaded by EQ IN
        if (eqMix > 0.0)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const double dry = frame[(size_t) ch];
                auto x = dry;

                for (auto& f : filters)
                    if (f.active)
                        x = f.process (x, ch);

                frame[(size_t) ch] = (float) (dry + (x - dry) * eqMix);
            }
        }

        // Dynamics: a stereo-linked VCA compressor with a 6 dB soft knee, crossfaded by DYN IN
        if (dynMix > 0.0 && (v[ratio] > 1.001 || v[makeup] > 0.001))
        {
            const auto level = juce::jmax (std::abs (frame[0]), std::abs (frame[1]));
            const auto levelDb = level > 1.0e-6f ? 20.0 * std::log10 ((double) level) : -120.0;
            const auto over = levelDb - v[threshold], knee = 6.0, slope = 1.0 / v[ratio] - 1.0;
            double targetDb = 0.0;

            if (2.0 * over > knee)
                targetDb = slope * over;
            else if (2.0 * over > -knee)
                targetDb = slope * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);

            // Smoothed in the dB domain: attack towards more reduction, release back
            envelopeDb = targetDb < envelopeDb ? targetDb + (envelopeDb - targetDb) * attackCoeff
                                               : targetDb + (envelopeDb - targetDb) * releaseCoeff;
            lastReductionDb = envelopeDb;

            const auto gain = std::pow (10.0, (envelopeDb + v[makeup]) / 20.0);
            const auto mix = gain * dynMix + (1.0 - dynMix);

            frame[0] = (float) (frame[0] * mix);
            frame[1] = (float) (frame[1] * mix);
        }
        else
        {
            lastReductionDb = 0.0;
        }

        // Drive: into a soft, slightly asymmetric clipper, level-compensated (clean and the same
        // loudness at low levels; harmonics and rounded peaks as it's pushed), then a DC blocker
        if (v[drive] > 0.001)
        {
            const auto amount = v[drive] / 10.0;
            const auto pre = std::pow (10.0, amount * 18.0 / 20.0);   // up to +18 dB into the clipper
            const auto bias = 0.12 * amount;                          // the asymmetry: even harmonics
            const auto slopeAtZero = 1.0 - std::tanh (bias) * std::tanh (bias);
            const auto dcCoeff = 1.0 - juce::MathConstants<double>::twoPi * 8.0 / sampleRate;

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto x = (double) frame[(size_t) ch] * pre;
                const auto shaped = (std::tanh (x + bias) - std::tanh (bias)) / (slopeAtZero * pre);

                auto& dc = dcState[(size_t) ch];   // y = x - x1 + R y1
                const auto y = shaped - dc.x1 + dcCoeff * dc.y1;
                dc.x1 = shaped;
                dc.y1 = y;
                frame[(size_t) ch] = (float) y;
            }
        }
    }

    static constexpr int controlBlock = 16;   // samples between filter updates while a knob moves

    std::array<std::atomic<float>, numParams> params;
    std::array<double, numParams> smoothed {}, lastDesigned {};
    std::array<Filter, numFilters> filters {};
    double sampleRate = 48000.0, envelopeDb = 0.0, lastReductionDb = 0.0, attackCoeff = 0.0, releaseCoeff = 0.0;
    struct DcState { double x1 = 0, y1 = 0; };
    std::array<DcState, 2> dcState {};
    std::atomic<float> gainReductionDb { 0.0f };
};
