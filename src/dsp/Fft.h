// FBKSuppressor - Fft.h
//
// Minimal, allocation-free radix-2 FFT. Everything is sized and precomputed in
// the constructor so that transform() can be called from the audio thread.
//
// We deliberately do not use a third-party FFT here: the transform sizes are
// small (256-2048), the cost is a rounding error next to the rest of the chain,
// and a self-contained implementation keeps the core DSP library free of any
// dependency that could pull an allocator or a thread pool into the real-time
// path.
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace fbk
{
using Complex = std::complex<float>;

class Fft
{
public:
    explicit Fft (int order);

    int size() const noexcept { return size_; }
    int order() const noexcept { return order_; }

    // In-place complex forward/inverse. inverse() applies the 1/N scaling.
    void forwardComplex (Complex* data) const noexcept;
    void inverseComplex (Complex* data) const noexcept;

    // Real input of size(), full complex output of size() bins. Bins above
    // size()/2 are the conjugate mirror; callers normally read 0..size()/2.
    void forwardReal (const float* in, Complex* out) const noexcept;

    // Hermitian complex input (only 0..size()/2 need be valid), real output.
    // The imaginary residue is discarded, so this is exact for spectra that are
    // genuinely conjugate-symmetric and a projection for those that are not.
    void inverseReal (const Complex* in, float* out) const noexcept;

private:
    void permute (Complex* data) const noexcept;

    int order_;
    int size_;
    std::vector<int> bitRev_;
    std::vector<Complex> twiddle_;   // forward twiddles, one block per stage
    mutable std::vector<Complex> scratch_;
};
} // namespace fbk
