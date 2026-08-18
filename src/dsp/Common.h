// FBKSuppressor - Common.h
//
// Shared constants, small real-time-safe helpers, and the configuration struct
// that ties the whole chain to a sample rate.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace fbk
{
// ---------------------------------------------------------------------------
// Fixed analysis geometry.
//
// The analysis FFT is 2048 points at every supported sample rate. We keep the
// *sample* count fixed rather than the *duration* so that the transform stays a
// power of two at 44.1 kHz as well as 48 kHz; the resulting bin spacing
// (21.5 Hz at 44.1 k, 23.4 Hz at 48 k) is close enough that a single set of
// detector thresholds serves both. Band edges are specified in Hz and remapped
// per rate, so nothing else in the chain cares which rate we are running at.
//
// The hop is 128 samples (~2.7 ms). Feedback grows exponentially, so the tone
// tracker has to run fast; the broadband mask changes far more slowly and is
// redesigned every kMaskDecimation hops instead.
// ---------------------------------------------------------------------------
constexpr int kFftOrder       = 11;              // 2048
constexpr int kFftSize        = 1 << kFftOrder;
constexpr int kNumBins        = kFftSize / 2 + 1;
constexpr int kHopSize        = 128;
constexpr int kMaskDecimation = 4;               // mask redesign cadence, in hops

// Minimum-phase mask filter geometry. 256-point design FFT, 64 usable taps.
constexpr int kDesignOrder = 8;                  // 256
constexpr int kDesignSize  = 1 << kDesignOrder;
constexpr int kMaskTaps    = 64;

// Quality-mode lookahead, in samples. Reported to the host as latency.
constexpr int kLookaheadSamples = 128;

// Number of simultaneously tracked feedback tones.
constexpr int kMaxTones = 12;

// Number of mains harmonics the hum canceller locks to.
constexpr int kMaxHumHarmonics = 12;

constexpr float kEpsilon = 1.0e-20f;

// Our own pi rather than std::numbers::pi from <numbers>. That header is C++20
// and GCC has it, but Apple Clang's libc++ on the macOS CI runner does not - the
// build failed there while passing on Linux. Since the whole point of keeping the
// DSP free of dependencies is portability, a constant costs nothing and removes
// the landmine. (This is also why the CI runs the tests natively on all three
// platforms rather than trusting Linux to speak for the others.)
constexpr double kPi  = 3.14159265358979323846;
constexpr float  kPiF = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
inline float dbToGain (float db) noexcept { return std::pow (10.0f, 0.05f * db); }
inline float gainToDb (float g)  noexcept { return 20.0f * std::log10 (std::max (g, 1.0e-12f)); }

inline float clampf (float v, float lo, float hi) noexcept
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Guard against denormals and NaNs leaking into recursive state. Called on
// every value that feeds a feedback path.
inline float sanitise (float v) noexcept
{
    if (! std::isfinite (v))
        return 0.0f;
    if (std::abs (v) < 1.0e-25f)
        return 0.0f;
    return v;
}

// One-pole smoother with separate attack and release coefficients, expressed in
// milliseconds so the behaviour is identical at both sample rates.
class AttackRelease
{
public:
    void prepare (double sampleRate, float attackMs, float releaseMs, float initial = 0.0f) noexcept
    {
        const auto coeff = [sampleRate] (float ms) -> float
        {
            if (ms <= 0.0f)
                return 0.0f;
            return static_cast<float> (std::exp (-1.0 / (1.0e-3 * static_cast<double> (ms) * sampleRate)));
        };
        attack_  = coeff (attackMs);
        release_ = coeff (releaseMs);
        value_   = initial;
    }

    float process (float target) noexcept
    {
        const float c = target > value_ ? attack_ : release_;
        value_ = target + c * (value_ - target);
        return value_;
    }

    float value() const noexcept { return value_; }
    void reset (float v) noexcept { value_ = v; }

private:
    float attack_ { 0.0f }, release_ { 0.0f }, value_ { 0.0f };
};

// A plain circular delay line of floats. Used for the Quality-mode lookahead
// and by the capture logger.
class DelayLine
{
public:
    void prepare (int maxDelay)
    {
        buffer_.assign (static_cast<size_t> (std::max (maxDelay, 1)) + 1, 0.0f);
        write_ = 0;
    }

    void clear() noexcept { std::fill (buffer_.begin(), buffer_.end(), 0.0f); write_ = 0; }

    // Push one sample, return the sample `delay` positions back.
    float process (float x, int delay) noexcept
    {
        const int n = static_cast<int> (buffer_.size());
        buffer_[static_cast<size_t> (write_)] = x;
        int read = write_ - delay;
        while (read < 0)
            read += n;
        write_ = (write_ + 1) % n;
        return buffer_[static_cast<size_t> (read)];
    }

private:
    std::vector<float> buffer_;
    int write_ { 0 };
};
} // namespace fbk
