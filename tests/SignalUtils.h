// Signal generation and measurement helpers for the DSP tests.
#pragma once

#include "Common.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace test
{
// Deterministic PRNG so test results are reproducible across platforms.
class Rng
{
public:
    explicit Rng (uint32_t seed = 12345u) : state_ (seed) {}

    float uniform() noexcept
    {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<float> (state_ >> 8) / static_cast<float> (1u << 24) * 2.0f - 1.0f;
    }

    float gaussian() noexcept
    {
        // Sum of uniforms: adequate for a noise floor, and cheap.
        float s = 0.0f;
        for (int i = 0; i < 4; ++i)
            s += uniform();
        return s * 0.5f;
    }

private:
    uint32_t state_;
};

// Magnitude of a single frequency component, by Goertzel. Exact-frequency rather
// than bin-quantised, which matters when measuring 40 dB of cancellation.
inline double goertzelMagnitude (const float* x, int n, double freqHz, double sampleRate)
{
    const double w = 2.0 * fbk::kPi * freqHz / sampleRate;
    const double coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double s0 = static_cast<double> (x[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double real = s1 - s2 * std::cos (w);
    const double imag = s2 * std::sin (w);
    return 2.0 * std::sqrt (real * real + imag * imag) / static_cast<double> (n);
}

inline double rms (const float* x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i)
        s += static_cast<double> (x[i]) * static_cast<double> (x[i]);
    return std::sqrt (s / static_cast<double> (std::max (1, n)));
}

inline double toDb (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-12)); }

// A synthetic voice: harmonic stack on a moving fundamental with vibrato and
// per-harmonic jitter, plus a formant-ish spectral envelope. The point of the
// vibrato and jitter is that they are what a real voice has and a feedback tone
// does not - so any processor that survives this test without dulling the
// harmonics is distinguishing the two for the right reason.
class SyntheticVoice
{
public:
    SyntheticVoice (double sampleRate, double f0 = 130.0)
        : sampleRate_ (sampleRate), f0_ (f0)
    {
        phases_.assign (kHarmonics, 0.0);
    }

    float next() noexcept
    {
        // 5 Hz vibrato, +/- 3% - typical for a sung or projected speaking voice.
        const double vib = 1.0 + 0.03 * std::sin (2.0 * fbk::kPi * 5.0 * t_);
        // Slow pitch contour so the harmonics never sit still.
        const double contour = 1.0 + 0.12 * std::sin (2.0 * fbk::kPi * 0.7 * t_);
        const double f0 = f0_ * vib * contour;

        float out = 0.0f;
        for (int h = 0; h < kHarmonics; ++h)
        {
            const double fh = f0 * static_cast<double> (h + 1);
            if (fh > sampleRate_ * 0.45)
                break;

            // Formant-ish envelope: peaks near 700 Hz and 1800 Hz.
            const double e1 = std::exp (-std::pow ((fh - 700.0) / 500.0, 2.0));
            const double e2 = 0.7 * std::exp (-std::pow ((fh - 1800.0) / 700.0, 2.0));
            const double amp = (0.25 + e1 + e2) / static_cast<double> (h + 1);

            phases_[static_cast<size_t> (h)] += 2.0 * fbk::kPi * fh / sampleRate_;
            if (phases_[static_cast<size_t> (h)] > 2.0 * fbk::kPi)
                phases_[static_cast<size_t> (h)] -= 2.0 * fbk::kPi;

            out += static_cast<float> (amp * std::sin (phases_[static_cast<size_t> (h)]));
        }

        // Amplitude envelope with syllabic gaps, so a noise estimator has
        // somewhere to find the floor.
        const double env = 0.55 + 0.45 * std::sin (2.0 * fbk::kPi * 2.5 * t_);
        t_ += 1.0 / sampleRate_;

        return static_cast<float> (out * env * 0.12);
    }

    double fundamentalAt (double /*t*/) const { return f0_; }

private:
    static constexpr int kHarmonics = 40;
    double sampleRate_;
    double f0_;
    double t_ { 0.0 };
    std::vector<double> phases_;
};
} // namespace test
