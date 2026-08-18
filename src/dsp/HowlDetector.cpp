#include "HowlDetector.h"

namespace fbk
{
void HowlDetector::prepare (double sampleRate, const ErbBands& bands)
{
    bands_      = &bands;
    sampleRate_ = sampleRate;
    numBins_    = bands.numBins();
    binWidthHz_ = bands.binWidthHz();

    base_ = HowlThresholds {};
    thr_  = base_;

    minBin_ = std::max (1, static_cast<int> (std::floor (thr_.minFreqHz / binWidthHz_)));
    maxBin_ = std::min (numBins_ - 2,
                        static_cast<int> (std::floor (static_cast<float> (numBins_ - 1)
                                                      * thr_.maxFreqNyquistFraction)));

    // A tone is "the same tone" as last frame if it lands within ~1.5 bins.
    freqMatchHz_ = 1.5f * binWidthHz_;

    const double framesPerSecond = sampleRate / static_cast<double> (kHopSize);
    confAttack_  = static_cast<float> (std::exp (-1.0 / (1.0e-3 * base_.confidenceAttackMs  * framesPerSecond)));
    confRelease_ = static_cast<float> (std::exp (-1.0 / (1.0e-3 * base_.confidenceReleaseMs * framesPerSecond)));

    calibrated_ = false;
    observeOnly_ = false;
    numPriors_ = 0;
    tones_.assign (kMaxTones, TrackedTone {});
    localFloor_.assign (static_cast<size_t> (numBins_), 0.0f);
    candidates_.assign (kMaxCandidates, Candidate {});
    reset();
}

void HowlDetector::reset() noexcept
{
    for (auto& t : tones_)
        t = TrackedTone {};
    numConfirmed_ = 0;
}

void HowlDetector::setSensitivity (float s) noexcept
{
    s = clampf (s, 0.0f, 1.0f);
    sensitivity_ = s;
    const HowlThresholds& base = calibrated_ ? calibrated_thr_ : base_;
    // Higher sensitivity lowers every ratio threshold and shortens the required
    // persistence. The span is deliberately modest: pushing the thresholds too
    // far down starts catching sustained vocal notes, which is the one failure
    // mode that would audibly damage a voice.
    const float shift = (0.5f - s) * 8.0f;              // +/- 4 dB
    thr_ = base;
    thr_.paprDb = base.paprDb + shift;
    thr_.pnprDb = base.pnprDb + shift;
    thr_.phprDb = base.phprDb + shift;
    thr_.ipmpFrames = std::max (3, static_cast<int> (std::lround (
                          static_cast<float> (base.ipmpFrames) * (1.6f - 1.2f * s))));
    thr_.imsdMax = base.imsdMax * (0.6f + 0.9f * s);
    thr_.fsdMaxHz = base.fsdMaxHz * (0.5f + 1.2f * s);
    thr_.fsdMaxFraction = base.fsdMaxFraction * (0.5f + 1.2f * s);
}

void HowlDetector::setCalibratedThresholds (float pnprDb, float phprDb, float fsdMaxHz,
                                            float absoluteFloorDb,
                                            float localProminenceDb) noexcept
{
    calibrated_thr_ = base_;
    if (pnprDb > 0.0f)            calibrated_thr_.pnprDb = pnprDb;
    if (phprDb > 0.0f)            calibrated_thr_.phprDb = phprDb;
    if (fsdMaxHz > 0.0f)          calibrated_thr_.fsdMaxHz = fsdMaxHz;
    if (absoluteFloorDb < 0.0f)   calibrated_thr_.absoluteFloorDb = absoluteFloorDb;
    if (localProminenceDb > 0.0f) calibrated_thr_.localProminenceDb = localProminenceDb;
    calibrated_ = true;

    // Re-apply the sensitivity control on top of the new base.
    const HowlThresholds savedBase = base_;
    base_ = calibrated_thr_;
    setSensitivity (sensitivity_);
    base_ = savedBase;
}

void HowlDetector::clearCalibratedThresholds() noexcept
{
    calibrated_ = false;
    setSensitivity (sensitivity_);
}

void HowlDetector::setModePriors (const float* freqsHz, int count) noexcept
{
    numPriors_ = std::min (count, kMaxPriors);
    for (int i = 0; i < numPriors_; ++i)
        priors_[i] = freqsHz[i];
}

bool HowlDetector::isNearPrior (float freqHz) const noexcept
{
    for (int i = 0; i < numPriors_; ++i)
        if (std::abs (priors_[i] - freqHz) < 0.015f * freqHz)
            return true;
    return false;
}

int HowlDetector::findOrCreateSlot (float freqHz) noexcept
{
    // Existing track within the match window?
    for (int i = 0; i < kMaxTones; ++i)
        if (tones_[static_cast<size_t> (i)].active
            && std::abs (tones_[static_cast<size_t> (i)].freqHz - freqHz) <= freqMatchHz_)
            return i;

    // Free slot?
    for (int i = 0; i < kMaxTones; ++i)
        if (! tones_[static_cast<size_t> (i)].active)
            return i;

    // Otherwise evict the weakest unconfirmed track.
    int worst = -1;
    float worstMag = 0.0f;
    for (int i = 0; i < kMaxTones; ++i)
    {
        const auto& t = tones_[static_cast<size_t> (i)];
        if (t.confirmed)
            continue;
        if (worst < 0 || t.magnitude < worstMag)
        {
            worst = i;
            worstMag = t.magnitude;
        }
    }
    return worst;
}

void HowlDetector::updateCriteria (TrackedTone& t, const float* magnitude, float frameAvgPower,
                                   float measuredFreqHz) noexcept
{
    const int k = t.bin;
    const float peakPower = magnitude[k] * magnitude[k];

    // --- PAPR -------------------------------------------------------------
    t.paprDb = 10.0f * std::log10 ((peakPower + kEpsilon) / (frameAvgPower + kEpsilon));

    // --- PNPR: peak against its skirts a few bins out ----------------------
    // Offsets start at 3 bins so we measure outside the window's own main lobe;
    // an undamped sinusoid is already at the floor there, a vibrato-modulated
    // vocal harmonic is not.
    float neighbourPower = 0.0f;
    int   neighbourCount = 0;
    for (int d = 3; d <= 6; ++d)
    {
        if (k - d >= 1)               { neighbourPower += magnitude[k - d] * magnitude[k - d]; ++neighbourCount; }
        if (k + d <= numBins_ - 1)    { neighbourPower += magnitude[k + d] * magnitude[k + d]; ++neighbourCount; }
    }
    neighbourPower /= static_cast<float> (std::max (1, neighbourCount));
    t.pnprDb = 10.0f * std::log10 ((peakPower + kEpsilon) / (neighbourPower + kEpsilon));

    // --- PHPR: peak against its subharmonics -------------------------------
    // If this peak belongs to a harmonic series (i.e. a voice), at least one of
    // k/2 or k/3 carries real energy.
    float subPower = 0.0f;
    for (int m = 2; m <= 5; ++m)
    {
        const int ks = k / m;
        if (ks < 1)
            continue;
        // Take the best of a small neighbourhood: the subharmonic need not land
        // exactly on a bin centre.
        float best = 0.0f;
        for (int d = -1; d <= 1; ++d)
        {
            const int kk = ks + d;
            if (kk >= 1 && kk <= numBins_ - 1)
                best = std::max (best, magnitude[kk] * magnitude[kk]);
        }
        subPower = std::max (subPower, best);
    }
    t.phprDb = 10.0f * std::log10 ((peakPower + kEpsilon) / (subPower + kEpsilon));

    // --- IPMP / IMSD -------------------------------------------------------
    const float logMag = 20.0f * std::log10 (magnitude[k] + 1.0e-12f);

    if (t.historyCount < TrackedTone::kHistory)
    {
        t.logMagHistory[t.historyCount] = logMag;
        t.freqHistory[t.historyCount] = measuredFreqHz;
        ++t.historyCount;
    }
    else
    {
        for (int i = 1; i < TrackedTone::kHistory; ++i)
        {
            t.logMagHistory[i - 1] = t.logMagHistory[i];
            t.freqHistory[i - 1]   = t.freqHistory[i];
        }
        t.logMagHistory[TrackedTone::kHistory - 1] = logMag;
        t.freqHistory[TrackedTone::kHistory - 1]   = measuredFreqHz;
    }

    if (t.historyCount >= 4)
    {
        // Frame-to-frame dB slope: mean tells us whether it is building, and the
        // standard deviation is the IMSD statistic.
        const int n = t.historyCount - 1;
        float sum = 0.0f;
        for (int i = 0; i < n; ++i)
            sum += t.logMagHistory[i + 1] - t.logMagHistory[i];
        const float mean = sum / static_cast<float> (n);

        float var = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float d = (t.logMagHistory[i + 1] - t.logMagHistory[i]) - mean;
            var += d * d;
        }
        t.slopeMeanDb    = mean;
        t.slopeDeviation = std::sqrt (var / static_cast<float> (n));
    }
    else
    {
        t.slopeMeanDb    = 0.0f;
        t.slopeDeviation = 1.0e6f;   // not enough history to judge
    }

    // --- FSD: standard deviation of the measured peak frequency --------------
    if (t.historyCount >= 4)
    {
        float sum = 0.0f;
        for (int i = 0; i < t.historyCount; ++i)
            sum += t.freqHistory[i];
        const float mean = sum / static_cast<float> (t.historyCount);

        float var = 0.0f;
        for (int i = 0; i < t.historyCount; ++i)
        {
            const float d = t.freqHistory[i] - mean;
            var += d * d;
        }
        t.freqDeviation = std::sqrt (var / static_cast<float> (t.historyCount));
    }
    else
    {
        t.freqDeviation = 1.0e6f;
    }
}

void HowlDetector::process (const float* magnitude) noexcept
{
    // Frame average power, excluding DC.
    float frameAvgPower = 0.0f;
    for (int k = 1; k < numBins_; ++k)
        frameAvgPower += magnitude[k] * magnitude[k];
    frameAvgPower /= static_cast<float> (std::max (1, numBins_ - 1));

    for (auto& t : tones_)
        t.framesSinceSeen++;

    // --- Local spectral floor ----------------------------------------------
    // A moving average of power over +/- localFloorHalfWidth bins, by running
    // sum, so this is O(numBins) rather than O(numBins * width).
    //
    // This replaces what was originally a test against the minimum-statistics
    // noise estimate, which turned out to be exactly wrong for this purpose.
    // Minimum statistics tracks the running minimum of each bin, and a sustained
    // feedback tone *is* stationary - so after a second or two the noise estimate
    // at the howling bin rises to meet the howl and the gate rejects the very
    // thing it was looking for. Measured: the tone was tracked for 838 frames and
    // then silently dropped. A local floor asks the right question instead - does
    // this peak stand out from its spectral neighbourhood - and a single narrow
    // peak contributes only ~2% to a 49-bin average, so it cannot mask itself.
    //
    // It also discriminates for free: an isolated tone towers over its
    // neighbourhood, whereas a harmonic of a voice sits in a comb of similar
    // peaks that raises the local floor around it.
    {
        const int hw = thr_.localFloorHalfWidth;
        double sum = 0.0;
        int count = 0;
        for (int k = 0; k <= std::min (hw, numBins_ - 1); ++k)
        {
            sum += static_cast<double> (magnitude[k]) * magnitude[k];
            ++count;
        }
        for (int k = 0; k < numBins_; ++k)
        {
            localFloor_[static_cast<size_t> (k)] =
                static_cast<float> (sum / static_cast<double> (std::max (1, count)));

            const int add = k + hw + 1;
            const int drop = k - hw;
            if (add < numBins_)
            {
                sum += static_cast<double> (magnitude[add]) * magnitude[add];
                ++count;
            }
            if (drop >= 0)
            {
                sum -= static_cast<double> (magnitude[drop]) * magnitude[drop];
                --count;
            }
        }
    }

    // --- Candidate gathering -----------------------------------------------
    const float absoluteFloor = dbToGain (thr_.absoluteFloorDb);
    const float absoluteFloorPower = absoluteFloor * absoluteFloor
                                   * static_cast<float> (kFftSize) * 0.25f;
    const float prominenceDb = observeOnly_ ? observeProminenceDb_ : thr_.localProminenceDb;
    const float prominenceRatio = dbToGain (prominenceDb) * dbToGain (prominenceDb);

    int numCandidates = 0;
    for (int k = minBin_; k <= maxBin_ && numCandidates < kMaxCandidates; ++k)
    {
        const float m = magnitude[k];
        if (! (m > magnitude[k - 1] && m >= magnitude[k + 1]))
            continue;

        const float p = m * m;
        if (p < absoluteFloorPower)
            continue;

        const float floorPower = localFloor_[static_cast<size_t> (k)];
        if (p < floorPower * prominenceRatio)
            continue;

        // Parabolic interpolation on the log magnitude for sub-bin frequency.
        // Sub-bin accuracy is what makes the frequency-stability criterion
        // possible at all, and it gives the canceller's phase-locked loop a
        // starting point within a few hertz.
        const float a = 20.0f * std::log10 (magnitude[k - 1] + 1.0e-12f);
        const float b = 20.0f * std::log10 (m + 1.0e-12f);
        const float c = 20.0f * std::log10 (magnitude[k + 1] + 1.0e-12f);
        const float denom = a - 2.0f * b + c;
        float delta = 0.0f;
        if (std::abs (denom) > 1.0e-9f)
            delta = clampf (0.5f * (a - c) / denom, -0.5f, 0.5f);

        auto& cand = candidates_[static_cast<size_t> (numCandidates++)];
        cand.bin = k;
        cand.freqHz = (static_cast<float> (k) + delta) * binWidthHz_;
        cand.prominence = p / (floorPower + kEpsilon);
    }

    // Strongest candidates claim slots first, by selection sort over a small
    // fixed array. Without this, a dozen incidental peaks can evict a real track
    // simply by being encountered earlier in bin order.
    for (int i = 0; i < numCandidates; ++i)
    {
        int best = i;
        for (int j = i + 1; j < numCandidates; ++j)
            if (candidates_[static_cast<size_t> (j)].prominence
                > candidates_[static_cast<size_t> (best)].prominence)
                best = j;
        if (best != i)
            std::swap (candidates_[static_cast<size_t> (i)], candidates_[static_cast<size_t> (best)]);
    }

    for (int i = 0; i < numCandidates; ++i)
    {
        const auto& cand = candidates_[static_cast<size_t> (i)];

        const int slot = findOrCreateSlot (cand.freqHz);
        if (slot < 0)
            continue;

        auto& t = tones_[static_cast<size_t> (slot)];

        if (! t.active || std::abs (t.freqHz - cand.freqHz) > freqMatchHz_)
        {
            // New track.
            t = TrackedTone {};
            t.active = true;
            t.freqHz = cand.freqHz;
        }
        else if (t.framesSinceSeen == 0)
        {
            // Already updated this frame by a stronger candidate: two peaks fell
            // inside one match window. Leave the stronger one alone.
            continue;
        }
        else
        {
            // Continuing track: ease the frequency estimate towards the new
            // measurement so a slow drift is followed without jitter.
            t.freqHz += 0.25f * (cand.freqHz - t.freqHz);
        }

        t.bin = cand.bin;
        t.magnitude = magnitude[cand.bin];
        t.localProminenceDb = 10.0f * std::log10 (cand.prominence + kEpsilon);
        t.framesSinceSeen = 0;
        t.persistence = std::min (t.persistence + 1, 100000);

        updateCriteria (t, magnitude, frameAvgPower, cand.freqHz);
    }

    // --- Confirmation and confidence --------------------------------------
    numConfirmed_ = 0;
    for (auto& t : tones_)
    {
        if (! t.active)
            continue;

        // Drop a track that has not been seen for ~90 ms of frames, but only
        // once its confidence has decayed, so cancellation fades rather than
        // stops dead.
        if (t.framesSinceSeen > 0)
        {
            t.confidence = t.confidence * confRelease_;
            if (t.framesSinceSeen > 32 && t.confidence < 0.01f)
            {
                t = TrackedTone {};
                continue;
            }
            if (t.confidence > 0.005f)
                ++numConfirmed_;
            continue;
        }

        // A candidate on a known room mode needs less evidence: the persistence
        // requirement drops and the ratio thresholds relax slightly. This is what
        // the profiling phase buys - faster engagement where we already know the
        // room rings, with no pre-emptive attenuation anywhere.
        const bool onPrior = isNearPrior (t.freqHz);
        const float priorRelaxDb = onPrior ? 3.0f : 0.0f;
        const int   priorIpmp = onPrior
                              ? std::max (3, static_cast<int> (std::lround (
                                    static_cast<float> (thr_.ipmpFrames) * 0.4f)))
                              : thr_.ipmpFrames;

        const bool papr = t.paprDb > thr_.paprDb - priorRelaxDb;
        const bool pnpr = t.pnprDb > thr_.pnprDb - priorRelaxDb;
        const bool phpr = t.phprDb > thr_.phprDb - priorRelaxDb;
        const bool ipmp = t.persistence >= priorIpmp;
        // IMSD only counts as evidence when the tone is actually building or
        // holding steady, not decaying.
        const bool imsd = t.slopeDeviation < thr_.imsdMax && t.slopeMeanDb > -0.5f;
        // FSD: the tone must be standing still, in both absolute and relative
        // terms. The proportional limit is what stops a low-frequency howl being
        // held to an unreasonably tight absolute tolerance.
        const float fsdLimit = std::max (thr_.fsdMaxHz, thr_.fsdMaxFraction * t.freqHz);
        const bool fsd = t.freqDeviation < fsdLimit;

        t.confirmed = ! observeOnly_ && papr && pnpr && phpr && ipmp && imsd && fsd;

        const float target = t.confirmed ? 1.0f : 0.0f;
        const float c = target > t.confidence ? confAttack_ : confRelease_;
        t.confidence = target + c * (t.confidence - target);

        if (t.confidence > 0.005f)
            ++numConfirmed_;
    }
}
} // namespace fbk
