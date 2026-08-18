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
    calibrator_.prepare (sampleRate_, bands_);
    noise_.prepare (sampleRate_, bands_);
    detector_.prepare (sampleRate_, bands_);
    tonal_.prepare (sampleRate_);
    hum_.prepare (sampleRate_);
    mask_.prepare (sampleRate_, bands_);
    highPass_.prepare (sampleRate_);
    dryDelay_.prepare (kLookaheadSamples + 8);
    capture_.prepare (sampleRate_, 12.0f);

    // Four seconds of headroom in the ring at 25 Hz, so a consumer that only
    // wakes a couple of times a second never causes a drop.
    sweep_.prepare (sampleRate_);
    telemetry_.prepare (100);
    eventTrigger_.prepare (sampleRate_);
    powerCoeff_ = static_cast<float> (std::exp (-1.0 / (0.100 * sampleRate_)));

    bandEnergy_.assign (kNumBands, 0.0f);
    separatorPresence_.assign (kNumBands, 0.0f);
    meterBandNoise_.assign (kNumBands, 0.0f);
    meterBandPower_.assign (kNumBands, 0.0f);
    bandProtection_.assign (kNumBands, 0.8f);
    modePriors_.assign (kMaxRoomModes, 0.0f);

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
    eventTrigger_.reset();
    telemetryDecimator_ = 0;
    sampleClock_ = 0;
    diffPower_ = inPower_ = 0.0f;
    differenceDb_ = -120.0f;
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
    telemetry_.setEnabled (params_.telemetryEnabled);
    eventTrigger_.setDifferenceThresholdDb (params_.eventDifferenceThresholdDb);

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
    ms.voiceProtectionPerBand = profileApplied_ ? bandProtection_.data() : nullptr;
    mask_.setSettings (ms);

    highPass_.setCutoff (params_.highPassHz);
}

void FeedbackSuppressor::beginCalibration (CalibrationPhase phase) noexcept
{
    calibrator_.begin (phase);

    // While measuring a voice, the detector observes but never confirms, so the
    // plugin cannot act on the signal it is characterising. During the room-mode
    // phase the opposite is true: cancellation must stay fully active, because
    // being protected is what makes pushing the gain safe.
    detector_.setObserveOnly (phase == CalibrationPhase::voice);
}

void FeedbackSuppressor::finishCalibration() noexcept
{
    calibrator_.finish();
    detector_.setObserveOnly (false);
}

void FeedbackSuppressor::cancelCalibration() noexcept
{
    calibrator_.cancel();
    detector_.setObserveOnly (false);
}

void FeedbackSuppressor::applyProfile (const VoiceProfile& p) noexcept
{
    if (! p.valid)
        return;

    if (p.hasSuggestions)
        detector_.setCalibratedThresholds (p.suggestedPnprDb,
                                           p.suggestedPhprDb,
                                           p.suggestedFsdMaxHz,
                                           p.suggestedAbsoluteFloorDb,
                                           p.suggestedLocalProminenceDb);

    if (p.hasVoice)
    {
        for (int b = 0; b < kNumBands; ++b)
            bandProtection_[static_cast<size_t> (b)] = clampf (p.suggestedVoiceProtection[b], 0.0f, 1.0f);
    }

    if (p.numModes > 0)
    {
        const int n = std::min (p.numModes, kMaxRoomModes);
        for (int i = 0; i < n; ++i)
            modePriors_[static_cast<size_t> (i)] = p.modes[i].freqHz;
        detector_.setModePriors (modePriors_.data(), n);
    }

    profileApplied_ = true;
    params_.profileApplied = true;
    setParameters (params_);   // re-push, so MaskSettings picks up the per-band array
}

void FeedbackSuppressor::clearProfile() noexcept
{
    detector_.clearCalibratedThresholds();
    detector_.clearModePriors();
    std::fill (bandProtection_.begin(), bandProtection_.end(), params_.voiceProtection);
    profileApplied_ = false;
    params_.profileApplied = false;
    setParameters (params_);
}

void FeedbackSuppressor::onAnalysisFrame() noexcept
{
    const float* power = analyser_.power();
    const float* magnitude = analyser_.magnitude();

    noise_.update (power);
    detector_.process (magnitude);

    if (calibrator_.isRunning())
    {
        calibrator_.processFrame (power, magnitude, detector_);

        // Room-mode phase: report whatever the cancellers are actually working on.
        // This is the measurement - the frequencies the suppressor had to fight
        // while the gain was pushed - and it only exists because cancellation is
        // left on throughout.
        if (calibrator_.phase() == CalibrationPhase::roomModes)
        {
            const float frameSeconds = static_cast<float> (kHopSize / sampleRate_);
            const TrackedTone* tones = detector_.tones();
            for (int i = 0; i < detector_.numTones(); ++i)
            {
                const auto& t = tones[i];
                if (t.active && t.confidence > 0.5f)
                    calibrator_.reportEngagedTone (t.freqHz, t.pnprDb,
                                                   frameSeconds * t.confidence);
            }
        }
    }

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
            metering_.toneAttenuationDb[i] = tones[i].active ? tonal_.attenuationDb (i) : 0.0f;
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

    // --- Event trigger and telemetry ---------------------------------------
    const TrackedTone* allTones = detector_.tones();
    int confirmedCount = 0;
    float firstConfirmedHz = 0.0f;
    for (int i = 0; i < detector_.numTones(); ++i)
    {
        if (allTones[i].active && allTones[i].confirmed)
        {
            ++confirmedCount;
            if (firstConfirmedHz <= 0.0f)
                firstConfirmedHz = allTones[i].freqHz;
        }
    }
    eventTrigger_.update (confirmedCount, firstConfirmedHz, differenceDb_);

    // ~25 Hz: fast enough to see a detection develop, slow enough to be free.
    if (telemetry_.isEnabled() && ++telemetryDecimator_ >= 15)
    {
        telemetryDecimator_ = 0;

        auto& f = telemetryScratch_;
        f.sampleTime = sampleClock_;
        for (int b = 0; b < kNumBands; ++b)
        {
            f.bandInputDb[b] = metering_.bandInputDb[b];
            f.bandNoiseDb[b] = metering_.bandNoiseDb[b];
            f.bandGainDb[b]  = metering_.bandGainDb[b];
        }
        f.speechPresence = noise_.overallSpeechPresence();
        f.inputPeak = metering_.inputPeak;
        f.outputPeak = metering_.outputPeak;
        f.differenceDb = differenceDb_;
        f.humFundamentalHz = hum_.detectedFundamental();
        f.humActive = hum_.isActive();
        f.confirmedTones = confirmedCount;

        int active = 0;
        for (int i = 0; i < detector_.numTones(); ++i)
        {
            const auto& t = allTones[i];
            if (! t.active)
                continue;
            auto& dst = f.tones[active++];
            dst.freqHz = t.freqHz;
            dst.confidence = t.confidence;
            dst.paprDb = t.paprDb;
            dst.pnprDb = t.pnprDb;
            dst.phprDb = t.phprDb;
            dst.prominenceDb = t.localProminenceDb;
            dst.imsd = t.slopeDeviation;
            dst.fsdHz = t.freqDeviation;
            dst.persistence = t.persistence;
            dst.confirmed = t.confirmed;
        }
        f.numActiveTones = active;

        telemetry_.push (f);
    }
}

void FeedbackSuppressor::processBlock (float* data, int numSamples) noexcept
{
    // A running sweep takes over the channel completely.
    if (sweep_.isRunning())
    {
        sweepWasRunning_ = true;
        sweep_.process (data, data, numSamples);
        sampleClock_ += numSamples;
        return;
    }

    if (sweepWasRunning_)
    {
        // The detector and cancellers spent the last few seconds looking at our own
        // sweep. Clear that out rather than carrying it into normal operation.
        sweepWasRunning_ = false;
        analyser_.reset();
        noise_.reset();
        detector_.reset();
        tonal_.reset();
        hum_.reset();
        mask_.reset();
        highPass_.reset();
        detector_.setSensitivity (params_.feedbackSensitivity);
    }

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

        // Keep the clock and the difference figure honest while bypassed, so
        // telemetry timestamps stay continuous across a bypass and the difference
        // reads as zero rather than as a stale value from before.
        sampleClock_ += numSamples;
        diffPower_ = 0.0f;
        differenceDb_ = -120.0f;
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

        // The difference signal is exactly what the plugin is doing to the audio.
        // Tracking it as a fraction of the input's energy makes it a single honest
        // number for "how much am I changing this", which is the thing worth
        // watching on a voice.
        const float diff = dry - out;
        diffPower_ = powerCoeff_ * diffPower_ + (1.0f - powerCoeff_) * diff * diff;
        inPower_   = powerCoeff_ * inPower_   + (1.0f - powerCoeff_) * input * input;

        capture_.push (input, out);
    }

    metering_.inputPeak = inPeak;
    metering_.outputPeak = outPeak;
    differenceDb_ = 10.0f * std::log10 ((diffPower_ + kEpsilon) / (inPower_ + kEpsilon));
    sampleClock_ += numSamples;
}
} // namespace fbk
