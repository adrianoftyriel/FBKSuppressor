#include "TonalCanceller.h"

#include <numbers>

namespace fbk
{
// ===========================================================================
// SinusoidCanceller
// ===========================================================================
void SinusoidCanceller::prepare (double sampleRate) noexcept
{
    sampleRate_ = sampleRate;

    // Engage ramp ~8 ms: fast enough to catch a howl, slow enough not to click.
    engageCoeff_ = static_cast<float> (std::exp (-1.0 / (0.008 * sampleRate)));
    levelCoeff_  = static_cast<float> (std::exp (-1.0 / (0.050 * sampleRate)));

    // Frequency glide ~5 ms towards the detector's estimate.
    freqGlide_ = 1.0f - static_cast<float> (std::exp (-1.0 / (0.005 * sampleRate)));

    // Input power estimate for NLMS normalisation, ~30 ms.
    powerCoeff_ = static_cast<float> (std::exp (-1.0 / (0.030 * sampleRate)));

    // The PLL correction is applied every pllInterval_ samples. Its gain is set
    // so that a 1 Hz frequency error is corrected in roughly 50 ms.
    pllInterval_ = 64;
    pllGain_     = 0.25f;

    reset();
}

void SinusoidCanceller::reset() noexcept
{
    oscR_ = 1.0f; oscI_ = 0.0f;
    wr_ = wi_ = 0.0f;
    prevWr_ = prevWi_ = 0.0f;
    pllRate_ = 0.0f;
    pllCounter_ = 0;
    renormCounter_ = 0;
    engage_ = 0.0f;
    inLevel_ = outLevel_ = 0.0f;
    powerEst_ = 0.0f;
    attenDb_ = 0.0f;
    updateRotation();
}

void SinusoidCanceller::updateRotation() noexcept
{
    const double w = 2.0 * std::numbers::pi * static_cast<double> (freqHz_) / sampleRate_;
    rotR_ = static_cast<float> (std::cos (w));
    rotI_ = static_cast<float> (std::sin (w));
}

void SinusoidCanceller::setFrequency (float hz) noexcept
{
    targetFreqHz_ = clampf (hz, 10.0f, static_cast<float> (sampleRate_ * 0.49));
}

void SinusoidCanceller::setFrequencyImmediate (float hz) noexcept
{
    targetFreqHz_ = clampf (hz, 10.0f, static_cast<float> (sampleRate_ * 0.49));
    freqHz_ = targetFreqHz_;
    wr_ = wi_ = 0.0f;
    pllRate_ = 0.0f;
    updateRotation();
}

float SinusoidCanceller::process (float x) noexcept
{
    // Engage ramp.
    engage_ = engageTarget_ + engageCoeff_ * (engage_ - engageTarget_);

    if (engage_ < 1.0e-5f && engageTarget_ <= 0.0f && ! alwaysAdapt_)
    {
        // Fully disengaged: let the weight decay so a re-engage starts clean,
        // and pass audio through untouched.
        wr_ *= 0.999f;
        wi_ *= 0.999f;
        powerEst_ = powerCoeff_ * powerEst_ + (1.0f - powerCoeff_) * x * x;
        return x;
    }

    // Advance the oscillator.
    const float nr = oscR_ * rotR_ - oscI_ * rotI_;
    const float ni = oscR_ * rotI_ + oscI_ * rotR_;
    oscR_ = nr;
    oscI_ = ni;

    if (++renormCounter_ >= 256)
    {
        renormCounter_ = 0;
        const float mag = std::sqrt (oscR_ * oscR_ + oscI_ * oscI_);
        if (mag > 1.0e-6f)
        {
            const float inv = 1.0f / mag;
            oscR_ *= inv;
            oscI_ *= inv;
        }
        else
        {
            oscR_ = 1.0f;
            oscI_ = 0.0f;
        }
    }

    // Running input power, for step normalisation.
    powerEst_ = powerCoeff_ * powerEst_ + (1.0f - powerCoeff_) * x * x;

    // Synthesised sinusoid: 2 * Re(w * osc). The adaptation always works on the
    // full residual (x - est) even when engagement is partial, so the weight
    // converges on the true component rather than on a fraction of it.
    const float est = 2.0f * (wr_ * oscR_ - wi_ * oscI_);
    const float adaptErr = x - est;
    const float out = x - est * engage_;

    // Power-normalised leaky LMS. The gradient uses conj(osc).
    const float g = mu_ * adaptErr / (2.0f * powerEst_ + 1.0e-9f);
    wr_ = (1.0f - leak_) * wr_ + g * oscR_;
    wi_ = (1.0f - leak_) * wi_ - g * oscI_;

    // Hard bound on the weight. A sinusoidal component cannot legitimately be
    // larger than a few times the signal's own RMS, so anything beyond that is a
    // divergence and is clamped rather than allowed to run away.
    const float maxAmp = 4.0f * std::sqrt (2.0f * powerEst_) + 1.0e-6f;
    const float wMag = 2.0f * std::sqrt (wr_ * wr_ + wi_ * wi_);
    if (wMag > maxAmp)
    {
        const float scale = maxAmp / wMag;
        wr_ *= scale;
        wi_ *= scale;
    }
    wr_ = sanitise (wr_);
    wi_ = sanitise (wi_);

    // --- Frequency lock ----------------------------------------------------
    if (pllRangeHz_ > 0.0f && ++pllCounter_ >= pllInterval_)
    {
        pllCounter_ = 0;
        const float magSq = wr_ * wr_ + wi_ * wi_;
        if (magSq > 1.0e-12f)
        {
            // Rotation of the weight over the interval: angle(w * conj(prevW)).
            const float cr = wr_ * prevWr_ + wi_ * prevWi_;
            const float ci = wi_ * prevWr_ - wr_ * prevWi_;
            if (std::abs (cr) + std::abs (ci) > 1.0e-15f)
            {
                const float dPhase = std::atan2 (ci, cr);
                // A residual frequency error of df Hz rotates the weight by
                // 2*pi*df*interval/fs radians per interval.
                const float dfHz = dPhase * static_cast<float> (sampleRate_)
                                 / (2.0f * std::numbers::pi_v<float> * static_cast<float> (pllInterval_));
                // Only trust corrections within half a bin; anything larger is
                // a mis-track rather than a lock error.
                const float maxCorrection = 0.5f * static_cast<float> (sampleRate_) / static_cast<float> (kFftSize);
                pllRate_ += pllGain_ * clampf (dfHz, -maxCorrection, maxCorrection);
                pllRate_ = clampf (pllRate_, -pllRangeHz_, pllRangeHz_);
            }
        }
        else
        {
            pllRate_ *= 0.9f;
        }
        prevWr_ = wr_;
        prevWi_ = wi_;
    }

    // Glide towards the detector's frequency plus the PLL correction.
    const float desired = targetFreqHz_ + pllRate_;
    const float newFreq = freqHz_ + freqGlide_ * (desired - freqHz_);
    if (std::abs (newFreq - freqHz_) > 1.0e-4f)
    {
        freqHz_ = clampf (newFreq, 10.0f, static_cast<float> (sampleRate_ * 0.49));
        updateRotation();
    }

    // Attenuation metering.
    inLevel_  = levelCoeff_ * inLevel_  + (1.0f - levelCoeff_) * x * x;
    outLevel_ = levelCoeff_ * outLevel_ + (1.0f - levelCoeff_) * out * out;
    attenDb_  = 10.0f * std::log10 ((outLevel_ + kEpsilon) / (inLevel_ + kEpsilon));

    return out;
}

// ===========================================================================
// TonalCanceller
// ===========================================================================
void TonalCanceller::prepare (double sampleRate) noexcept
{
    sampleRate_ = sampleRate;
    cancellers_.resize (kMaxTones);
    assignedFreq_.assign (kMaxTones, 0.0f);
    inUse_.assign (kMaxTones, false);
    for (auto& c : cancellers_)
        c.prepare (sampleRate);
    reset();
}

void TonalCanceller::reset() noexcept
{
    for (auto& c : cancellers_)
    {
        c.reset();
        c.setEngage (0.0f);
    }
    std::fill (assignedFreq_.begin(), assignedFreq_.end(), 0.0f);
    std::fill (inUse_.begin(), inUse_.end(), false);
    numEngaged_ = 0;
}

void TonalCanceller::updateFromDetector (const TrackedTone* tones, int numTones, float depth) noexcept
{
    numEngaged_ = 0;
    const int n = std::min (numTones, static_cast<int> (cancellers_.size()));

    for (int i = 0; i < n; ++i)
    {
        const auto& t = tones[i];
        auto& c = cancellers_[static_cast<size_t> (i)];

        if (! t.active || t.confidence <= 0.005f)
        {
            c.setEngage (0.0f);
            inUse_[static_cast<size_t> (i)] = false;
            continue;
        }

        // A brand-new assignment, or the track has moved a long way: snap the
        // oscillator rather than glide, so we do not sweep through the audio.
        if (! inUse_[static_cast<size_t> (i)]
            || std::abs (assignedFreq_[static_cast<size_t> (i)] - t.freqHz) > 50.0f)
        {
            c.setFrequencyImmediate (t.freqHz);
        }
        else
        {
            c.setFrequency (t.freqHz);
        }

        assignedFreq_[static_cast<size_t> (i)] = t.freqHz;
        inUse_[static_cast<size_t> (i)] = true;

        // Step size scales with confidence: a tentative track adapts slowly and
        // therefore narrowly, so a false positive can do very little damage
        // before the detector drops it. At full confidence mu = 1.2e-3 gives a
        // ~40 ms convergence and a notch roughly 4 Hz wide.
        const float mu = 2.0e-4f + 1.0e-3f * t.confidence;
        c.setStepSize (mu);
        c.setLeak (2.0e-6f);
        // Enough range to pull onto the true frequency from the parabolic
        // estimate, but not so much that two nearby tones can collapse onto one.
        c.setPllRange (20.0f);
        c.setEngage (t.confidence * depth);
        ++numEngaged_;
    }
}

float TonalCanceller::process (float x) noexcept
{
    for (auto& c : cancellers_)
        x = c.process (x);
    return x;
}

float TonalCanceller::totalAttenuationDb() const noexcept
{
    float sum = 0.0f;
    for (const auto& c : cancellers_)
        sum += c.attenuationDb();
    return sum;
}

// ===========================================================================
// HumCanceller
// ===========================================================================
void HumCanceller::prepare (double sampleRate) noexcept
{
    sampleRate_ = sampleRate;
    harmonics_.resize (kMaxHumHarmonics);
    for (auto& h : harmonics_)
        h.prepare (sampleRate);

    // 0.5 s window: ~0.3 Hz bandwidth, which puts the other candidate 30 dB down.
    probe50_.prepare (sampleRate, 50.0f, 0.5f);
    probe60_.prepare (sampleRate, 60.0f, 0.5f);

    // Detection smoothing over roughly 300 ms of analysis frames.
    const double framesPerSecond = sampleRate / static_cast<double> (kHopSize);
    detectSmooth_ = static_cast<float> (std::exp (-1.0 / (0.30 * framesPerSecond)));

    reset();
}

void HumCanceller::reset() noexcept
{
    for (auto& h : harmonics_)
    {
        h.reset();
        h.setEngage (0.0f);
    }

    probe50_.reset();
    probe60_.reset();

    active_ = false;
    smooth50_ = smooth60_ = 0.0f;
    fundamentalHz_ = 50.0f;
    holdFrames_ = 0;
}

void HumCanceller::setNumHarmonics (int n) noexcept
{
    numHarmonics_ = std::clamp (n, 1, kMaxHumHarmonics);
}

void HumCanceller::setDepth (float d) noexcept
{
    depth_ = clampf (d, 0.0f, 1.0f);
}

void HumCanceller::updateDetection() noexcept
{
    if (! enabled_)
    {
        for (auto& h : harmonics_)
            h.setEngage (0.0f);
        active_ = false;
        return;
    }

    const float c50 = probe50_.coherenceRatio();
    const float c60 = probe60_.coherenceRatio();
    smooth50_ = detectSmooth_ * smooth50_ + (1.0f - detectSmooth_) * c50;
    smooth60_ = detectSmooth_ * smooth60_ + (1.0f - detectSmooth_) * c60;

    const bool prefer60 = smooth60_ > smooth50_;
    const float winner = prefer60 ? smooth60_ : smooth50_;
    const float loser  = prefer60 ? smooth50_ : smooth60_;

    // Two conditions to engage. The coherent component at the winning frequency
    // must be a meaningful fraction of the broadband level, and it must beat the
    // other candidate clearly - if both score similarly there is broadband
    // low-frequency energy rather than hum, and we leave it alone.
    const bool strongEnough = winner > 0.02f;
    const bool decisive     = winner > 2.0f * loser + 0.005f;

    if (strongEnough && decisive)
    {
        fundamentalHz_ = prefer60 ? 60.0f : 50.0f;
        holdFrames_ = 200;              // ~0.5 s of hysteresis
        active_ = true;
    }
    else if (holdFrames_ > 0)
    {
        --holdFrames_;
    }
    else
    {
        active_ = false;
    }

    const float engage = active_ ? depth_ : 0.0f;
    for (int m = 0; m < kMaxHumHarmonics; ++m)
    {
        auto& h = harmonics_[static_cast<size_t> (m)];
        if (m >= numHarmonics_)
        {
            h.setEngage (0.0f);
            continue;
        }

        const float f = fundamentalHz_ * static_cast<float> (m + 1);
        if (f > static_cast<float> (sampleRate_ * 0.45))
        {
            h.setEngage (0.0f);
            continue;
        }

        h.setFrequency (f);
        // Higher harmonics get a slightly larger step because absolute frequency
        // drift in the mains scales with harmonic number.
        h.setStepSize (6.0e-4f * (1.0f + 0.15f * static_cast<float> (m)));
        h.setLeak (5.0e-7f);
        h.setPllRange (0.5f * static_cast<float> (m + 1));
        h.setEngage (engage);
    }
}

float HumCanceller::process (float x) noexcept
{
    if (! enabled_)
        return x;

    // Probes first, on the unmodified input: they must measure what is arriving,
    // not what is left after cancellation, or they would switch off their own
    // detection as soon as it succeeded.
    probe50_.process (x);
    probe60_.process (x);

    for (int m = 0; m < numHarmonics_; ++m)
        x = harmonics_[static_cast<size_t> (m)].process (x);
    return x;
}
} // namespace fbk
