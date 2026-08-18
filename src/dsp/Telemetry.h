// FBKSuppressor - Telemetry.h
//
// A compact record of what the detector was thinking, written continuously.
//
// Why telemetry rather than audio
// -------------------------------
// Logging audio continuously costs about 1 GB per hour for a dry/wet pair at 24
// bit. Logging the detector's state costs about 11 MB per hour - and it is more
// useful, because a recording tells you what a problem sounded like whereas this
// tells you why the detector did or did not act. When a tone gets through, the
// question is never "what did it sound like", it is "which criterion failed", and
// only one of those two records can answer it.
//
// So the split is: cheap telemetry always, expensive audio only around events.
//
// The audio thread writes fixed-size POD frames into a preallocated single-
// producer single-consumer ring and never blocks, allocates, or touches a file.
// A consumer on another thread drains it. If the consumer falls behind, the
// producer drops frames and counts them rather than stalling the audio thread -
// dropping diagnostics is always preferable to a dropout.
#pragma once

#include "Common.h"
#include "ErbBands.h"
#include "HowlDetector.h"

#include <atomic>

namespace fbk
{
// One record per emitted frame. Deliberately plain and fixed-size so the ring can
// be a flat array and a consumer can write it out without interpretation.
struct TelemetryFrame
{
    long long sampleTime { 0 };        // samples since prepare()

    float bandInputDb[kNumBands] {};
    float bandNoiseDb[kNumBands] {};
    float bandGainDb[kNumBands] {};

    float speechPresence { 0.0f };
    float inputPeak { 0.0f };
    float outputPeak { 0.0f };
    float differenceDb { -120.0f };    // level of (input - output): what we did
    float humFundamentalHz { 0.0f };
    bool  humActive { false };
    int   confirmedTones { 0 };

    // Per-tone state, including every criterion value, so a detection - or a
    // near-miss - can be reconstructed exactly.
    struct Tone
    {
        float freqHz { 0.0f };
        float confidence { 0.0f };
        float paprDb { 0.0f };
        float pnprDb { 0.0f };
        float phprDb { 0.0f };
        float prominenceDb { 0.0f };
        float imsd { 0.0f };
        float fsdHz { 0.0f };
        int   persistence { 0 };
        bool  confirmed { false };
    };
    Tone tones[kMaxTones] {};
    int  numActiveTones { 0 };
};

// ---------------------------------------------------------------------------
class TelemetryRing
{
public:
    void prepare (int capacityFrames)
    {
        capacity_ = std::max (16, capacityFrames);
        frames_.assign (static_cast<size_t> (capacity_), TelemetryFrame {});
        write_.store (0, std::memory_order_relaxed);
        read_.store (0, std::memory_order_relaxed);
        dropped_.store (0, std::memory_order_relaxed);
    }

    void setEnabled (bool e) noexcept { enabled_.store (e, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled_.load (std::memory_order_relaxed); }

    // Audio thread. Never blocks; drops and counts if the consumer is behind.
    bool push (const TelemetryFrame& f) noexcept
    {
        if (! enabled_.load (std::memory_order_relaxed) || capacity_ <= 0)
            return false;

        const int w = write_.load (std::memory_order_relaxed);
        const int next = (w + 1) % capacity_;
        if (next == read_.load (std::memory_order_acquire))
        {
            dropped_.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        frames_[static_cast<size_t> (w)] = f;
        write_.store (next, std::memory_order_release);
        return true;
    }

    // Consumer thread.
    bool pop (TelemetryFrame& out) noexcept
    {
        const int r = read_.load (std::memory_order_relaxed);
        if (r == write_.load (std::memory_order_acquire))
            return false;

        out = frames_[static_cast<size_t> (r)];
        read_.store ((r + 1) % capacity_, std::memory_order_release);
        return true;
    }

    int droppedFrames() const noexcept { return dropped_.load (std::memory_order_relaxed); }
    void resetDropped() noexcept { dropped_.store (0, std::memory_order_relaxed); }

private:
    int capacity_ { 0 };
    std::vector<TelemetryFrame> frames_;
    std::atomic<int> write_ { 0 }, read_ { 0 }, dropped_ { 0 };
    std::atomic<bool> enabled_ { false };
};

// ---------------------------------------------------------------------------
// Decides when the rolling audio buffer is worth keeping.
//
// Two triggers. A newly confirmed tone is the obvious one. The second is a jump in
// the difference signal - input minus output - because that is by definition "the
// plugin just did something substantial", which catches the case that matters most
// and is hardest to notice: the processor acting when it should not have.
class EventTrigger
{
public:
    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate;
        // Do not re-fire within 5 s, or one sustained problem produces fifty dumps.
        holdoffFrames_ = static_cast<int> (5.0 * sampleRate / static_cast<double> (kHopSize));
        reset();
    }

    void reset() noexcept
    {
        sinceLast_ = holdoffFrames_;
        previousConfirmed_ = 0;
        pending_.store (false, std::memory_order_relaxed);
        reasonToneHz_ = 0.0f;
        reasonIsDifference_ = false;
    }

    void setDifferenceThresholdDb (float db) noexcept { differenceThresholdDb_ = db; }

    // Once per analysis frame.
    void update (int confirmedTones, float firstConfirmedHz, float differenceDb) noexcept
    {
        if (sinceLast_ < holdoffFrames_)
            ++sinceLast_;

        const bool newDetection = confirmedTones > previousConfirmed_;
        const bool loudDifference = differenceDb > differenceThresholdDb_;
        previousConfirmed_ = confirmedTones;

        if ((newDetection || loudDifference) && sinceLast_ >= holdoffFrames_)
        {
            sinceLast_ = 0;
            reasonToneHz_ = newDetection ? firstConfirmedHz : 0.0f;
            reasonIsDifference_ = ! newDetection;
            pending_.store (true, std::memory_order_release);
        }
    }

    // Consumer thread: returns true once per event and clears the flag.
    bool consume() noexcept { return pending_.exchange (false, std::memory_order_acq_rel); }

    float reasonToneHz() const noexcept { return reasonToneHz_; }
    bool reasonWasDifference() const noexcept { return reasonIsDifference_; }

private:
    double sampleRate_ { 48000.0 };
    int  holdoffFrames_ { 1000 };
    int  sinceLast_ { 0 };
    int  previousConfirmed_ { 0 };
    float differenceThresholdDb_ { -24.0f };
    float reasonToneHz_ { 0.0f };
    bool  reasonIsDifference_ { false };
    std::atomic<bool> pending_ { false };
};
} // namespace fbk
