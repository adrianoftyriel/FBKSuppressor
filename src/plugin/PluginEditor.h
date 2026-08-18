// FBKSuppressor - PluginEditor.h
//
// The display exists to answer three questions quickly, mid-show:
// what is it detecting, how much is it actually doing, and is there CPU headroom.
#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"

// Draws the per-band input spectrum, the tracked noise floor, the mask the
// suppressor is applying, and the frequency of every tone the detector is
// currently cancelling. Seeing the mask alongside the spectrum is the fastest way
// to tell whether the processor is working in the gaps or chewing on the voice.
class AnalyserDisplay final : public juce::Component,
                              private juce::Timer
{
public:
    explicit AnalyserDisplay (FBKSuppressorProcessor&);
    ~AnalyserDisplay() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    float frequencyToX (float hz, juce::Rectangle<float> area) const;

    FBKSuppressorProcessor& processor_;
    fbk::Metering snapshot_ {};
    float smoothedInput_[fbk::kNumBands] {};
    float smoothedGain_[fbk::kNumBands] {};
    bool  primed_ { false };
};

class FBKSuppressorEditor final : public juce::AudioProcessorEditor,
                                  private juce::Timer
{
public:
    explicit FBKSuppressorEditor (FBKSuppressorProcessor&);
    ~FBKSuppressorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Control
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    void addControl (Control&, const char* paramId, const juce::String& text,
                     juce::Slider::SliderStyle style);
    void addToggle (juce::ToggleButton&, std::unique_ptr<ButtonAttachment>&,
                    const char* paramId, const juce::String& text);
    void timerCallback() override;
    void saveCapture();

    FBKSuppressorProcessor& processor_;

    juce::Label titleLabel_, statusLabel_, latencyLabel_;

    juce::ToggleButton bypassButton_, qualityButton_;
    std::unique_ptr<ButtonAttachment> bypassAttachment_, qualityAttachment_;

    Control strength_;

    juce::ToggleButton fbButton_, nrButton_, humButton_, drButton_, hpButton_;
    std::unique_ptr<ButtonAttachment> fbAttachment_, nrAttachment_, humAttachment_,
                                      drAttachment_, hpAttachment_;

    Control fbSensitivity_, fbDepth_;
    Control nrAmount_, nrMaxAtten_, nrVoiceProtect_;
    Control humDepth_, humHarmonics_;
    Control drAmount_, drRt60_;
    Control hpFreq_;

    juce::ToggleButton captureButton_;
    juce::TextButton   saveCaptureButton_;

    AnalyserDisplay display_;

    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FBKSuppressorEditor)
};
