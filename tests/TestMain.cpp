// FBKSuppressor DSP tests.
//
// The tests that matter most here are the ones that verify the two claims the
// whole design rests on: that the audio path adds no latency, and that removing a
// feedback tone does not remove the voice sitting on top of it.
#include "TestFramework.h"
#include "SignalUtils.h"

#include "Analyser.h"
#include "Calibration.h"
#include "SweepMeasurement.h"
#include "Telemetry.h"
#include "ErbBands.h"
#include "FeedbackSuppressor.h"
#include "Fft.h"
#include "MaskFilter.h"
#include "NeuralSeparator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <utility>
#include <string>
#include <limits>
#include <cstring>
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
void testHistogramPercentiles()
{
    beginTest ("Histogram percentiles");

    Histogram h;
    h.prepare (0.0f, 100.0f);

    // Uniform 0..99: the median should land near 50 and the extremes near the ends.
    for (int i = 0; i < 100; ++i)
        h.add (static_cast<float> (i));

    CHECK (h.count() == 100);
    CHECK_CLOSE (static_cast<double> (h.percentile (0.50f)), 50.0, 2.0);
    CHECK_CLOSE (static_cast<double> (h.percentile (0.05f)), 5.0, 2.0);
    CHECK_CLOSE (static_cast<double> (h.percentile (0.95f)), 95.0, 2.0);
    CHECK_CLOSE (static_cast<double> (h.mean()), 49.5, 1.0);

    // Values outside the range are counted, so a percentile in the overflow
    // returns the range limit rather than silently reporting an in-range value.
    Histogram h2;
    h2.prepare (0.0f, 10.0f);
    for (int i = 0; i < 50; ++i)
        h2.add (-5.0f);
    for (int i = 0; i < 50; ++i)
        h2.add (500.0f);
    CHECK (h2.count() == 100);
    CHECK_CLOSE (static_cast<double> (h2.percentile (0.10f)), 0.0, 0.01);
    CHECK_CLOSE (static_cast<double> (h2.percentile (0.90f)), 10.0, 0.01);
}

// ===========================================================================
// Runs the voice calibration phase on a synthetic voice and returns the profile.
VoiceProfile calibrateOnVoice (FeedbackSuppressor& fs, double sr, double seconds, double f0)
{
    SyntheticVoice voice (sr, f0);
    Rng rng (41);
    const int n = static_cast<int> (sr * seconds);
    std::vector<float> block (256);

    fs.beginCalibration (CalibrationPhase::voice);
    int written = 0;
    while (written < n)
    {
        for (auto& v : block)
            v = voice.next() + 0.0015f * rng.gaussian();
        fs.processBlock (block.data(), static_cast<int> (block.size()));
        written += static_cast<int> (block.size());
    }
    fs.finishCalibration();
    return fs.profile();
}

void testCalibrationMeasuresTheVoice()
{
    beginTest ("Calibration measures the voice it will be judged against");

    const double sr = 48000.0;
    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    fs.setParameters (defaultParams());

    const auto p = calibrateOnVoice (fs, sr, 20.0, 130.0);

    CHECK (p.hasVoice);
    CHECK (p.valid);
    CHECK (p.hasSuggestions);
    info ("criterion samples collected = %.0f", static_cast<double> (p.voiceCriterionSamples));
    CHECK_GT (p.voiceCriterionSamples, 200);

    // f0 should land near the synthetic fundamental. The generator applies a
    // +/-12% contour, so allow for that rather than demanding an exact match.
    info2 ("f0 low %.0f Hz, high %.0f Hz",
           static_cast<double> (p.f0LowHz), static_cast<double> (p.f0HighHz));
    info ("f0 median = %.0f Hz", static_cast<double> (p.f0MedianHz));
    CHECK_GT (p.f0MedianHz, 100.0f);
    CHECK_LT (p.f0MedianHz, 170.0f);

    info2 ("voice PNPR p95 = %.1f dB -> suggested threshold %.1f dB",
           static_cast<double> (p.voicePnprP95Db), static_cast<double> (p.suggestedPnprDb));
    info2 ("voice prominence median %.1f dB, p95 %.1f dB",
           static_cast<double> (p.voiceProminenceMedianDb),
           static_cast<double> (p.voiceProminenceP95Db));
    info ("suggested prominence gate = %.1f dB", static_cast<double> (p.suggestedLocalProminenceDb));
    info2 ("voice FSD p05 = %.2f Hz, median %.2f Hz",
           static_cast<double> (p.voiceFsdP05Hz), static_cast<double> (p.voiceFsdMedianHz));
    info ("suggested FSD threshold = %.2f Hz", static_cast<double> (p.suggestedFsdMaxHz));

    // The whole point: each suggested threshold must sit outside what this voice
    // actually does. PNPR above the voice's 95th percentile, so a tone has to be
    // more isolated than almost anything the voice produced.
    CHECK_GT (p.suggestedPnprDb, p.voicePnprP95Db);

    // FSD is the reverse - feedback is steadier than a voice - so the threshold
    // goes below the voice's typical wander. Asserted against the median rather
    // than the 5th percentile because the suggestion is clamped to a sane range,
    // and a clamp hit would otherwise read as a failure when the behaviour is
    // correct.
    CHECK_GT (p.voiceFsdMedianHz, 0.0f);
    CHECK_LT (p.suggestedFsdMaxHz, p.voiceFsdMedianHz);
    CHECK (p.suggestedFsdMaxHz >= 0.4f && p.suggestedFsdMaxHz <= 8.0f);

    // The prominence gate must end up above what the voice produces.
    CHECK_GT (p.suggestedLocalProminenceDb, p.voiceProminenceMedianDb);
    CHECK (p.suggestedLocalProminenceDb >= 8.0f && p.suggestedLocalProminenceDb <= 20.0f);

    // Per-band protection must be strongest where the voice actually lives.
    int loudest = 0, quietest = 0;
    for (int b = 1; b < kNumBands; ++b)
    {
        if (p.bandVoiceDb[b] > p.bandVoiceDb[loudest]) loudest = b;
        if (p.bandVoiceDb[b] < p.bandVoiceDb[quietest]) quietest = b;
    }
    info2 ("protection: loudest band %.2f, quietest band %.2f",
           static_cast<double> (p.suggestedVoiceProtection[loudest]),
           static_cast<double> (p.suggestedVoiceProtection[quietest]));
    CHECK_GT (p.suggestedVoiceProtection[loudest], p.suggestedVoiceProtection[quietest]);
}

// ===========================================================================
void testObserveOnlyDoesNotProcess()
{
    beginTest ("Voice calibration never acts on the signal it is measuring");

    const double sr = 48000.0;
    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    auto params = defaultParams();
    params.denoiseEnabled = false;
    params.humEnabled = false;
    params.highPassEnabled = false;
    fs.setParameters (params);

    // A dead-steady tone - exactly what the detector would normally pounce on.
    const double toneHz = 1500.0;
    const int n = static_cast<int> (sr * 4.0);
    std::vector<float> sig (static_cast<size_t> (n));
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        phase += 2.0 * fbk::kPi * toneHz / sr;
        sig[static_cast<size_t> (i)] = static_cast<float> (0.3 * std::sin (phase));
    }

    fs.beginCalibration (CalibrationPhase::voice);
    auto out = run (fs, sig, 128);
    fs.finishCalibration();

    const int mStart = n - static_cast<int> (sr);
    const int mLen = static_cast<int> (sr);
    const double atten = toDb (goertzelMagnitude (out.data() + mStart, mLen, toneHz, sr))
                       - toDb (goertzelMagnitude (sig.data() + mStart, mLen, toneHz, sr));
    info ("tone attenuation during voice calibration = %.2f dB", atten);

    // Nothing may be cancelled while characterising a voice, or the measurement
    // would be of the processed signal rather than the real one.
    CHECK_LT (std::abs (atten), 0.5);
}

// ===========================================================================
void testProfileKeepsVoiceSafeAndTonesCaught()
{
    beginTest ("Applying a profile keeps the voice safe and still catches tones");

    const double sr = 48000.0;

    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    auto params = defaultParams();
    params.humEnabled = false;
    params.highPassEnabled = false;
    fs.setParameters (params);

    const auto p = calibrateOnVoice (fs, sr, 20.0, 150.0);
    CHECK (p.hasVoice);

    fs.applyProfile (p);
    CHECK (fs.hasProfileApplied());

    // A held note must still not be confirmed with calibrated thresholds - if
    // calibration made the detector trigger-happy, this is where it shows.
    {
        const int n = static_cast<int> (sr * 5.0);
        SyntheticVoice held (sr, 220.0);
        std::vector<float> sig (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            sig[static_cast<size_t> (i)] = 2.5f * held.next();

        const auto out = run (fs, sig, 128);
        const int mStart = n - static_cast<int> (sr * 2.0);
        const int mLen = static_cast<int> (sr * 2.0);
        const double db = toDb (rms (out.data() + mStart, mLen))
                        - toDb (rms (sig.data() + mStart, mLen));
        info ("held note with profile applied = %+.2f dB", db);
        CHECK_LT (std::abs (db), 1.5);
    }

    // And a real feedback tone must still be cancelled hard.
    {
        fs.reset();
        const double toneHz = 2890.0;
        const int n = static_cast<int> (sr * 5.0);
        Rng rng (17);
        std::vector<float> sig (static_cast<size_t> (n));
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            phase += 2.0 * fbk::kPi * toneHz / sr;
            const double t = static_cast<double> (i) / sr;
            const double ramp = std::min (1.0, t / 1.0);
            sig[static_cast<size_t> (i)] =
                static_cast<float> (0.3 * ramp * std::sin (phase)) + 0.002f * rng.gaussian();
        }

        const auto out = run (fs, sig, 128);
        const int mStart = n - static_cast<int> (sr);
        const int mLen = static_cast<int> (sr);
        const double atten = toDb (goertzelMagnitude (out.data() + mStart, mLen, toneHz, sr))
                           - toDb (goertzelMagnitude (sig.data() + mStart, mLen, toneHz, sr));
        info ("tone attenuation with profile applied = %.1f dB", atten);
        CHECK_LT (atten, -20.0);
    }
}

// ===========================================================================
void testModePriorsSpeedUpEngagement()
{
    beginTest ("Room-mode priors make engagement measurably faster");

    const double sr = 48000.0;
    const double toneHz = 1830.0;

    // How much tone energy leaks through in the first second, with and without a
    // prior at that frequency. Faster engagement means less leaked energy - an
    // end-to-end measurement rather than an internal counter.
    const auto leakedEnergy = [&] (bool withPrior) -> double
    {
        FeedbackSuppressor fs;
        fs.prepare (sr, 256);
        auto params = defaultParams();
        params.denoiseEnabled = false;
        params.humEnabled = false;
        params.highPassEnabled = false;
        fs.setParameters (params);

        if (withPrior)
        {
            VoiceProfile p;
            p.valid = true;
            p.numModes = 1;
            p.modes[0].freqHz = toneHz;
            p.modes[0].strengthDb = 30.0f;
            p.modes[0].engagedSeconds = 10.0f;
            p.modes[0].hits = 20;
            fs.applyProfile (p);
        }

        // Tone switches on abruptly at t = 0.25 s over a light noise floor.
        const int n = static_cast<int> (sr * 1.5);
        Rng rng (61);
        std::vector<float> sig (static_cast<size_t> (n));
        double phase = 0.0;
        const int onset = static_cast<int> (sr * 0.25);
        for (int i = 0; i < n; ++i)
        {
            phase += 2.0 * fbk::kPi * toneHz / sr;
            const float tone = i >= onset ? static_cast<float> (0.3 * std::sin (phase)) : 0.0f;
            sig[static_cast<size_t> (i)] = tone + 0.002f * rng.gaussian();
        }

        const auto out = run (fs, sig, 64);

        double energy = 0.0;
        for (int i = onset; i < n; ++i)
            energy += static_cast<double> (out[static_cast<size_t> (i)]) * out[static_cast<size_t> (i)];
        return energy;
    };

    const double without = leakedEnergy (false);
    const double with = leakedEnergy (true);

    info2 ("leaked tone energy: no prior %.4f, with prior %.4f", without, with);
    info ("reduction = %.1f%%", 100.0 * (without - with) / std::max (without, 1e-12));

    // The prior must help, and must not somehow make things worse.
    CHECK_LT (with, without);
}


// ===========================================================================
// A stand-in for the v0.2 network: reports fixed values, so each test controls
// exactly what the "model" claims and can check what the DSP does with it.
class MockSeparator final : public SpectralSeparator
{
public:
    MockSeparator (float voice, float feedback, bool ready = true)
        : voice_ (voice), feedback_ (feedback), ready_ (ready) {}

    void prepare (double, const ErbBands&) override {}
    void reset() override {}
    void process (const float*, SeparatorOutput& out) override
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            out.voicePresence[b] = voice_;
            out.feedbackProb[b] = feedback_;
        }
        out.voiceActivity = voice_;
    }
    bool isReady() const override { return ready_; }
    const char* name() const override { return "mock"; }

private:
    float voice_, feedback_;
    bool ready_;
};

// ===========================================================================
void testLearnedFeedbackPriorSpeedsEngagement()
{
    beginTest ("Learned feedback prior makes engagement faster, like a mode prior");

    const double sr = 48000.0;
    const double toneHz = 1830.0;

    // Same end-to-end measurement as the mode-prior test: tone energy leaking
    // through in the first second after an abrupt onset. The baseline is a model
    // that is attached and ready but says "no feedback", so the comparison isolates
    // the prior from the presence replacement that comes with any attached model.
    const auto leaked = [&] (float feedbackProb) -> double
    {
        FeedbackSuppressor fs;
        fs.prepare (sr, 256);
        auto params = defaultParams();
        params.denoiseEnabled = false;
        params.humEnabled = false;
        params.highPassEnabled = false;
        fs.setParameters (params);

        MockSeparator sep (0.0f, feedbackProb);
        fs.setSeparator (&sep);

        const int n = static_cast<int> (sr * 1.5);
        const int onset = static_cast<int> (sr * 0.25);
        Rng rng (61);
        std::vector<float> sig (static_cast<size_t> (n));
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            phase += 2.0 * fbk::kPi * toneHz / sr;
            const float tone = i >= onset ? static_cast<float> (0.3 * std::sin (phase)) : 0.0f;
            sig[static_cast<size_t> (i)] = tone + 0.002f * rng.gaussian();
        }

        const auto out = run (fs, sig, 64);
        double e = 0.0;
        for (int i = onset; i < n; ++i)
            e += static_cast<double> (out[static_cast<size_t> (i)]) * out[static_cast<size_t> (i)];
        return e;
    };

    const double unsure = leaked (0.0f);
    const double sure = leaked (1.0f);
    info2 ("leaked tone energy: model says 0 -> %.4f, model says 1 -> %.4f", unsure, sure);
    info ("reduction = %.1f%%", 100.0 * (unsure - sure) / std::max (unsure, 1e-12));
    CHECK_LT (sure, unsure);
}

// ===========================================================================
void testWrongModelCannotMakeSungNoteFeedback()
{
    beginTest ("A confidently wrong model cannot turn a sung note into feedback");

    // The safety property of the prior: it relaxes ratio and persistence evidence
    // only, and never touches the frequency-stability criterion. So a model that
    // insists every band is feedback, with zero voice presence, still cannot make
    // the detector confirm a moving voice.
    const double sr = 48000.0;
    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    auto p = defaultParams();
    p.denoiseEnabled = false;
    p.humEnabled = false;
    p.highPassEnabled = false;
    fs.setParameters (p);

    MockSeparator sep (0.0f, 1.0f);
    fs.setSeparator (&sep);

    const int n = static_cast<int> (sr * 6.0);
    SyntheticVoice voice (sr, 220.0);
    std::vector<float> sig (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        sig[static_cast<size_t> (i)] = 2.5f * voice.next();

    // Process in 0.25 s slices and take the worst confirmed count seen at any
    // point, not just the end - a false positive that came and went still matters.
    std::vector<float> out = sig;
    int worstConfirmed = 0;
    const int slice = static_cast<int> (sr * 0.25);
    for (int pos = 0; pos < n; pos += slice)
    {
        const int len = std::min (slice, n - pos);
        for (int o = 0; o < len; o += 128)
            fs.processBlock (out.data() + pos + o, std::min (128, len - o));
        worstConfirmed = std::max (worstConfirmed, fs.metering().confirmedTones);
    }

    const int mStart = n - static_cast<int> (sr * 2.0);
    const int mLen = static_cast<int> (sr * 2.0);
    const double db = toDb (rms (out.data() + mStart, mLen)) - toDb (rms (sig.data() + mStart, mLen));
    info ("held-note level change = %+.2f dB", db);
    info ("worst confirmed-tone count over the run = %.0f", static_cast<double> (worstConfirmed));

    CHECK_LT (std::abs (db), 1.5);
    CHECK (worstConfirmed == 0);
}

// ===========================================================================
void testSeparatorPresenceReachesTheMask()
{
    beginTest ("Separator voice presence reaches the mask and is detachable");

    const double sr = 48000.0;
    const int n = static_cast<int> (sr * 4.0);

    Rng rng (23);
    std::vector<float> noise (static_cast<size_t> (n));
    for (auto& v : noise)
        v = 0.05f * rng.gaussian();

    const auto outputRms = [&] (SpectralSeparator* sep) -> double
    {
        FeedbackSuppressor fs;
        fs.prepare (sr, 256);
        auto p = defaultParams();
        p.feedbackEnabled = false;
        p.humEnabled = false;
        p.highPassEnabled = false;
        p.denoiseAmount = 1.0f;
        p.voiceProtection = 1.0f;
        fs.setParameters (p);
        fs.setSeparator (sep);
        const auto out = run (fs, noise, 128);
        return rms (out.data() + n / 2, n / 2);
    };

    MockSeparator saysNoVoice (0.0f, 0.0f), saysVoice (1.0f, 0.0f), notReady (1.0f, 1.0f, false);

    const double none = outputRms (nullptr);
    const double noVoice = outputRms (&saysNoVoice);
    const double voice = outputRms (&saysVoice);
    const double idle = outputRms (&notReady);

    info2 ("steady noise out: no model %.5f, model says no voice %.5f", none, noVoice);
    info2 ("                  model says voice %.5f, model not ready %.5f", voice, idle);

    // Where the model says "voice", the mask backs off and more passes through.
    CHECK_GT (voice, noVoice * 1.05);
    // A model that is attached but not ready must change nothing at all.
    CHECK_CLOSE (idle, none, 1.0e-12);
}


// ===========================================================================
// Neural separator tests. There are no trained weights in the repository, so the
// tests build models with deterministic random weights. That is enough to pin
// down everything that is a property of the engine rather than of training: the
// file format, the arithmetic, robustness, real-time safety, and - the one that
// matters most - that no model at all, however it was trained, can make the
// detector mistake a held note for feedback.
struct TestModel
{
    std::vector<unsigned char> bytes;
    std::vector<std::pair<std::string, std::vector<float>>> tensors;
    std::vector<std::pair<std::string, std::vector<uint32_t>>> shapes;

    const std::vector<float>& get (const char* n) const
    {
        for (const auto& t : tensors)
            if (t.first == n)
                return t.second;
        std::abort();
    }
};

void appendU32 (std::vector<unsigned char>& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back (static_cast<unsigned char> ((v >> (8 * i)) & 0xFF));
}

TestModel makeModel (int H, int T, int S, uint32_t seed, float scale)
{
    TestModel m;
    Rng rng (seed);
    const auto add = [&] (const char* name, std::vector<uint32_t> dims, int fanIn)
    {
        size_t n = 1;
        for (auto d : dims)
            n *= d;
        std::vector<float> v (n);
        for (auto& x : v)
            x = scale * rng.uniform() / std::sqrt (static_cast<float> (fanIn));
        m.tensors.emplace_back (name, std::move (v));
        m.shapes.emplace_back (name, std::move (dims));
    };

    const uint32_t h = static_cast<uint32_t> (H), t = static_cast<uint32_t> (T);
    const int O = NeuralSeparator::kNumOutputs, F = NeuralSeparator::kNumFeatures;
    add ("in.weight", { h, static_cast<uint32_t> (F) }, F);
    add ("in.bias", { h }, F);
    add ("film.trunk.weight", { t, 4 }, 4);
    add ("film.trunk.bias", { t }, 4);
    add ("film.gamma.weight", { h, t }, T);
    add ("film.gamma.bias", { h }, T);
    add ("film.beta.weight", { h, t }, T);
    add ("film.beta.bias", { h }, T);
    add ("core.weight_ih", { 3 * h, h }, H);
    add ("core.weight_hh", { 3 * h, h }, H);
    add ("core.bias_ih", { 3 * h }, H);
    add ("core.bias_hh", { 3 * h }, H);
    add ("head.weight", { static_cast<uint32_t> (O), h }, H);
    add ("head.bias", { static_cast<uint32_t> (O) }, H);
    if (S > 0)
    {
        const uint32_t sh = static_cast<uint32_t> (S);
        add ("smoother.weight_ih", { 3 * sh, static_cast<uint32_t> (O) }, O);
        add ("smoother.weight_hh", { 3 * sh, sh }, S);
        add ("smoother.bias_ih", { 3 * sh }, S);
        add ("smoother.bias_hh", { 3 * sh }, S);
        add ("smoother.out.weight", { static_cast<uint32_t> (O), sh }, S);
        add ("smoother.out.bias", { static_cast<uint32_t> (O) }, S);
    }

    appendU32 (m.bytes, 0x4D4B4246u);
    appendU32 (m.bytes, 1u);
    appendU32 (m.bytes, static_cast<uint32_t> (m.tensors.size()));
    for (size_t i = 0; i < m.tensors.size(); ++i)
    {
        const auto& name = m.tensors[i].first;
        appendU32 (m.bytes, static_cast<uint32_t> (name.size()));
        m.bytes.insert (m.bytes.end(), name.begin(), name.end());
        appendU32 (m.bytes, static_cast<uint32_t> (m.shapes[i].second.size()));
        for (auto d : m.shapes[i].second)
            appendU32 (m.bytes, d);
        const auto* raw = reinterpret_cast<const unsigned char*> (m.tensors[i].second.data());
        m.bytes.insert (m.bytes.end(), raw, raw + m.tensors[i].second.size() * sizeof (float));
    }
    return m;
}

// An independent implementation of the documented network, in double precision and
// written from the description in NeuralSeparator.h rather than from the engine.
struct ReferenceNet
{
    const TestModel& m;
    int H, T, S;
    std::vector<double> hc, hs;
    double prev[kNumBands] {};
    bool hasPrev { false };

    ReferenceNet (const TestModel& model, int h, int t, int s)
        : m (model), H (h), T (t), S (s), hc (static_cast<size_t> (h), 0.0),
          hs (static_cast<size_t> (std::max (s, 1)), 0.0) {}

    static double sig (double x) { return 1.0 / (1.0 + std::exp (-x)); }

    std::vector<double> dense (const char* w, const char* b, const std::vector<double>& x, int out) const
    {
        const auto& W = m.get (w);
        const auto& B = m.get (b);
        std::vector<double> y (static_cast<size_t> (out));
        for (int o = 0; o < out; ++o)
        {
            double a = B[static_cast<size_t> (o)];
            for (size_t i = 0; i < x.size(); ++i)
                a += static_cast<double> (W[static_cast<size_t> (o) * x.size() + i]) * x[i];
            y[static_cast<size_t> (o)] = a;
        }
        return y;
    }

    void gru (const std::string& p, const std::vector<double>& x, std::vector<double>& h, int n) const
    {
        const auto& Wi = m.get ((p + ".weight_ih").c_str());
        const auto& Wh = m.get ((p + ".weight_hh").c_str());
        const auto& Bi = m.get ((p + ".bias_ih").c_str());
        const auto& Bh = m.get ((p + ".bias_hh").c_str());
        const auto row = [&] (int r, const std::vector<float>& W, const std::vector<double>& v,
                              const std::vector<float>& B)
        {
            double a = B[static_cast<size_t> (r)];
            for (size_t i = 0; i < v.size(); ++i)
                a += static_cast<double> (W[static_cast<size_t> (r) * v.size() + i]) * v[i];
            return a;
        };
        std::vector<double> next (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            const double r = sig (row (i, Wi, x, Bi) + row (i, Wh, h, Bh));
            const double z = sig (row (n + i, Wi, x, Bi) + row (n + i, Wh, h, Bh));
            const double c = std::tanh (row (2 * n + i, Wi, x, Bi) + r * row (2 * n + i, Wh, h, Bh));
            next[static_cast<size_t> (i)] = (1.0 - z) * c + z * h[static_cast<size_t> (i)];
        }
        h = next;
    }

    // Returns the 65 output probabilities.
    std::vector<double> step (const float* energy, const float* control)
    {
        double db[kNumBands];
        double mx = -120.0;
        for (int b = 0; b < kNumBands; ++b)
        {
            db[b] = 10.0 * std::log10 (static_cast<double> (energy[b]) + 1e-12);
            mx = std::max (mx, db[b]);
        }
        std::vector<double> f (64);
        for (int b = 0; b < kNumBands; ++b)
        {
            f[static_cast<size_t> (b)] = std::clamp ((db[b] - mx) / 60.0, -1.0, 0.0);
            f[static_cast<size_t> (kNumBands + b)] =
                hasPrev ? std::clamp ((db[b] - prev[b]) / 20.0, -1.0, 1.0) : 0.0;
            prev[b] = db[b];
        }
        hasPrev = true;

        std::vector<double> c (control, control + 4);
        auto tr = dense ("film.trunk.weight", "film.trunk.bias", c, T);
        for (auto& v : tr)
            v = std::tanh (v);
        const auto g = dense ("film.gamma.weight", "film.gamma.bias", tr, H);
        const auto be = dense ("film.beta.weight", "film.beta.bias", tr, H);

        auto x = dense ("in.weight", "in.bias", f, H);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = (1.0 + g[i]) * std::tanh (x[i]) + be[i];

        gru ("core", x, hc, H);
        auto l = dense ("head.weight", "head.bias", hc, 65);
        if (S > 0)
        {
            std::vector<double> p (65);
            for (size_t i = 0; i < 65; ++i)
                p[i] = sig (l[i]);
            gru ("smoother", p, hs, S);
            const auto d = dense ("smoother.out.weight", "smoother.out.bias", hs, 65);
            for (size_t i = 0; i < 65; ++i)
                l[i] += d[i];
        }
        for (auto& v : l)
            v = sig (v);
        return l;
    }
};

void testNeuralSeparatorLoadValidation()
{
    beginTest ("Neural separator: model file validation");

    const auto good = makeModel (24, 8, 12, 5, 1.0f);
    NeuralSeparator sep;
    CHECK (! sep.isReady());

    auto r = sep.load (good.bytes.data(), good.bytes.size());
    info ("parameters in test model = %.0f", static_cast<double> (sep.parameterCount()));
    CHECK (r.ok);
    CHECK (sep.isReady());
    CHECK (sep.hiddenSize() == 24 && sep.smootherSize() == 12);

    // A model without the optional smoother is valid.
    const auto noSmoother = makeModel (16, 4, 0, 6, 1.0f);
    r = sep.load (noSmoother.bytes.data(), noSmoother.bytes.size());
    CHECK (r.ok && sep.smootherSize() == 0);

    // Every way a file can be wrong must be refused, leave the separator unloaded,
    // and never read out of bounds. Truncating at every length covers the lot.
    int accepted = 0;
    for (size_t len = 0; len < good.bytes.size(); len += 7)
    {
        sep.load (good.bytes.data(), len);
        if (sep.isReady())
            ++accepted;
    }
    info ("truncated prefixes accepted = %.0f", static_cast<double> (accepted));
    CHECK (accepted == 0);

    auto bad = good.bytes;
    bad[0] ^= 0xFF;
    CHECK (! sep.load (bad.data(), bad.size()).ok && ! sep.isReady());

    bad = good.bytes;
    bad[4] = 9;   // version
    CHECK (! sep.load (bad.data(), bad.size()).ok);

    // A NaN weight anywhere is refused.
    bad = good.bytes;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy (bad.data() + bad.size() - 4, &nan, 4);
    CHECK (! sep.load (bad.data(), bad.size()).ok);

    // Wrong shape: swap the head to the wrong output count.
    {
        TestModel wrong = makeModel (16, 4, 0, 7, 1.0f);
        for (size_t i = 0; i < wrong.shapes.size(); ++i)
            if (wrong.shapes[i].first == "head.bias")
                wrong.shapes[i].second[0] = 64;
        // Rebuild the byte stream from the altered shapes (data length now mismatches
        // the dims, which the reader must also catch as truncation or misparse).
        std::vector<unsigned char> b;
        appendU32 (b, 0x4D4B4246u); appendU32 (b, 1u);
        appendU32 (b, static_cast<uint32_t> (wrong.tensors.size()));
        for (size_t i = 0; i < wrong.tensors.size(); ++i)
        {
            const auto& nm = wrong.tensors[i].first;
            appendU32 (b, static_cast<uint32_t> (nm.size()));
            b.insert (b.end(), nm.begin(), nm.end());
            appendU32 (b, static_cast<uint32_t> (wrong.shapes[i].second.size()));
            for (auto d : wrong.shapes[i].second)
                appendU32 (b, d);
            const auto* raw = reinterpret_cast<const unsigned char*> (wrong.tensors[i].second.data());
            const size_t bytesToWrite = static_cast<size_t> ([&] { size_t n = 1; for (auto d : wrong.shapes[i].second) n *= d; return n; }()) * 4;
            b.insert (b.end(), raw, raw + std::min (bytesToWrite, wrong.tensors[i].second.size() * 4));
        }
        CHECK (! sep.load (b.data(), b.size()).ok);
    }
    CHECK (! sep.isReady());
}

void testNeuralSeparatorMatchesReference()
{
    beginTest ("Neural separator: matches an independent double-precision reference");

    for (const int smoother : { 0, 12 })
    {
        const auto model = makeModel (24, 8, smoother, 11u + static_cast<uint32_t> (smoother), 1.5f);
        NeuralSeparator sep;
        CHECK (sep.load (model.bytes.data(), model.bytes.size()).ok);
        ErbBands bands;
        bands.prepare (48000.0);
        sep.prepare (48000.0, bands);

        ReferenceNet ref (model, 24, 8, smoother);
        Rng rng (99);
        double worst = 0.0;
        float control[4] = { 0.5f, 1.0f, 0.5f, 0.8f };
        for (int frame = 0; frame < 300; ++frame)
        {
            float energy[kNumBands];
            for (int b = 0; b < kNumBands; ++b)
                energy[b] = std::pow (10.0f, 2.0f * rng.uniform() - 4.0f * static_cast<float> (b) / kNumBands);

            // Change the controls part-way, so FiLM re-evaluation is exercised.
            if (frame % 50 == 25)
                for (auto& c : control)
                    c = 0.5f + 0.5f * rng.uniform();
            sep.setControl (control, 4);

            SeparatorOutput out;
            sep.process (energy, out);
            const auto want = ref.step (energy, control);

            for (int b = 0; b < kNumBands; ++b)
            {
                worst = std::max (worst, std::abs (static_cast<double> (out.voicePresence[b]) - want[static_cast<size_t> (b)]));
                worst = std::max (worst, std::abs (static_cast<double> (out.feedbackProb[b]) - want[static_cast<size_t> (kNumBands + b)]));
            }
            worst = std::max (worst, std::abs (static_cast<double> (out.voiceActivity) - want[64]));
        }
        info2 ("smoother size %.0f: worst deviation from reference over 300 frames = %.3g",
               static_cast<double> (smoother), worst);
        CHECK_LT (worst, 1.0e-4);
    }
}

void testNeuralSeparatorRobustness()
{
    beginTest ("Neural separator: hostile input cannot poison the output");

    const auto model = makeModel (24, 8, 12, 17, 3.0f);
    NeuralSeparator sep;
    CHECK (sep.load (model.bytes.data(), model.bytes.size()).ok);

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float hostile[] = { nan, inf, -inf, 1.0e30f, -5.0f, 0.0f, 1.0e-45f };

    bool allFinite = true, inRange = true;
    SeparatorOutput out;
    for (int frame = 0; frame < 200; ++frame)
    {
        float energy[kNumBands];
        for (int b = 0; b < kNumBands; ++b)
            energy[b] = hostile[(b + frame) % 7];
        sep.process (energy, out);
        for (int b = 0; b < kNumBands; ++b)
        {
            const float v = out.voicePresence[b], f = out.feedbackProb[b];
            allFinite = allFinite && std::isfinite (v) && std::isfinite (f);
            inRange = inRange && v >= 0.0f && v <= 1.0f && f >= 0.0f && f <= 1.0f;
        }
    }
    CHECK (allFinite);
    CHECK (inRange);

    // And it recovers: normal input afterwards produces the same answer a fresh
    // instance gives, once the recurrent state has settled.
    NeuralSeparator fresh;
    CHECK (fresh.load (model.bytes.data(), model.bytes.size()).ok);
    sep.reset();
    Rng rng (3);
    SeparatorOutput a, b;
    for (int frame = 0; frame < 20; ++frame)
    {
        float energy[kNumBands];
        for (auto& e : energy)
            e = std::pow (10.0f, 3.0f * rng.uniform() - 2.0f);
        sep.process (energy, a);
        fresh.process (energy, b);
    }
    double dev = 0.0;
    for (int i = 0; i < kNumBands; ++i)
        dev = std::max (dev, static_cast<double> (std::abs (a.voicePresence[i] - b.voicePresence[i])));
    info ("deviation from a fresh instance after reset = %.3g", dev);
    CHECK_LT (dev, 1.0e-6);
}

void testArbitraryModelCannotMakeSungNoteFeedback()
{
    beginTest ("No model, however it was trained, can turn a sung note into feedback");

    // Several arbitrary models with large weights, so their outputs swing hard and
    // often saturate. Whatever they claim, the held note must be untouched: the
    // prior never relaxes the frequency-stability criterion.
    const double sr = 48000.0;
    for (uint32_t seed = 1; seed <= 4; ++seed)
    {
        const auto model = makeModel (32, 8, 16, 100u + seed, 4.0f);
        NeuralSeparator sep;
        CHECK (sep.load (model.bytes.data(), model.bytes.size()).ok);

        FeedbackSuppressor fs;
        fs.prepare (sr, 256);
        auto p = defaultParams();
        p.denoiseEnabled = false;
        p.humEnabled = false;
        p.highPassEnabled = false;
        fs.setParameters (p);
        fs.setSeparator (&sep);

        const int n = static_cast<int> (sr * 5.0);
        SyntheticVoice voice (sr, 220.0);
        std::vector<float> sig (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            sig[static_cast<size_t> (i)] = 2.5f * voice.next();

        std::vector<float> out = sig;
        int worstConfirmed = 0;
        for (int pos = 0; pos < n; pos += 128)
        {
            fs.processBlock (out.data() + pos, std::min (128, n - pos));
            worstConfirmed = std::max (worstConfirmed, fs.metering().confirmedTones);
        }

        const int mStart = n - static_cast<int> (sr * 2.0);
        const int mLen = static_cast<int> (sr * 2.0);
        const double db = toDb (rms (out.data() + mStart, mLen)) - toDb (rms (sig.data() + mStart, mLen));
        info2 ("seed %.0f: held-note change %+.2f dB", static_cast<double> (seed), db);
        CHECK (worstConfirmed == 0);
        CHECK_LT (std::abs (db), 1.5);
    }
}

void testNeuralSeparatorBlockSizeInvariance()
{
    beginTest ("Neural separator: output independent of host block size");

    const double sr = 48000.0;
    const auto model = makeModel (24, 8, 12, 41, 2.0f);

    const auto render = [&] (int blockSize)
    {
        NeuralSeparator sep;
        sep.load (model.bytes.data(), model.bytes.size());
        FeedbackSuppressor fs;
        fs.prepare (sr, blockSize);
        auto p = defaultParams();
        p.dereverbEnabled = true;
        fs.setParameters (p);
        fs.setSeparator (&sep);

        SyntheticVoice voice (sr, 150.0);
        Rng rng (8);
        std::vector<float> sig (static_cast<size_t> (sr * 2.0));
        for (auto& v : sig)
            v = voice.next() + 0.01f * rng.gaussian();
        return run (fs, sig, blockSize);
    };

    const auto a = render (16);
    const auto b = render (512);
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max (worst, static_cast<double> (std::abs (a[i] - b[i])));
    info ("max deviation 16 vs 512 with a model attached = %.3g", worst);
    CHECK_LT (worst, 1.0e-9);
}

void testNeuralSeparatorNoAllocation()
{
    beginTest ("Neural separator: no allocation in the audio path");

    const double sr = 48000.0;
    const auto model = makeModel (48, 12, 24, 77, 1.5f);
    NeuralSeparator sep;
    CHECK (sep.load (model.bytes.data(), model.bytes.size()).ok);

    FeedbackSuppressor fs;
    fs.prepare (sr, 128);
    auto p = defaultParams();
    fs.setParameters (p);
    fs.setSeparator (&sep);

    SyntheticVoice voice (sr, 145.0);
    Rng rng (13);
    std::vector<float> block (128);
    for (int i = 0; i < 200; ++i)
    {
        for (auto& v : block)
            v = voice.next() + 0.002f * rng.gaussian();
        fs.processBlock (block.data(), 128);
    }

    rtcheck::arm();
    for (int i = 0; i < 2000; ++i)
    {
        for (auto& v : block)
            v = voice.next() + 0.002f * rng.gaussian();
        // Controls change constantly, which re-evaluates the FiLM branch.
        p.feedbackSensitivity = 0.2f + 0.6f * static_cast<float> (i % 50) / 50.0f;
        p.maxAttenuationDb = 6.0f + static_cast<float> (i % 12);
        fs.setParameters (p);
        fs.processBlock (block.data(), 128);
    }
    const int allocations = rtcheck::disarm();
    info ("allocations during 2000 blocks with a model attached = %.0f", static_cast<double> (allocations));
    CHECK (allocations == 0);
}

// ===========================================================================
void testTelemetryRing()
{
    beginTest ("Telemetry ring: lossless when drained, drops rather than blocks");

    TelemetryRing ring;
    ring.prepare (16);
    ring.setEnabled (true);

    TelemetryFrame f {};
    TelemetryFrame out {};

    // Drained as we go: nothing lost, values intact.
    for (int i = 0; i < 100; ++i)
    {
        f.sampleTime = i;
        CHECK (ring.push (f));
        CHECK (ring.pop (out));
        CHECK (out.sampleTime == i);
    }
    CHECK (ring.droppedFrames() == 0);
    CHECK (! ring.pop (out));

    // Consumer stalls: the producer must drop and count, never block or overwrite
    // unread data. Dropping diagnostics beats an audio dropout every time.
    ring.resetDropped();
    int accepted = 0;
    for (int i = 0; i < 100; ++i)
        if (ring.push (f))
            ++accepted;

    info ("accepted %.0f of 100 with a stalled consumer, ", static_cast<double> (accepted));
    info ("dropped = %.0f", static_cast<double> (ring.droppedFrames()));
    CHECK (accepted < 100);
    CHECK (accepted + ring.droppedFrames() == 100);

    // What was accepted must still be readable and uncorrupted.
    int recovered = 0;
    while (ring.pop (out))
        ++recovered;
    CHECK (recovered == accepted);

    // Disabled means silent.
    ring.setEnabled (false);
    CHECK (! ring.push (f));
}

// ===========================================================================
void testTelemetryCapturesDetection()
{
    beginTest ("Telemetry records the criteria behind a detection");

    const double sr = 48000.0;
    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    auto params = defaultParams();
    params.telemetryEnabled = true;
    params.humEnabled = false;
    params.highPassEnabled = false;
    fs.setParameters (params);

    const double toneHz = 2450.0;
    const int n = static_cast<int> (sr * 3.0);
    Rng rng (73);
    std::vector<float> sig (static_cast<size_t> (n));
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        phase += 2.0 * fbk::kPi * toneHz / sr;
        sig[static_cast<size_t> (i)] =
            static_cast<float> (0.3 * std::sin (phase)) + 0.002f * rng.gaussian();
    }

    // Drain as we process, standing in for the consumer thread.
    int frames = 0, framesWithConfirmed = 0;
    float bestFreq = 0.0f;
    bool timeMonotonic = true;
    long long previousTime = -1;

    TelemetryFrame tf {};
    int pos = 0;
    while (pos < n)
    {
        const int len = std::min (256, n - pos);
        fs.processBlock (sig.data() + pos, len);
        pos += len;

        while (fs.telemetry().pop (tf))
        {
            ++frames;
            if (tf.sampleTime <= previousTime)
                timeMonotonic = false;
            previousTime = tf.sampleTime;

            if (tf.confirmedTones > 0)
            {
                ++framesWithConfirmed;
                for (int i = 0; i < tf.numActiveTones; ++i)
                    if (tf.tones[i].confirmed)
                        bestFreq = tf.tones[i].freqHz;
            }
        }
    }

    info ("telemetry frames drained = %.0f", static_cast<double> (frames));
    info ("frames with a confirmed tone = %.0f", static_cast<double> (framesWithConfirmed));
    info ("recorded tone frequency = %.1f Hz", static_cast<double> (bestFreq));

    // Roughly 25 frames a second over 3 seconds.
    CHECK_GT (frames, 50);
    CHECK (timeMonotonic);
    CHECK (fs.telemetry().droppedFrames() == 0);
    CHECK_GT (framesWithConfirmed, 5);
    CHECK_CLOSE (static_cast<double> (bestFreq), toneHz, 30.0);

    // The difference figure must show the plugin doing real work on a signal that
    // is almost entirely feedback.
    info ("difference = %.1f dB of input energy removed", static_cast<double> (fs.differenceDb()));
    CHECK_GT (fs.differenceDb(), -6.0f);
}

// ===========================================================================
void testEventTriggerFiresAndHoldsOff()
{
    beginTest ("Event trigger fires on detection and then holds off");

    const double sr = 48000.0;
    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    auto params = defaultParams();
    params.telemetryEnabled = true;
    params.humEnabled = false;
    params.highPassEnabled = false;
    fs.setParameters (params);
    fs.captureRing().setEnabled (true);

    const double toneHz = 3300.0;
    const int n = static_cast<int> (sr * 8.0);
    Rng rng (91);
    std::vector<float> sig (static_cast<size_t> (n));
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        phase += 2.0 * fbk::kPi * toneHz / sr;
        sig[static_cast<size_t> (i)] =
            static_cast<float> (0.3 * std::sin (phase)) + 0.002f * rng.gaussian();
    }

    int events = 0;
    int pos = 0;
    while (pos < n)
    {
        const int len = std::min (256, n - pos);
        fs.processBlock (sig.data() + pos, len);
        pos += len;
        if (fs.eventTrigger().consume())
            ++events;
    }

    info ("events over 8 s of continuous feedback = %.0f", static_cast<double> (events));

    // At least one, and the 5 s holdoff must stop a single sustained problem from
    // producing an event every frame.
    CHECK_GT (events, 0);
    CHECK_LT (events, 4);

    // And the rolling capture must actually hold audio to go with it.
    std::vector<float> dry, wet;
    const int captured = fs.captureRing().snapshot (dry, wet);
    info ("capture ring holds %.0f samples", static_cast<double> (captured));
    CHECK_GT (captured, static_cast<int> (sr));
}

// ===========================================================================
void testSweepRecoversAKnownRoom()
{
    beginTest ("Sweep deconvolution recovers a known impulse response");

    const double sr = 48000.0;

    // A synthetic "room": direct sound at 200 samples, a discrete reflection 500
    // samples later at half amplitude, and an exponentially decaying diffuse tail
    // tuned for an RT60 of about 0.4 s. If the deconvolution is right, all three
    // features come back out.
    const int irLength = 24000;              // 500 ms
    const int directDelay = 200;
    const int reflectionOffset = 500;
    const double tauAmplitude = 2783.0;      // -> RT60 ~ 0.4 s
    const double expectedRt60 = 6.9 * tauAmplitude / sr;

    std::vector<float> ir (static_cast<size_t> (irLength), 0.0f);
    ir[static_cast<size_t> (directDelay)] = 1.0f;
    ir[static_cast<size_t> (directDelay + reflectionOffset)] = 0.5f;

    Rng rng (137);
    for (int i = directDelay; i < irLength; ++i)
    {
        const double env = std::exp (-static_cast<double> (i - directDelay) / tauAmplitude);
        ir[static_cast<size_t> (i)] += static_cast<float> (0.12 * env * rng.gaussian());
    }

    SweepMeasurement sweep;
    sweep.prepare (sr, 3.0f, 1.5f);
    sweep.setLevelDb (-26.0f);
    sweep.start();
    CHECK (sweep.isRunning());

    // Run the loop one sample at a time: the microphone hears the emitted signal
    // convolved with the room, so the input at each instant depends only on what
    // has already been emitted.
    std::vector<float> emitted (static_cast<size_t> (irLength), 0.0f);
    int writePos = 0;
    int guard = 0;
    const int maxSamples = static_cast<int> (sr * 6.0);

    while (sweep.isRunning() && guard < maxSamples)
    {
        // Microphone return from the emitted history.
        float mic = 0.0f;
        for (int k = 0; k < irLength; ++k)
        {
            int idx = writePos - k;
            while (idx < 0)
                idx += irLength;
            mic += ir[static_cast<size_t> (k)] * emitted[static_cast<size_t> (idx)];
        }

        float out = 0.0f;
        sweep.process (&mic, &out, 1);

        writePos = (writePos + 1) % irLength;
        emitted[static_cast<size_t> (writePos)] = out;
        ++guard;
    }

    CHECK (! sweep.isRunning());
    info ("recorded peak = %.1f dBFS", static_cast<double> (sweep.measuredPeakDbFS()));
    CHECK (! sweep.clipped());
    CHECK (! sweep.tooQuiet());

    sweep.computeImpulseResponse();
    CHECK (sweep.hasResult());

    const auto& recovered = sweep.impulseResponse();
    CHECK_GT (static_cast<int> (recovered.size()), 4000);

    // The loop delay must come back. One sample of slack for the fact that the
    // emitted sample is stored after the call.
    info2 ("measured loop delay %.0f samples, expected %.0f",
           static_cast<double> (sweep.directDelaySamples()), static_cast<double> (directDelay));
    CHECK (std::abs (sweep.directDelaySamples() - directDelay) <= 4);

    // Find the direct peak and the reflection within the recovered response.
    int peakIdx = 0;
    float peakVal = 0.0f;
    for (int i = 0; i < static_cast<int> (recovered.size()); ++i)
        if (std::abs (recovered[static_cast<size_t> (i)]) > peakVal)
        {
            peakVal = std::abs (recovered[static_cast<size_t> (i)]);
            peakIdx = i;
        }

    CHECK_CLOSE (static_cast<double> (peakVal), 1.0, 0.02);   // normalised to 1

    // The reflection: search a small window around where it should be, because the
    // diffuse tail is also present there.
    float reflection = 0.0f;
    for (int d = -4; d <= 4; ++d)
    {
        const int i = peakIdx + reflectionOffset + d;
        if (i >= 0 && i < static_cast<int> (recovered.size()))
            reflection = std::max (reflection, std::abs (recovered[static_cast<size_t> (i)]));
    }
    info ("recovered reflection amplitude = %.3f (expected ~0.5)", static_cast<double> (reflection));
    CHECK_CLOSE (static_cast<double> (reflection), 0.5, 0.12);

    info2 ("RT60: measured %.3f s, expected %.3f s",
           static_cast<double> (sweep.estimatedRt60Seconds()), expectedRt60);
    CHECK_GT (sweep.estimatedRt60Seconds(), 0.25f);
    CHECK_LT (sweep.estimatedRt60Seconds(), 0.60f);
}

// ===========================================================================
void testSweepTakesOverAndRestores()
{
    beginTest ("A running sweep takes over the channel, then normal service resumes");

    const double sr = 48000.0;
    FeedbackSuppressor fs;
    fs.prepare (sr, 256);
    auto params = defaultParams();
    fs.setParameters (params);

    fs.sweep().prepare (sr, 1.0f, 0.5f);
    fs.sweep().setLevelDb (-20.0f);
    fs.sweep().start();
    CHECK (fs.isSweeping());

    // Feed silence in; the output must be the sweep, not silence.
    const int n = static_cast<int> (sr * 2.0);
    std::vector<float> buffer (static_cast<size_t> (n), 0.0f);
    int pos = 0;
    while (pos < n)
    {
        const int len = std::min (256, n - pos);
        fs.processBlock (buffer.data() + pos, len);
        pos += len;
    }

    CHECK (! fs.isSweeping());

    const double emittedRms = rms (buffer.data(), static_cast<int> (sr));
    info ("emitted sweep RMS over the first second = %.4f", emittedRms);
    CHECK_GT (emittedRms, 0.01);

    // After the sweep, normal processing must resume cleanly - and the detector
    // must not still be chasing the sweep it just heard.
    fs.reset();
    SyntheticVoice voice (sr, 140.0);
    std::vector<float> sig (static_cast<size_t> (static_cast<int> (sr * 3.0)));
    for (auto& v : sig)
        v = voice.next();

    const auto out = run (fs, sig, 256);

    bool finite = true;
    for (float v : out)
        if (! std::isfinite (v))
            finite = false;
    CHECK (finite);

    const int mStart = static_cast<int> (sr * 1.5);
    const int mLen = static_cast<int> (sr * 1.0);
    const double db = toDb (rms (out.data() + mStart, mLen)) - toDb (rms (sig.data() + mStart, mLen));
    info ("voice level after a sweep = %+.2f dB", db);
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
    testHistogramPercentiles();
    testCalibrationMeasuresTheVoice();
    testObserveOnlyDoesNotProcess();
    testProfileKeepsVoiceSafeAndTonesCaught();
    testModePriorsSpeedUpEngagement();
    testLearnedFeedbackPriorSpeedsEngagement();
    testWrongModelCannotMakeSungNoteFeedback();
    testSeparatorPresenceReachesTheMask();
    testNeuralSeparatorLoadValidation();
    testNeuralSeparatorMatchesReference();
    testNeuralSeparatorRobustness();
    testArbitraryModelCannotMakeSungNoteFeedback();
    testNeuralSeparatorBlockSizeInvariance();
    testNeuralSeparatorNoAllocation();
    testTelemetryRing();
    testTelemetryCapturesDetection();
    testEventTriggerFiresAndHoldsOff();
    testSweepRecoversAKnownRoom();
    testSweepTakesOverAndRestores();
    testNoAllocationOnAudioThread();
    testPhaseModeSwitchIsClean();
    testRealTimeFactor();

    return test::summary();
}
