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
//
// The window is laid out the way a channel strip is, because that is what the
// operator's hands already know:
//
//   top     - identity, bypass, latency and the running status line, then the tabs
//   middle  - the analyser, a third of the height, wide enough to read from arm's
//             length
//   bottom  - half the height, five module strips of vertical faders
//   right   - Strength, one large fader from the top of the analyser to the
//             bottom of the window, outside the tabs so it stays reachable no
//             matter which tab is showing
#pragma once

#include <JuceHeader.h>

#include "PluginProcessor.h"

// ---------------------------------------------------------------------------
// The carbon-fibre chassis everything sits on. The weave is drawn once into a
// small tile and then tiled over the area: generating it per repaint would be
// absurd for something that never changes.
class CarbonBackground
{
public:
    void paint (juce::Graphics&, juce::Rectangle<int> area);

private:
    juce::Image tile_;
};

// ---------------------------------------------------------------------------
// Physical controls: faders with a real slot, real travel and a machined cap,
// buttons and tabs that look pressed rather than tinted. A fader you can read at
// a glance from three metres away is worth more here than anything subtle.
class FbkLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    FbkLookAndFeel();

    // Set on a Slider's properties to get the larger Strength treatment.
    static constexpr const char* largeFaderProperty = "fbkLargeFader";

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool highlighted, bool down) override;

    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool highlighted, bool down) override;
    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}
    int  getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override;

    // Geometry, shared so that anything drawing a scale beside a fader agrees
    // with the fader about where the travel actually starts and ends.
    static float capHeight (bool large) noexcept { return large ? 34.0f : 22.0f; }
    static juce::Range<float> travel (juce::Rectangle<float> sliderArea, bool large) noexcept;

    static void drawFaderCap (juce::Graphics&, juce::Rectangle<float>);
    static void drawSlot (juce::Graphics&, juce::Rectangle<float>);
    static void drawPanel (juce::Graphics&, juce::Rectangle<float>, juce::Colour topRail,
                           bool dimmed = false);
    static void drawLed (juce::Graphics&, juce::Rectangle<float>, juce::Colour, bool lit);
};

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
// The round illuminated switch at the head of each module strip.
class LedButton final : public juce::Button
{
public:
    explicit LedButton (juce::Colour accent);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    juce::Colour accent_;
};

// One vertical fader, with its name and live value printed underneath. The value
// is drawn rather than put in a text box: it is read far more often than it is
// typed into, and a text box that size is unreadable across a stage.
class FaderStrip final : public juce::Component
{
public:
    FaderStrip (FBKSuppressorProcessor&, const char* paramId, juce::String name,
                juce::Colour accent, juce::String suffix, int decimals);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::String valueText() const;

    juce::Slider slider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment_;
    juce::String name_, suffix_;
    int decimals_;
};

// What a module strip is made of: an enable switch, a name, a live badge, and
// one fader per parameter.
struct FaderSpec
{
    const char* paramId;
    const char* name;
    const char* suffix;
    int decimals;
};

class ModuleStrip final : public juce::Component
{
public:
    ModuleStrip (FBKSuppressorProcessor&, juce::String title, const char* enableParamId,
                 juce::Colour accent, std::initializer_list<FaderSpec>);

    void paint (juce::Graphics&) override;
    void resized() override;

    // The live one-line readout in the strip's header - detected tones, the hum
    // frequency, how much is actually being taken out.
    void setBadge (const juce::String&);
    bool isOn() const { return power_.getToggleState(); }

private:
    void refreshDimming();

    juce::String title_, badge_;
    juce::Colour accent_;
    LedButton power_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> powerAttachment_;
    juce::OwnedArray<FaderStrip> faders_;
};

// The master control, and the only one that is always visible. It is deliberately
// the largest thing on the surface: when something goes wrong mid-show this is
// the fader you reach for without looking.
class StrengthColumn final : public juce::Component
{
public:
    explicit StrengthColumn (FBKSuppressorProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Slider slider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment_;
};

// ---------------------------------------------------------------------------
class ProcessPanel final : public juce::Component,
                           private juce::Timer
{
public:
    explicit ProcessPanel (FBKSuppressorProcessor&);
    ~ProcessPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    FBKSuppressorProcessor& processor_;
    CarbonBackground carbon_;

    AnalyserDisplay display_;
    juce::OwnedArray<ModuleStrip> modules_;
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
    CarbonBackground carbon_;

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
    CarbonBackground carbon_;

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

    // Declared first so it outlives every child that draws with it.
    FbkLookAndFeel lookAndFeel_;
    CarbonBackground carbon_;

    juce::Label titleLabel_, subtitleLabel_, statusLabel_, latencyLabel_;
    juce::ToggleButton bypassButton_, qualityButton_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        bypassAttachment_, qualityAttachment_;

    juce::TabbedComponent tabs_ { juce::TabbedButtonBar::TabsAtTop };
    StrengthColumn strength_;

    juce::Rectangle<int> brandArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FBKSuppressorEditor)
};
