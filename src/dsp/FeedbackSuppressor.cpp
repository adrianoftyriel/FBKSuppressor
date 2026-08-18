#include "FeedbackSuppressor.h"

namespace fbk
{
FeedbackSuppressor::FeedbackSuppressor() = default;

void FeedbackSuppressor::prepare (double sampleRate, int maxBlockSize)
{
    sampleRate_   = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlockSize_ = std::max (1, maxBlockSize);

    bands_.prepare (sampleRate_);
    analyser_.prepare (sampleRate_);
    noise_.prepare (sampleRate_, bands_);
    detector_.prepare (sampleRate_, bands_);
    tonal_.prepare (sampleRate_);
    hum_.prepare (sampleRate_);
    mask_.prepare (sampleRate_, bands_);
    highPass_.prepare (sampleRate_);
    dryDelay_.prepare (kLookaheadSamples + 8);
    capture_.prepare (sampleRate_, 12.0f);

    bandEnergy_.assign (kNumBands, 0.0f);
    separatorPresence_.assign (kNumBands, 0.0f);
    meterBandNoise_.assign (kNumBands, 0.0f);
    meterBandPower_.assign (kNumBands, 0.0f);

    paramsDirty_ = true;
    applyPhaseMode();
    reset();
}

void FeedbackSuppressor::reset() noexcept
{
    analyser_.reset();
    noise_.reset();
    detector_.reset();
    tonal_.reset();
    hum_.reset();
    mask_.reset();
    highPass_.reset();
    dryDelay_.clear();
    metering_ = Metering {};
    meterFrameCounter_ = 0;
}

void FeedbackSuppressor::applyPhaseMode() noexcept
{
    const PhaseMode wanted = params_.qualityMode ? PhaseMode::linearPhase
                                                 : PhaseMode::minimumPhase;
    if (mask_.phaseMode() != wanted)
    {
        mask_.setPhaseMode (wanted);
        dryDelay_.clear();
    }
    latencySamples_ = mask_.latencySamples();
}

void FeedbackSuppressor::setParameters (const Parameters& p) noexcept
{
    params_ = p;
    paramsDirty_ = true;

    applyPhaseMode();

    detector_.setSensitivity (params_.feedbackSensitivity);

    hum_.setEnabled (params_.humEnabled);
    hum_.setNumHarmonics (params_.humHarmonics);
    hum_.setDepth (params_.humDepth);

    MaskSettings ms;
    ms.denoiseEnabled  = params_.denoiseEnabled;
    ms.denoiseAmount   = params_.denoiseAmount;
    ms.maxAttenuationDb = params_.maxAttenuationDb;
    ms.dereverbEnabled = params_.dereverbEnabled;
    ms.dereverbAmount  = params_.dereverbAmount;
    ms.rt60Seconds     = params_.rt60Seconds;
    ms.voiceProtection = params_.voiceProtection;
    mask_.setSettings (ms);

    highPass_.setCutoff (params_.highPassHz);
}

void FeedbackSuppressor::onAnalysisFrame() noexcept
{
    const float* power = analyser_.power();
    const float* magnitude = analyser_.magnitude();

    noise_.update (power);
    detector_.process (magnitude);

    // v0.2: if a learned separator is attached, hand it the band energies. Its
    // per-band voice presence is intended to replace the heuristic estimate. The
    // call is made here, on the frame boundary, and never touches the audio path.
    if (separator_ != nullptr && separator_->isReady())
    {
        bands_.binsToBands (power, bandEnergy_.data());
        separator_->process (bandEnergy_.data(), separatorPresence_.data());
    }

    if (params_.feedbackEnabled)
        tonal_.updateFromDetector (detector_.tones(), detector_.numTones(), params_.feedbackDepth);
    else
        tonal_.updateFromDetector (nullptr, 0, 0.0f);

    hum_.updateDetection();

    mask_.updateMask (power, noise_);

    // --- Metering, at a rate a UI can actually use (~25 Hz) -----------------
    if (++meterFrameCounter_ >= 12)
    {
        meterFrameCounter_ = 0;

        metering_.confirmedTones = detector_.numConfirmed();
        const TrackedTone* tones = detector_.tones();
        for (int i = 0; i < kMaxTones; ++i)
        {
            metering_.toneFrequencies[i]   = tones[i].active ? tones[i].freqHz : 0.0f;
            metering_.toneConfidence[i]    = tones[i].active ? tones[i].confidence : 0.0f;
            metering_.toneAttenuationDb[i] = 0.0f;
        }

        const float* bg = mask_.bandGains();
        const float* np = noise_.noisePower();
        bands_.binsToBands (np, meterBandNoise_.data());
        bands_.binsToBands (power, meterBandPower_.data());

        for (int b = 0; b < kNumBands; ++b)
        {
            metering_.bandGainDb[b]  = gainToDb (bg[b]);
            metering_.bandNoiseDb[b] = 10.0f * std::log10 (meterBandNoise_[static_cast<size_t> (b)] + kEpsilon);
            metering_.bandInputDb[b] = 10.0f * std::log10 (meterBandPower_[static_cast<size_t> (b)] + kEpsilon);
        }

        metering_.speechPresence    = noise_.overallSpeechPresence();
        metering_.humFundamentalHz  = hum_.detectedFundamental();
        metering_.humActive         = hum_.isActive();
        metering_.meanAttenuationDb = mask_.meanAttenuationDb();
    }
}

void FeedbackSuppressor::processBlock (float* data, int numSamples) noexcept
{
    if (params_.bypass)
    {
        // Still run the analyser so that detection state stays current and
        // un-bypassing does not have to converge from cold. The audio is
        // untouched, but the dry delay must still be fed so that switching phase
        // mode does not produce a discontinuity.
        for (int n = 0; n < numSamples; ++n)
        {
            float x = data[n];
            if (! std::isfinite (x))
                x = 0.0f;
            if (analyser_.pushSample (clampf (x, -16.0f, 16.0f)))
                onAnalysisFrame();
            if (latencySamples_ > 0)
                data[n] = dryDelay_.process (x, latencySamples_);
        }
        return;
    }

    const float wetAmount = clampf (params_.strength, 0.0f, 1.0f);
    const float dryAmount = 1.0f - wetAmount;

    float inPeak = metering_.inputPeak;
    float outPeak = metering_.outputPeak;

    for (int n = 0; n < numSamples; ++n)
    {
        // Sanitise at the boundary. A host can hand us NaN, an infinity or an
        // absurd magnitude - a misbehaving upstream plugin, a driver glitch, a
        // denormal storm - and every adaptive element downstream integrates what
        // it is given. Clamping here at +24 dBFS is what keeps a single bad sample
        // from destabilising the adaptive weights, which is exactly what happened
        // before this line existed.
        float input = data[n];
        if (! std::isfinite (input))
            input = 0.0f;
        input = clampf (input, -16.0f, 16.0f);

        // Analysis always sees the unprocessed input. See the header note.
        if (analyser_.pushSample (input))
            onAnalysisFrame();

        float x = input;

        if (params_.highPassEnabled)
            x = highPass_.process (x);

        if (params_.humEnabled)
            x = hum_.process (x);

        if (params_.feedbackEnabled)
            x = tonal_.process (x);

        x = mask_.process (x);

        // Delay-match the dry path so the Strength control is a true crossfade in
        // both phase modes rather than a comb filter in Quality mode.
        const float dry = latencySamples_ > 0 ? dryDelay_.process (input, latencySamples_) : input;

        float out = dryAmount * dry + wetAmount * x;
        if (! std::isfinite (out))
            out = 0.0f;

        data[n] = out;

        inPeak  = std::max (inPeak * 0.99999f, std::abs (input));
        outPeak = std::max (outPeak * 0.99999f, std::abs (out));

        capture_.push (input, out);
    }

    metering_.inputPeak = inPeak;
    metering_.outputPeak = outPeak;
}
} // namespace fbk
