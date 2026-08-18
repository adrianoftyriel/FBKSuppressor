// FBKSuppressor - Analyser.h
//
// Trailing, analysis-only STFT.
//
// This is the component that makes zero added latency possible. A conventional
// spectral processor analyses a window, modifies it, and resynthesises by
// overlap-add - and overlap-add is what forces a delay of one window length,
// because the output for a given instant is not complete until every window
// overlapping that instant has been added in. RNNoise pays ~10 ms for this and
// DeepFilterNet pays 40 ms (20 ms window plus two frames of lookahead).
//
// We never resynthesise. The STFT here only ever *measures*: the window covers
// the most recent kFftSize samples, ending at the newest sample the host has
// given us. Everything derived from it (band gains, tone estimates, noise floor)
// is then applied to the signal by a causal time-domain filter. So the analysis
// can use a long window - and therefore have real frequency resolution - while
// the audio path itself is never delayed by even one sample.
//
// The window is asymmetric: a long half-Hann rise followed by a short half-Hann
// fall, peaking at 7/8 of its length. A symmetric window would put its centre of
// mass ~21 ms in the past, which makes the estimate sluggish exactly when a
// feedback tone is ramping up. The asymmetric shape pulls the effective
// measurement centroid to roughly 8 ms ago while keeping sidelobes low enough to
// resolve a howl from a vocal harmonic. Because there is no resynthesis there is
// no COLA constraint, so we are free to shape the window purely for measurement
// quality.
#pragma once

#include "Common.h"
#include "Fft.h"

namespace fbk
{
class Analyser
{
public:
    Analyser();

    void prepare (double sampleRate);
    void reset() noexcept;

    // Push one input sample. Returns true when a new analysis frame has just
    // been computed (i.e. every kHopSize samples).
    bool pushSample (float x) noexcept;

    // Valid after pushSample() returns true.
    const Complex* spectrum()  const noexcept { return spectrum_.data(); }
    const float*   magnitude() const noexcept { return magnitude_.data(); }   // kNumBins
    const float*   power()     const noexcept { return power_.data(); }       // kNumBins
    const float*   phase()     const noexcept { return phase_.data(); }       // kNumBins
    const float*   previousPhase() const noexcept { return prevPhase_.data(); }

    long long frameIndex() const noexcept { return frameIndex_; }
    double sampleRate() const noexcept { return sampleRate_; }

    // Effective delay, in samples, between the newest input sample and the
    // centre of mass of the analysis window. Purely informational - it is not
    // latency, because the output is never held back; it only bounds how stale
    // a spectral estimate can be.
    float measurementCentroidSamples() const noexcept { return centroidSamples_; }

private:
    void computeFrame() noexcept;

    Fft fft_;
    double sampleRate_ { 48000.0 };

    std::vector<float>   window_;
    std::vector<float>   ring_;        // kFftSize, circular
    int                  writePos_ { 0 };
    int                  hopCounter_ { 0 };
    long long            frameIndex_ { 0 };
    float                centroidSamples_ { 0.0f };

    std::vector<float>   framed_;      // windowed time-domain frame
    std::vector<Complex> spectrum_;
    std::vector<float>   magnitude_, power_, phase_, prevPhase_;
};
} // namespace fbk
