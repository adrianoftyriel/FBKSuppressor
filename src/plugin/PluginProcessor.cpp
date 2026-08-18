#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
using namespace juce;

auto boolParam (const char* id, const char* name, bool def)
{
    return std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def);
}

auto pctParam (const char* id, const char* name, float def)
{
    return std::make_unique<AudioParameterFloat> (
        ParameterID { id, 1 }, name,
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), def * 100.0f,
        AudioParameterFloatAttributes().withLabel ("%"));
}
} // namespace

// ===========================================================================
juce::AudioProcessorValueTreeState::ParameterLayout FBKSuppressorProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (boolParam (fbkparam::bypass, "Bypass", false));
    layout.add (pctParam  (fbkparam::strength, "Strength", 1.0f));

    // Quality mode trades the zero-latency guarantee for a linear-phase filter.
    // Defaulted off, because the whole point of the plugin is that it can sit in
    // a live monitor path without adding delay.
    layout.add (boolParam (fbkparam::qualityMode, "Quality Mode (adds latency)", false));

    layout.add (boolParam (fbkparam::fbEnabled, "Feedback Suppression", true));
    layout.add (pctParam  (fbkparam::fbSensitivity, "Feedback Sensitivity", 0.5f));
    layout.add (pctParam  (fbkparam::fbDepth, "Feedback Depth", 1.0f));

    layout.add (boolParam (fbkparam::nrEnabled, "Noise Reduction", true));
    layout.add (pctParam  (fbkparam::nrAmount, "Noise Amount", 0.5f));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { fbkparam::nrMaxAtten, 1 }, "Noise Max Attenuation",
        NormalisableRange<float> (3.0f, 24.0f, 0.1f), 9.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (pctParam (fbkparam::nrVoiceProtect, "Voice Protection", 0.8f));

    layout.add (boolParam (fbkparam::humEnabled, "Hum Removal", true));
    layout.add (pctParam  (fbkparam::humDepth, "Hum Depth", 1.0f));
    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { fbkparam::humHarmonics, 1 }, "Hum Harmonics", 1, fbk::kMaxHumHarmonics, 8));

    // Dereverb is the one stage that genuinely changes how a voice sounds, so it
    // is off unless asked for.
    layout.add (boolParam (fbkparam::drEnabled, "Dereverb", false));
    layout.add (pctParam  (fbkparam::drAmount, "Dereverb Amount", 0.6f));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { fbkparam::drRt60, 1 }, "Room RT60",
        NormalisableRange<float> (0.15f, 2.5f, 0.01f), 0.6f,
        AudioParameterFloatAttributes().withLabel ("s")));

    // Diagnostics are parameters so their state is saved with the session and can
    // be automated or recalled - a logging switch you have to remember to turn on
    // is a logging switch that is off when you need it.
    layout.add (boolParam (fbkparam::telemetry, "Telemetry Logging", false));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { fbkparam::eventThreshold, 1 }, "Event Capture Threshold",
        NormalisableRange<float> (-40.0f, -3.0f, 0.5f), -15.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (boolParam (fbkparam::hpEnabled, "Rumble Filter", true));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { fbkparam::hpFreq, 1 }, "Rumble Filter Freq",
        NormalisableRange<float> (20.0f, 120.0f, 1.0f, 0.5f), 35.0f,
        AudioParameterFloatAttributes().withLabel ("Hz")));

    return layout;
}

// ===========================================================================
FBKSuppressorProcessor::FBKSuppressorProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::mono(), true)
                          // Disabled by default. This is the hook for true
                          // adaptive feedback cancellation: given the PA send as
                          // a reference, the feedback path can be estimated and
                          // subtracted outright, which removes no voice energy at
                          // all. Nothing reads it yet.
                          .withInput ("Sidechain", juce::AudioChannelSet::mono(), false)),
      apvts_ (*this, nullptr, "PARAMETERS", createLayout())
{
    auto get = [this] (const char* id) { return apvts_.getRawParameterValue (id); };

    raw_.bypass         = get (fbkparam::bypass);
    raw_.strength       = get (fbkparam::strength);
    raw_.qualityMode    = get (fbkparam::qualityMode);
    raw_.fbEnabled      = get (fbkparam::fbEnabled);
    raw_.fbSensitivity  = get (fbkparam::fbSensitivity);
    raw_.fbDepth        = get (fbkparam::fbDepth);
    raw_.nrEnabled      = get (fbkparam::nrEnabled);
    raw_.nrAmount       = get (fbkparam::nrAmount);
    raw_.nrMaxAtten     = get (fbkparam::nrMaxAtten);
    raw_.nrVoiceProtect = get (fbkparam::nrVoiceProtect);
    raw_.humEnabled     = get (fbkparam::humEnabled);
    raw_.humDepth       = get (fbkparam::humDepth);
    raw_.humHarmonics   = get (fbkparam::humHarmonics);
    raw_.drEnabled      = get (fbkparam::drEnabled);
    raw_.drAmount       = get (fbkparam::drAmount);
    raw_.drRt60         = get (fbkparam::drRt60);
    raw_.hpEnabled      = get (fbkparam::hpEnabled);
    raw_.hpFreq         = get (fbkparam::hpFreq);
    raw_.telemetry      = get (fbkparam::telemetry);
    raw_.eventThreshold = get (fbkparam::eventThreshold);
}

FBKSuppressorProcessor::~FBKSuppressorProcessor() = default;

// ===========================================================================
bool FBKSuppressorProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainIn  = layouts.getMainInputChannelSet();
    const auto& mainOut = layouts.getMainOutputChannelSet();

    if (mainIn.isDisabled() || mainOut.isDisabled())
        return false;
    if (mainIn != mainOut)
        return false;

    // Mono or stereo. Each channel is processed independently; there is no
    // stereo linking, because two microphones do not share feedback modes.
    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.inputBuses.size() > 1)
    {
        const auto& side = layouts.getChannelSet (true, 1);
        if (! side.isDisabled() && side != juce::AudioChannelSet::mono()
            && side != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

void FBKSuppressorProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampleRate_ = sampleRate;

    const int numChannels = std::max (1, getMainBusNumInputChannels());

    channels_.clear();
    channels_.reserve (static_cast<size_t> (numChannels));
    for (int c = 0; c < numChannels; ++c)
    {
        auto s = std::make_unique<fbk::FeedbackSuppressor>();
        s->prepare (sampleRate, samplesPerBlock);
        channels_.push_back (std::move (s));
    }

    pullParameters();

    reportedLatency_ = channels_.empty() ? 0 : channels_.front()->latencySamples();
    setLatencySamples (reportedLatency_);

    const bool cap = captureEnabled_.load();
    for (auto& ch : channels_)
        ch->captureRing().setEnabled (cap);

    cpuLoad_.store (0.0f);
}

void FBKSuppressorProcessor::releaseResources()
{
    for (auto& ch : channels_)
        ch->reset();
}

void FBKSuppressorProcessor::pullParameters()
{
    fbk::Parameters p;

    p.bypass              = raw_.bypass->load() > 0.5f;
    p.strength            = raw_.strength->load() * 0.01f;
    p.qualityMode         = raw_.qualityMode->load() > 0.5f;

    p.feedbackEnabled     = raw_.fbEnabled->load() > 0.5f;
    p.feedbackSensitivity = raw_.fbSensitivity->load() * 0.01f;
    p.feedbackDepth       = raw_.fbDepth->load() * 0.01f;

    p.denoiseEnabled      = raw_.nrEnabled->load() > 0.5f;
    p.denoiseAmount       = raw_.nrAmount->load() * 0.01f;
    p.maxAttenuationDb    = raw_.nrMaxAtten->load();
    p.voiceProtection     = raw_.nrVoiceProtect->load() * 0.01f;

    p.humEnabled          = raw_.humEnabled->load() > 0.5f;
    p.humDepth            = raw_.humDepth->load() * 0.01f;
    p.humHarmonics        = static_cast<int> (raw_.humHarmonics->load());

    p.dereverbEnabled     = raw_.drEnabled->load() > 0.5f;
    p.dereverbAmount      = raw_.drAmount->load() * 0.01f;
    p.rt60Seconds         = raw_.drRt60->load();

    p.highPassEnabled     = raw_.hpEnabled->load() > 0.5f;
    p.highPassHz          = raw_.hpFreq->load();

    p.telemetryEnabled    = raw_.telemetry->load() > 0.5f;
    p.eventDifferenceThresholdDb = raw_.eventThreshold->load();
    p.profileApplied      = profileAppliedFlag_;

    cachedParams_ = p;
    for (auto& ch : channels_)
        ch->setParameters (p);
}

void FBKSuppressorProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                           juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto startTime = juce::Time::getHighResolutionTicks();

    const int numSamples = buffer.getNumSamples();
    const int numIn  = getMainBusNumInputChannels();
    const int numOut = getMainBusNumOutputChannels();

    for (int c = numIn; c < numOut; ++c)
        buffer.clear (c, 0, numSamples);

    if (channels_.empty() || numSamples <= 0)
        return;

    pullParameters();

    // Quality mode changes the reported latency. Tell the host only when it
    // actually changes: calling setLatencySamples every block makes some hosts
    // rebuild their graph continuously.
    const int wanted = channels_.front()->latencySamples();
    if (wanted != reportedLatency_)
    {
        reportedLatency_ = wanted;
        setLatencySamples (wanted);
    }

    const int n = std::min (numIn, static_cast<int> (channels_.size()));
    for (int c = 0; c < n; ++c)
        channels_[static_cast<size_t> (c)]->processBlock (buffer.getWritePointer (c), numSamples);

    // Cost of this block relative to its own duration, smoothed. Shown in the
    // editor so there is never any guesswork about headroom before a show.
    const auto endTime = juce::Time::getHighResolutionTicks();
    const double elapsed = juce::Time::highResolutionTicksToSeconds (endTime - startTime);
    const double blockDuration = static_cast<double> (numSamples) / sampleRate_;
    if (blockDuration > 0.0)
    {
        const float instant = static_cast<float> (elapsed / blockDuration);
        const float previous = cpuLoad_.load();
        cpuLoad_.store (previous + 0.05f * (instant - previous));
    }
}

// ===========================================================================
const fbk::Metering* FBKSuppressorProcessor::metering() const noexcept
{
    if (channels_.empty())
        return nullptr;
    return &channels_.front()->metering();
}

float FBKSuppressorProcessor::measurementCentroidMs() const noexcept
{
    if (channels_.empty())
        return 0.0f;
    return channels_.front()->measurementCentroidMs();
}

void FBKSuppressorProcessor::setCaptureEnabled (bool enabled)
{
    captureEnabled_.store (enabled);
    for (auto& ch : channels_)
        ch->captureRing().setEnabled (enabled);
}

bool FBKSuppressorProcessor::writeCapture (const juce::File& destination, juce::String& errorOut)
{
    if (channels_.empty())
    {
        errorOut = "Plugin is not prepared.";
        return false;
    }

    std::vector<float> dry, wet;
    const int n = channels_.front()->captureRing().snapshot (dry, wet);
    if (n <= 0)
    {
        errorOut = "Nothing captured yet. Enable capture and let audio run.";
        return false;
    }

    // Two channels: the untouched input and the processed output, so a problem
    // can be heard both ways round and the difference examined directly.
    juce::AudioBuffer<float> out (2, n);
    out.copyFrom (0, 0, dry.data(), n);
    out.copyFrom (1, 0, wet.data(), n);

    destination.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (destination.createOutputStream());
    if (stream == nullptr)
    {
        errorOut = "Could not open " + destination.getFullPathName() + " for writing.";
        return false;
    }

    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions()
                             .withSampleRate (sampleRate_)
                             .withNumChannels (2)
                             .withBitsPerSample (24);

    std::unique_ptr<juce::OutputStream> asBase (std::move (stream));
    auto writer = wav.createWriterFor (asBase, options);

    if (writer == nullptr)
    {
        errorOut = "Could not create a WAV writer.";
        return false;
    }

    writer->writeFromAudioSampleBuffer (out, 0, n);
    writer.reset();

    return true;
}

// ===========================================================================
void FBKSuppressorProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts_.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void FBKSuppressorProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts_.state.getType()))
            apvts_.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* FBKSuppressorProcessor::createEditor()
{
    return new FBKSuppressorEditor (*this);
}

// ===========================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FBKSuppressorProcessor();
}

// ===========================================================================
// Parameters new in this revision are declared in createLayout above; the
// calibration, telemetry and sweep surfaces are all message-thread only and are
// implemented below.
// ===========================================================================

void FBKSuppressorProcessor::beginCalibration (fbk::CalibrationPhase phase)
{
    for (auto& ch : channels_)
        ch->beginCalibration (phase);
}

void FBKSuppressorProcessor::finishCalibration()
{
    for (auto& ch : channels_)
        ch->finishCalibration();

    // Channel 0 is the reference: calibrating against two microphones at once
    // would average two different voices into one meaningless profile.
    if (auto* ch = primaryChannel())
        workingProfile_ = ch->profile();
}

void FBKSuppressorProcessor::cancelCalibration()
{
    for (auto& ch : channels_)
        ch->cancelCalibration();
}

fbk::CalibrationPhase FBKSuppressorProcessor::calibrationPhase() const
{
    if (channels_.empty())
        return fbk::CalibrationPhase::idle;
    return channels_.front()->calibrationPhase();
}

float FBKSuppressorProcessor::calibrationProgress() const
{
    return channels_.empty() ? 0.0f : channels_.front()->calibrationProgress();
}

float FBKSuppressorProcessor::calibrationElapsed() const
{
    return channels_.empty() ? 0.0f : channels_.front()->calibrationElapsedSeconds();
}

const fbk::VoiceProfile& FBKSuppressorProcessor::workingProfile() const
{
    return workingProfile_;
}

void FBKSuppressorProcessor::applyWorkingProfile()
{
    if (! workingProfile_.valid)
        return;

    // Every channel gets the same profile. The voice is the same voice even when
    // there are several microphones on it; only the room modes could differ, and a
    // shared prior list only ever makes the detector quicker, never more
    // destructive.
    for (auto& ch : channels_)
        ch->applyProfile (workingProfile_);

    profileAppliedFlag_ = true;
}

void FBKSuppressorProcessor::clearAppliedProfile()
{
    for (auto& ch : channels_)
        ch->clearProfile();
    profileAppliedFlag_ = false;
}

bool FBKSuppressorProcessor::isProfileApplied() const
{
    return profileAppliedFlag_;
}

// ---------------------------------------------------------------------------
namespace
{
juce::var bandArrayToVar (const float* values, int count)
{
    juce::Array<juce::var> a;
    for (int i = 0; i < count; ++i)
        a.add (values[i]);
    return juce::var (a);
}

void varToBandArray (const juce::var& v, float* out, int count)
{
    if (auto* a = v.getArray())
        for (int i = 0; i < count && i < a->size(); ++i)
            out[i] = static_cast<float> (static_cast<double> ((*a)[i]));
}
} // namespace

bool FBKSuppressorProcessor::saveProfile (const juce::File& file, juce::String& errorOut)
{
    if (! workingProfile_.valid)
    {
        errorOut = "No calibration data yet. Run at least one phase first.";
        return false;
    }

    const auto& p = workingProfile_;
    auto* root = new juce::DynamicObject();

    root->setProperty ("format", "FBKSuppressorProfile");
    root->setProperty ("version", p.version);
    root->setProperty ("sampleRate", p.sampleRate);

    root->setProperty ("hasNoise", p.hasNoise);
    root->setProperty ("bandNoiseDb", bandArrayToVar (p.bandNoiseDb, fbk::kNumBands));
    root->setProperty ("broadbandNoiseDbFS", p.broadbandNoiseDbFS);

    root->setProperty ("hasVoice", p.hasVoice);
    root->setProperty ("bandVoiceDb", bandArrayToVar (p.bandVoiceDb, fbk::kNumBands));
    root->setProperty ("f0LowHz", p.f0LowHz);
    root->setProperty ("f0MedianHz", p.f0MedianHz);
    root->setProperty ("f0HighHz", p.f0HighHz);
    root->setProperty ("voicePnprP95Db", p.voicePnprP95Db);
    root->setProperty ("voicePhprP95Db", p.voicePhprP95Db);
    root->setProperty ("voiceFsdP05Hz", p.voiceFsdP05Hz);
    root->setProperty ("voiceFsdMedianHz", p.voiceFsdMedianHz);
    root->setProperty ("voiceProminenceP95Db", p.voiceProminenceP95Db);
    root->setProperty ("voiceProminenceMedianDb", p.voiceProminenceMedianDb);
    root->setProperty ("voiceCriterionSamples", p.voiceCriterionSamples);

    root->setProperty ("hasSuggestions", p.hasSuggestions);
    root->setProperty ("suggestedPnprDb", p.suggestedPnprDb);
    root->setProperty ("suggestedPhprDb", p.suggestedPhprDb);
    root->setProperty ("suggestedFsdMaxHz", p.suggestedFsdMaxHz);
    root->setProperty ("suggestedLocalProminenceDb", p.suggestedLocalProminenceDb);
    root->setProperty ("suggestedAbsoluteFloorDb", p.suggestedAbsoluteFloorDb);
    root->setProperty ("suggestedVoiceProtection",
                       bandArrayToVar (p.suggestedVoiceProtection, fbk::kNumBands));

    juce::Array<juce::var> modes;
    for (int i = 0; i < p.numModes; ++i)
    {
        auto* m = new juce::DynamicObject();
        m->setProperty ("freqHz", p.modes[i].freqHz);
        m->setProperty ("strengthDb", p.modes[i].strengthDb);
        m->setProperty ("engagedSeconds", p.modes[i].engagedSeconds);
        m->setProperty ("hits", p.modes[i].hits);
        modes.add (juce::var (m));
    }
    root->setProperty ("modes", juce::var (modes));

    const juce::var rootVar (root);
    if (! file.replaceWithText (juce::JSON::toString (rootVar, false)))
    {
        errorOut = "Could not write " + file.getFullPathName();
        return false;
    }
    return true;
}

bool FBKSuppressorProcessor::loadProfile (const juce::File& file, juce::String& errorOut)
{
    if (! file.existsAsFile())
    {
        errorOut = "No such file: " + file.getFullPathName();
        return false;
    }

    const auto parsed = juce::JSON::parse (file.loadFileAsString());
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr || obj->getProperty ("format").toString() != "FBKSuppressorProfile")
    {
        errorOut = "Not an FBKSuppressor profile.";
        return false;
    }

    fbk::VoiceProfile p {};
    p.version = static_cast<int> (obj->getProperty ("version"));
    p.sampleRate = static_cast<double> (obj->getProperty ("sampleRate"));

    p.hasNoise = static_cast<bool> (obj->getProperty ("hasNoise"));
    varToBandArray (obj->getProperty ("bandNoiseDb"), p.bandNoiseDb, fbk::kNumBands);
    p.broadbandNoiseDbFS = static_cast<float> (static_cast<double> (obj->getProperty ("broadbandNoiseDbFS")));

    p.hasVoice = static_cast<bool> (obj->getProperty ("hasVoice"));
    varToBandArray (obj->getProperty ("bandVoiceDb"), p.bandVoiceDb, fbk::kNumBands);
    p.f0LowHz    = static_cast<float> (static_cast<double> (obj->getProperty ("f0LowHz")));
    p.f0MedianHz = static_cast<float> (static_cast<double> (obj->getProperty ("f0MedianHz")));
    p.f0HighHz   = static_cast<float> (static_cast<double> (obj->getProperty ("f0HighHz")));
    p.voicePnprP95Db = static_cast<float> (static_cast<double> (obj->getProperty ("voicePnprP95Db")));
    p.voicePhprP95Db = static_cast<float> (static_cast<double> (obj->getProperty ("voicePhprP95Db")));
    p.voiceFsdP05Hz  = static_cast<float> (static_cast<double> (obj->getProperty ("voiceFsdP05Hz")));
    p.voiceFsdMedianHz = static_cast<float> (static_cast<double> (obj->getProperty ("voiceFsdMedianHz")));
    p.voiceProminenceP95Db = static_cast<float> (static_cast<double> (obj->getProperty ("voiceProminenceP95Db")));
    p.voiceProminenceMedianDb = static_cast<float> (static_cast<double> (obj->getProperty ("voiceProminenceMedianDb")));
    p.voiceCriterionSamples = static_cast<int> (obj->getProperty ("voiceCriterionSamples"));

    p.hasSuggestions = static_cast<bool> (obj->getProperty ("hasSuggestions"));
    p.suggestedPnprDb = static_cast<float> (static_cast<double> (obj->getProperty ("suggestedPnprDb")));
    p.suggestedPhprDb = static_cast<float> (static_cast<double> (obj->getProperty ("suggestedPhprDb")));
    p.suggestedFsdMaxHz = static_cast<float> (static_cast<double> (obj->getProperty ("suggestedFsdMaxHz")));
    p.suggestedLocalProminenceDb =
        static_cast<float> (static_cast<double> (obj->getProperty ("suggestedLocalProminenceDb")));
    p.suggestedAbsoluteFloorDb =
        static_cast<float> (static_cast<double> (obj->getProperty ("suggestedAbsoluteFloorDb")));
    varToBandArray (obj->getProperty ("suggestedVoiceProtection"),
                    p.suggestedVoiceProtection, fbk::kNumBands);

    if (auto* modes = obj->getProperty ("modes").getArray())
    {
        p.numModes = juce::jmin (modes->size(), fbk::kMaxRoomModes);
        for (int i = 0; i < p.numModes; ++i)
            if (auto* m = (*modes)[i].getDynamicObject())
            {
                p.modes[i].freqHz = static_cast<float> (static_cast<double> (m->getProperty ("freqHz")));
                p.modes[i].strengthDb = static_cast<float> (static_cast<double> (m->getProperty ("strengthDb")));
                p.modes[i].engagedSeconds = static_cast<float> (static_cast<double> (m->getProperty ("engagedSeconds")));
                p.modes[i].hits = static_cast<int> (m->getProperty ("hits"));
            }
    }

    p.valid = p.hasNoise || p.hasVoice || p.numModes > 0;
    if (! p.valid)
    {
        errorOut = "Profile contains no usable measurements.";
        return false;
    }

    // A profile measured at a different sample rate is still usable - every
    // threshold in it is expressed in Hz or dB, not in bins or samples - but it is
    // worth saying so rather than silently accepting it.
    if (std::abs (p.sampleRate - sampleRate_) > 1.0)
        errorOut = "Loaded (measured at " + juce::String (p.sampleRate / 1000.0, 1)
                 + " kHz, now running at " + juce::String (sampleRate_ / 1000.0, 1) + " kHz).";

    workingProfile_ = p;
    return true;
}

// ---------------------------------------------------------------------------
void FBKSuppressorProcessor::startSweep (float levelDb)
{
    for (auto& ch : channels_)
    {
        ch->sweep().setLevelDb (levelDb);
        ch->sweep().start();
    }
}

void FBKSuppressorProcessor::abortSweep()
{
    for (auto& ch : channels_)
        ch->sweep().abort();
}

bool FBKSuppressorProcessor::isSweeping() const
{
    return ! channels_.empty() && channels_.front()->isSweeping();
}

float FBKSuppressorProcessor::sweepProgress() const
{
    return channels_.empty() ? 0.0f : channels_.front()->sweep().progress();
}

juce::String FBKSuppressorProcessor::sweepSummary() const
{
    if (channels_.empty())
        return "not prepared";

    auto& sw = channels_.front()->sweep();
    if (sw.isRunning())
        return "measuring... " + juce::String (juce::roundToInt (sw.progress() * 100.0f)) + "%";
    if (! sw.hasResult())
        return "no measurement yet";

    juce::String s;
    s << "peak " << juce::String (sw.measuredPeakDbFS(), 1) << " dBFS";
    s << ", loop delay " << sw.directDelaySamples() << " samples";
    s << ", RT60 " << juce::String (sw.estimatedRt60Seconds(), 2) << " s";
    if (sw.clipped())
        s << "  [CLIPPED - redo at a lower level]";
    else if (sw.tooQuiet())
        s << "  [too quiet - raise the level or the mic gain]";
    return s;
}

bool FBKSuppressorProcessor::exportImpulseResponse (const juce::File& file, juce::String& errorOut)
{
    auto* ch = primaryChannel();
    if (ch == nullptr)
    {
        errorOut = "Plugin is not prepared.";
        return false;
    }

    auto& sw = ch->sweep();
    if (sw.isRunning())
    {
        errorOut = "Sweep still running.";
        return false;
    }

    // Deconvolution is a half-megasample FFT; it belongs here on the message
    // thread, never on the audio thread.
    sw.computeImpulseResponse();

    const auto& ir = sw.impulseResponse();
    if (ir.empty())
    {
        errorOut = "No impulse response - run a sweep first.";
        return false;
    }

    juce::AudioBuffer<float> buffer (1, static_cast<int> (ir.size()));
    buffer.copyFrom (0, 0, ir.data(), static_cast<int> (ir.size()));

    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
    {
        errorOut = "Could not open " + file.getFullPathName();
        return false;
    }

    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions()
                             .withSampleRate (sw.sampleRate())
                             .withNumChannels (1)
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    auto writer = wav.createWriterFor (stream, options);
    if (writer == nullptr)
    {
        errorOut = "Could not create a WAV writer.";
        return false;
    }

    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    writer.reset();
    return true;
}

// ===========================================================================
// DiagnosticsWriter
// ===========================================================================
DiagnosticsWriter::DiagnosticsWriter (FBKSuppressorProcessor& p)
    : juce::Thread ("FBKSuppressor Diagnostics"), processor_ (p)
{
    startThread (juce::Thread::Priority::low);
}

DiagnosticsWriter::~DiagnosticsWriter()
{
    stopThread (2000);
    telemetryStream_.reset();
}

void DiagnosticsWriter::setFolder (const juce::File& f)
{
    const juce::ScopedLock sl (lock_);
    if (folder_ == f)
        return;
    folder_ = f;
    // Force a new file, so switching folders mid-session does not keep appending
    // to the old one.
    telemetryStream_.reset();
    sessionStamp_.clear();
}

juce::File DiagnosticsWriter::folder() const
{
    const juce::ScopedLock sl (lock_);
    return folder_;
}

juce::String DiagnosticsWriter::lastMessage() const
{
    const juce::ScopedLock sl (lock_);
    return lastMessage_;
}

void DiagnosticsWriter::openTelemetryFile()
{
    const juce::ScopedLock sl (lock_);
    if (folder_ == juce::File{})
        return;

    if (! folder_.exists())
        folder_.createDirectory();

    sessionStamp_ = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    const auto file = folder_.getChildFile ("fbk-telemetry-" + sessionStamp_ + ".csv");

    file.deleteFile();
    telemetryStream_ = std::make_unique<juce::FileOutputStream> (file);
    if (telemetryStream_->failedToOpen())
    {
        telemetryStream_.reset();
        lastMessage_ = "Could not open telemetry file in " + folder_.getFullPathName();
        return;
    }

    // One row per frame. Deliberately a wide flat schema rather than anything
    // nested: the point is that this drops straight into a spreadsheet or a pandas
    // DataFrame without a parser.
    juce::String header ("time_s,difference_db,speech_presence,in_peak,out_peak,"
                         "hum_hz,hum_active,confirmed_tones");
    for (int b = 0; b < fbk::kNumBands; ++b)
        header << ",in_db_" << b;
    for (int b = 0; b < fbk::kNumBands; ++b)
        header << ",gain_db_" << b;
    // The four strongest tracks, with every criterion, so a detection or a
    // near-miss can be reconstructed exactly from the log alone.
    for (int t = 0; t < 4; ++t)
        header << ",t" << t << "_hz,t" << t << "_conf,t" << t << "_papr,t" << t << "_pnpr"
               << ",t" << t << "_phpr,t" << t << "_prom,t" << t << "_imsd,t" << t << "_fsd"
               << ",t" << t << "_persist,t" << t << "_confirmed";
    header << juce::newLine;

    telemetryStream_->writeText (header, false, false, nullptr);
    lastMessage_ = "Logging to " + file.getFileName();
}

void DiagnosticsWriter::writeFrame (const fbk::TelemetryFrame& f)
{
    if (telemetryStream_ == nullptr)
        return;

    const double sr = processor_.currentSampleRate();
    juce::String row;
    row.preallocateBytes (768);

    row << juce::String (static_cast<double> (f.sampleTime) / (sr > 0.0 ? sr : 48000.0), 3)
        << ',' << juce::String (f.differenceDb, 1)
        << ',' << juce::String (f.speechPresence, 3)
        << ',' << juce::String (f.inputPeak, 4)
        << ',' << juce::String (f.outputPeak, 4)
        << ',' << juce::String (f.humFundamentalHz, 0)
        << ',' << (f.humActive ? 1 : 0)
        << ',' << f.confirmedTones;

    for (int b = 0; b < fbk::kNumBands; ++b)
        row << ',' << juce::String (f.bandInputDb[b], 1);
    for (int b = 0; b < fbk::kNumBands; ++b)
        row << ',' << juce::String (f.bandGainDb[b], 1);

    for (int t = 0; t < 4; ++t)
    {
        if (t < f.numActiveTones)
        {
            const auto& tone = f.tones[t];
            row << ',' << juce::String (tone.freqHz, 1)
                << ',' << juce::String (tone.confidence, 3)
                << ',' << juce::String (tone.paprDb, 1)
                << ',' << juce::String (tone.pnprDb, 1)
                << ',' << juce::String (tone.phprDb, 1)
                << ',' << juce::String (tone.prominenceDb, 1)
                << ',' << juce::String (tone.imsd, 2)
                << ',' << juce::String (tone.fsdHz, 2)
                << ',' << tone.persistence
                << ',' << (tone.confirmed ? 1 : 0);
        }
        else
        {
            row << ",,,,,,,,,,";
        }
    }

    row << juce::newLine;
    telemetryStream_->writeText (row, false, false, nullptr);
    framesWritten_.fetch_add (1);
}

void DiagnosticsWriter::writeEventCapture()
{
    juce::File target;
    {
        const juce::ScopedLock sl (lock_);
        if (folder_ == juce::File{})
            return;
        target = folder_;
    }

    auto* ch = processor_.primaryChannel();
    if (ch == nullptr)
        return;

    juce::String reason ("event");
    if (ch->eventTrigger().reasonWasDifference())
        reason = "difference";
    else if (ch->eventTrigger().reasonToneHz() > 0.0f)
        reason = juce::String (juce::roundToInt (ch->eventTrigger().reasonToneHz())) + "Hz";

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    const auto file = target.getChildFile ("fbk-event-" + stamp + "-" + reason + ".wav");

    juce::String error;
    if (processor_.writeCapture (file, error))
    {
        eventsWritten_.fetch_add (1);
        const juce::ScopedLock sl (lock_);
        lastMessage_ = "Captured " + file.getFileName();
    }
    else
    {
        const juce::ScopedLock sl (lock_);
        lastMessage_ = "Capture failed: " + error;
    }
}

void DiagnosticsWriter::run()
{
    while (! threadShouldExit())
    {
        auto* ch = processor_.primaryChannel();

        if (ch != nullptr && folder() != juce::File{})
        {
            if (ch->telemetry().isEnabled())
            {
                if (telemetryStream_ == nullptr)
                    openTelemetryFile();

                fbk::TelemetryFrame frame;
                int drained = 0;
                while (ch->telemetry().pop (frame) && drained < 4096)
                {
                    writeFrame (frame);
                    ++drained;
                }

                if (telemetryStream_ != nullptr && drained > 0)
                    telemetryStream_->flush();
            }

            // Event captures happen whether or not telemetry logging is on: the
            // audio around a problem is worth keeping either way.
            if (ch->eventTrigger().consume())
                writeEventCapture();
        }

        // 200 ms is comfortably faster than the ring can fill at 25 Hz with 100
        // frames of headroom, so nothing is ever dropped in normal operation.
        wait (200);
    }

    if (telemetryStream_ != nullptr)
        telemetryStream_->flush();
}
