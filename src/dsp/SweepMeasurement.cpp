#include "SweepMeasurement.h"

namespace fbk
{
namespace
{
// 20 Hz to just under Nyquist. Starting at 20 Hz rather than higher because the
// low end is where a room's modal behaviour lives, and that is a large part of
// what we are trying to characterise.
constexpr double kStartHz = 20.0;
constexpr double kEndFractionOfNyquist = 0.95;
} // namespace

void SweepMeasurement::prepare (double sampleRate, float sweepSeconds, float tailSeconds)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    sweepSeconds_ = std::max (0.5f, sweepSeconds);
    tailSeconds_ = std::max (0.25f, tailSeconds);

    sweepSamples_ = static_cast<int> (sampleRate_ * static_cast<double> (sweepSeconds_));
    totalSamples_ = sweepSamples_ + static_cast<int> (sampleRate_ * static_cast<double> (tailSeconds_));

    sweep_.assign (static_cast<size_t> (sweepSamples_), 0.0f);
    inverse_.assign (static_cast<size_t> (sweepSamples_), 0.0f);
    recorded_.assign (static_cast<size_t> (totalSamples_), 0.0f);

    buildSweep();

    position_ = 0;
    running_ = false;
    haveResult_ = false;
}

void SweepMeasurement::buildSweep()
{
    const double f1 = kStartHz;
    const double f2 = sampleRate_ * 0.5 * kEndFractionOfNyquist;
    const double T = static_cast<double> (sweepSamples_) / sampleRate_;
    const double lnRatio = std::log (f2 / f1);

    // Farina's exponential sweep. Phase is integrated in closed form so there is
    // no accumulated rounding across a multi-second sweep.
    for (int n = 0; n < sweepSamples_; ++n)
    {
        const double t = static_cast<double> (n) / sampleRate_;
        const double phase = 2.0 * kPi * f1 * T / lnRatio * (std::exp (t / T * lnRatio) - 1.0);
        sweep_[static_cast<size_t> (n)] = static_cast<float> (std::sin (phase));
    }

    // Fade the ends so the loudspeaker is not asked for a step. 20 ms in, 50 ms
    // out - the tail fade matters more because it is at high frequency where a
    // discontinuity is most audible and most damaging to a driver.
    const int fadeIn = std::min (sweepSamples_ / 4, static_cast<int> (0.020 * sampleRate_));
    const int fadeOut = std::min (sweepSamples_ / 4, static_cast<int> (0.050 * sampleRate_));
    for (int n = 0; n < fadeIn; ++n)
    {
        const float w = 0.5f - 0.5f * std::cos (kPiF * static_cast<float> (n) / static_cast<float> (fadeIn));
        sweep_[static_cast<size_t> (n)] *= w;
    }
    for (int n = 0; n < fadeOut; ++n)
    {
        const float w = 0.5f - 0.5f * std::cos (kPiF * static_cast<float> (n) / static_cast<float> (fadeOut));
        sweep_[static_cast<size_t> (sweepSamples_ - 1 - n)] *= w;
    }

    // Inverse filter: the sweep reversed, with a 6 dB/octave amplitude correction
    // so that sweep convolved with inverse is a delta. Because the correction is
    // monotonic in frequency, harmonic distortion products land at negative time
    // and therefore outside the causal impulse response - which is the whole
    // reason to use an exponential sweep on a PA that is certainly not linear.
    // The envelope is a function of position *within the inverse filter*, not
    // within the original sweep. The reversed sweep begins at f2 and ends at f1, so
    // the envelope starts at 0 dB and falls to -6*log2(f2/f1) dB - that is, it
    // attenuates the low-frequency end, compensating the sweep's 1/f energy
    // density.
    //
    // Getting this backwards is not a subtle error but it is a quiet one: it boosts
    // the low end by 20*log10(f2/f1) - about 61 dB over a 20 Hz to 22.8 kHz sweep -
    // so the deconvolution still produces a plausible-looking impulse response, but
    // one smeared by low-frequency energy with its peak in the wrong place. Measured
    // that way, a 200-sample loop delay came back as 1106 and a reflection at half
    // amplitude came back at 0.16.
    for (int n = 0; n < sweepSamples_; ++n)
    {
        const int src = sweepSamples_ - 1 - n;
        const double tInverse = static_cast<double> (n) / sampleRate_;
        const double amp = std::exp (-tInverse / T * lnRatio);
        inverse_[static_cast<size_t> (n)] = static_cast<float> (sweep_[static_cast<size_t> (src)] * amp);
    }
}

void SweepMeasurement::start() noexcept
{
    std::fill (recorded_.begin(), recorded_.end(), 0.0f);
    position_ = 0;
    haveResult_ = false;
    clipped_ = false;
    tooQuiet_ = false;
    recordedPeak_ = 0.0f;
    peakDbFS_ = -120.0f;
    running_ = true;
}

void SweepMeasurement::abort() noexcept
{
    running_ = false;
    position_ = 0;
}

float SweepMeasurement::progress() const noexcept
{
    if (totalSamples_ <= 0)
        return 0.0f;
    return clampf (static_cast<float> (position_) / static_cast<float> (totalSamples_), 0.0f, 1.0f);
}

bool SweepMeasurement::process (const float* input, float* output, int numSamples) noexcept
{
    if (! running_)
        return false;

    const float gain = dbToGain (levelDb_);

    for (int n = 0; n < numSamples; ++n)
    {
        if (position_ >= totalSamples_)
        {
            output[n] = 0.0f;
            continue;
        }

        recorded_[static_cast<size_t> (position_)] = input[n];

        // Track the level as it arrives rather than after deconvolution: a
        // measurement into a clipped preamp is worthless, and the operator should
        // find that out while still standing at the console.
        const float mag = std::abs (input[n]);
        if (mag > recordedPeak_)
            recordedPeak_ = mag;

        // Silence during the tail: that is when the room's decay is captured.
        output[n] = position_ < sweepSamples_
                  ? sweep_[static_cast<size_t> (position_)] * gain
                  : 0.0f;

        ++position_;
    }

    if (position_ >= totalSamples_)
    {
        peakDbFS_ = gainToDb (recordedPeak_);
        clipped_ = recordedPeak_ > 0.985f;
        tooQuiet_ = peakDbFS_ < -45.0f;
        running_ = false;
    }

    return true;
}

void SweepMeasurement::computeImpulseResponse()
{
    if (running_ || recorded_.empty())
        return;

    // Level diagnostics were taken as the audio arrived; see process().

    // Linear convolution of the recording with the inverse filter, by FFT.
    const int needed = totalSamples_ + sweepSamples_;
    int order = 1;
    while ((1 << order) < needed)
        ++order;

    Fft fft (order);
    const int n = fft.size();

    std::vector<Complex> a (static_cast<size_t> (n), Complex {});
    std::vector<Complex> b (static_cast<size_t> (n), Complex {});

    for (int i = 0; i < totalSamples_; ++i)
        a[static_cast<size_t> (i)] = Complex (recorded_[static_cast<size_t> (i)], 0.0f);
    for (int i = 0; i < sweepSamples_; ++i)
        b[static_cast<size_t> (i)] = Complex (inverse_[static_cast<size_t> (i)], 0.0f);

    fft.forwardComplex (a.data());
    fft.forwardComplex (b.data());
    for (int i = 0; i < n; ++i)
        a[static_cast<size_t> (i)] *= b[static_cast<size_t> (i)];
    fft.inverseComplex (a.data());

    // The delta lands at the end of the inverse filter's length. Everything before
    // it is distortion product; everything after is the acoustic path.
    int peakIndex = 0;
    float peakValue = 0.0f;
    const int searchFrom = std::max (0, sweepSamples_ - static_cast<int> (0.05 * sampleRate_));
    const int searchTo = std::min (n, sweepSamples_ + static_cast<int> (0.5 * sampleRate_));
    for (int i = searchFrom; i < searchTo; ++i)
    {
        const float v = std::abs (a[static_cast<size_t> (i)].real());
        if (v > peakValue)
        {
            peakValue = v;
            peakIndex = i;
        }
    }

    directDelay_ = peakIndex - (sweepSamples_ - 1);

    // Keep one second from a few samples before the peak, so the leading edge is
    // not clipped off by an off-by-one in the peak search.
    const int preRoll = 16;
    const int start = std::max (0, peakIndex - preRoll);
    const int length = std::min (static_cast<int> (sampleRate_), n - start);

    impulse_.assign (static_cast<size_t> (std::max (0, length)), 0.0f);
    const float norm = peakValue > 0.0f ? 1.0f / peakValue : 1.0f;
    for (int i = 0; i < length; ++i)
        impulse_[static_cast<size_t> (i)] = a[static_cast<size_t> (start + i)].real() * norm;

    analyseImpulse();
    haveResult_ = true;
}

void SweepMeasurement::analyseImpulse()
{
    rt60_ = 0.0f;
    if (impulse_.size() < 64)
        return;

    // RT60 by Schroeder backward integration: integrate energy from the end
    // backwards, then fit the -5 dB to -25 dB span and extrapolate to -60. Fitting
    // over that span rather than the whole curve avoids both the direct sound at
    // the start and the noise floor at the end.
    const int n = static_cast<int> (impulse_.size());
    std::vector<double> schroeder (static_cast<size_t> (n), 0.0);

    double running = 0.0;
    for (int i = n - 1; i >= 0; --i)
    {
        running += static_cast<double> (impulse_[static_cast<size_t> (i)])
                 * impulse_[static_cast<size_t> (i)];
        schroeder[static_cast<size_t> (i)] = running;
    }

    if (schroeder[0] <= 0.0)
        return;

    const double total = schroeder[0];
    int i5 = -1, i25 = -1;
    for (int i = 0; i < n; ++i)
    {
        const double db = 10.0 * std::log10 (schroeder[static_cast<size_t> (i)] / total + 1.0e-30);
        if (i5 < 0 && db <= -5.0)
            i5 = i;
        if (i25 < 0 && db <= -25.0)
        {
            i25 = i;
            break;
        }
    }

    if (i5 >= 0 && i25 > i5)
    {
        // 20 dB of decay took (i25 - i5) samples, so 60 dB takes three times that.
        const double seconds = static_cast<double> (i25 - i5) / sampleRate_;
        rt60_ = static_cast<float> (3.0 * seconds);
    }
}
} // namespace fbk
