// FBKSuppressor - Biquad.h
//
// Transposed direct form II biquad, plus a cascaded Butterworth high-pass used
// only for subsonic rumble. Note this is the one genuinely subtractive-by-
// frequency element in the chain: it is defaulted to 35 Hz, below the fundamental
// of any human voice (the lowest bass fundamentals sit near 65 Hz), so it removes
// stage thump and HVAC rumble without touching vocal content. It is not, and must
// not be confused with, ringing a room out.
#pragma once

#include "Common.h"


namespace fbk
{
class Biquad
{
public:
    void reset() noexcept { z1_ = z2_ = 0.0f; }

    void setCoefficients (float b0, float b1, float b2, float a1, float a2) noexcept
    {
        b0_ = b0; b1_ = b1; b2_ = b2; a1_ = a1; a2_ = a2;
    }

    float process (float x) noexcept
    {
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        z1_ = sanitise (z1_);
        z2_ = sanitise (z2_);
        return y;
    }

private:
    float b0_ { 1.0f }, b1_ { 0.0f }, b2_ { 0.0f }, a1_ { 0.0f }, a2_ { 0.0f };
    float z1_ { 0.0f }, z2_ { 0.0f };
};

// Two cascaded biquads -> 4th-order Butterworth high-pass, 24 dB/octave.
class HighPass
{
public:
    void prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate;
        setCutoff (cutoffHz_);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : stages_)
            s.reset();
    }

    void setCutoff (float hz) noexcept
    {
        cutoffHz_ = clampf (hz, 10.0f, static_cast<float> (sampleRate_ * 0.45));

        // Butterworth Q values for a 4th-order cascade.
        static constexpr float qs[2] = { 0.54119610f, 1.30656296f };
        const double w0 = 2.0 * kPi * static_cast<double> (cutoffHz_) / sampleRate_;
        const double cosw = std::cos (w0);
        const double sinw = std::sin (w0);

        for (int i = 0; i < 2; ++i)
        {
            const double alpha = sinw / (2.0 * static_cast<double> (qs[i]));
            const double a0 = 1.0 + alpha;
            const double b0 = (1.0 + cosw) * 0.5 / a0;
            const double b1 = -(1.0 + cosw) / a0;
            const double b2 = b0;
            const double a1 = (-2.0 * cosw) / a0;
            const double a2 = (1.0 - alpha) / a0;
            stages_[static_cast<size_t> (i)].setCoefficients (
                static_cast<float> (b0), static_cast<float> (b1), static_cast<float> (b2),
                static_cast<float> (a1), static_cast<float> (a2));
        }
    }

    float process (float x) noexcept
    {
        for (auto& s : stages_)
            x = s.process (x);
        return x;
    }

private:
    double sampleRate_ { 48000.0 };
    float  cutoffHz_ { 35.0f };
    Biquad stages_[2];
};
} // namespace fbk
