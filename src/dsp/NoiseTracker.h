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

    // Probability-like speech presence in [0,1], per band.
    const float* speechPresence() const noexcept { return presence_.data(); }// kNumBands

    // Broadband speech presence in [0,1].
    float overallSpeechPresence() const noexcept { return overallPresence_; }

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
};
} // namespace fbk
