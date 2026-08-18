// FBKSuppressor - Calibration.h
//
// Measures the things the detector currently guesses at.
//
// The idea
// --------
// Every threshold in HowlDetector was chosen against a synthetic voice. That is
// the weakest part of the design: the thresholds decide whether a canceller
// engages on someone's vocal harmonic, and they were never measured against the
// voice they will actually be used on.
//
// The fix is not to guess better. It is to run the detector's own criteria over a
// recording of the actual voice in the actual room, build a histogram of what each
// criterion *does* on that voice, and place each threshold just outside that
// distribution. A feedback tone then has to be more tone-like than anything this
// particular voice has ever produced before it is confirmed.
//
// That is why this class histograms PNPR, PHPR and FSD rather than just measuring
// a spectrum: the criteria are what the decision is made on, so the criteria are
// what needs calibrating. Measuring the voice's average spectrum is useful too,
// but secondary.
//
// Three capture phases, each explicitly started by the operator:
//
//   roomNoise  - nobody talking, PA at show gain. Measures the per-band noise
//                floor, which seeds the noise tracker and sets the absolute peak
//                gate so it is expressed in real dBFS rather than a guess.
//   voice      - the performer talking and singing across their range, PA down.
//                Measures the LTAS, the f0 range, and the criterion histograms.
//   roomModes  - gain pushed past show level with cancellation left ON. Harvests
//                the frequencies the suppressor actually had to fight. See
//                RoomModeProfiler.
//
// Nothing here ever changes behaviour on its own. A completed profile has to be
// applied deliberately.
#pragma once

#include "Common.h"
#include "ErbBands.h"
#include "HowlDetector.h"

namespace fbk
{
constexpr int kMaxRoomModes = 32;

// ---------------------------------------------------------------------------
// Fixed-bin histogram with percentile readout. No allocation after prepare, and
// no sorting - percentiles come from the cumulative counts, which is all the
// precision a threshold needs.
class Histogram
{
public:
    static constexpr int kBins = 96;

    void prepare (float minValue, float maxValue) noexcept
    {
        min_ = minValue;
        max_ = maxValue;
        scale_ = static_cast<float> (kBins) / std::max (1.0e-9f, max_ - min_);
        reset();
    }

    void reset() noexcept
    {
        for (auto& b : bins_)
            b = 0;
        count_ = 0;
        sum_ = 0.0;
        below_ = above_ = 0;
    }

    void add (float v) noexcept
    {
        if (! std::isfinite (v))
            return;

        ++count_;
        sum_ += static_cast<double> (v);

        if (v < min_) { ++below_; return; }
        if (v >= max_) { ++above_; return; }

        const int b = static_cast<int> ((v - min_) * scale_);
        ++bins_[static_cast<size_t> (std::clamp (b, 0, kBins - 1))];
    }

    int count() const noexcept { return count_; }
    float mean() const noexcept { return count_ > 0 ? static_cast<float> (sum_ / count_) : 0.0f; }

    // Value below which `fraction` of the samples fall. Values that landed
    // outside the histogram range are counted, so a percentile that falls in the
    // overflow returns the range limit rather than lying.
    float percentile (float fraction) const noexcept
    {
        if (count_ <= 0)
            return 0.0f;

        const int target = static_cast<int> (fraction * static_cast<float> (count_));
        int cumulative = below_;
        if (cumulative > target)
            return min_;

        for (int b = 0; b < kBins; ++b)
        {
            cumulative += bins_[static_cast<size_t> (b)];
            if (cumulative > target)
                return min_ + (static_cast<float> (b) + 0.5f) / scale_;
        }
        return max_;
    }

private:
    float min_ { 0.0f }, max_ { 1.0f }, scale_ { 1.0f };
    int   bins_[kBins] {};
    int   count_ { 0 }, below_ { 0 }, above_ { 0 };
    double sum_ { 0.0 };
};

// ---------------------------------------------------------------------------
struct RoomMode
{
    float freqHz { 0.0f };
    float strengthDb { 0.0f };     // peak prominence over the local floor
    float engagedSeconds { 0.0f }; // how long the canceller was working on it
    int   hits { 0 };
};

struct VoiceProfile
{
    bool  valid { false };
    int   version { 1 };
    double sampleRate { 48000.0 };

    // --- room noise phase ---
    bool  hasNoise { false };
    float bandNoiseDb[kNumBands] {};
    float broadbandNoiseDbFS { -90.0f };

    // --- voice phase ---
    bool  hasVoice { false };
    float bandVoiceDb[kNumBands] {};     // long-term average spectrum
    float f0LowHz { 0.0f }, f0MedianHz { 0.0f }, f0HighHz { 0.0f };

    // Criterion distributions measured on this voice. These are the numbers that
    // matter: each suggested threshold sits just outside what the voice does.
    float voicePnprP95Db { 0.0f };
    float voicePhprP95Db { 0.0f };
    float voiceFsdP05Hz { 0.0f };        // low percentile: voice rarely steadier
    float voiceFsdMedianHz { 0.0f };
    // How far this voice's peaks stand above their spectral neighbourhood. This is
    // the criterion that does the most work at runtime, because a peak that fails
    // it is never even evaluated against the others. A low male voice with closely
    // spaced harmonics barely produces isolated peaks at all; a soprano, whose
    // harmonics are hundreds of hertz apart, produces them constantly - and that is
    // exactly the voice most at risk of being mistaken for feedback.
    float voiceProminenceP95Db { 0.0f };
    float voiceProminenceMedianDb { 0.0f };
    int   voiceCriterionSamples { 0 };

    // --- suggested thresholds, derived from the above ---
    bool  hasSuggestions { false };
    float suggestedPnprDb { 10.0f };
    float suggestedPhprDb { 8.0f };
    float suggestedFsdMaxHz { 2.5f };
    float suggestedLocalProminenceDb { 12.0f };
    float suggestedAbsoluteFloorDb { -96.0f };
    float suggestedVoiceProtection[kNumBands] {};

    // --- room modes phase ---
    int      numModes { 0 };
    RoomMode modes[kMaxRoomModes] {};
};

// ---------------------------------------------------------------------------
enum class CalibrationPhase
{
    idle,
    roomNoise,
    voice,
    roomModes
};

class Calibrator
{
public:
    void prepare (double sampleRate, const ErbBands& bands);
    void reset() noexcept;

    void begin (CalibrationPhase phase) noexcept;
    void cancel() noexcept;
    // Ends the current phase and folds its results into the working profile.
    void finish() noexcept;

    CalibrationPhase phase() const noexcept { return phase_; }
    bool isRunning() const noexcept { return phase_ != CalibrationPhase::idle; }
    float elapsedSeconds() const noexcept;
    float progress() const noexcept;   // 0..1 against the recommended duration

    // Once per analysis frame, with the detector so its criteria can be sampled.
    void processFrame (const float* power, const float* magnitude,
                       const HowlDetector& detector) noexcept;

    // Room-mode phase: report a tone the canceller is currently working on.
    void reportEngagedTone (float freqHz, float strengthDb, float seconds) noexcept;

    const VoiceProfile& profile() const noexcept { return profile_; }
    VoiceProfile& mutableProfile() noexcept { return profile_; }

    // Recompute suggestedX from whatever phases have completed.
    void deriveSuggestions() noexcept;

    static float recommendedSeconds (CalibrationPhase p) noexcept
    {
        switch (p)
        {
            case CalibrationPhase::roomNoise: return 15.0f;
            case CalibrationPhase::voice:     return 45.0f;
            case CalibrationPhase::roomModes: return 90.0f;
            default:                          return 0.0f;
        }
    }

private:
    void accumulateNoise (const float* power) noexcept;
    void accumulateVoice (const float* power, const float* magnitude,
                          const HowlDetector& detector) noexcept;
    float estimateF0 (const float* magnitude) const noexcept;

    const ErbBands* bands_ { nullptr };
    double sampleRate_ { 48000.0 };
    CalibrationPhase phase_ { CalibrationPhase::idle };
    long long frames_ { 0 };

    VoiceProfile profile_ {};

    std::vector<float> bandAccum_, bandScratch_;
    double frameCount_ { 0.0 };

    Histogram pnpr_, phpr_, fsd_, f0_, prominence_;

    // Room-mode accumulation.
    RoomMode modeAccum_[kMaxRoomModes] {};
    int numModeAccum_ { 0 };
};
} // namespace fbk
