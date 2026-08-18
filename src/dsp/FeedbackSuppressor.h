// FBKSuppressor - FeedbackSuppressor.h
//
// The whole chain, per channel. Host-agnostic and JUCE-free so it can be unit
// tested directly.
//
// Order of operations, and why
// ---------------------------
//   1. High-pass (35 Hz, optional)   - subsonic rumble only, below all voice.
//   2. Hum canceller                 - subtractive, exact harmonic series.
//   3. Tonal canceller               - subtractive, tracked feedback tones.
//   4. Mask filter                   - broadband noise and optional dereverb,
//                                      applied as a causal minimum-phase FIR.
//   5. Strength mix                  - dry/wet, with the dry path delay-matched.
//
// The subtractive stages come first because they are surgical: they take out a
// specific waveform and leave the rest of the signal, including any voice energy
// at the same frequency, untouched. Running them before the mask also means the
// mask estimator never sees a howl in its noise statistics, which would otherwise
// drag a whole ERB band down and take vocal harmonics with it.
//
// The analysis chain is fed the *input* signal, never the processed output. That
// is deliberate: if the detector watched the output it would see a successfully
// cancelled tone as absent, disengage, and let the feedback back in - an
// oscillation between suppressed and not. Watching the input means the canceller
// stays engaged for exactly as long as the loop is still trying to ring.
#pragma once

#include "Analyser.h"
#include "Biquad.h"
#include "Calibration.h"
#include "CaptureRing.h"
#include "Common.h"
#include "ErbBands.h"
#include "HowlDetector.h"
#include "MaskFilter.h"
#include "NoiseTracker.h"
#include "SpectralSeparator.h"
#include "TonalCanceller.h"

namespace fbk
{
struct Parameters
{
    bool  bypass { false };
    float strength { 1.0f };            // 0..1 overall wet amount

    bool  qualityMode { false };        // true -> linear phase, kLookaheadSamples latency

    bool  feedbackEnabled { true };
    float feedbackSensitivity { 0.5f }; // 0..1
    float feedbackDepth { 1.0f };       // 0..1

    bool  denoiseEnabled { true };
    float denoiseAmount { 0.5f };
    float maxAttenuationDb { 9.0f };
    float voiceProtection { 0.8f };

    bool  humEnabled { true };
    float humDepth { 1.0f };
    int   humHarmonics { 8 };

    bool  dereverbEnabled { false };
    float dereverbAmount { 0.6f };
    float rt60Seconds { 0.6f };

    bool  highPassEnabled { true };
    float highPassHz { 35.0f };

    // Set when a calibration profile has been applied. Purely informational for
    // the UI; the profile's effects are pushed into the components directly.
    bool  profileApplied { false };
};

struct Metering
{
    int   confirmedTones { 0 };
    float toneFrequencies[kMaxTones] {};
    float toneConfidence[kMaxTones] {};
    float toneAttenuationDb[kMaxTones] {};
    float bandGainDb[kNumBands] {};
    float bandNoiseDb[kNumBands] {};
    float bandInputDb[kNumBands] {};
    float speechPresence { 0.0f };
    float humFundamentalHz { 0.0f };
    bool  humActive { false };
    float meanAttenuationDb { 0.0f };
    float inputPeak { 0.0f };
    float outputPeak { 0.0f };
};

class FeedbackSuppressor
{
public:
    FeedbackSuppressor();

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    void setParameters (const Parameters& p) noexcept;
    const Parameters& parameters() const noexcept { return params_; }

    // Samples of latency the host must compensate. 0 in strict mode.
    int latencySamples() const noexcept { return latencySamples_; }

    void processBlock (float* data, int numSamples) noexcept;

    const Metering& metering() const noexcept { return metering_; }
    CaptureRing& captureRing() noexcept { return capture_; }

    // --- Calibration ------------------------------------------------------
    // Explicit-apply only: beginCalibration/finishCalibration measure, and
    // applyProfile is a separate deliberate act. Measuring never changes
    // behaviour, so a calibration pass cannot surprise anyone mid-show.
    void beginCalibration (CalibrationPhase phase) noexcept;
    void finishCalibration() noexcept;
    void cancelCalibration() noexcept;
    CalibrationPhase calibrationPhase() const noexcept { return calibrator_.phase(); }
    float calibrationProgress() const noexcept { return calibrator_.progress(); }
    float calibrationElapsedSeconds() const noexcept { return calibrator_.elapsedSeconds(); }
    const VoiceProfile& profile() const noexcept { return calibrator_.profile(); }
    Calibrator& calibrator() noexcept { return calibrator_; }

    void applyProfile (const VoiceProfile&) noexcept;
    void clearProfile() noexcept;
    bool hasProfileApplied() const noexcept { return profileApplied_; }

    // v0.2 hook. Ownership stays with the caller; pass nullptr to revert to the
    // heuristic presence estimate.
    void setSeparator (SpectralSeparator* s) noexcept { separator_ = s; }

    double sampleRate() const noexcept { return sampleRate_; }
    const ErbBands& bands() const noexcept { return bands_; }
    float measurementCentroidMs() const noexcept
    {
        return static_cast<float> (1000.0 * static_cast<double> (analyser_.measurementCentroidSamples())
                                   / sampleRate_);
    }

private:
    void onAnalysisFrame() noexcept;
    void applyPhaseMode() noexcept;

    double sampleRate_ { 48000.0 };
    int    maxBlockSize_ { 512 };
    int    latencySamples_ { 0 };

    Parameters params_ {};
    bool paramsDirty_ { true };

    ErbBands       bands_;
    Analyser       analyser_;
    NoiseTracker   noise_;
    HowlDetector   detector_;
    TonalCanceller tonal_;
    HumCanceller   hum_;
    MaskFilter     mask_;
    HighPass       highPass_;
    DelayLine      dryDelay_;
    CaptureRing    capture_;

    SpectralSeparator* separator_ { nullptr };
    std::vector<float> bandEnergy_, separatorPresence_;
    // Metering scratch. Members rather than function-local statics: a
    // thread_local static would allocate on first use, and first use is on the
    // audio thread.
    std::vector<float> meterBandNoise_, meterBandPower_;

    Calibrator calibrator_;
    bool profileApplied_ { false };
    // Owned copy, because MaskSettings holds a pointer into it.
    std::vector<float> bandProtection_;
    std::vector<float> modePriors_;

    Metering metering_ {};
    int meterFrameCounter_ { 0 };
};
} // namespace fbk
