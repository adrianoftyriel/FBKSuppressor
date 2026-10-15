#include "NeuralSeparator.h"

#include <cstring>
#include <string>
#include <utility>

namespace fbk
{
namespace
{
constexpr std::uint32_t kMagic = 0x4D4B4246u;   // 'F' 'B' 'K' 'M', little-endian
constexpr std::uint32_t kVersion = 1u;
constexpr std::uint32_t kMaxTensors = 64u;
constexpr std::uint32_t kMaxNameLen = 64u;
constexpr std::uint32_t kMaxDims = 4u;

// Finite check by exponent bits. The DSP core builds with fast-math on every
// toolchain, where std::isfinite is permitted to fold to true; reading the bits
// cannot be optimised away.
inline bool finiteF (float v) noexcept
{
    std::uint32_t u;
    std::memcpy (&u, &v, sizeof (u));
    return (u & 0x7F800000u) != 0x7F800000u;
}

inline float sigmoidf (float x) noexcept
{
    x = clampf (x, -30.0f, 30.0f);
    return 1.0f / (1.0f + std::exp (-x));
}

struct Tensor
{
    std::string name;
    std::uint32_t dims[kMaxDims] {};
    std::uint32_t ndim { 0 };
    const unsigned char* data { nullptr };
    std::size_t count { 0 };
};

class Reader
{
public:
    Reader (const unsigned char* p, std::size_t n) : p_ (p), end_ (p + n) {}

    bool u32 (std::uint32_t& v) noexcept
    {
        if (static_cast<std::size_t> (end_ - p_) < 4)
            return false;
        std::memcpy (&v, p_, 4);
        p_ += 4;
        return true;
    }

    bool bytes (std::size_t n, const unsigned char*& out) noexcept
    {
        if (static_cast<std::size_t> (end_ - p_) < n)
            return false;
        out = p_;
        p_ += n;
        return true;
    }

private:
    const unsigned char* p_;
    const unsigned char* end_;
};

const Tensor* findTensor (const std::vector<Tensor>& all, const char* name)
{
    for (const auto& t : all)
        if (t.name == name)
            return &t;
    return nullptr;
}

bool shapeIs (const Tensor& t, std::uint32_t d0, std::uint32_t d1 = 0) noexcept
{
    if (d1 == 0)
        return t.ndim == 1 && t.dims[0] == d0;
    return t.ndim == 2 && t.dims[0] == d0 && t.dims[1] == d1;
}

void copyFloats (const Tensor& t, std::vector<float>& dst)
{
    dst.resize (t.count);
    if (t.count > 0)
        std::memcpy (dst.data(), t.data, t.count * sizeof (float));
}
} // namespace

// ---------------------------------------------------------------------------
void NeuralSeparator::Dense::forward (const float* x, float* y) const noexcept
{
    for (int o = 0; o < out; ++o)
    {
        const float* row = w.data() + static_cast<std::size_t> (o) * static_cast<std::size_t> (in);
        float acc = b[static_cast<std::size_t> (o)];
        for (int i = 0; i < in; ++i)
            acc += row[i] * x[i];
        y[o] = acc;
    }
}

void NeuralSeparator::Gru::clearState() noexcept
{
    std::fill (h.begin(), h.end(), 0.0f);
}

void NeuralSeparator::Gru::step (const float* x) noexcept
{
    const int h3 = 3 * hid;
    for (int r = 0; r < h3; ++r)
    {
        const float* wi = wih.data() + static_cast<std::size_t> (r) * static_cast<std::size_t> (in);
        float a = bih[static_cast<std::size_t> (r)];
        for (int i = 0; i < in; ++i)
            a += wi[i] * x[i];
        gi[static_cast<std::size_t> (r)] = a;

        const float* wh = whh.data() + static_cast<std::size_t> (r) * static_cast<std::size_t> (hid);
        float g = bhh[static_cast<std::size_t> (r)];
        for (int i = 0; i < hid; ++i)
            g += wh[i] * h[static_cast<std::size_t> (i)];
        gh[static_cast<std::size_t> (r)] = g;
    }

    for (int i = 0; i < hid; ++i)
    {
        const auto u = static_cast<std::size_t> (i);
        const float r = sigmoidf (gi[u] + gh[u]);
        const float z = sigmoidf (gi[u + static_cast<std::size_t> (hid)] + gh[u + static_cast<std::size_t> (hid)]);
        const float n = std::tanh (gi[u + 2 * static_cast<std::size_t> (hid)]
                                   + r * gh[u + 2 * static_cast<std::size_t> (hid)]);
        h[u] = (1.0f - z) * n + z * h[u];
    }
}

// ---------------------------------------------------------------------------
void NeuralSeparator::computeFeatures (const float* bandEnergy, float* prevLevelDb,
                                       bool hasPrev, float* featuresOut) noexcept
{
    float db[kNumBands];
    float maxDb = -120.0f;
    for (int b = 0; b < kNumBands; ++b)
    {
        float e = bandEnergy[b];
        if (! finiteF (e) || e < 0.0f)
            e = 0.0f;
        db[b] = 10.0f * std::log10 (e + 1.0e-12f);
        maxDb = std::max (maxDb, db[b]);
    }

    for (int b = 0; b < kNumBands; ++b)
    {
        featuresOut[b] = clampf ((db[b] - maxDb) / 60.0f, -1.0f, 0.0f);
        featuresOut[kNumBands + b] = hasPrev ? clampf ((db[b] - prevLevelDb[b]) / 20.0f, -1.0f, 1.0f)
                                             : 0.0f;
        prevLevelDb[b] = db[b];
    }
}

// ---------------------------------------------------------------------------
NeuralSeparator::LoadResult NeuralSeparator::load (const void* data, std::size_t size)
{
    unload();

    const auto fail = [this] (const char* msg) -> LoadResult
    {
        unload();
        return { false, msg };
    };

    if (data == nullptr || size < 12)
        return fail ("model too small");

    Reader rd (static_cast<const unsigned char*> (data), size);
    std::uint32_t magic = 0, version = 0, count = 0;
    if (! rd.u32 (magic) || ! rd.u32 (version) || ! rd.u32 (count))
        return fail ("truncated header");
    if (magic != kMagic)
        return fail ("not an FBKM model (bad magic)");
    if (version != kVersion)
        return fail ("unsupported model version");
    if (count == 0 || count > kMaxTensors)
        return fail ("implausible tensor count");

    std::vector<Tensor> tensors;
    tensors.reserve (count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        Tensor t;
        std::uint32_t nameLen = 0;
        if (! rd.u32 (nameLen) || nameLen == 0 || nameLen > kMaxNameLen)
            return fail ("bad tensor name length");
        const unsigned char* nameBytes = nullptr;
        if (! rd.bytes (nameLen, nameBytes))
            return fail ("truncated tensor name");
        t.name.assign (reinterpret_cast<const char*> (nameBytes), nameLen);

        if (! rd.u32 (t.ndim) || t.ndim == 0 || t.ndim > kMaxDims)
            return fail ("bad tensor rank");

        std::size_t n = 1;
        for (std::uint32_t d = 0; d < t.ndim; ++d)
        {
            if (! rd.u32 (t.dims[d]) || t.dims[d] == 0 || t.dims[d] > 1u << 20)
                return fail ("bad tensor dimension");
            n *= t.dims[d];
            if (n > (1u << 24))
                return fail ("tensor too large");
        }
        t.count = n;

        if (! rd.bytes (n * sizeof (float), t.data))
            return fail ("truncated tensor data");
        for (std::size_t k = 0; k < n; ++k)
        {
            float v;
            std::memcpy (&v, t.data + k * sizeof (float), sizeof (float));
            if (! finiteF (v))
                return fail ("non-finite weight");
        }
        tensors.push_back (std::move (t));
    }

    // --- Derive sizes from the tensors that define them. -------------------
    const Tensor* inB = findTensor (tensors, "in.bias");
    const Tensor* trunkB = findTensor (tensors, "film.trunk.bias");
    if (inB == nullptr || trunkB == nullptr || inB->ndim != 1 || trunkB->ndim != 1)
        return fail ("missing in.bias or film.trunk.bias");

    const int H = static_cast<int> (inB->dims[0]);
    const int T = static_cast<int> (trunkB->dims[0]);
    if (H > kMaxHidden || T > kMaxHidden)
        return fail ("hidden size above the supported maximum");

    const auto uH = static_cast<std::uint32_t> (H);
    const auto uT = static_cast<std::uint32_t> (T);

    const auto dense = [&] (const std::string& prefix, std::uint32_t outN, std::uint32_t inN,
                            Dense& d) -> bool
    {
        const Tensor* w = findTensor (tensors, (prefix + ".weight").c_str());
        const Tensor* b = findTensor (tensors, (prefix + ".bias").c_str());
        if (w == nullptr || b == nullptr || ! shapeIs (*w, outN, inN) || ! shapeIs (*b, outN))
            return false;
        d.in = static_cast<int> (inN);
        d.out = static_cast<int> (outN);
        copyFloats (*w, d.w);
        copyFloats (*b, d.b);
        return true;
    };

    const auto gru = [&] (const std::string& prefix, std::uint32_t inN, std::uint32_t hidN,
                          Gru& g) -> bool
    {
        const Tensor* wih = findTensor (tensors, (prefix + ".weight_ih").c_str());
        const Tensor* whh = findTensor (tensors, (prefix + ".weight_hh").c_str());
        const Tensor* bih = findTensor (tensors, (prefix + ".bias_ih").c_str());
        const Tensor* bhh = findTensor (tensors, (prefix + ".bias_hh").c_str());
        if (wih == nullptr || whh == nullptr || bih == nullptr || bhh == nullptr)
            return false;
        if (! shapeIs (*wih, 3 * hidN, inN) || ! shapeIs (*whh, 3 * hidN, hidN)
            || ! shapeIs (*bih, 3 * hidN) || ! shapeIs (*bhh, 3 * hidN))
            return false;
        g.in = static_cast<int> (inN);
        g.hid = static_cast<int> (hidN);
        copyFloats (*wih, g.wih);
        copyFloats (*whh, g.whh);
        copyFloats (*bih, g.bih);
        copyFloats (*bhh, g.bhh);
        g.h.assign (hidN, 0.0f);
        g.gi.assign (3 * static_cast<std::size_t> (hidN), 0.0f);
        g.gh.assign (3 * static_cast<std::size_t> (hidN), 0.0f);
        return true;
    };

    if (! dense ("in", uH, kNumFeatures, in_))
        return fail ("in.* has the wrong shape (expected [H,64])");
    if (! dense ("film.trunk", uT, kNumControls, trunk_dense_))
        return fail ("film.trunk.* has the wrong shape (expected [T,4])");
    if (! dense ("film.gamma", uH, uT, gamma_) || ! dense ("film.beta", uH, uT, beta_))
        return fail ("film.gamma/beta have the wrong shape (expected [H,T])");
    if (! gru ("core", uH, uH, core_))
        return fail ("core.* has the wrong shape (expected GRU H->H)");
    if (! dense ("head", kNumOutputs, uH, head_))
        return fail ("head.* has the wrong shape (expected [65,H])");

    // The smoother is optional: a model without one is simply unsmoothed.
    const Tensor* sBhh = findTensor (tensors, "smoother.bias_hh");
    if (sBhh != nullptr)
    {
        if (sBhh->ndim != 1 || sBhh->dims[0] % 3 != 0)
            return fail ("smoother.bias_hh has the wrong shape");
        const std::uint32_t S = sBhh->dims[0] / 3;
        if (S == 0 || S > static_cast<std::uint32_t> (kMaxHidden))
            return fail ("smoother size out of range");
        if (! gru ("smoother", kNumOutputs, S, smoother_))
            return fail ("smoother.* has the wrong shape (expected GRU 65->S)");
        if (! dense ("smoother.out", kNumOutputs, S, smootherOut_))
            return fail ("smoother.out.* has the wrong shape (expected [65,S])");
        smootherHidden_ = static_cast<int> (S);
    }

    hidden_ = H;
    trunk_ = T;

    int params = 0;
    for (const auto& t : tensors)
        params += static_cast<int> (t.count);
    parameterCount_ = params;

    // Everything the audio path touches is sized here, once.
    filmGamma_.assign (static_cast<std::size_t> (H), 0.0f);
    filmBeta_.assign (static_cast<std::size_t> (H), 0.0f);
    trunkAct_.assign (static_cast<std::size_t> (T), 0.0f);
    x_.assign (static_cast<std::size_t> (H), 0.0f);
    logits_.assign (kNumOutputs, 0.0f);
    probs_.assign (kNumOutputs, 0.0f);
    smootherDelta_.assign (kNumOutputs, 0.0f);

    filmDirty_ = true;
    hasPrev_ = false;
    loaded_ = true;
    return { true, "" };
}

void NeuralSeparator::unload() noexcept
{
    loaded_ = false;
    hidden_ = trunk_ = smootherHidden_ = parameterCount_ = 0;
    in_ = Dense {};
    trunk_dense_ = Dense {};
    gamma_ = Dense {};
    beta_ = Dense {};
    head_ = Dense {};
    smootherOut_ = Dense {};
    core_ = Gru {};
    smoother_ = Gru {};
}

// ---------------------------------------------------------------------------
void NeuralSeparator::prepare (double, const ErbBands&)
{
    // Nothing is rate-dependent: the inputs are ERB-band levels, which are already
    // defined in Hz, and the hop is the same 128 samples at both rates.
    reset();
}

void NeuralSeparator::reset()
{
    hasPrev_ = false;
    std::fill (std::begin (prevLevelDb_), std::end (prevLevelDb_), 0.0f);
    if (loaded_)
    {
        core_.clearState();
        if (smootherHidden_ > 0)
            smoother_.clearState();
    }
}

void NeuralSeparator::setControl (const float* control, int count) noexcept
{
    for (int i = 0; i < kNumControls && i < count; ++i)
    {
        const float v = finiteF (control[i]) ? clampf (control[i], 0.0f, 1.0f) : 0.0f;
        if (v != control_[i])
        {
            control_[i] = v;
            filmDirty_ = true;
        }
    }
}

void NeuralSeparator::updateFilm() noexcept
{
    trunk_dense_.forward (control_, trunkAct_.data());
    for (auto& v : trunkAct_)
        v = std::tanh (v);
    gamma_.forward (trunkAct_.data(), filmGamma_.data());
    beta_.forward (trunkAct_.data(), filmBeta_.data());
    filmDirty_ = false;
}

void NeuralSeparator::process (const float* bandEnergy, SeparatorOutput& out)
{
    out = SeparatorOutput {};
    if (! loaded_)
        return;

    computeFeatures (bandEnergy, prevLevelDb_, hasPrev_, features_);
    hasPrev_ = true;

    if (filmDirty_)
        updateFilm();

    in_.forward (features_, x_.data());
    for (int i = 0; i < hidden_; ++i)
    {
        const auto u = static_cast<std::size_t> (i);
        x_[u] = (1.0f + filmGamma_[u]) * std::tanh (x_[u]) + filmBeta_[u];
    }

    core_.step (x_.data());
    head_.forward (core_.h.data(), logits_.data());

    if (smootherHidden_ > 0)
    {
        for (int i = 0; i < kNumOutputs; ++i)
            probs_[static_cast<std::size_t> (i)] = sigmoidf (logits_[static_cast<std::size_t> (i)]);
        smoother_.step (probs_.data());
        smootherOut_.forward (smoother_.h.data(), smootherDelta_.data());
        for (int i = 0; i < kNumOutputs; ++i)
            logits_[static_cast<std::size_t> (i)] += smootherDelta_[static_cast<std::size_t> (i)];
    }

    // A model that produces garbage must not poison the chain. If anything is
    // non-finite, drop the recurrent state, report "no information", and carry on.
    float sum = 0.0f;
    for (int i = 0; i < kNumOutputs; ++i)
        sum += logits_[static_cast<std::size_t> (i)];
    if (! finiteF (sum))
    {
        reset();
        return;
    }

    for (int b = 0; b < kNumBands; ++b)
    {
        out.voicePresence[b] = sigmoidf (logits_[static_cast<std::size_t> (b)]);
        out.feedbackProb[b] = sigmoidf (logits_[static_cast<std::size_t> (kNumBands + b)]);
    }
    out.voiceActivity = sigmoidf (logits_[static_cast<std::size_t> (2 * kNumBands)]);
}
} // namespace fbk
