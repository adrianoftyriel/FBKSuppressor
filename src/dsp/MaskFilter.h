// FBKSuppressor - MaskFilter.h
//
// Broadband noise and reverb suppression applied as a causal time-domain filter.
//
// The zero-latency trick
// ---------------------
// The usual way to apply a spectral gain mask is to multiply it into the STFT and
// resynthesise by overlap-add. That costs a full window of latency, and it is
// where RNNoise's 10 ms and DeepFilterNet's 40 ms come from.
//
// Instead we treat the mask as the *magnitude response of a filter* and realise
// that filter directly in the time domain. Of all filters with a given magnitude
// response, the minimum-phase one has the smallest possible group delay: its
// impulse response is causal with its energy packed into the first few taps. So
// we compute a minimum-phase FIR from the mask by the standard cepstral method -
// take the real cepstrum of the log magnitude, fold the anti-causal half onto the
// causal half, exponentiate back - and convolve the input with it. Nothing is
// held back, nothing is delayed, and the plugin reports zero latency to the host.
//
// Two things make this practical rather than merely theoretically possible.
// First, the mask is built on 32 ERB bands and interpolated smoothly between band
// centres, so its magnitude response has no sharp edges and the corresponding
// minimum-phase impulse response decays inside 64 taps. Second, the mask is
// redesigned only every kMaskDecimation hops and the coefficients are
// interpolated between designs, so there is no zipper noise and the design cost
// is amortised.
//
// The cost is phase: a minimum-phase filter is not phase-linear. For a smooth
// magnitude response of the depth used here (limited to 12 dB by default) the
// phase distortion is well below audibility on speech, and it is anyway
// preferable to the alternative of delaying a live monitor path. Quality mode
// exists for anyone who would rather have the linear phase and pay 2.7 ms for it.
// (limited to 9 dB by default)
//
// Voice colour
// -----------
// A band-gain mask is, by construction, the thing that can dull a voice: pull a
// band down and every harmonic in it comes down together. Three constraints hold
// that in check:
//   * a hard limit on maximum attenuation per band (default 9 dB), so no band
//     can ever be gutted no matter what the estimator thinks;
//   * the attenuation is scaled back in bands where speech presence is high, so
//     the mask acts on the gaps rather than on the voice;
//   * a decision-directed a-priori SNR estimate rather than a raw per-frame one,
//     which is what stops the mask from flapping and producing the "underwater"
//     artefact that makes processed voices sound synthetic.
// The neural separator planned for v0.2 replaces this stage precisely because a
// learned mask can be far more selective than any of these heuristics.
#pragma once

#include "Common.h"
#include "ErbBands.h"
#include "Fft.h"
#include "NoiseTracker.h"

namespace fbk
{
enum class PhaseMode
{
    minimumPhase,   // strict zero latency
    linearPhase     // Quality mode, kLookaheadSamples of reported latency
};

struct MaskSettings
{
    bool  denoiseEnabled { true };
    float denoiseAmount { 0.5f };          // 0..1
    float maxAttenuationDb { 9.0f };       // per band, hard ceiling

    bool  dereverbEnabled { false };       // off by default: this is the stage
                                           // that genuinely alters voice character
    float dereverbAmount { 0.6f };
    float rt60Seconds { 0.6f };
    float dereverbMaxAttenuationDb { 9.0f };

    // How strongly speech presence protects a band from attenuation.
    float voiceProtection { 0.8f };        // 0..1

    // Optional per-band override, from a calibration profile's long-term average
    // spectrum: bands carrying more of this particular voice get protected harder.
    // Null means use the scalar above for every band. The pointed-to storage must
    // outlive the MaskFilter, and in practice is owned by FeedbackSuppressor.
    const float* voiceProtectionPerBand { nullptr };
};

class MaskFilter
{
public:
    MaskFilter();

    void prepare (double sampleRate, const ErbBands& bands);
    void reset() noexcept;

    void setSettings (const MaskSettings& s) noexcept { settings_ = s; }
    void setPhaseMode (PhaseMode m) noexcept;
    PhaseMode phaseMode() const noexcept { return phaseMode_; }

    // Reported latency in samples for the current phase mode.
    int latencySamples() const noexcept
    {
        return phaseMode_ == PhaseMode::linearPhase ? kLookaheadSamples : 0;
    }

    // Once per analysis frame. Returns true if the filter was redesigned.
    bool updateMask (const float* power, const NoiseTracker& noise) noexcept;

    float process (float x) noexcept;

    const float* bandGains() const noexcept { return bandGain_.data(); }   // kNumBands
    float meanAttenuationDb() const noexcept { return meanAttenDb_; }

private:
    void designMinimumPhase (const float* binGains) noexcept;
    void designLinearPhase (const float* binGains) noexcept;
    void resampleToDesignGrid (const float* binGains) noexcept;

    Fft designFft_;
    const ErbBands* bands_ { nullptr };
    double sampleRate_ { 48000.0 };
    int    numBins_ { kNumBins };
    MaskSettings settings_ {};
    PhaseMode phaseMode_ { PhaseMode::minimumPhase };

    int   frameCounter_ { 0 };
    float meanAttenDb_ { 0.0f };

    // Estimator state.
    std::vector<float> bandPower_, bandNoise_, priorSnr_, prevGain_;
    std::vector<float> bandGain_, binGain_;

    // Dereverb state: a short history of band powers plus the running late-field
    // estimate.
    static constexpr int kReverbDelayFrames = 2;
    std::vector<std::vector<float>> powerHistory_;
    int  historyWrite_ { 0 };
    std::vector<float> lateReverb_;
    float reverbDecay_ { 0.0f };

    // Design/convolution state.
    std::vector<float> designMag_;      // kDesignSize/2 + 1
    std::vector<Complex> designSpec_;   // kDesignSize
    std::vector<float> cepstrum_;

    // All tap buffers are sized once, at the maximum any phase mode needs, so
    // switching mode is a change of numTaps_ and a fill - never a reallocation.
    // setPhaseMode() is reachable from setParameters(), which the plugin calls
    // from processBlock, so an allocation here would be an allocation on the
    // audio thread every time the Quality Mode button is pressed.
    static constexpr int kMaxTapsAnyMode = 2 * kLookaheadSamples + 1;
    int numTaps_ { kMaskTaps };
    std::vector<float> coeffs_, targetCoeffs_, coeffStep_;
    std::vector<float> history_;
    int  historyPos_ { 0 };
    int  interpSamplesRemaining_ { 0 };
    int  interpLength_ { kHopSize * kMaskDecimation };
};
} // namespace fbk
