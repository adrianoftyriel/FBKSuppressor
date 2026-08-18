#include "Fft.h"

#include <cmath>
#include <numbers>

namespace fbk
{
Fft::Fft (int order)
    : order_ (order), size_ (1 << order)
{
    bitRev_.resize (static_cast<size_t> (size_));
    const int shift = 32 - order_;
    for (int i = 0; i < size_; ++i)
    {
        // Reverse the low `order_` bits of i.
        unsigned int v = static_cast<unsigned int> (i);
        v = ((v & 0xaaaaaaaau) >> 1) | ((v & 0x55555555u) << 1);
        v = ((v & 0xccccccccu) >> 2) | ((v & 0x33333333u) << 2);
        v = ((v & 0xf0f0f0f0u) >> 4) | ((v & 0x0f0f0f0fu) << 4);
        v = ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8);
        v = (v >> 16) | (v << 16);
        bitRev_[static_cast<size_t> (i)] = static_cast<int> (v >> shift);
    }

    // Twiddles laid out stage by stage: stage s (half = 1<<s) contributes
    // `half` entries. Total = size_ - 1.
    twiddle_.reserve (static_cast<size_t> (size_));
    for (int half = 1; half < size_; half <<= 1)
    {
        const double step = -std::numbers::pi / static_cast<double> (half);
        for (int j = 0; j < half; ++j)
        {
            const double a = step * static_cast<double> (j);
            twiddle_.emplace_back (static_cast<float> (std::cos (a)),
                                   static_cast<float> (std::sin (a)));
        }
    }

    scratch_.resize (static_cast<size_t> (size_));
}

void Fft::permute (Complex* data) const noexcept
{
    for (int i = 0; i < size_; ++i)
    {
        const int j = bitRev_[static_cast<size_t> (i)];
        if (j > i)
            std::swap (data[i], data[j]);
    }
}

void Fft::forwardComplex (Complex* data) const noexcept
{
    permute (data);

    const Complex* tw = twiddle_.data();
    for (int half = 1; half < size_; half <<= 1)
    {
        const int step = half << 1;
        for (int base = 0; base < size_; base += step)
        {
            for (int j = 0; j < half; ++j)
            {
                Complex& a = data[base + j];
                Complex& b = data[base + j + half];
                const Complex t = b * tw[j];
                b = a - t;
                a = a + t;
            }
        }
        tw += half;
    }
}

void Fft::inverseComplex (Complex* data) const noexcept
{
    // conj -> forward -> conj -> scale
    for (int i = 0; i < size_; ++i)
        data[i] = std::conj (data[i]);

    forwardComplex (data);

    const float scale = 1.0f / static_cast<float> (size_);
    for (int i = 0; i < size_; ++i)
        data[i] = std::conj (data[i]) * scale;
}

void Fft::forwardReal (const float* in, Complex* out) const noexcept
{
    for (int i = 0; i < size_; ++i)
        out[i] = Complex (in[i], 0.0f);

    forwardComplex (out);
}

void Fft::inverseReal (const Complex* in, float* out) const noexcept
{
    const int half = size_ / 2;
    scratch_[0] = Complex (in[0].real(), 0.0f);
    scratch_[static_cast<size_t> (half)] = Complex (in[half].real(), 0.0f);

    for (int k = 1; k < half; ++k)
    {
        scratch_[static_cast<size_t> (k)] = in[k];
        scratch_[static_cast<size_t> (size_ - k)] = std::conj (in[k]);
    }

    inverseComplex (scratch_.data());

    for (int i = 0; i < size_; ++i)
        out[i] = scratch_[static_cast<size_t> (i)].real();
}
} // namespace fbk
