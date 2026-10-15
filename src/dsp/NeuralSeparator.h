// FBKSuppressor - NeuralSeparator.h
//
// The v0.2 learned separator: a small recurrent network that reads the 32 ERB
// band energies the analyser already computes and reports, per band, how likely
// that band is to hold voice and how likely it is to hold feedback.
//
// It is an inference engine only. There are no trained weights in this repository;
// until a model file is loaded, isReady() is false and the chain behaves exactly
// as v0.1. Training is in training/, and produces the file this class loads.
//
// Why hand-written rather than a library: the topology is tiny and fixed, the
// whole engine is ~300 lines, it allocates nothing after load(), and the DSP core
// stays free of third-party dependencies so it keeps compiling on every CI
// toolchain with nothing fetched. A model with hidden size 64 is about 47k
// parameters, so roughly that many multiply-adds per 128-sample hop.
//
// Topology (all dimensions come from the tensor shapes in the model file):
//
//   features f[64]  = 32 band levels relative to the frame's loudest band,
//                     then 32 frame-to-frame level changes
//   x  = tanh( In(f) )                                       [H]
//   t  = tanh( Trunk(control[4]) )                           [T]
//   x  = (1 + Gamma(t)) * x + Beta(t)                        FiLM, [H]
//   h  = GRU_core(x)                                         [H]
//   l  = Head(h)                                             [65] logits
//   l += SmootherOut( GRU_smoother( sigmoid(l) ) )           optional, [S]
//   voicePresence = sigmoid(l[0..31])
//   feedbackProb  = sigmoid(l[32..63])
//   voiceActivity = sigmoid(l[64])
//
// The control vector is {feedback sensitivity, feedback depth, max cut / 18 dB,
// voice protection}, each in [0,1]. Conditioning on it (FiLM) means one model
// serves every setting of those controls rather than needing one per setting.
//
// Every input is level-relative and every band is defined in Hz on the ERB scale,
// so the same weights serve 44.1 kHz and 48 kHz with no resampler and no
// per-rate path.
//
// GRU convention is PyTorch's, so a torch.nn.GRU exports directly:
//   r = sigmoid(Wir x + bir + Whr h + bhr)
//   z = sigmoid(Wiz x + biz + Whz h + bhz)
//   n = tanh   (Win x + bin + r * (Whn h + bhn))
//   h = (1 - z) * n + z * h
// with the gate rows stacked [r; z; n] in weight_ih and weight_hh.
//
// Model file ("FBKM", little-endian, which is every platform this ships on):
//   u32 magic 'FBKM' (0x4D4B4246), u32 version (1), u32 tensorCount
//   per tensor: u32 nameLen, name bytes, u32 ndim, u32 dims[ndim], f32 data[]
//
// Threading: load() allocates and must not run on the audio thread, nor while the
// separator is attached to a running suppressor. Load first, then attach.
// process() allocates nothing.
#pragma once

#include "SpectralSeparator.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbk
{
class NeuralSeparator final : public SpectralSeparator
{
public:
    static constexpr int kNumFeatures = 2 * kNumBands;     // 64
    static constexpr int kNumOutputs  = 2 * kNumBands + 1; // 65
    static constexpr int kNumControls = 4;
    static constexpr int kMaxHidden   = 512;               // sanity cap on a model file

    struct LoadResult
    {
        bool ok { false };
        const char* error { "" };   // static string, valid for the program's lifetime
    };

    // Parse and validate a model. On any failure the separator is left unloaded
    // (isReady() false) rather than half-loaded. Not for the audio thread.
    LoadResult load (const void* data, std::size_t size);
    void unload() noexcept;

    int hiddenSize() const noexcept { return hidden_; }
    int smootherSize() const noexcept { return smootherHidden_; }
    int parameterCount() const noexcept { return parameterCount_; }

    // SpectralSeparator
    void prepare (double sampleRate, const ErbBands& bands) override;
    void reset() override;
    void setControl (const float* control, int count) noexcept override;
    void process (const float* bandEnergy, SeparatorOutput& out) override;
    bool isReady() const override { return loaded_; }
    const char* name() const override { return "FBKM neural separator"; }

    // The feature definition, exposed so training code and tests can pin it down.
    // `prevLevelDb` carries the previous frame's per-band dB levels (updated in
    // place); pass hasPrev = false on the first frame so its deltas are zero.
    static void computeFeatures (const float* bandEnergy, float* prevLevelDb,
                                 bool hasPrev, float* featuresOut) noexcept;

private:
    struct Dense
    {
        int in { 0 }, out { 0 };
        std::vector<float> w, b;   // w is [out][in], row-major
        void forward (const float* x, float* y) const noexcept;
    };

    struct Gru
    {
        int in { 0 }, hid { 0 };
        std::vector<float> wih, whh, bih, bhh;     // [3*hid][in], [3*hid][hid]
        std::vector<float> h, gi, gh;              // state and scratch
        void step (const float* x) noexcept;
        void clearState() noexcept;
    };

    void updateFilm() noexcept;

    bool loaded_ { false };
    int  hidden_ { 0 }, trunk_ { 0 }, smootherHidden_ { 0 };
    int  parameterCount_ { 0 };

    Dense in_, trunk_dense_, gamma_, beta_, head_, smootherOut_;
    Gru   core_, smoother_;

    float control_[kNumControls] { 0.5f, 1.0f, 0.5f, 0.8f };
    bool  filmDirty_ { true };

    std::vector<float> filmGamma_, filmBeta_, trunkAct_, x_, logits_, probs_, smootherDelta_;
    float features_[kNumFeatures] {};
    float prevLevelDb_[kNumBands] {};
    bool  hasPrev_ { false };
};
} // namespace fbk
