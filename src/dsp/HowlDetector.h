// FBKSuppressor - HowlDetector.h
//
// Acoustic-feedback detection using the criteria established in the howling
// literature, combined by logical conjunction rather than used singly.
//
// No one measurement separates a howl from a sustained sung note. What works is
// requiring several independent properties at once:
//
//   PAPR  peak-to-average power ratio. Feedback ends up loud relative to the
//         rest of the spectrum. Cheap, but a loud vowel passes it too, so this
//         is only ever used as a gate.
//   PNPR  peak-to-neighbouring power ratio. A howl is an undamped sinusoid, so
//         its bandwidth is essentially zero and its skirts collapse straight
//         into the noise floor. A vocal harmonic is frequency-modulated by
//         vibrato and jitter, so it always occupies several bins. This is the
//         single most discriminative criterion.
//   PHPR  peak-to-harmonic power ratio. Voiced speech is harmonic: if a peak is
//         the n-th harmonic of a voice, its subharmonics carry energy. An
//         isolated feedback tone has nothing below it. Computed against the
//         second and third subharmonics.
//   IPMP  interframe peak magnitude persistence. Feedback sits at one frequency
//         for as long as the loop gain stays above unity. Speech does not.
//   IMSD  interframe magnitude slope deviation. Feedback builds exponentially,
//         which is a straight line in dB - so the *deviation* of its frame-to-
//         frame dB slope is small while the slope itself is positive. Speech
//         onsets are loud but erratic.
//   FSD   frequency standard deviation. Not from the published criteria set, and
//         added here because the published set alone was not enough: in testing,
//         a held sung note produced harmonics that passed PAPR, PHPR and IPMP
//         together and were confirmed as feedback. The reason is that PHPR only
//         looks at integer subharmonics, so a prime-numbered harmonic of a voice
//         has nothing below it and scores exactly like an isolated tone.
//         Frequency stability separates them decisively: an acoustic feedback
//         mode is fixed by the room and the loop, and its measured frequency
//         moves by well under a hertz from frame to frame, whereas a voice is in
//         constant motion from vibrato and pitch contour - tens of hertz on the
//         upper harmonics. This is measured from the sub-bin interpolated peak
//         frequency, so its resolution is far better than the bin spacing.
//
// A candidate is confirmed only when *all* of these hold. The published studies
// found pairwise conjunctions sufficient for detection alone, but the cost
// asymmetry here is severe: a missed howl is briefly unpleasant, while a false
// positive means a canceller quietly subtracting part of someone's voice. So the
// full conjunction is used and the sensitivity control relaxes the thresholds
// rather than the logic. Confirmed tones then ramp a confidence value, and the
// canceller's adaptation rate scales with it, so even a false positive that does
// slip through can only act slowly and narrowly before the detector drops it.
//
// Note what this class does *not* do: it never decides to attenuate a frequency
// region. It reports "there is a coherent sinusoid at 2317 Hz". Removing that
// sinusoid is a subtraction, handled downstream, and it leaves any voice energy
// sharing that frequency in place. That is the whole difference between this and
// ringing a room out with notches.
#pragma once

#include "Common.h"
#include "ErbBands.h"

namespace fbk
{
struct TrackedTone
{
    bool  active { false };
    bool  confirmed { false };
    float freqHz { 0.0f };
    float magnitude { 0.0f };
    int   bin { 0 };

    // Criterion values, kept for the UI and for diagnostics.
    float paprDb { 0.0f };
    float pnprDb { 0.0f };
    float phprDb { 0.0f };
    int   persistence { 0 };       // IPMP, frames
    float slopeDeviation { 0.0f };  // IMSD
    float slopeMeanDb { 0.0f };
    float freqDeviation { 0.0f };   // FSD, Hz
    float localProminenceDb { 0.0f }; // how far this peak stands above its
                                      // spectral neighbourhood

    float confidence { 0.0f };    // 0..1, smoothed engage amount
    int   framesSinceSeen { 0 };

    // Rolling history for the IMSD and FSD statistics.
    static constexpr int kHistory = 8;
    float logMagHistory[kHistory] {};
    float freqHistory[kHistory] {};
    int   historyCount { 0 };
};

struct HowlThresholds
{
    float paprDb { 8.0f };
    float pnprDb { 10.0f };
    float phprDb { 8.0f };
    int   ipmpFrames { 12 };      // ~32 ms at a 128-sample hop
    float imsdMax { 3.0f };       // dB
    float fsdMaxHz { 2.5f };      // absolute cap on frame-to-frame wander
    float fsdMaxFraction { 0.004f }; // and a proportional cap, 0.4% of frequency
    float absoluteFloorDb { -96.0f }; // ignore peaks quieter than this
    float localProminenceDb { 12.0f }; // peak must stand this far above its
                                       // local spectral floor to be a candidate
    int   localFloorHalfWidth { 24 };  // bins either side, for that floor
    float minFreqHz { 100.0f };
    float maxFreqNyquistFraction { 0.90f };
    float confidenceAttackMs { 12.0f };
    float confidenceReleaseMs { 250.0f };
};

class HowlDetector
{
public:
    void prepare (double sampleRate, const ErbBands& bands);
    void reset() noexcept;

    void setSensitivity (float s) noexcept;   // 0..1, 0.5 = nominal

    // Thresholds measured from a calibration profile, replacing the defaults.
    // Passing a non-positive value leaves that threshold at its default.
    void setCalibratedThresholds (float pnprDb, float phprDb, float fsdMaxHz,
                                  float absoluteFloorDb, float localProminenceDb) noexcept;
    void clearCalibratedThresholds() noexcept;

    // Known room resonances, from the profiling phase. A candidate landing on one
    // of these needs less evidence before it is confirmed, because we already know
    // this room rings there. Nothing is attenuated pre-emptively - this only
    // changes how quickly the detector believes a tone that is genuinely present,
    // which is the whole difference between this and ringing a room out.
    void setModePriors (const float* freqsHz, int count) noexcept;
    void clearModePriors() noexcept { numPriors_ = 0; }
    int numModePriors() const noexcept { return numPriors_; }

    // Observe-only: keep tracking and measuring criteria, but never confirm. Used
    // while calibrating against a voice, so the plugin cannot act on the very
    // signal it is measuring.
    // Observe-only widens the peak-picking aperture as well as suppressing
    // confirmation. At runtime the local-prominence gate is the first line of
    // defence and it rejects most vocal harmonics outright - which is correct, but
    // it also means a calibration pass would see nothing at all to measure. In
    // observe mode the gate drops to observeProminenceDb so the tracker admits the
    // wider population of peaks a voice actually produces, and the criteria get
    // computed for them through exactly the same code path.
    void setObserveOnly (bool o) noexcept { observeOnly_ = o; }
    bool isObserveOnly() const noexcept { return observeOnly_; }
    void setObserveProminenceDb (float db) noexcept { observeProminenceDb_ = db; }

    // Call once per analysis frame.
    void process (const float* magnitude) noexcept;

    const TrackedTone* tones() const noexcept { return tones_.data(); }
    int numTones() const noexcept { return kMaxTones; }
    int numConfirmed() const noexcept { return numConfirmed_; }

    const HowlThresholds& thresholds() const noexcept { return thr_; }

private:
    int findOrCreateSlot (float freqHz) noexcept;
    void updateCriteria (TrackedTone& t, const float* magnitude, float frameAvgPower,
                         float measuredFreqHz) noexcept;

    const ErbBands* bands_ { nullptr };
    double sampleRate_ { 48000.0 };
    int    numBins_ { kNumBins };
    int    minBin_ { 1 }, maxBin_ { kNumBins - 1 };

    HowlThresholds thr_ {};
    HowlThresholds base_ {};
    float sensitivity_ { 0.5f };
    float confAttack_ { 0.0f }, confRelease_ { 0.0f };
    float binWidthHz_ { 1.0f };
    float freqMatchHz_ { 30.0f };

    bool isNearPrior (float freqHz) const noexcept;

    std::vector<TrackedTone> tones_;
    std::vector<float> localFloor_;

    // Calibration state.
    bool  observeOnly_ { false };
    float observeProminenceDb_ { 3.0f };
    bool  calibrated_ { false };
    HowlThresholds calibrated_thr_ {};
    static constexpr int kMaxPriors = 32;
    float priors_[kMaxPriors] {};
    int   numPriors_ { 0 };
    int numConfirmed_ { 0 };

    // Candidate scratch, sized once so the per-frame work allocates nothing.
    static constexpr int kMaxCandidates = 64;
    struct Candidate { int bin; float freqHz; float prominence; };
    std::vector<Candidate> candidates_;
};
} // namespace fbk
