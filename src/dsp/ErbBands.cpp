#include "ErbBands.h"

namespace fbk
{
void ErbBands::prepare (double sampleRate)
{
    sampleRate_  = sampleRate;
    numBins_     = kNumBins;
    binWidthHz_  = static_cast<float> (sampleRate / static_cast<double> (kFftSize));

    start_.assign (kNumBands, 0);
    end_.assign (kNumBands, 0);
    centreHz_.assign (kNumBands, 0.0f);
    centreBin_.assign (kNumBands, 0.0f);
    binToBand_.assign (static_cast<size_t> (numBins_), 0);

    // Span 50 Hz to just under Nyquist. Below 50 Hz there is nothing we want to
    // model with a wide band - subsonic rumble is handled by the high-pass and
    // mains hum by its own dedicated canceller.
    const float loHz = 50.0f;
    const float hiHz = static_cast<float> (sampleRate * 0.5) * 0.985f;
    const float loErb = hzToErb (loHz);
    const float hiErb = hzToErb (hiHz);

    int previousEnd = 1;   // bin 0 (DC) is never part of a band
    for (int b = 0; b < kNumBands; ++b)
    {
        const float t0 = static_cast<float> (b)     / static_cast<float> (kNumBands);
        const float t1 = static_cast<float> (b + 1) / static_cast<float> (kNumBands);
        const float f0 = erbToHz (loErb + (hiErb - loErb) * t0);
        const float f1 = erbToHz (loErb + (hiErb - loErb) * t1);

        int s = static_cast<int> (std::floor (f0 / binWidthHz_));
        int e = static_cast<int> (std::ceil  (f1 / binWidthHz_));

        s = std::max (s, previousEnd);
        e = std::min (std::max (e, s + 1), numBins_);
        // Leave at least one bin per remaining band.
        e = std::min (e, numBins_ - (kNumBands - 1 - b));
        e = std::max (e, s + 1);

        start_[static_cast<size_t> (b)] = s;
        end_[static_cast<size_t> (b)]   = e;
        previousEnd = e;

        centreBin_[static_cast<size_t> (b)] = 0.5f * static_cast<float> (s + e - 1);
        centreHz_[static_cast<size_t> (b)]  = centreBin_[static_cast<size_t> (b)] * binWidthHz_;
    }

    // Make the top band reach Nyquist so no bin is left unassigned.
    end_[kNumBands - 1] = numBins_;
    centreBin_[kNumBands - 1] = 0.5f * static_cast<float> (start_[kNumBands - 1] + numBins_ - 1);
    centreHz_[kNumBands - 1]  = centreBin_[kNumBands - 1] * binWidthHz_;

    for (int b = 0; b < kNumBands; ++b)
        for (int k = start_[static_cast<size_t> (b)]; k < end_[static_cast<size_t> (b)]; ++k)
            binToBand_[static_cast<size_t> (k)] = b;

    // DC and anything below the first band belong to band 0.
    for (int k = 0; k < start_[0]; ++k)
        binToBand_[static_cast<size_t> (k)] = 0;
}

void ErbBands::binsToBands (const float* bins, float* bands) const noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        const int s = start_[static_cast<size_t> (b)];
        const int e = end_[static_cast<size_t> (b)];
        float sum = 0.0f;
        for (int k = s; k < e; ++k)
            sum += bins[k];
        bands[b] = sum / static_cast<float> (std::max (1, e - s));
    }
}

void ErbBands::bandsToBins (const float* bands, float* bins) const noexcept
{
    for (int k = 0; k < numBins_; ++k)
    {
        const float x = static_cast<float> (k);

        if (x <= centreBin_[0])
        {
            bins[k] = bands[0];
            continue;
        }
        if (x >= centreBin_[kNumBands - 1])
        {
            bins[k] = bands[kNumBands - 1];
            continue;
        }

        const int b = binToBand_[static_cast<size_t> (k)];
        int lo = b, hi = b;
        if (x < centreBin_[static_cast<size_t> (b)])
            lo = std::max (0, b - 1);
        else
            hi = std::min (kNumBands - 1, b + 1);

        const float c0 = centreBin_[static_cast<size_t> (lo)];
        const float c1 = centreBin_[static_cast<size_t> (hi)];
        const float t  = (c1 > c0) ? (x - c0) / (c1 - c0) : 0.0f;
        bins[k] = bands[lo] + (bands[hi] - bands[lo]) * clampf (t, 0.0f, 1.0f);
    }
}
} // namespace fbk
