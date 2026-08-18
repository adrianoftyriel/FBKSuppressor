// FBKSuppressor - SweepMeasurement.h
//
// Measures the feedback path - console output, PA, room, microphone, back to here -
// by emitting an exponential sine sweep and deconvolving the microphone return.
//
// Why this works from an insert
// -----------------------------
// The plugin sits on a channel whose output feeds the PA and whose input is the
// microphone. So it is already wired across exactly the loop we want to measure:
// emit the sweep downstream, record what comes back up the mic, and the recording
// is the sweep convolved with the whole acoustic path. Deconvolve and you have the
// feedback path's impulse response.
//
// That impulse response is what makes realistic training data possible for v0.2.
// Synthesising feedback by running public room impulse responses in a loop is an
// approximation; running a closed loop through the path actually measured in the
// room the plugin will be used in is not.
//
// Why an exponential sweep rather than noise or an impulse
// -------------------------------------------------------
// An exponential (log) sweep spends equal energy per octave, which matches how
// both rooms and microphones behave, and its harmonic distortion products fold
// into negative time on deconvolution - so loudspeaker nonlinearity lands outside
// the causal part of the answer instead of smearing through it. For measuring a PA
// at gig level, where the drivers are certainly not linear, that property matters
// more than anything else on offer.
//
// Safety
// ------
// This emits full-band audio to a PA at a level the operator chooses. It must be
// impossible to start by accident, it stops on its own, and it never loops. The
// audio thread only ever reads from a precomputed table and writes into a
// preallocated buffer; the deconvolution is arithmetic-heavy and happens off the
// audio thread.
#pragma once

#include "Common.h"
#include "Fft.h"

namespace fbk
{
class SweepMeasurement
{
public:
    // Sweep of `sweepSeconds`, then `tailSeconds` of silence to capture the decay.
    void prepare (double sampleRate, float sweepSeconds = 3.0f, float tailSeconds = 1.5f);

    void setLevelDb (float db) noexcept { levelDb_ = clampf (db, -60.0f, -6.0f); }
    float levelDb() const noexcept { return levelDb_; }

    // Starting is always an explicit act; there is no way to arm this implicitly.
    void start() noexcept;
    void abort() noexcept;

    bool isRunning() const noexcept { return running_; }
    bool hasResult() const noexcept { return haveResult_; }
    float progress() const noexcept;

    // Audio thread. Replaces the block's contents with the sweep while running and
    // records `input` for later deconvolution. Returns true if it wrote output.
    bool process (const float* input, float* output, int numSamples) noexcept;

    // Off the audio thread, once isRunning() has gone false. Deconvolves the
    // recording into the impulse response.
    void computeImpulseResponse();

    const std::vector<float>& impulseResponse() const noexcept { return impulse_; }
    const std::vector<float>& recording() const noexcept { return recorded_; }

    // Diagnostics from the last measurement.
    float measuredPeakDbFS() const noexcept { return peakDbFS_; }
    float estimatedRt60Seconds() const noexcept { return rt60_; }
    int   directDelaySamples() const noexcept { return directDelay_; }
    bool  clipped() const noexcept { return clipped_; }
    bool  tooQuiet() const noexcept { return tooQuiet_; }

    double sampleRate() const noexcept { return sampleRate_; }

private:
    void buildSweep();
    void analyseImpulse();

    double sampleRate_ { 48000.0 };
    float  sweepSeconds_ { 3.0f }, tailSeconds_ { 1.5f };
    float  levelDb_ { -20.0f };

    int sweepSamples_ { 0 }, totalSamples_ { 0 };
    std::vector<float> sweep_;         // the emitted signal
    std::vector<float> inverse_;       // time-reversed, amplitude-corrected
    std::vector<float> recorded_;      // microphone return
    std::vector<float> impulse_;

    int  position_ { 0 };
    bool running_ { false };
    bool haveResult_ { false };

    float recordedPeak_ { 0.0f };
    float peakDbFS_ { -120.0f }, rt60_ { 0.0f };
    int   directDelay_ { 0 };
    bool  clipped_ { false }, tooQuiet_ { false };
};
} // namespace fbk
