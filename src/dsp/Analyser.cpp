#include "Analyser.h"

#include <numbers>

namespace fbk
{
Analyser::Analyser() : fft_ (kFftOrder) {}

void Analyser::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;

    window_.assign (kFftSize, 0.0f);

    // Asymmetric analysis window: rise over the first 7/8, fall over the last
    // 1/8. Peak sits at kFftSize * 7/8, i.e. 256 samples before the newest
    // sample at 2048 points.
    const int fall = kFftSize / 8;
    const int rise = kFftSize - fall;
    for (int n = 0; n < rise; ++n)
    {
        const double t = static_cast<double> (n) / static_cast<double> (rise);
        window_[static_cast<size_t> (n)] =
            static_cast<float> (0.5 - 0.5 * std::cos (std::numbers::pi * t));
    }
    for (int n = 0; n < fall; ++n)
    {
        const double t = static_cast<double> (n) / static_cast<double> (fall);
        window_[static_cast<size_t> (rise + n)] =
            static_cast<float> (0.5 + 0.5 * std::cos (std::numbers::pi * t));
    }

    // Centre of mass of the window, measured backwards from the newest sample.
    double num = 0.0, den = 0.0;
    for (int n = 0; n < kFftSize; ++n)
    {
        const double w = window_[static_cast<size_t> (n)];
        num += w * static_cast<double> (kFftSize - 1 - n);
        den += w;
    }
    centroidSamples_ = static_cast<float> (den > 0.0 ? num / den : 0.0);

    ring_.assign (kFftSize, 0.0f);
    framed_.assign (kFftSize, 0.0f);
    spectrum_.assign (kFftSize, Complex {});
    magnitude_.assign (kNumBins, 0.0f);
    power_.assign (kNumBins, 0.0f);
    phase_.assign (kNumBins, 0.0f);
    prevPhase_.assign (kNumBins, 0.0f);

    reset();
}

void Analyser::reset() noexcept
{
    std::fill (ring_.begin(), ring_.end(), 0.0f);
    std::fill (magnitude_.begin(), magnitude_.end(), 0.0f);
    std::fill (power_.begin(), power_.end(), 0.0f);
    std::fill (phase_.begin(), phase_.end(), 0.0f);
    std::fill (prevPhase_.begin(), prevPhase_.end(), 0.0f);
    writePos_ = 0;
    hopCounter_ = 0;
    frameIndex_ = 0;
}

bool Analyser::pushSample (float x) noexcept
{
    ring_[static_cast<size_t> (writePos_)] = x;
    writePos_ = (writePos_ + 1) & (kFftSize - 1);

    if (++hopCounter_ < kHopSize)
        return false;

    hopCounter_ = 0;
    computeFrame();
    ++frameIndex_;
    return true;
}

void Analyser::computeFrame() noexcept
{
    // writePos_ is where the *next* sample will go, so the oldest sample of the
    // trailing window sits exactly there.
    for (int n = 0; n < kFftSize; ++n)
    {
        const int idx = (writePos_ + n) & (kFftSize - 1);
        framed_[static_cast<size_t> (n)] = ring_[static_cast<size_t> (idx)] * window_[static_cast<size_t> (n)];
    }

    fft_.forwardReal (framed_.data(), spectrum_.data());

    std::swap (phase_, prevPhase_);
    for (int k = 0; k < kNumBins; ++k)
    {
        const Complex c = spectrum_[static_cast<size_t> (k)];
        const float re = c.real(), im = c.imag();
        const float p  = re * re + im * im;
        power_[static_cast<size_t> (k)]     = p;
        magnitude_[static_cast<size_t> (k)] = std::sqrt (p);
        phase_[static_cast<size_t> (k)]     = std::atan2 (im, re);
    }
}
} // namespace fbk
