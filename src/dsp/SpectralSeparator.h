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
class SpectralSeparator
{
public:
    virtual ~SpectralSeparator() = default;

    virtual void prepare (double sampleRate, const ErbBands& bands) = 0;
    virtual void reset() = 0;

    // Called once per analysis frame. bandEnergy is kNumBands linear power
    // values; voicePresenceOut must be filled with kNumBands values in [0,1].
    virtual void process (const float* bandEnergy, float* voicePresenceOut) = 0;

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
    void process (const float*, float* voicePresenceOut) override
    {
        for (int b = 0; b < kNumBands; ++b)
            voicePresenceOut[b] = 0.0f;
    }
    bool isReady() const override { return false; }
    const char* name() const override { return "none (v0.1 DSP only)"; }
};
} // namespace fbk
