#include "Calibration.h"

namespace fbk
{
void Calibrator::prepare (double sampleRate, const ErbBands& bands)
{
    bands_ = &bands;
    sampleRate_ = sampleRate;

    bandAccum_.assign (kNumBands, 0.0f);
    bandScratch_.assign (kNumBands, 0.0f);

    // Ranges chosen to cover everything these criteria realistically produce, so
    // percentiles rarely land in the overflow bins.
    pnpr_.prepare (-20.0f, 60.0f);
    phpr_.prepare (-40.0f, 80.0f);
    fsd_.prepare (0.0f, 96.0f);        // Hz of frame-to-frame frequency wander
    f0_.prepare (50.0f, 500.0f);
    prominence_.prepare (0.0f, 48.0f);

    profile_ = VoiceProfile {};
    profile_.sampleRate = sampleRate;

    reset();
}

void Calibrator::reset() noexcept
{
    phase_ = CalibrationPhase::idle;
    frames_ = 0;
    frameCount_ = 0.0;
    std::fill (bandAccum_.begin(), bandAccum_.end(), 0.0f);
    pnpr_.reset();
    phpr_.reset();
    fsd_.reset();
    f0_.reset();
    prominence_.reset();
    numModeAccum_ = 0;
    for (auto& m : modeAccum_)
        m = RoomMode {};
}

void Calibrator::begin (CalibrationPhase p) noexcept
{
    phase_ = p;
    frames_ = 0;
    frameCount_ = 0.0;
    std::fill (bandAccum_.begin(), bandAccum_.end(), 0.0f);

    if (p == CalibrationPhase::voice)
    {
        pnpr_.reset();
        phpr_.reset();
        fsd_.reset();
        f0_.reset();
        prominence_.reset();
    }
    else if (p == CalibrationPhase::roomModes)
    {
        numModeAccum_ = 0;
        for (auto& m : modeAccum_)
            m = RoomMode {};
    }
}

void Calibrator::cancel() noexcept
{
    phase_ = CalibrationPhase::idle;
    frames_ = 0;
}

float Calibrator::elapsedSeconds() const noexcept
{
    return static_cast<float> (static_cast<double> (frames_) * kHopSize / sampleRate_);
}

float Calibrator::progress() const noexcept
{
    const float target = recommendedSeconds (phase_);
    if (target <= 0.0f)
        return 0.0f;
    return clampf (elapsedSeconds() / target, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
void Calibrator::accumulateNoise (const float* power) noexcept
{
    bands_->binsToBands (power, bandScratch_.data());
    for (int b = 0; b < kNumBands; ++b)
        bandAccum_[static_cast<size_t> (b)] += bandScratch_[static_cast<size_t> (b)];
    frameCount_ += 1.0;
}

float Calibrator::estimateF0 (const float* magnitude) const noexcept
{
    // Harmonic product spectrum. Almost free here because the STFT already
    // exists, and robust enough for the range statistics we actually need - we
    // want to know roughly where this voice lives, not to track pitch for
    // resynthesis.
    const float binHz = bands_->binWidthHz();
    const int loBin = std::max (2, static_cast<int> (60.0f / binHz));
    const int hiBin = std::min (bands_->numBins() / 5, static_cast<int> (500.0f / binHz));

    float bestScore = 0.0f;
    int bestBin = 0;

    for (int k = loBin; k <= hiBin; ++k)
    {
        float score = 0.0f;
        for (int h = 1; h <= 5; ++h)
        {
            const int kh = k * h;
            if (kh >= bands_->numBins())
                break;
            score += magnitude[kh];
        }
        if (score > bestScore)
        {
            bestScore = score;
            bestBin = k;
        }
    }

    return bestBin > 0 ? static_cast<float> (bestBin) * binHz : 0.0f;
}

void Calibrator::accumulateVoice (const float* power, const float* magnitude,
                                  const HowlDetector& detector) noexcept
{
    bands_->binsToBands (power, bandScratch_.data());
    for (int b = 0; b < kNumBands; ++b)
        bandAccum_[static_cast<size_t> (b)] += bandScratch_[static_cast<size_t> (b)];
    frameCount_ += 1.0;

    // Sample the detector's criteria on whatever peaks it is currently tracking.
    // These are vocal harmonics by construction - the PA is down and the only
    // thing in the room is the voice - so this is a direct measurement of how
    // tone-like this particular voice looks to the detector.
    const TrackedTone* tones = detector.tones();
    for (int i = 0; i < detector.numTones(); ++i)
    {
        const auto& t = tones[i];
        if (! t.active || t.framesSinceSeen != 0)
            continue;

        // Four frames is what updateCriteria() itself needs before it produces a
        // valid freqDeviation, so that is the right bar here too. Requiring the
        // full eight-frame history collected nothing at all: a voice with vibrato
        // and pitch contour moves its upper harmonics far more than the tracker's
        // match window per frame, so tracks are constantly retired and recreated
        // and rarely survive eight frames. The peaks that matter for calibration
        // are exactly the short-lived, wandering ones.
        if (t.historyCount < 4)
            continue;

        pnpr_.add (t.pnprDb);
        phpr_.add (t.phprDb);
        prominence_.add (t.localProminenceDb);
        if (t.freqDeviation < 1.0e5f)
            fsd_.add (t.freqDeviation);
    }

    const float f0 = estimateF0 (magnitude);
    if (f0 > 60.0f)
        f0_.add (f0);
}

void Calibrator::processFrame (const float* power, const float* magnitude,
                               const HowlDetector& detector) noexcept
{
    if (phase_ == CalibrationPhase::idle)
        return;

    ++frames_;

    switch (phase_)
    {
        case CalibrationPhase::roomNoise: accumulateNoise (power); break;
        case CalibrationPhase::voice:     accumulateVoice (power, magnitude, detector); break;
        case CalibrationPhase::roomModes: break;   // driven by reportEngagedTone
        case CalibrationPhase::idle:      break;
    }
}

void Calibrator::reportEngagedTone (float freqHz, float strengthDb, float seconds) noexcept
{
    if (phase_ != CalibrationPhase::roomModes || freqHz <= 0.0f)
        return;

    // Merge into an existing mode if it is within 1.5%. Room modes are stable, so
    // a proportional window groups the same mode across gain changes without
    // merging genuinely distinct neighbours.
    for (int i = 0; i < numModeAccum_; ++i)
    {
        auto& m = modeAccum_[i];
        if (std::abs (m.freqHz - freqHz) < 0.015f * freqHz)
        {
            // Weighted mean frequency, so repeated hits sharpen the estimate.
            const float w = 1.0f / static_cast<float> (m.hits + 1);
            m.freqHz += w * (freqHz - m.freqHz);
            m.strengthDb = std::max (m.strengthDb, strengthDb);
            m.engagedSeconds += seconds;
            ++m.hits;
            return;
        }
    }

    if (numModeAccum_ < kMaxRoomModes)
    {
        auto& m = modeAccum_[numModeAccum_++];
        m.freqHz = freqHz;
        m.strengthDb = strengthDb;
        m.engagedSeconds = seconds;
        m.hits = 1;
        return;
    }

    // Table full: replace the weakest mode if this one is stronger.
    int weakest = 0;
    for (int i = 1; i < numModeAccum_; ++i)
        if (modeAccum_[i].engagedSeconds < modeAccum_[weakest].engagedSeconds)
            weakest = i;

    if (seconds > modeAccum_[weakest].engagedSeconds)
    {
        auto& m = modeAccum_[weakest];
        m.freqHz = freqHz;
        m.strengthDb = strengthDb;
        m.engagedSeconds = seconds;
        m.hits = 1;
    }
}

// ---------------------------------------------------------------------------
void Calibrator::finish() noexcept
{
    const auto finished = phase_;
    phase_ = CalibrationPhase::idle;

    if (finished == CalibrationPhase::roomNoise && frameCount_ > 0.0)
    {
        double broadband = 0.0;
        for (int b = 0; b < kNumBands; ++b)
        {
            const float meanPower = bandAccum_[static_cast<size_t> (b)]
                                  / static_cast<float> (frameCount_);
            profile_.bandNoiseDb[b] = 10.0f * std::log10 (meanPower + kEpsilon);
            broadband += static_cast<double> (meanPower);
        }
        // Undo the analysis window's gain so the figure is a real dBFS level
        // rather than an FFT-domain number.
        const double windowGain = 0.25 * static_cast<double> (kFftSize) * kFftSize;
        profile_.broadbandNoiseDbFS =
            10.0f * std::log10 (static_cast<float> (broadband / windowGain) + kEpsilon);
        profile_.hasNoise = true;
    }
    else if (finished == CalibrationPhase::voice && frameCount_ > 0.0)
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            const float meanPower = bandAccum_[static_cast<size_t> (b)]
                                  / static_cast<float> (frameCount_);
            profile_.bandVoiceDb[b] = 10.0f * std::log10 (meanPower + kEpsilon);
        }

        profile_.f0LowHz    = f0_.percentile (0.05f);
        profile_.f0MedianHz = f0_.percentile (0.50f);
        profile_.f0HighHz   = f0_.percentile (0.95f);

        profile_.voicePnprP95Db = pnpr_.percentile (0.95f);
        profile_.voicePhprP95Db = phpr_.percentile (0.95f);
        profile_.voiceFsdP05Hz  = fsd_.percentile (0.05f);
        profile_.voiceFsdMedianHz = fsd_.percentile (0.50f);
        profile_.voiceProminenceP95Db = prominence_.percentile (0.95f);
        profile_.voiceProminenceMedianDb = prominence_.percentile (0.50f);
        profile_.voiceCriterionSamples = fsd_.count();
        profile_.hasVoice = true;
    }
    else if (finished == CalibrationPhase::roomModes)
    {
        // Rank by engaged time: a mode the suppressor had to fight for longer is
        // the one more worth pre-arming against.
        for (int i = 0; i < numModeAccum_; ++i)
            for (int j = i + 1; j < numModeAccum_; ++j)
                if (modeAccum_[j].engagedSeconds > modeAccum_[i].engagedSeconds)
                    std::swap (modeAccum_[i], modeAccum_[j]);

        profile_.numModes = numModeAccum_;
        for (int i = 0; i < numModeAccum_; ++i)
            profile_.modes[i] = modeAccum_[i];
    }

    deriveSuggestions();
    profile_.valid = profile_.hasNoise || profile_.hasVoice || profile_.numModes > 0;
}

void Calibrator::deriveSuggestions() noexcept
{
    if (profile_.hasVoice && profile_.voiceCriterionSamples > 200)
    {
        // Place each threshold just outside what this voice actually does.
        //
        // PNPR and PHPR: sit above the 95th percentile of the voice, with a
        // margin, so a tone has to be more isolated and less harmonic than almost
        // anything the voice produced.
        profile_.suggestedPnprDb = std::max (4.0f, profile_.voicePnprP95Db + 2.0f);
        profile_.suggestedPhprDb = std::max (4.0f, profile_.voicePhprP95Db + 2.0f);

        // FSD is the reverse: a feedback tone is *steadier* than a voice, so the
        // threshold goes below the voice distribution. The 5th percentile is the
        // steadiest this voice ever gets; sit under it with a margin. Clamped to a
        // sane band so an unusually steady or unusually wobbly calibration cannot
        // produce a useless threshold.
        const float below = profile_.voiceFsdP05Hz * 0.6f;
        profile_.suggestedFsdMaxHz = clampf (below, 0.4f, 8.0f);

        // The prominence gate goes above what this voice produces, with a margin.
        // Clamped to a sane band in both directions: too low and vocal harmonics
        // reach the criteria stage unnecessarily, too high and genuinely weak
        // early feedback is never even considered. Feedback tones typically stand
        // 30-40 dB above their neighbourhood, so there is plenty of room between
        // the two.
        profile_.suggestedLocalProminenceDb =
            clampf (profile_.voiceProminenceP95Db + 3.0f, 8.0f, 20.0f);
    }

    if (profile_.hasNoise)
    {
        // Peak gate 6 dB above the measured floor, so it is expressed in this
        // room's terms rather than as a fixed guess.
        profile_.suggestedAbsoluteFloorDb = clampf (profile_.broadbandNoiseDbFS + 6.0f,
                                                    -120.0f, -30.0f);
    }

    if (profile_.hasVoice)
    {
        // Per-band voice protection from the long-term average spectrum: bands
        // that carry a lot of this voice get protected hard, bands that carry
        // little get protected less so the mask can still work there.
        float peak = -200.0f;
        for (int b = 0; b < kNumBands; ++b)
            peak = std::max (peak, profile_.bandVoiceDb[b]);

        for (int b = 0; b < kNumBands; ++b)
        {
            // 0 dB below peak -> 0.95, 40 dB below -> 0.35.
            const float belowPeak = peak - profile_.bandVoiceDb[b];
            const float t = clampf (belowPeak / 40.0f, 0.0f, 1.0f);
            profile_.suggestedVoiceProtection[b] = 0.95f - 0.60f * t;
        }
    }

    profile_.hasSuggestions = profile_.hasVoice || profile_.hasNoise;
}
} // namespace fbk
