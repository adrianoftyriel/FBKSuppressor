// FBKSuppressor - ErbBands.h
//
// ERB-scaled band layout, built at prepare() time for whatever sample rate the
// host gives us. This is the piece that makes native 44.1 kHz and 48 kHz
// operation possible without resampling: band edges are defined in Hz on a
// perceptual scale, then mapped onto the FFT grid for the current rate. Nothing
// downstream needs a per-rate code path, and no resampler is ever inserted
// (a resampler would add exactly the latency this plugin exists to avoid).
//
// RNNoise uses 22 Bark-ish bands and DeepFilterNet uses 32 ERB bands; we follow
// DeepFilterNet's count, which gives finer resolution through the vocal formant
// region where preserving colour matters most.
#pragma once

#include "Common.h"

namespace fbk
{
constexpr int kNumBands = 32;

class ErbBands
{
public:
    void prepare (double sampleRate);

    int numBands() const noexcept { return kNumBands; }
    int numBins()  const noexcept { return numBins_; }

    int  bandStart (int b) const noexcept { return start_[static_cast<size_t> (b)]; }
    int  bandEnd   (int b) const noexcept { return end_[static_cast<size_t> (b)]; }   // exclusive
    int  bandWidth (int b) const noexcept { return end_[static_cast<size_t> (b)] - start_[static_cast<size_t> (b)]; }
    float centreHz (int b) const noexcept { return centreHz_[static_cast<size_t> (b)]; }
    int  bandForBin (int bin) const noexcept { return binToBand_[static_cast<size_t> (bin)]; }
    float binHz (int bin) const noexcept { return static_cast<float> (bin) * binWidthHz_; }
    float binWidthHz() const noexcept { return binWidthHz_; }

    // Average a per-bin quantity into per-band values.
    void binsToBands (const float* bins, float* bands) const noexcept;

    // Spread a per-band gain back over bins with linear interpolation between
    // band centres, so the resulting magnitude response is smooth. A smooth
    // response is what keeps the derived minimum-phase filter short, which is
    // in turn what keeps its group delay negligible.
    void bandsToBins (const float* bands, float* bins) const noexcept;

private:
    static float hzToErb (float hz) noexcept
    {
        return 21.4f * std::log10 (1.0f + 0.00437f * hz);
    }

    static float erbToHz (float erb) noexcept
    {
        return (std::pow (10.0f, erb / 21.4f) - 1.0f) / 0.00437f;
    }

    double sampleRate_ { 48000.0 };
    int    numBins_ { kNumBins };
    float  binWidthHz_ { 1.0f };
    std::vector<int>   start_, end_, binToBand_;
    std::vector<float> centreHz_, centreBin_;
};
} // namespace fbk
