// FBKSuppressor DSP tests.
//
// The tests that matter most here are the ones that verify the two claims the
// whole design rests on: that the audio path adds no latency, and that removing a
// feedback tone does not remove the voice sitting on top of it.
#include "TestFramework.h"
#include "SignalUtils.h"

#include "Analyser.h"
#include "ErbBands.h"
#include "FeedbackSuppressor.h"
#include "Fft.h"
#include "MaskFilter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <new>

#ifdef _MSC_VER
 #include <malloc.h>
#endif

// ---------------------------------------------------------------------------
// Allocation tracking, so real-time safety is verified rather than asserted.
//
// The audio thread must never allocate: an allocation can take a lock, and a
// lock on the audio thread is a dropout. Reading the code is not enough to be
// sure - a std::vector::assign hidden three calls down inside a parameter setter
// is exactly the kind of thing that gets missed, and did get missed here until
// this test existed. So we replace global operator new and count.
namespace rtcheck
{
std::atomic<bool> armed { false };
std::atomic<int>  count { 0 };

inline void arm()    { count.store (0); armed.store (true); }
inline int  disarm() { armed.store (false); return count.load(); }
} // namespace rtcheck

static void noteAllocation() noexcept
{
    if (rtcheck::armed.load (std::memory_order_relaxed))
        rtcheck::count.fetch_add (1, std::memory_order_relaxed);
}

void* operator new (std::size_t n)
{
    noteAllocation();
    if (void* p = std::malloc (n ? n : 1))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t n) { return ::operator new (n); }

// Aligned allocation is the one place where the three platforms genuinely differ.
// MSVC never implemented C11 aligned_alloc, so std::aligned_alloc does not exist
// there; it provides _aligned_malloc instead, and memory from it must be released
// with _aligned_free rather than free. The aligned operator delete overloads below
// are only ever called for aligned allocations, so the pairing stays correct.
namespace rtcheck
{
inline void* alignedAlloc (std::size_t bytes, std::size_t align)
{
   #ifdef _MSC_VER
    return _aligned_malloc (bytes, align);
   #else
    // std::aligned_alloc requires the size to be a multiple of the alignment.
    const std::size_t rounded = ((bytes + align - 1) / align) * align;
    return std::aligned_alloc (align, rounded ? rounded : align);
   #endif
}

inline void alignedFree (void* p) noexcept
{
   #ifdef _MSC_VER
    _aligned_free (p);
   #else
    std::free (p);
   #endif
}
} // namespace rtcheck

void* operator new (std::size_t n, std::align_val_t a)
{
    noteAllocation();
    const std::size_t align = static_cast<std::size_t> (a);
    if (void* p = rtcheck::alignedAlloc (n ? n : align, align))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t n, std::align_val_t a) { return ::operator new (n, a); }

void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
void operator delete (void* p, std::align_val_t) noexcept { rtcheck::alignedFree (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { rtcheck::alignedFree (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { rtcheck::alignedFree (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { rtcheck::alignedFree (p); }

using namespace fbk;
using namespace test;

namespace
{
constexpr double kRates[] = { 44100.0, 48000.0 };

Parameters defaultParams()
{
    Parameters p;
    p.strength = 1.0f;
    p.feedbackEnabled = true;
    p.denoiseEnabled = true;
    p.humEnabled = true;
    p.dereverbEnabled = false;
    p.highPassEnabled = true;
    return p;
}

// Run a signal through a suppressor in blocks, returning the output.
std::vector<float> run (FeedbackSuppressor& fs, const std::vector<float>& in, int blockSize)
{
    std::vector<float> out = in;
    int pos = 0;
    while (pos < static_cast<int> (out.size()))
    {
        const int n = std::min (blockSize, static_cast<int> (out.size()) - pos);
        fs.processBlock (out.data() + pos, n);
        pos += n;
    }
    return out;
}

// ===========================================================================
void testFft()
{
    beginTest ("FFT round trip and Parseval");

    Fft fft (10);
    const int n = fft.size();

    Rng rng (7);
    std::vector<float> in (static_cast<size_t> (n));
    for (auto& v : in)
        v = rng.uniform();

    std::vector<Complex> spec (static_cast<size_t> (n));
    fft.forwardReal (in.data(), spec.data());

    std::vector<float> back (static_cast<size_t> (n), 0.0f);
    fft.inverseReal (spec.data(), back.data());

    double maxErr = 0.0;
    for (int i = 0; i < n; ++i)
        maxErr = std::max (maxErr, std::abs (static_cast<double> (in[static_cast<size_t> (i)] - back[static_cast<size_t> (i)])));
    info ("max round-trip error = %.3g", maxErr);
    CHECK_LT (maxErr, 1.0e-4);

    // A pure bin should land in exactly that bin.
    const int bin = 37;
    for (int i = 0; i < n; ++i)
        in[static_cast<size_t> (i)] = static_cast<float> (
            std::cos (2.0 * fbk::kPi * static_cast<double> (bin) * i / n));
    fft.forwardReal (in.data(), spec.data());

    int peak = 0;
    float peakMag = 0.0f;
    for (int k = 0; k <= n / 2; ++k)
    {
        const float m = std::abs (spec[static_cast<size_t> (k)]);
        if (m > peakMag) { peakMag = m; peak = k; }
    }
    CHECK (peak == bin);
}

// ===========================================================================
void testErbBands()
{
    beginTest ("ERB band layout at both sample rates");

    for (double sr : kRates)
    {
        ErbBands bands;
        bands.prepare (sr);

        // Every bin must belong to exactly one band, bands must be contiguous,
        // non-empty and cover the whole spectrum.
        CHECK (bands.bandStart (0) >= 1);
        CHECK (bands.bandEnd (kNumBands - 1) == bands.numBins());

        bool contiguous = true, nonEmpty = true, monotonic = true;
        for (int b = 0; b < kNumBands; ++b)
        {
            if (bands.bandWidth (b) < 1)
                nonEmpty = false;
            if (b > 0)
            {
                if (bands.bandStart (b) != bands.bandEnd (b - 1))
                    contiguous = false;
                if (! (bands.centreHz (b) > bands.centreHz (b - 1)))
                    monotonic = false;
            }
        }
        CHECK (contiguous);
        CHECK (nonEmpty);
        CHECK (monotonic);

        info2 ("sr %.0f: band 0 centre %.1f Hz", sr, static_cast<double> (bands.centreHz (0)));
        info2 ("sr %.0f: top band centre %.1f Hz", sr, static_cast<double> (bands.centreHz (kNumBands - 1)));

        // Round trip a flat gain: bands -> bins must stay flat.
        std::vector<float> bandGains (kNumBands, 0.5f);
        std::vector<float> binGains (static_cast<size_t> (bands.numBins()), 0.0f);
        bands.bandsToBins (bandGains.data(), binGains.data());
        double maxDev = 0.0;
        for (int k = 0; k < bands.numBins(); ++k)
            maxDev = std::max (maxDev, std::abs (static_cast<double> (binGains[static_cast<size_t> (k)]) - 0.5));
        CHECK_LT (maxDev, 1.0e-5);
    }
}

// ===========================================================================
void testMinimumPhaseDesign()
{
    beginTest ("Minimum-phase mask filter: magnitude accuracy and causality");

    ErbBands bands;
    bands.prepare (48000.0);

    NoiseTracker noise;
    noise.prepare (48000.0, bands);

    MaskFilter mask;
    MaskSettings ms;
    ms.denoiseEnabled = true;
    ms.denoiseAmount = 1.0f;
    ms.maxAttenuationDb = 12.0f;
    ms.voiceProtection = 0.0f;
    mask.setSettings (ms);
    mask.prepare (48000.0, bands);
    mask.setSettings (ms);

    // Drive the mask with a spectrum that has a strong low band and a floor
    // elsewhere, so the resulting mask is genuinely non-flat.
    std::vector<float> power (static_cast<size_t> (bands.numBins()), 1.0e-6f);
    for (int k = 1; k < 40; ++k)
        power[static_cast<size_t> (k)] = 1.0f;

    for (int f = 0; f < 400; ++f)
    {
        noise.update (power.data());
        mask.updateMask (power.data(), noise);
    }

    // Measure the realised response by feeding an impulse through.
    // A minimum-phase filter must be causal with its energy at the front, which
    // is exactly what makes zero-latency operation possible.
    std::vector<float> imp (1024, 0.0f);
    imp[0] = 1.0f;
    for (auto& v : imp)
        v = mask.process (v);

    CHECK (std::abs (imp[0]) > 0.05f);

    // Energy centroid of the impulse response, in samples.
    double num = 0.0, den = 0.0;
    for (int i = 0; i < 1024; ++i)
    {
        const double e = static_cast<double> (imp[static_cast<size_t> (i)]) * imp[static_cast<size_t> (i)];
        num += e * i;
        den += e;
    }
    const double centroid = den > 0.0 ? num / den : 0.0;
    info ("minimum-phase impulse energy centroid = %.2f samples", centroid);
    // A minimum-phase realisation of a smooth mask should keep essentially all of
    // its energy inside a handful of samples.
    CHECK_LT (centroid, 8.0);

    // And the tail must have decayed inside the tap budget.
    double tailEnergy = 0.0;
    for (int i = kMaskTaps; i < 1024; ++i)
        tailEnergy += static_cast<double> (imp[static_cast<size_t> (i)]) * imp[static_cast<size_t> (i)];
    CHECK_LT (tailEnergy / std::max (den, 1.0e-30), 1.0e-6);
}

// ===========================================================================
void testZeroLatency()
{
    beginTest ("Zero added latency in strict mode");

    for (double sr : kRates)
    {
        FeedbackSuppressor fs;
        fs.prepare (sr, 512);
        auto p = defaultParams();
        p.qualityMode = false;
        p.highPassEnabled = false;   // a high-pass has its own group delay
        fs.setParameters (p);

        CHECK (fs.latencySamples() == 0);

        // Silence, then a single impulse. In strict mode the very first output
        // sample must already respond: nothing is being held back.
        std::vector<float> sig (4096, 0.0f);
        sig[2048] = 1.0f;
        auto out = run (fs, sig, 16);   // 16-sample blocks, the live-sound case

        CHECK (std::abs (out[2048]) > 0.05f);

        // Nothing may appear before the impulse.
        double preEnergy = 0.0;
        for (int i = 0; i < 2048; ++i)
            preEnergy += std::abs (static_cast<double> (out[static_cast<size_t> (i)]));
        CHECK_LT (preEnergy, 1.0e-6);

        info2 ("sr %.0f: first-sample response = %.4f", sr, std::abs (static_cast<double> (out[2048])));
    }

    // Measurement centroid is informational only - it is how stale a spectral
    // estimate can be, not latency, because the output is never delayed.
    FeedbackSuppressor fs;
    fs.prepare (48000.0, 512);
    info ("analysis measurement centroid = %.2f ms (not latency)",
          static_cast<double> (fs.measurementCentroidMs()));
}

// ===========================================================================
void testQualityModeLatency()
{
    beginTest ("Quality mode reports and realises its stated latency");

    FeedbackSuppressor fs;
    fs.prepare (48000.0, 512);
    auto p = defaultParams();
    p.qualityMode = true;
    p.highPassEnabled = false;
    fs.setParameters (p);

    CHECK (fs.latencySamples() == kLookaheadSamples);

    std::vector<float> sig (8192, 0.0f);
    sig[4096] = 1.0f;
    auto out = run (fs, sig, 64);

    // Peak should land at the declared latency.
    int peak = 0;
    float peakMag = 0.0f;
    for (int i = 4000; i < 4600; ++i)
        if (std::abs (out[static_cast<size_t> (i)]) > peakMag)
        {
            peakMag = std::abs (out[static_cast<size_t> (i)]);
            peak = i;
        }
    const int measured = peak - 4096;
    info ("quality-mode measured peak delay = %.0f samples", static_cast<double> (measured));
    CHECK (std::abs (measured - kLookaheadSamples) <= 2);
}

// ===========================================================================
void testUnityAtZeroStrength()
{
    beginTest ("Strength = 0 is exactly unity");

    FeedbackSuppressor fs;
    fs.prepare (48000.0, 256);
    auto p = defaultParams();
    p.strength = 0.0f;
    fs.setParameters (p);

    Rng rng (99);
    std::vector<float> sig (8192);
    for (auto& v : sig)
        v = 0.2f * rng.uniform();

    const auto out = run (fs, sig, 128);

    double maxDiff = 0.0;
    for (size_t i = 0; i < sig.size(); ++i)
        maxDiff = std::max (maxDiff, std::abs (static_cast<double> (sig[i] - out[i])));
    info ("max deviation from input = %.3g", maxDiff);
    CHECK_LT (maxDiff, 1.0e-7);
}

// ===========================================================================
void testFeedbackSuppression()
{
    beginTest ("Feedback tone suppression");

    for (double sr : kRates)
    {
        FeedbackSuppressor fs;
        fs.prepare (sr, 128);
        auto p = defaultParams();
        p.denoiseEnabled = false;    // isolate the tonal canceller
        p.humEnabled = false;
        p.highPassEnabled = false;
        p.feedbackSensitivity = 0.5f;
        fs.setParameters (p);

        const double toneHz = 2137.0;   // deliberately off any bin centre
        const int n = static_cast<int> (sr * 4.0);

        Rng rng (3);
        std::vector<float> sig (static_cast<size_t> (n));
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            phase += 2.0 * fbk::kPi * toneHz / sr;
            // Feedback builds up over the first second, then sustains.
            const double t = static_cast<double> (i) / sr;
            const double ramp = std::min (1.0, t / 1.0);
            sig[static_cast<size_t> (i)] =
                static_cast<float> (0.3 * ramp * std::sin (phase)) + 0.002f * rng.gaussian();
        }

        const auto out = run (fs, sig, 64);

        // Measure over the last second, once everything has converged.
        const int mStart = n - static_cast<int> (sr);
        const int mLen = static_cast<int> (sr);
        const double inMag  = goertzelMagnitude (sig.data() + mStart, mLen, toneHz, sr);
        const double outMag = goertzelMagnitude (out.data() + mStart, mLen, toneHz, sr);
        const double attenDb = toDb (outMag) - toDb (inMag);

        info2 ("sr %.0f: tone attenuation = %.1f dB", sr, attenDb);
        CHECK_LT (attenDb, -20.0);

        // The broadband noise floor away from the tone must be essentially
        // untouched - this is the difference between subtracting a tone and
        // notching a frequency region.
        const double offMagIn  = goertzelMagnitude (sig.data() + mStart, mLen, toneHz + 400.0, sr);
        const double offMagOut = goertzelMagnitude (out.data() + mStart, mLen, toneHz + 400.0, sr);
        const double offDb = toDb (offMagOut) - toDb (offMagIn);
        info2 ("sr %.0f: level 400 Hz away = %+.2f dB", sr, offDb);
        CHECK_LT (std::abs (offDb), 1.5);
    }
}

// ===========================================================================
void testVoicePreservation()
{
    beginTest ("Voice character preserved (harmonic stack with vibrato)");

    const double sr = 48000.0;

    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    auto p = defaultParams();
    p.denoiseAmount = 0.7f;
    p.humEnabled = false;
    p.highPassEnabled = false;
    fs.setParameters (p);

    const int n = static_cast<int> (sr * 6.0);
    SyntheticVoice voice (sr, 130.0);
    Rng rng (11);

    std::vector<float> sig (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        sig[static_cast<size_t> (i)] = voice.next() + 0.0015f * rng.gaussian();

    const auto out = run (fs, sig, 128);

    // Compare band energies over the last three seconds. A processor that dulls a
    // voice shows up here as broad attenuation across the formant region.
    const int mStart = n - static_cast<int> (sr * 3.0);
    const int mLen = n - mStart;

    Fft fft (11);
    ErbBands bands;
    bands.prepare (sr);

    std::vector<float> inBand (kNumBands, 0.0f), outBand (kNumBands, 0.0f);
    std::vector<Complex> spec (static_cast<size_t> (fft.size()));
    std::vector<float> binPow (static_cast<size_t> (kNumBins), 0.0f);
    std::vector<float> tmp (kNumBands, 0.0f);
    int frames = 0;

    for (int pos = mStart; pos + fft.size() < mStart + mLen; pos += 512)
    {
        for (int which = 0; which < 2; ++which)
        {
            const float* src = (which == 0 ? sig.data() : out.data()) + pos;
            fft.forwardReal (src, spec.data());
            for (int k = 0; k < kNumBins; ++k)
                binPow[static_cast<size_t> (k)] = std::norm (spec[static_cast<size_t> (k)]);
            bands.binsToBands (binPow.data(), tmp.data());
            auto& dst = (which == 0 ? inBand : outBand);
            for (int b = 0; b < kNumBands; ++b)
                dst[static_cast<size_t> (b)] += tmp[static_cast<size_t> (b)];
        }
        ++frames;
    }

    CHECK_GT (frames, 10);

    double worstDb = 0.0;
    int worstBand = 0;
    double sumAbsDb = 0.0;
    int counted = 0;

    for (int b = 0; b < kNumBands; ++b)
    {
        const double f = static_cast<double> (bands.centreHz (b));
        if (f < 100.0 || f > 6000.0)
            continue;   // outside the region carrying voice character

        const double db = 10.0 * std::log10 ((outBand[static_cast<size_t> (b)] + 1.0e-20)
                                           / (inBand[static_cast<size_t> (b)] + 1.0e-20));
        sumAbsDb += std::abs (db);
        ++counted;
        if (std::abs (db) > std::abs (worstDb))
        {
            worstDb = db;
            worstBand = b;
        }
    }

    info2 ("worst band change %.2f dB at %.0f Hz", worstDb, static_cast<double> (bands.centreHz (worstBand)));
    info ("mean absolute band change = %.2f dB", sumAbsDb / std::max (1, counted));

    // No band in the voice region may move by more than 3 dB, and on average the
    // spectrum must be nearly unchanged. This is the test that fails first if the
    // mask or the cancellers start acting on the voice itself.
    CHECK_LT (std::abs (worstDb), 3.0);
    CHECK_LT (sumAbsDb / std::max (1, counted), 1.0);
}

// ===========================================================================
void testVoiceWithFeedback()
{
    beginTest ("Feedback removed from under a voice, voice intact");

    const double sr = 48000.0;

    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    auto p = defaultParams();
    p.humEnabled = false;
    p.highPassEnabled = false;
    p.denoiseAmount = 0.5f;
    fs.setParameters (p);

    const int n = static_cast<int> (sr * 6.0);
    const double toneHz = 3183.0;

    SyntheticVoice voice (sr, 150.0);
    Rng rng (23);

    std::vector<float> sig (static_cast<size_t> (n));
    std::vector<float> voiceOnly (static_cast<size_t> (n));
    double phase = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / sr;
        const float v = voice.next();
        voiceOnly[static_cast<size_t> (i)] = v;

        phase += 2.0 * fbk::kPi * toneHz / sr;
        // Feedback starts at t = 1 s and rings up.
        const double ramp = clampf (static_cast<float> ((t - 1.0) / 1.5), 0.0f, 1.0f);
        sig[static_cast<size_t> (i)] =
            v + static_cast<float> (0.25 * ramp * std::sin (phase)) + 0.0015f * rng.gaussian();
    }

    const auto out = run (fs, sig, 128);

    const int mStart = n - static_cast<int> (sr * 2.0);
    const int mLen = static_cast<int> (sr * 2.0);

    const double toneIn  = goertzelMagnitude (sig.data() + mStart, mLen, toneHz, sr);
    const double toneOut = goertzelMagnitude (out.data() + mStart, mLen, toneHz, sr);
    const double toneAtten = toDb (toneOut) - toDb (toneIn);
    info ("tone under voice: attenuation = %.1f dB", toneAtten);
    CHECK_LT (toneAtten, -18.0);

    // The voice must survive. Compare the output against the clean voice, not
    // against the mixed input, so the tone does not flatter the result.
    const double voiceRmsClean = rms (voiceOnly.data() + mStart, mLen);
    const double voiceRmsOut   = rms (out.data() + mStart, mLen);
    const double voiceDb = toDb (voiceRmsOut) - toDb (voiceRmsClean);
    info ("output level vs clean voice = %+.2f dB", voiceDb);
    CHECK_LT (std::abs (voiceDb), 2.0);
}

// ===========================================================================
void testHumCancellation()
{
    beginTest ("Mains hum cancellation");

    for (double fundamental : { 50.0, 60.0 })
    {
        const double sr = 48000.0;
        FeedbackSuppressor fs;
        fs.prepare (sr, 256);
        auto p = defaultParams();
        p.feedbackEnabled = false;
        p.denoiseEnabled = false;
        p.humEnabled = true;
        p.humHarmonics = 8;
        p.highPassEnabled = false;
        fs.setParameters (p);

        const int n = static_cast<int> (sr * 5.0);
        Rng rng (5);
        std::vector<float> sig (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            const double t = static_cast<double> (i) / sr;
            double h = 0.0;
            for (int m = 1; m <= 6; ++m)
                h += (0.08 / m) * std::sin (2.0 * fbk::kPi * fundamental * m * t);
            sig[static_cast<size_t> (i)] = static_cast<float> (h) + 0.001f * rng.gaussian();
        }

        const auto out = run (fs, sig, 128);

        const int mStart = n - static_cast<int> (sr * 1.5);
        const int mLen = static_cast<int> (sr * 1.5);

        double worstAtten = -1000.0;
        for (int m = 1; m <= 4; ++m)
        {
            const double f = fundamental * m;
            const double a = toDb (goertzelMagnitude (out.data() + mStart, mLen, f, sr))
                           - toDb (goertzelMagnitude (sig.data() + mStart, mLen, f, sr));
            worstAtten = std::max (worstAtten, a);
        }
        info2 ("%.0f Hz hum: worst harmonic attenuation = %.1f dB", fundamental, worstAtten);
        CHECK (fs.metering().humActive);
        CHECK_CLOSE (static_cast<double> (fs.metering().humFundamentalHz), fundamental, 0.5);
        CHECK_LT (worstAtten, -20.0);
    }
}

// ===========================================================================
void testNumericalRobustness()
{
    beginTest ("Numerical robustness: NaN, inf, denormals, long silence");

    FeedbackSuppressor fs;
    fs.prepare (48000.0, 256);
    fs.setParameters (defaultParams());

    // Hostile input.
    std::vector<float> sig (4096, 0.0f);
    sig[10]  = std::numeric_limits<float>::quiet_NaN();
    sig[100] = std::numeric_limits<float>::infinity();
    sig[200] = -std::numeric_limits<float>::infinity();
    sig[300] = 1.0e30f;
    sig[400] = 1.0e-42f;   // denormal
    for (int i = 1000; i < 2000; ++i)
        sig[static_cast<size_t> (i)] = 0.5f;

    auto out = run (fs, sig, 64);

    bool allFinite = true;
    for (float v : out)
        if (! std::isfinite (v))
            allFinite = false;
    CHECK (allFinite);

    // After the hostile input, normal signal must still pass sensibly.
    std::vector<float> good (48000, 0.0f);
    Rng rng (31);
    for (auto& v : good)
        v = 0.1f * rng.uniform();
    auto goodOut = run (fs, good, 64);

    bool stillFinite = true;
    for (float v : goodOut)
        if (! std::isfinite (v))
            stillFinite = false;
    CHECK (stillFinite);

    const double outRms = rms (goodOut.data(), static_cast<int> (goodOut.size()));
    info ("post-recovery output RMS = %.4f", outRms);
    CHECK_GT (outRms, 0.01);

    // 20 seconds of silence must not accumulate anything. The first second is
    // skipped: the mask FIR and the high-pass legitimately hold a tail of the
    // preceding signal, and asserting that a filter empties instantly would be
    // asserting something false. From one second on the output must be exactly
    // zero - no drifting oscillator, no self-oscillation, no denormal grind.
    std::vector<float> silence (48000 * 20, 0.0f);
    auto silenceOut = run (fs, silence, 512);

    double tailMax = 0.0;
    for (int i = 0; i < 48000; ++i)
        tailMax = std::max (tailMax, std::abs (static_cast<double> (silenceOut[static_cast<size_t> (i)])));
    info ("filter tail during first second of silence = %.3g", tailMax);

    double maxAbs = 0.0;
    for (size_t i = 48000; i < silenceOut.size(); ++i)
        maxAbs = std::max (maxAbs, std::abs (static_cast<double> (silenceOut[i])));
    info ("max output over the following 19 s of silence = %.3g", maxAbs);
    CHECK_LT (maxAbs, 1.0e-9);
}

// ===========================================================================
void testBlockSizeInvariance()
{
    beginTest ("Output independent of host block size");

    const double sr = 48000.0;
    const int n = static_cast<int> (sr * 2.0);

    SyntheticVoice voice (sr, 140.0);
    std::vector<float> sig (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        sig[static_cast<size_t> (i)] = voice.next();

    std::vector<std::vector<float>> results;
    for (int blockSize : { 16, 32, 64, 128, 480, 512 })
    {
        FeedbackSuppressor fs;
        fs.prepare (sr, 512);
        fs.setParameters (defaultParams());
        results.push_back (run (fs, sig, blockSize));
    }

    // Every block size must give bit-identical output: the chain is driven
    // sample by sample off an internal hop counter, so the host's buffering must
    // not be observable at all. This is what lets it run at a 16-sample buffer.
    double worst = 0.0;
    for (size_t r = 1; r < results.size(); ++r)
        for (size_t i = 0; i < results[0].size(); ++i)
            worst = std::max (worst, std::abs (static_cast<double> (results[0][i] - results[r][i])));

    info ("max deviation across block sizes = %.3g", worst);
    CHECK_LT (worst, 1.0e-9);
}

// ===========================================================================
void testSustainedNoteNotTreatedAsFeedback()
{
    beginTest ("Sustained sung note is not mistaken for feedback");

    const double sr = 48000.0;
    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    auto p = defaultParams();
    p.denoiseEnabled = false;
    p.humEnabled = false;
    p.highPassEnabled = false;
    p.feedbackSensitivity = 0.5f;
    fs.setParameters (p);

    // A held note: strong, sustained, harmonically rich, with the vibrato and
    // jitter a real voice has. Every one of these harmonics is loud, persistent
    // and narrow-ish - which is why PAPR or persistence alone would misfire here
    // and why the criteria are combined.
    const int n = static_cast<int> (sr * 6.0);
    SyntheticVoice voice (sr, 220.0);
    std::vector<float> sig (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        sig[static_cast<size_t> (i)] = 2.5f * voice.next();

    const auto out = run (fs, sig, 128);

    const int mStart = n - static_cast<int> (sr * 2.0);
    const int mLen = static_cast<int> (sr * 2.0);

    const double inRms = rms (sig.data() + mStart, mLen);
    const double outRms = rms (out.data() + mStart, mLen);
    const double db = toDb (outRms) - toDb (inRms);
    info ("held-note level change = %+.2f dB", db);
    info ("confirmed tones during held note = %.0f", static_cast<double> (fs.metering().confirmedTones));

    CHECK_LT (std::abs (db), 1.5);
}
// ===========================================================================
void testNoAllocationOnAudioThread()
{
    beginTest ("No allocation in the audio path");

    const double sr = 48000.0;
    FeedbackSuppressor fs;
    fs.prepare (sr, 128);

    auto p = defaultParams();
    p.dereverbEnabled = true;
    fs.setParameters (p);
    fs.captureRing().setEnabled (true);

    SyntheticVoice voice (sr, 145.0);
    Rng rng (13);
    std::vector<float> block (128);

    // Warm up outside the armed window: the first frames touch every code path
    // once, and anything that legitimately allocates once at start-up should be
    // in prepare(), not counted here.
    for (int i = 0; i < 200; ++i)
    {
        for (auto& v : block)
            v = voice.next() + 0.002f * rng.gaussian();
        fs.processBlock (block.data(), static_cast<int> (block.size()));
    }

    rtcheck::arm();

    for (int i = 0; i < 2000; ++i)
    {
        for (auto& v : block)
            v = voice.next() + 0.002f * rng.gaussian();

        // Sweep parameters while processing, because parameter changes are
        // applied from inside processBlock and are the likeliest place for a
        // hidden allocation to be sitting.
        p.feedbackSensitivity = 0.2f + 0.6f * static_cast<float> (i % 50) / 50.0f;
        p.denoiseAmount = 0.3f + 0.5f * static_cast<float> (i % 30) / 30.0f;
        p.humHarmonics = 4 + (i % 8);
        p.maxAttenuationDb = 6.0f + static_cast<float> (i % 12);

        // And toggle the phase mode, which changes the filter length. This is
        // the case that was allocating: switching modes used to resize four
        // coefficient vectors, on the audio thread, on every press.
        p.qualityMode = ((i / 100) % 2) == 1;

        fs.setParameters (p);
        fs.processBlock (block.data(), static_cast<int> (block.size()));
    }

    const int allocations = rtcheck::disarm();
    info ("allocations during 2000 blocks with parameter sweeps = %.0f",
          static_cast<double> (allocations));
    CHECK (allocations == 0);
}

// ===========================================================================
void testPhaseModeSwitchIsClean()
{
    beginTest ("Switching phase mode mid-stream stays well behaved");

    const double sr = 48000.0;
    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    auto p = defaultParams();
    fs.setParameters (p);

    SyntheticVoice voice (sr, 135.0);
    const int n = static_cast<int> (sr * 4.0);
    std::vector<float> sig (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        sig[static_cast<size_t> (i)] = voice.next();

    std::vector<float> out = sig;
    int pos = 0;
    int toggles = 0;
    while (pos < n)
    {
        const int len = std::min (256, n - pos);

        // Flip modes twice a second, in the middle of continuous audio.
        if ((pos / 24000) % 2 == 1 && ! p.qualityMode) { p.qualityMode = true;  ++toggles; fs.setParameters (p); }
        if ((pos / 24000) % 2 == 0 && p.qualityMode)   { p.qualityMode = false; ++toggles; fs.setParameters (p); }

        fs.processBlock (out.data() + pos, len);
        pos += len;
    }

    CHECK_GT (toggles, 2);

    bool finite = true;
    double peak = 0.0;
    for (float v : out)
    {
        if (! std::isfinite (v))
            finite = false;
        peak = std::max (peak, std::abs (static_cast<double> (v)));
    }
    CHECK (finite);

    // The output must not blow up. A discontinuity at the switch is expected and
    // unavoidable - the filter length changes - but it must stay bounded.
    const double inPeak = *std::max_element (sig.begin(), sig.end());
    info2 ("input peak %.3f, output peak %.3f", inPeak, peak);
    CHECK_LT (peak, inPeak * 4.0 + 0.1);
}

// ===========================================================================
void testRealTimeFactor()
{
    beginTest ("CPU cost / real-time factor");

    // Measured at a 16-sample buffer, which is the setting that matters: it is
    // where a live insert actually runs and where per-block overhead hurts most.
    // Reported as a real-time factor - the fraction of a single core needed to
    // keep up with one mono channel.
    for (int blockSize : { 16, 64, 512 })
    {
        const double sr = 48000.0;
        FeedbackSuppressor fs;
        fs.prepare (sr, blockSize);
        auto p = defaultParams();
        p.dereverbEnabled = true;   // worst case: every stage active
        fs.setParameters (p);

        const int seconds = 10;
        const int n = static_cast<int> (sr) * seconds;
        SyntheticVoice voice (sr, 140.0);
        Rng rng (77);
        std::vector<float> sig (static_cast<size_t> (n));
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            phase += 2.0 * fbk::kPi * 2500.0 / sr;
            sig[static_cast<size_t> (i)] = voice.next()
                                         + static_cast<float> (0.1 * std::sin (phase))
                                         + 0.002f * rng.gaussian();
        }

        const auto t0 = std::chrono::steady_clock::now();
        (void) run (fs, sig, blockSize);
        const auto t1 = std::chrono::steady_clock::now();

        const double elapsed = std::chrono::duration<double> (t1 - t0).count();
        const double rtf = elapsed / static_cast<double> (seconds);
        std::printf ("  info  block %4d: %.2f s wall for %d s audio -> RTF %.4f "
                     "(%.1f%% of one core, ~%d instances per core)\n",
                     blockSize, elapsed, seconds, rtf, rtf * 100.0,
                     static_cast<int> (1.0 / std::max (rtf, 1.0e-6)));

        // A generous ceiling: CI runners are slow and shared. The point of the
        // assertion is to catch an accidental order-of-magnitude regression, not
        // to benchmark the runner.
        CHECK_LT (rtf, 0.5);
    }
}
} // namespace

int main()
{
    std::printf ("FBKSuppressor DSP test suite\n");

    testFft();
    testErbBands();
    testMinimumPhaseDesign();
    testZeroLatency();
    testQualityModeLatency();
    testUnityAtZeroStrength();
    testFeedbackSuppression();
    testVoicePreservation();
    testVoiceWithFeedback();
    testHumCancellation();
    testNumericalRobustness();
    testBlockSizeInvariance();
    testSustainedNoteNotTreatedAsFeedback();
    testNoAllocationOnAudioThread();
    testPhaseModeSwitchIsClean();
    testRealTimeFactor();

    return test::summary();
}
