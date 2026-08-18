// FBKSuppressor - TonalCanceller.h
//
// Subtractive cancellation of coherent sinusoids: feedback tones and mains hum.
//
// Why this and not a notch filter
// ------------------------------
// A notch filter removes a *frequency*. Everything at that frequency goes,
// including the vocal harmonic that happens to sit there, and the notch keeps
// removing it for as long as it is engaged. Ringing out a room is this applied
// pre-emptively across a dozen frequencies, which is precisely the technique the
// brief rules out.
//
// This class removes a *signal*. For each tracked tone it runs a unit-amplitude
// complex oscillator at the tone's frequency and adapts a complex weight (an
// amplitude and a phase) by leaky LMS so that the oscillator, scaled by that
// weight, matches the sinusoidal component present in the input. It then
// subtracts that synthesised sinusoid. What is removed is one specific coherent
// waveform; anything else at or near the same frequency is left alone, because
// it does not correlate with the oscillator over the adaptation window.
//
// The adaptation is power-normalised (NLMS rather than plain LMS). Three reasons,
// all of which showed up as test failures with plain LMS: convergence time
// becomes independent of signal level, so a quiet howl is caught as fast as a
// loud one; the step size becomes a dimensionless number that maps directly onto
// a convergence time and therefore onto a notch bandwidth; and the update cannot
// diverge when handed an absurd input sample, which plain LMS does readily.
//
// The effective notch bandwidth is mu * fs / (4*pi), which at the values used
// here is around 4 Hz - equivalent to a Q in the hundreds to thousands, far
// narrower than any filter you would implement by hand, and far narrower than the
// tens of Hz a real voice moves through from vibrato and pitch contour alone. A
// vocal harmonic wanders in and out of that band continuously and never
// correlates long enough to be learned. A feedback tone sits still and is learned
// within a few tens of milliseconds.
//
// Frequency lock
// --------------
// The detector's parabolic peak interpolation gets us within a few Hz. Any
// residual error shows up as a steady rotation of the adapted weight, so we
// measure that rotation and fold it back into the oscillator frequency. This is
// an ordinary phase-locked loop and it pulls the estimate onto the true tone
// frequency, which matters: a 1 Hz error caps achievable cancellation at around
// 20 dB, while a locked oscillator reaches 40 dB or more.
#pragma once

#include "Common.h"
#include "HowlDetector.h"

namespace fbk
{
class SinusoidCanceller
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    void setFrequency (float hz) noexcept;          // glides
    void setFrequencyImmediate (float hz) noexcept; // snaps
    void setStepSize (float mu) noexcept { mu_ = mu; }

    // Maximum frequency correction the phase-locked loop may apply, in Hz. Set
    // to zero to pin the oscillator to exactly the frequency it was given.
    //
    // Pinning matters for the hum probes. They discriminate 50 Hz from 60 Hz by
    // sitting at those exact frequencies and reporting how much coherent energy
    // they find, and a free-running PLL simply slides whichever probe is wrong
    // onto the component that is actually there - at which point both probes
    // report the same value and the discrimination is gone. (Measured: both read
    // 0.82 against a pure 50 Hz harmonic series.) The hum *harmonics* still get a
    // small range, because real mains frequency does drift by a fraction of a
    // hertz and the drift scales with harmonic number.
    void setPllRange (float hz) noexcept
    {
        pllRangeHz_ = std::max (0.0f, hz);
        if (pllRangeHz_ <= 0.0f)
            pllRate_ = 0.0f;
    }
    void setLeak (float leak) noexcept { leak_ = leak; }
    void setEngage (float e) noexcept { engageTarget_ = clampf (e, 0.0f, 1.0f); }

    // When set, the weight keeps adapting even at zero engagement. The canceller
    // then acts as a narrowband correlator that measures how much coherent energy
    // sits at its frequency without altering the signal - which is how the hum
    // stage decides between 50 and 60 Hz, at a frequency resolution the analysis
    // FFT cannot reach.
    void setAlwaysAdapt (bool a) noexcept { alwaysAdapt_ = a; }

    bool isIdle() const noexcept { return engageTarget_ <= 0.0f && engage_ < 1.0e-4f; }

    // Returns the input with the learned sinusoid removed.
    float process (float x) noexcept;

    float frequencyHz() const noexcept { return freqHz_; }
    float amplitude()  const noexcept { return 2.0f * std::sqrt (wr_ * wr_ + wi_ * wi_); }
    float attenuationDb() const noexcept { return attenDb_; }

    // Coherent amplitude at this frequency relative to the broadband RMS. Used as
    // a detection statistic by the hum stage.
    float coherenceRatio() const noexcept
    {
        return amplitude() / (std::sqrt (2.0f * powerEst_) + 1.0e-9f);
    }

private:
    void updateRotation() noexcept;

    double sampleRate_ { 48000.0 };
    float  freqHz_ { 1000.0f }, targetFreqHz_ { 1000.0f };
    float  freqGlide_ { 0.002f };

    // Recursive complex oscillator.
    float oscR_ { 1.0f }, oscI_ { 0.0f };
    float rotR_ { 1.0f }, rotI_ { 0.0f };
    int   renormCounter_ { 0 };

    // Adapted complex weight.
    float wr_ { 0.0f }, wi_ { 0.0f };
    float mu_ { 1.0e-3f };           // dimensionless NLMS step
    float leak_ { 1.0e-6f };
    float powerEst_ { 0.0f };        // running input power, for normalisation
    float powerCoeff_ { 0.999f };
    bool  alwaysAdapt_ { false };

    // Phase-locked frequency refinement.
    float prevWr_ { 0.0f }, prevWi_ { 0.0f };
    float pllRate_ { 0.0f };
    float pllGain_ { 0.0f };
    float pllRangeHz_ { 20.0f };
    int   pllCounter_ { 0 };
    int   pllInterval_ { 64 };

    float engage_ { 0.0f }, engageTarget_ { 0.0f }, engageCoeff_ { 0.999f };
    float inLevel_ { 0.0f }, outLevel_ { 0.0f }, attenDb_ { 0.0f };
    float levelCoeff_ { 0.999f };
};

// ---------------------------------------------------------------------------
// Bank of cancellers driven by the howling detector.
class TonalCanceller
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    // Called once per analysis frame: bind detector tracks to cancellers.
    void updateFromDetector (const TrackedTone* tones, int numTones, float depth) noexcept;

    // Per-sample, cascaded so each canceller adapts on the residual of the ones
    // before it.
    float process (float x) noexcept;

    int numEngaged() const noexcept { return numEngaged_; }
    float totalAttenuationDb() const noexcept;

private:
    std::vector<SinusoidCanceller> cancellers_;
    std::vector<float> assignedFreq_;
    std::vector<bool>  inUse_;
    double sampleRate_ { 48000.0 };
    int numEngaged_ { 0 };
};

// ---------------------------------------------------------------------------
// A pure measurement device: how much energy is coherent at one exact frequency.
//
// This is a single-bin DFT with an exponential window, which is the honest way to
// ask "is there a 50 Hz component here". The first attempt used an adaptive
// canceller's own weight as the measurement, which does not work: an LMS
// oscillator pinned at 60 Hz and fed a 50 Hz signal produces a weight that beats
// at 10 Hz with a large instantaneous magnitude, because the adaptation time
// constant is comparable to the beat period. Measured, that gave a 60 Hz probe a
// *higher* reading than the 50 Hz probe against a pure 50 Hz harmonic series.
//
// A one-pole complex accumulator has no such problem. With a 0.5 s time constant
// its bandwidth is around 0.3 Hz, so a 10 Hz offset is rejected by roughly 30 dB
// and the two candidates separate cleanly. The cost is that detection takes about
// a second to settle, which for mains hum is irrelevant.
class NarrowbandProbe
{
public:
    void prepare (double sampleRate, float freqHz, float timeConstantSeconds) noexcept
    {
        sampleRate_ = sampleRate;
        freqHz_ = freqHz;
        const double w = 2.0 * kPi * static_cast<double> (freqHz) / sampleRate;
        rotR_ = static_cast<float> (std::cos (w));
        rotI_ = static_cast<float> (std::sin (w));
        coeff_ = static_cast<float> (std::exp (-1.0 / (static_cast<double> (timeConstantSeconds) * sampleRate)));
        reset();
    }

    void reset() noexcept
    {
        oscR_ = 1.0f; oscI_ = 0.0f;
        accR_ = accI_ = 0.0f;
        power_ = 0.0f;
        renorm_ = 0;
    }

    void process (float x) noexcept
    {
        const float nr = oscR_ * rotR_ - oscI_ * rotI_;
        const float ni = oscR_ * rotI_ + oscI_ * rotR_;
        oscR_ = nr; oscI_ = ni;

        if (++renorm_ >= 256)
        {
            renorm_ = 0;
            const float m = std::sqrt (oscR_ * oscR_ + oscI_ * oscI_);
            if (m > 1.0e-6f) { oscR_ /= m; oscI_ /= m; }
            else             { oscR_ = 1.0f; oscI_ = 0.0f; }
        }

        // x * conj(osc), low-passed.
        const float pr = x * oscR_;
        const float pi = -x * oscI_;
        accR_ = coeff_ * accR_ + (1.0f - coeff_) * pr;
        accI_ = coeff_ * accI_ + (1.0f - coeff_) * pi;
        accR_ = sanitise (accR_);
        accI_ = sanitise (accI_);

        power_ = coeff_ * power_ + (1.0f - coeff_) * x * x;
    }

    // Amplitude of the coherent component at this frequency.
    float amplitude() const noexcept { return 2.0f * std::sqrt (accR_ * accR_ + accI_ * accI_); }

    // That amplitude relative to the broadband RMS, so the figure is level
    // independent.
    float coherenceRatio() const noexcept
    {
        return amplitude() / (std::sqrt (2.0f * power_) + 1.0e-9f);
    }

    float frequencyHz() const noexcept { return freqHz_; }

private:
    double sampleRate_ { 48000.0 };
    float freqHz_ { 50.0f };
    float oscR_ { 1.0f }, oscI_ { 0.0f }, rotR_ { 1.0f }, rotI_ { 0.0f };
    float accR_ { 0.0f }, accI_ { 0.0f }, power_ { 0.0f }, coeff_ { 0.999f };
    int renorm_ { 0 };
};

// ---------------------------------------------------------------------------
// Mains hum and buzz.
//
// Same subtractive machinery, but the frequency is known a priori to be an exact
// harmonic series on either 50 or 60 Hz, so we lock one fundamental and derive
// the harmonics from it.
//
// Choosing between 50 and 60 Hz cannot be done from the analysis FFT: at 2048
// points the bin spacing is 23 Hz, so consecutive hum harmonics are barely two
// bins apart and no comb-scoring scheme on that grid is trustworthy. Instead we
// run two always-adapting probe oscillators, at 50 and 60 Hz, with zero
// engagement - they alter nothing, they only measure how much coherent energy
// sits at their exact frequency. A narrowband correlator has effectively
// unlimited frequency resolution, so this is both simpler and far more reliable
// than any spectral test. Whichever probe wins by a clear margin selects the
// fundamental, with hysteresis so the choice does not flicker.
//
// Because hum is perfectly periodic, cancellation depth here is very high
// (typically 30-45 dB) at essentially no cost to the voice.
class HumCanceller
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    void setEnabled (bool e) noexcept { enabled_ = e; }
    void setNumHarmonics (int n) noexcept;
    void setDepth (float d) noexcept;

    // Called once per analysis frame to re-evaluate which fundamental is present.
    void updateDetection() noexcept;

    float process (float x) noexcept;

    float detectedFundamental() const noexcept { return fundamentalHz_; }
    bool  isActive() const noexcept { return enabled_ && active_; }
    float coherence50() const noexcept { return smooth50_; }
    float coherence60() const noexcept { return smooth60_; }

private:
    std::vector<SinusoidCanceller> harmonics_;
    NarrowbandProbe probe50_, probe60_;
    double sampleRate_ { 48000.0 };
    bool  enabled_ { true };
    bool  active_ { false };
    int   numHarmonics_ { 8 };
    float depth_ { 1.0f };
    float fundamentalHz_ { 50.0f };
    float smooth50_ { 0.0f }, smooth60_ { 0.0f };
    float detectSmooth_ { 0.99f };
    int   holdFrames_ { 0 };
};
} // namespace fbk
