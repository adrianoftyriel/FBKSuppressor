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
