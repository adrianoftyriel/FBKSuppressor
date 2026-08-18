// FBKSuppressor - PluginEditor.h
//
// Three tabs, because the three things you do with this plugin happen at
// different times and under different amounts of pressure.
//
//   Process     - what you touch during a show. Must be readable at a glance.
//   Calibrate   - what you do at soundcheck, once, deliberately.
//   Diagnostics - logging and measurement, for working out afterwards why
//                 something happened.
//
// Keeping them apart matters: nothing on the Calibrate or Diagnostics tabs should
// ever be reachable by accident while a show is running, and the Process tab
// should not be cluttered by controls you use twice a year.
#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"

// Draws the per-band input spectrum, the tracked noise floor, the mask being
// applied, and every tone currently being cancelled. Seeing the mask alongside the
// spectrum is the quickest way to tell whether the processor is working in the gaps
// or chewing on the voice.
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

// A labelled slider bound to a parameter.
struct BoundSlider
{
    juce::Slider slider;
    juce::Label  label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// Simple horizontal progress bar, drawn rather than using juce::ProgressBar so the
// value can be pulled from the processor each repaint without owning a double.
class SimpleProgress final : public juce::Component
{
public:
    void setProgress (float p) { progress_ = juce::jlimit (0.0f, 1.0f, p); repaint(); }
    void setText (const juce::String& t) { text_ = t; repaint(); }
    void paint (juce::Graphics&) override;

private:
    float progress_ { 0.0f };
    juce::String text_;
};

// ---------------------------------------------------------------------------
class ProcessPanel final : public juce::Component
{
public:
    ProcessPanel (FBKSuppressorProcessor&);
    void resized() override;

private:
    void addSlider (BoundSlider&, const char* paramId, const juce::String& text,
                    juce::Slider::SliderStyle);
    void addToggle (juce::ToggleButton&,
                    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>&,
                    const char* paramId, const juce::String& text);

    FBKSuppressorProcessor& processor_;

    AnalyserDisplay display_;
    BoundSlider strength_;

    juce::ToggleButton fbButton_, nrButton_, humButton_, drButton_, hpButton_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        fbAttachment_, nrAttachment_, humAttachment_, drAttachment_, hpAttachment_;

    BoundSlider fbSensitivity_, fbDepth_;
    BoundSlider nrAmount_, nrMaxAtten_, nrVoiceProtect_;
    BoundSlider humDepth_, humHarmonics_;
    BoundSlider drAmount_, drRt60_;
    BoundSlider hpFreq_;
};

// ---------------------------------------------------------------------------
class CalibratePanel final : public juce::Component,
                             private juce::Timer
{
public:
    CalibratePanel (FBKSuppressorProcessor&);
    ~CalibratePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void startPhase (fbk::CalibrationPhase);
    void refreshSummary();
    void saveProfile();
    void loadProfile();

    FBKSuppressorProcessor& processor_;

    juce::Label       instructions_;
    juce::TextButton  noiseButton_, voiceButton_, modesButton_, stopButton_;
    SimpleProgress    progress_;
    juce::TextEditor  summary_;
    juce::TextButton  applyButton_, clearButton_, saveButton_, loadButton_;
    juce::Label       statusLabel_;

    std::unique_ptr<juce::FileChooser> chooser_;
};

// ---------------------------------------------------------------------------
class DiagnosticsPanel final : public juce::Component,
                               private juce::Timer
{
public:
    DiagnosticsPanel (FBKSuppressorProcessor&);
    ~DiagnosticsPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void chooseFolder();
    void confirmAndStartSweep();
    void exportImpulse();

    FBKSuppressorProcessor& processor_;

    juce::Label        loggingHeader_, sweepHeader_;
    juce::ToggleButton telemetryButton_, captureButton_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> telemetryAttachment_;
    BoundSlider        eventThreshold_;
    juce::TextButton   folderButton_, saveCaptureButton_;
    juce::Label        folderLabel_, loggingStatus_;

    // Sweep level is deliberately not a parameter: it must not be automatable,
    // recallable, or restorable from a session. Every sweep is an explicit act with
    // a level chosen there and then.
    juce::Slider       sweepLevelSlider_;
    juce::Label        sweepLevelLabel_;
    juce::TextButton   sweepButton_, sweepAbortButton_, exportIrButton_;
    SimpleProgress     sweepProgress_;
    juce::Label        sweepSummary_;

    int separatorY_ { 0 };
    std::unique_ptr<juce::FileChooser> chooser_;
};

// ---------------------------------------------------------------------------
class FBKSuppressorEditor final : public juce::AudioProcessorEditor,
                                  private juce::Timer
{
public:
    explicit FBKSuppressorEditor (FBKSuppressorProcessor&);
    ~FBKSuppressorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    FBKSuppressorProcessor& processor_;

    juce::Label titleLabel_, statusLabel_, latencyLabel_;
    juce::ToggleButton bypassButton_, qualityButton_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        bypassAttachment_, qualityAttachment_;

    juce::TabbedComponent tabs_ { juce::TabbedButtonBar::TabsAtTop };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FBKSuppressorEditor)
};
