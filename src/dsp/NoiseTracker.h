// FBKSuppressor - NoiseTracker.h
//
// Per-bin noise power spectral density estimate by minimum statistics, plus a
// speech-presence estimate.
//
// Minimum statistics (Martin) works on the observation that in any window of a
// second or two, every frequency bin sees at least one moment where speech is
// absent, so the running minimum of the smoothed power tracks the noise floor.
// It needs no voice activity detector, which matters here because a VAD would be
// the first thing to fail on a live stage with a PA running.
//
// The search window is split into sub-windows so the estimate can rise as well
// as fall - important on a stage where the noise floor changes when the crowd
// reacts or an air handler cycles.
#pragma once

#include <array>

#include "Common.h"
#include "ErbBands.h"

namespace fbk
{
class NoiseTracker
{
public:
    void prepare (double sampleRate, const ErbBands& bands);
    void reset() noexcept;

    // Call once per analysis frame with the per-bin power spectrum.
    void update (const float* power) noexcept;

    const float* noisePower() const noexcept { return noise_.data(); }        // kNumBins
    const float* smoothedPower() const noexcept { return smoothed_.data(); } // kNumBins

    // Per-band a-posteriori SNR, linear.
    const float* bandSnr() const noexcept { return bandSnr_.data(); }        // kNumBands

    // Probability-like speech presence in [0,1], per band. While a learned
    // separator is supplying presence it replaces the heuristic estimate; the
    // heuristic keeps running underneath so detaching the model is seamless.
    const float* speechPresence() const noexcept
    {
        return externalActive_ ? external_.data() : presence_.data();   // kNumBands
    }

    // Broadband speech presence in [0,1].
    float overallSpeechPresence() const noexcept
    {
        return externalActive_ ? externalVad_ : overallPresence_;
    }

    // Supply presence from a learned separator (kNumBands values and a broadband
    // activity), or pass nullptr to fall back to the heuristic.
    void setExternalPresence (const float* perBand, float voiceActivity) noexcept
    {
        externalActive_ = (perBand != nullptr);
        externalVad_ = voiceActivity;
        if (perBand != nullptr)
            for (int b = 0; b < kNumBands; ++b)
                external_[static_cast<size_t> (b)] = clampf (perBand[b], 0.0f, 1.0f);
    }

private:
    const ErbBands* bands_ { nullptr };
    int numBins_ { kNumBins };

    float smoothAlpha_ { 0.8f };
    int   subWindowLen_ { 24 };     // frames
    int   numSubWindows_ { 8 };

    std::vector<float> smoothed_, noise_, currentMin_;
    std::vector<std::vector<float>> subMins_;
    int subIndex_ { 0 }, subCounter_ { 0 };

    std::vector<float> bandNoise_, bandPower_, bandSnr_, presence_;
    float overallPresence_ { 0.0f };
    bool  primed_ { false };

    std::array<float, kNumBands> external_ {};
    float externalVad_ { 0.0f };
    bool  externalActive_ { false };
};
} // namespace fbk
