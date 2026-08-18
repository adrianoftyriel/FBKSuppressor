#include "MaskFilter.h"


namespace fbk
{
MaskFilter::MaskFilter() : designFft_ (kDesignOrder) {}

void MaskFilter::prepare (double sampleRate, const ErbBands& bands)
{
    bands_      = &bands;
    sampleRate_ = sampleRate;
    numBins_    = bands.numBins();

    bandPower_.assign (kNumBands, 0.0f);
    bandNoise_.assign (kNumBands, 0.0f);
    priorSnr_.assign (kNumBands, 1.0f);
    prevGain_.assign (kNumBands, 1.0f);
    bandGain_.assign (kNumBands, 1.0f);
    binGain_.assign (static_cast<size_t> (numBins_), 1.0f);

    powerHistory_.assign (kReverbDelayFrames + 1, std::vector<float> (kNumBands, 0.0f));
    lateReverb_.assign (kNumBands, 0.0f);

    designMag_.assign (kDesignSize / 2 + 1, 1.0f);
    designSpec_.assign (kDesignSize, Complex {});
    cepstrum_.assign (kDesignSize, 0.0f);

    // Size the tap buffers once, for the largest mode.
    coeffs_.assign (static_cast<size_t> (kMaxTapsAnyMode), 0.0f);
    targetCoeffs_.assign (static_cast<size_t> (kMaxTapsAnyMode), 0.0f);
    coeffStep_.assign (static_cast<size_t> (kMaxTapsAnyMode), 0.0f);
    history_.assign (static_cast<size_t> (kMaxTapsAnyMode), 0.0f);

    // Late-field decay per hop, from RT60. A 60 dB decay over rt60 seconds means
    // the energy falls by exp(-2*delta*t) with delta = 3*ln(10)/rt60.
    const double delta = 3.0 * std::log (10.0) / std::max (0.05, static_cast<double> (settings_.rt60Seconds));
    reverbDecay_ = static_cast<float> (std::exp (-2.0 * delta * static_cast<double> (kHopSize) / sampleRate));

    setPhaseMode (phaseMode_);
    reset();
}

void MaskFilter::setPhaseMode (PhaseMode m) noexcept
{
    if (coeffs_.size() < static_cast<size_t> (kMaxTapsAnyMode))
        return;   // not prepared yet

    phaseMode_ = m;
    numTaps_ = (m == PhaseMode::linearPhase) ? kMaxTapsAnyMode : kMaskTaps;

    std::fill (coeffs_.begin(), coeffs_.end(), 0.0f);
    std::fill (targetCoeffs_.begin(), targetCoeffs_.end(), 0.0f);
    std::fill (coeffStep_.begin(), coeffStep_.end(), 0.0f);
    std::fill (history_.begin(), history_.end(), 0.0f);
    historyPos_ = 0;
    interpSamplesRemaining_ = 0;

    // Start as a pure pass-through: unity impulse at tap 0 for minimum phase, or
    // at the centre tap for linear phase.
    const int unityTap = (m == PhaseMode::linearPhase) ? kLookaheadSamples : 0;
    coeffs_[static_cast<size_t> (unityTap)] = 1.0f;
    targetCoeffs_[static_cast<size_t> (unityTap)] = 1.0f;
}

void MaskFilter::reset() noexcept
{
    std::fill (priorSnr_.begin(), priorSnr_.end(), 1.0f);
    std::fill (prevGain_.begin(), prevGain_.end(), 1.0f);
    std::fill (bandGain_.begin(), bandGain_.end(), 1.0f);
    std::fill (binGain_.begin(), binGain_.end(), 1.0f);
    std::fill (lateReverb_.begin(), lateReverb_.end(), 0.0f);
    for (auto& h : powerHistory_)
        std::fill (h.begin(), h.end(), 0.0f);
    historyWrite_ = 0;
    std::fill (history_.begin(), history_.end(), 0.0f);
    historyPos_ = 0;
    frameCounter_ = 0;
    interpSamplesRemaining_ = 0;
    meanAttenDb_ = 0.0f;

    const int unityTap = (phaseMode_ == PhaseMode::linearPhase) ? kLookaheadSamples : 0;
    std::fill (coeffs_.begin(), coeffs_.end(), 0.0f);
    std::fill (targetCoeffs_.begin(), targetCoeffs_.end(), 0.0f);
    std::fill (coeffStep_.begin(), coeffStep_.end(), 0.0f);
    coeffs_[static_cast<size_t> (unityTap)] = 1.0f;
    targetCoeffs_[static_cast<size_t> (unityTap)] = 1.0f;
}

bool MaskFilter::updateMask (const float* power, const NoiseTracker& noise) noexcept
{
    bands_->binsToBands (power, bandPower_.data());
    bands_->binsToBands (noise.noisePower(), bandNoise_.data());

    const float* presence = noise.speechPresence();

    // --- Dereverb late-field estimate --------------------------------------
    powerHistory_[static_cast<size_t> (historyWrite_)] = bandPower_;
    const int delayedIdx = (historyWrite_ + 1) % (kReverbDelayFrames + 1);
    historyWrite_ = delayedIdx;

    if (settings_.dereverbEnabled)
    {
        const double delta = 3.0 * std::log (10.0)
                           / std::max (0.05, static_cast<double> (settings_.rt60Seconds));
        reverbDecay_ = static_cast<float> (std::exp (-2.0 * delta
                                          * static_cast<double> (kHopSize) / sampleRate_));

        const auto& delayed = powerHistory_[static_cast<size_t> (delayedIdx)];
        for (int b = 0; b < kNumBands; ++b)
        {
            const size_t i = static_cast<size_t> (b);
            // Lebart-style: the late field now is the delayed direct energy,
            // decayed. Take the larger of the recursive estimate and the fresh
            // one so the estimate tracks a decaying tail properly.
            const float fresh = reverbDecay_ * delayed[i];
            lateReverb_[i] = std::max (reverbDecay_ * lateReverb_[i], fresh);
        }
    }

    // --- Per-band gain -----------------------------------------------------
    const float floorNoise   = dbToGain (-settings_.maxAttenuationDb);
    const float floorReverb  = dbToGain (-settings_.dereverbMaxAttenuationDb);
    float attenSum = 0.0f;

    for (int b = 0; b < kNumBands; ++b)
    {
        const size_t i = static_cast<size_t> (b);
        const float p = bandPower_[i] + kEpsilon;

        float gain = 1.0f;

        if (settings_.denoiseEnabled)
        {
            const float nse = bandNoise_[i] + kEpsilon;
            const float snrPost = p / nse;

            // Decision-directed a-priori SNR (Ephraim-Malah). The 0.94 weighting
            // on the previous frame is what keeps the mask steady; a per-frame
            // estimate here is the classic cause of musical noise.
            const float instantaneous = std::max (snrPost - 1.0f, 0.0f);
            priorSnr_[i] = 0.94f * (prevGain_[i] * prevGain_[i] * snrPost)
                         + 0.06f * instantaneous;
            priorSnr_[i] = std::max (priorSnr_[i], 1.0e-6f);

            // Wiener gain.
            float g = priorSnr_[i] / (1.0f + priorSnr_[i]);
            prevGain_[i] = g;

            // Scale the *attenuation* by the amount control, so amount=0 is
            // exactly unity gain rather than an approximation of it.
            g = 1.0f - settings_.denoiseAmount * (1.0f - g);

            // Voice protection: where speech is clearly present, pull the gain
            // back towards unity. This is the difference between a mask that
            // works in the gaps and one that chews on the voice itself.
            const float protect = settings_.voiceProtection * presence[i];
            g = g + protect * (1.0f - g);

            gain *= std::max (g, floorNoise);
        }

        if (settings_.dereverbEnabled)
        {
            const float r = settings_.dereverbAmount * lateReverb_[i];
            float g = (p - r) / p;
            g = clampf (g, 0.0f, 1.0f);
            const float protect = settings_.voiceProtection * presence[i] * 0.5f;
            g = g + protect * (1.0f - g);
            gain *= std::max (g, floorReverb);
        }

        bandGain_[i] = clampf (gain, 0.0f, 1.0f);
        attenSum += gainToDb (bandGain_[i]);
    }

    meanAttenDb_ = attenSum / static_cast<float> (kNumBands);

    // Only redesign the filter every kMaskDecimation frames. The mask moves on a
    // tens-of-milliseconds timescale; the tone tracker is what needs to be fast,
    // not this.
    if (++frameCounter_ < kMaskDecimation)
        return false;
    frameCounter_ = 0;

    bands_->bandsToBins (bandGain_.data(), binGain_.data());

    if (phaseMode_ == PhaseMode::linearPhase)
        designLinearPhase (binGain_.data());
    else
        designMinimumPhase (binGain_.data());

    // Schedule a linear coefficient interpolation over the next design interval.
    interpLength_ = kHopSize * kMaskDecimation;
    interpSamplesRemaining_ = interpLength_;
    const float inv = 1.0f / static_cast<float> (interpLength_);
    for (int t = 0; t < numTaps_; ++t)
        coeffStep_[static_cast<size_t> (t)] =
            (targetCoeffs_[static_cast<size_t> (t)] - coeffs_[static_cast<size_t> (t)]) * inv;

    return true;
}

void MaskFilter::resampleToDesignGrid (const float* binGains) noexcept
{
    // Map the analysis grid (numBins_ over [0, Nyquist]) onto the design grid
    // (kDesignSize/2+1 over the same range) by linear interpolation.
    const int nd = kDesignSize / 2;
    for (int k = 0; k <= nd; ++k)
    {
        const float pos = static_cast<float> (k) * static_cast<float> (numBins_ - 1)
                        / static_cast<float> (nd);
        const int i0 = static_cast<int> (pos);
        const int i1 = std::min (i0 + 1, numBins_ - 1);
        const float t = pos - static_cast<float> (i0);
        designMag_[static_cast<size_t> (k)] =
            std::max (binGains[i0] + (binGains[i1] - binGains[i0]) * t, 1.0e-4f);
    }
}

void MaskFilter::designMinimumPhase (const float* binGains) noexcept
{
    resampleToDesignGrid (binGains);

    const int n  = kDesignSize;
    const int nh = n / 2;

    // 1. Log magnitude, extended to a full even-symmetric real spectrum.
    for (int k = 0; k <= nh; ++k)
    {
        const float lm = std::log (designMag_[static_cast<size_t> (k)]);
        designSpec_[static_cast<size_t> (k)] = Complex (lm, 0.0f);
        if (k > 0 && k < nh)
            designSpec_[static_cast<size_t> (n - k)] = Complex (lm, 0.0f);
    }

    // 2. Real cepstrum.
    designFft_.inverseComplex (designSpec_.data());
    for (int i = 0; i < n; ++i)
        cepstrum_[static_cast<size_t> (i)] = designSpec_[static_cast<size_t> (i)].real();

    // 3. Fold the anti-causal half onto the causal half. This is the step that
    //    turns the response minimum-phase: it moves every zero inside the unit
    //    circle while leaving the magnitude untouched.
    designSpec_[0] = Complex (cepstrum_[0], 0.0f);
    for (int i = 1; i < nh; ++i)
        designSpec_[static_cast<size_t> (i)] = Complex (2.0f * cepstrum_[static_cast<size_t> (i)], 0.0f);
    designSpec_[static_cast<size_t> (nh)] = Complex (cepstrum_[static_cast<size_t> (nh)], 0.0f);
    for (int i = nh + 1; i < n; ++i)
        designSpec_[static_cast<size_t> (i)] = Complex (0.0f, 0.0f);

    // 4. Back to a complex log spectrum, then exponentiate.
    designFft_.forwardComplex (designSpec_.data());
    for (int i = 0; i < n; ++i)
    {
        const Complex l = designSpec_[static_cast<size_t> (i)];
        const float mag = std::exp (l.real());
        designSpec_[static_cast<size_t> (i)] = Complex (mag * std::cos (l.imag()),
                                                        mag * std::sin (l.imag()));
    }

    // 5. Impulse response.
    designFft_.inverseComplex (designSpec_.data());

    // 6. Truncate to numTaps_ with a short raised-cosine taper on the last
    //    eighth, so truncation does not put ripple into the magnitude response.
    const int taper = std::max (4, numTaps_ / 8);
    for (int t = 0; t < numTaps_; ++t)
    {
        float w = 1.0f;
        const int fromEnd = numTaps_ - 1 - t;
        if (fromEnd < taper)
        {
            const float x = static_cast<float> (fromEnd) / static_cast<float> (taper);
            w = 0.5f - 0.5f * std::cos (kPiF * x);
        }
        float v = designSpec_[static_cast<size_t> (t)].real() * w;
        if (! std::isfinite (v))
            v = (t == 0) ? 1.0f : 0.0f;
        targetCoeffs_[static_cast<size_t> (t)] = v;
    }
}

void MaskFilter::designLinearPhase (const float* binGains) noexcept
{
    resampleToDesignGrid (binGains);

    const int n  = kDesignSize;
    const int nh = n / 2;

    // Zero-phase spectrum -> symmetric impulse response centred at n/2.
    for (int k = 0; k <= nh; ++k)
    {
        const float m = designMag_[static_cast<size_t> (k)];
        designSpec_[static_cast<size_t> (k)] = Complex (m, 0.0f);
        if (k > 0 && k < nh)
            designSpec_[static_cast<size_t> (n - k)] = Complex (m, 0.0f);
    }

    designFft_.inverseComplex (designSpec_.data());

    // The impulse response is circularly centred at 0; unwrap it so it is
    // centred at kLookaheadSamples, and window it.
    const int centre = kLookaheadSamples;
    for (int t = 0; t < numTaps_; ++t)
    {
        const int offset = t - centre;                // -centre .. +centre
        int idx = offset;
        while (idx < 0)
            idx += n;
        idx %= n;

        // Hann window over the full tap range.
        const float w = 0.5f - 0.5f * std::cos (2.0f * kPiF
                        * static_cast<float> (t) / static_cast<float> (numTaps_ - 1));
        float v = designSpec_[static_cast<size_t> (idx)].real() * w;
        if (! std::isfinite (v))
            v = (t == centre) ? 1.0f : 0.0f;
        targetCoeffs_[static_cast<size_t> (t)] = v;
    }
}

float MaskFilter::process (float x) noexcept
{
    // Coefficient interpolation towards the newest design.
    if (interpSamplesRemaining_ > 0)
    {
        --interpSamplesRemaining_;
        for (int t = 0; t < numTaps_; ++t)
            coeffs_[static_cast<size_t> (t)] += coeffStep_[static_cast<size_t> (t)];
        if (interpSamplesRemaining_ == 0)
            std::copy (targetCoeffs_.begin(), targetCoeffs_.begin() + numTaps_, coeffs_.begin());
    }

    // Circular-buffer FIR. historyPos_ holds the newest sample.
    history_[static_cast<size_t> (historyPos_)] = x;

    float acc = 0.0f;
    int idx = historyPos_;
    for (int t = 0; t < numTaps_; ++t)
    {
        acc += coeffs_[static_cast<size_t> (t)] * history_[static_cast<size_t> (idx)];
        if (--idx < 0)
            idx = numTaps_ - 1;
    }

    if (++historyPos_ >= numTaps_)
        historyPos_ = 0;

    return acc;
}
} // namespace fbk
