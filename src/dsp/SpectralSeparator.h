// FBKSuppressor - SpectralSeparator.h
//
// Interface for the learned voice / non-voice separator planned for v0.2.
//
// v0.1 ships without one: everything in the current chain is signal processing
// with no trained parameters, which means it is fully testable and behaves
// predictably on material no model has seen. The interface exists now so that
// adding the network later is a substitution rather than a rewrite.
//
// The intended v0.2 implementation is an RTNeural model - compile-time-fixed
// topology, zero allocation, no internal thread pool - taking the same 32 ERB
// band energies the mask estimator already computes plus a small feature set
// (band energy deltas, pitch correlation, the tone tracker's own output) and
// returning per-band voice presence. Those presences replace the heuristic
// speechPresence() from NoiseTracker, which is currently the weakest link in
// preserving voice colour. Crucially, the network is only ever asked for a
// *presence* estimate, never for output audio: audio still leaves through the
// minimum-phase filter, so the zero-latency guarantee is a property of the
// architecture rather than of the model, and no retraining can break it.
//
// Model input must be defined in Hz-relative terms so the same weights serve
// 44.1 kHz and 48 kHz without resampling.
#pragma once

#include "Common.h"
#include "ErbBands.h"

namespace fbk
{
// What a separator reports for one analysis frame. Voice presence and feedback
// probability are separate heads, not complements: a band can be neither (steady
// room noise) and a sustained sung note can look feedback-like to a model without
// being voice-free. Keeping them independent lets the DSP decide how each is used.
struct SeparatorOutput
{
    float voicePresence[kNumBands] {};   // [0,1], per band
    float feedbackProb[kNumBands] {};    // [0,1], per band
    float voiceActivity { 0.0f };        // [0,1], broadband
};

class SpectralSeparator
{
public:
    virtual ~SpectralSeparator() = default;

    virtual void prepare (double sampleRate, const ErbBands& bands) = 0;
    virtual void reset() = 0;

    // The user-facing controls a model may be conditioned on, each in [0,1]:
    // {feedback sensitivity, feedback depth, max cut / 18 dB, voice protection}.
    // Called from the frame boundary before process(). Default: ignored.
    virtual void setControl (const float* /*control*/, int /*count*/) noexcept {}

    // Called once per analysis frame. bandEnergy is kNumBands linear power
    // values; every field of `out` must be filled, all values in [0,1].
    //
    // feedbackProb is advisory. It may make the detector believe a tone sooner, but
    // it can never confirm one on its own and it cannot override the frequency-
    // stability criterion (see HowlDetector::setLearnedPrior). A model that is
    // confidently wrong therefore costs speed, not voice.
    virtual void process (const float* bandEnergy, SeparatorOutput& out) = 0;

    virtual bool isReady() const = 0;
    virtual const char* name() const = 0;
};

// Placeholder used by v0.1. Reports not-ready, so the chain falls back to the
// heuristic speech-presence estimate.
class NullSeparator final : public SpectralSeparator
{
public:
    void prepare (double, const ErbBands&) override {}
    void reset() override {}
    void process (const float*, SeparatorOutput& out) override { out = SeparatorOutput {}; }
    bool isReady() const override { return false; }
    const char* name() const override { return "none (v0.1 DSP only)"; }
};
} // namespace fbk
