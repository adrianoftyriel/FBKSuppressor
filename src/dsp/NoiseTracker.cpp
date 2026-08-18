#include "NoiseTracker.h"

namespace fbk
{
void NoiseTracker::prepare (double sampleRate, const ErbBands& bands)
{
    bands_   = &bands;
    numBins_ = bands.numBins();

    // Hold the running minimum over roughly 1.5 s, split into 8 sub-windows.
    const double framesPerSecond = sampleRate / static_cast<double> (kHopSize);
    numSubWindows_ = 8;
    subWindowLen_  = std::max (4, static_cast<int> (1.5 * framesPerSecond / numSubWindows_));

    // Power smoothing time constant ~60 ms.
    smoothAlpha_ = static_cast<float> (std::exp (-static_cast<double> (kHopSize) / (0.060 * sampleRate)));

    smoothed_.assign (static_cast<size_t> (numBins_), 0.0f);
    noise_.assign (static_cast<size_t> (numBins_), 0.0f);
    currentMin_.assign (static_cast<size_t> (numBins_), 0.0f);
    subMins_.assign (static_cast<size_t> (numSubWindows_),
                     std::vector<float> (static_cast<size_t> (numBins_), 0.0f));

    bandNoise_.assign (kNumBands, 0.0f);
    bandPower_.assign (kNumBands, 0.0f);
    bandSnr_.assign (kNumBands, 1.0f);
    presence_.assign (kNumBands, 0.0f);

    reset();
}

void NoiseTracker::reset() noexcept
{
    std::fill (smoothed_.begin(), smoothed_.end(), 0.0f);
    std::fill (noise_.begin(), noise_.end(), 0.0f);
    std::fill (currentMin_.begin(), currentMin_.end(), 0.0f);
    for (auto& sw : subMins_)
        std::fill (sw.begin(), sw.end(), 0.0f);
    std::fill (bandSnr_.begin(), bandSnr_.end(), 1.0f);
    std::fill (presence_.begin(), presence_.end(), 0.0f);
    subIndex_ = 0;
    subCounter_ = 0;
    overallPresence_ = 0.0f;
    primed_ = false;
}

void NoiseTracker::update (const float* power) noexcept
{
    if (! primed_)
    {
        // Seed everything from the first frame so the estimate does not have to
        // climb up from zero (which would make the first second over-suppress).
        for (int k = 0; k < numBins_; ++k)
        {
            smoothed_[static_cast<size_t> (k)]   = power[k];
            noise_[static_cast<size_t> (k)]      = power[k];
            currentMin_[static_cast<size_t> (k)] = power[k];
            for (auto& sw : subMins_)
                sw[static_cast<size_t> (k)] = power[k];
        }
        primed_ = true;
    }

    const float a = smoothAlpha_;
    for (int k = 0; k < numBins_; ++k)
    {
        const size_t i = static_cast<size_t> (k);
        smoothed_[i] = a * smoothed_[i] + (1.0f - a) * power[k];
        currentMin_[i] = std::min (currentMin_[i], smoothed_[i]);
    }

    if (++subCounter_ >= subWindowLen_)
    {
        subCounter_ = 0;
        subMins_[static_cast<size_t> (subIndex_)] = currentMin_;
        subIndex_ = (subIndex_ + 1) % numSubWindows_;

        for (int k = 0; k < numBins_; ++k)
        {
            float m = subMins_[0][static_cast<size_t> (k)];
            for (int s = 1; s < numSubWindows_; ++s)
                m = std::min (m, subMins_[static_cast<size_t> (s)][static_cast<size_t> (k)]);
            noise_[static_cast<size_t> (k)] = m;
        }

        // Restart the running minimum from the current smoothed power so the
        // estimate can follow a rising noise floor.
        currentMin_ = smoothed_;
    }
    else
    {
        // Between sub-window boundaries, let the noise estimate fall immediately
        // but rise only slowly.
        for (int k = 0; k < numBins_; ++k)
        {
            const size_t i = static_cast<size_t> (k);
            noise_[i] = std::min (noise_[i], smoothed_[i]);
        }
    }

    // Band aggregation and speech presence.
    bands_->binsToBands (power, bandPower_.data());
    bands_->binsToBands (noise_.data(), bandNoise_.data());

    float presenceSum = 0.0f, weightSum = 0.0f;
    for (int b = 0; b < kNumBands; ++b)
    {
        const size_t i = static_cast<size_t> (b);
        const float snr = bandPower_[i] / (bandNoise_[i] + kEpsilon);
        bandSnr_[i] = snr;

        // Map SNR in dB through a soft ramp: 3 dB -> 0, 12 dB -> 1.
        const float snrDb = 10.0f * std::log10 (std::max (snr, 1.0e-12f));
        presence_[i] = clampf ((snrDb - 3.0f) / 9.0f, 0.0f, 1.0f);

        // Weight the broadband figure towards the speech-dominant region.
        const float f = bands_->centreHz (b);
        const float w = (f > 200.0f && f < 4000.0f) ? 1.0f : 0.25f;
        presenceSum += presence_[i] * w;
        weightSum   += w;
    }
    overallPresence_ = weightSum > 0.0f ? presenceSum / weightSum : 0.0f;
}
} // namespace fbk
