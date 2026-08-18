// FBKSuppressor - PluginProcessor.h
//
// The JUCE wrapper. All the signal processing lives in fbk_dsp, which has no
// dependency on JUCE at all; this file only translates between the host's world
// and that library. Keeping the boundary strict is what lets the DSP be tested
// without a plugin host, and it is also what would let the same core run in a
// standalone binary, a CLAP wrapper, or on a dedicated box later.
#pragma once

#include <JuceHeader.h>

#include "FeedbackSuppressor.h"

#include <atomic>
#include <memory>
#include <vector>

namespace fbkparam
{
// Parameter IDs, kept in one place so the editor and the processor cannot drift
// apart and so saved sessions keep loading.
inline constexpr const char* bypass          = "bypass";
inline constexpr const char* strength        = "strength";
inline constexpr const char* qualityMode     = "qualityMode";

inline constexpr const char* fbEnabled       = "fbEnabled";
inline constexpr const char* fbSensitivity   = "fbSensitivity";
inline constexpr const char* fbDepth         = "fbDepth";

inline constexpr const char* nrEnabled       = "nrEnabled";
inline constexpr const char* nrAmount        = "nrAmount";
inline constexpr const char* nrMaxAtten      = "nrMaxAtten";
inline constexpr const char* nrVoiceProtect  = "nrVoiceProtect";

inline constexpr const char* humEnabled      = "humEnabled";
inline constexpr const char* humDepth        = "humDepth";
inline constexpr const char* humHarmonics    = "humHarmonics";

inline constexpr const char* drEnabled       = "drEnabled";
inline constexpr const char* drAmount        = "drAmount";
inline constexpr const char* drRt60          = "drRt60";

inline constexpr const char* hpEnabled       = "hpEnabled";
inline constexpr const char* hpFreq          = "hpFreq";
} // namespace fbkparam

class FBKSuppressorProcessor final : public juce::AudioProcessor
{
public:
    FBKSuppressorProcessor();
    ~FBKSuppressorProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState& state() noexcept { return apvts_; }

    // Snapshot of the first channel's metering, for the editor. Read on the
    // message thread; the audio thread writes it without synchronisation, which
    // is acceptable for display values and avoids any lock on the audio path.
    const fbk::Metering* metering() const noexcept;
    int numActiveChannels() const noexcept { return static_cast<int> (channels_.size()); }
    double currentSampleRate() const noexcept { return sampleRate_; }
    float measurementCentroidMs() const noexcept;

    // Rolling capture, for sending real-room problem audio back for analysis.
    void setCaptureEnabled (bool);
    bool isCaptureEnabled() const noexcept { return captureEnabled_.load(); }
    // Writes the last ~12 seconds of dry and processed audio to a stereo WAV.
    // Called from the message thread only.
    bool writeCapture (const juce::File& destination, juce::String& errorOut);

    // Measured cost of the last block as a fraction of real time, smoothed.
    float cpuLoad() const noexcept { return cpuLoad_.load(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void pullParameters();

    juce::AudioProcessorValueTreeState apvts_;

    // One suppressor per channel. They are entirely independent: a shared
    // detector across channels would be wrong, because the feedback modes on two
    // different microphones are not the same modes.
    std::vector<std::unique_ptr<fbk::FeedbackSuppressor>> channels_;

    double sampleRate_ { 48000.0 };
    int    reportedLatency_ { 0 };

    std::atomic<bool> captureEnabled_ { false };
    std::atomic<float> cpuLoad_ { 0.0f };

    // Cached atomic parameter pointers, so processBlock never does a string
    // lookup or touches the parameter tree's locks.
    struct RawParams
    {
        std::atomic<float>* bypass {};
        std::atomic<float>* strength {};
        std::atomic<float>* qualityMode {};
        std::atomic<float>* fbEnabled {};
        std::atomic<float>* fbSensitivity {};
        std::atomic<float>* fbDepth {};
        std::atomic<float>* nrEnabled {};
        std::atomic<float>* nrAmount {};
        std::atomic<float>* nrMaxAtten {};
        std::atomic<float>* nrVoiceProtect {};
        std::atomic<float>* humEnabled {};
        std::atomic<float>* humDepth {};
        std::atomic<float>* humHarmonics {};
        std::atomic<float>* drEnabled {};
        std::atomic<float>* drAmount {};
        std::atomic<float>* drRt60 {};
        std::atomic<float>* hpEnabled {};
        std::atomic<float>* hpFreq {};
    } raw_;

    fbk::Parameters cachedParams_ {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FBKSuppressorProcessor)
};
