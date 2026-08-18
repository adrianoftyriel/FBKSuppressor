// FBKSuppressor - CaptureRing.h
//
// A rolling capture of the last N seconds of input and output, so that when
// something misbehaves on a real stage there is an actual recording of it rather
// than a description. The audio thread only ever writes into a preallocated ring;
// snapshotting and file writing happen on another thread (in the plugin layer,
// which owns the file format code). No allocation, no locks in process().
#pragma once

#include "Common.h"

#include <atomic>

namespace fbk
{
class CaptureRing
{
public:
    void prepare (double sampleRate, float seconds)
    {
        capacity_ = std::max (1024, static_cast<int> (sampleRate * static_cast<double> (seconds)));
        dry_.assign (static_cast<size_t> (capacity_), 0.0f);
        wet_.assign (static_cast<size_t> (capacity_), 0.0f);
        write_.store (0, std::memory_order_relaxed);
        filled_.store (0, std::memory_order_relaxed);
    }

    void setEnabled (bool e) noexcept { enabled_.store (e, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled_.load (std::memory_order_relaxed); }

    void push (float dry, float wet) noexcept
    {
        if (! enabled_.load (std::memory_order_relaxed) || capacity_ <= 0)
            return;

        int w = write_.load (std::memory_order_relaxed);
        dry_[static_cast<size_t> (w)] = dry;
        wet_[static_cast<size_t> (w)] = wet;
        w = (w + 1) % capacity_;
        write_.store (w, std::memory_order_release);

        const int f = filled_.load (std::memory_order_relaxed);
        if (f < capacity_)
            filled_.store (f + 1, std::memory_order_relaxed);
    }

    // Called from a non-audio thread. Copies the ring into chronological order.
    // A concurrent push may land mid-copy; for a diagnostic capture that is an
    // acceptable trade against any form of blocking on the audio thread.
    int snapshot (std::vector<float>& dryOut, std::vector<float>& wetOut) const
    {
        const int n = filled_.load (std::memory_order_acquire);
        if (n <= 0)
            return 0;
        const int w = write_.load (std::memory_order_acquire);

        dryOut.resize (static_cast<size_t> (n));
        wetOut.resize (static_cast<size_t> (n));

        const int start = ((w - n) % capacity_ + capacity_) % capacity_;
        for (int i = 0; i < n; ++i)
        {
            const int idx = (start + i) % capacity_;
            dryOut[static_cast<size_t> (i)] = dry_[static_cast<size_t> (idx)];
            wetOut[static_cast<size_t> (i)] = wet_[static_cast<size_t> (idx)];
        }
        return n;
    }

    int capacity() const noexcept { return capacity_; }

private:
    int capacity_ { 0 };
    std::vector<float> dry_, wet_;
    std::atomic<int> write_ { 0 };
    std::atomic<int> filled_ { 0 };
    std::atomic<bool> enabled_ { false };
};
} // namespace fbk
