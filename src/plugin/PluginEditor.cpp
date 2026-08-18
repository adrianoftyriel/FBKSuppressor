#include "PluginEditor.h"

namespace
{
constexpr float kMinHz = 40.0f;
constexpr float kMaxHz = 20000.0f;
constexpr float kMinDb = -90.0f;
constexpr float kMaxDb = -6.0f;

const juce::Colour kBackground   { 0xff14171c };
const juce::Colour kPanel        { 0xff1c2028 };
const juce::Colour kGrid         { 0xff2b3140 };
const juce::Colour kText         { 0xffd6dbe4 };
const juce::Colour kDim          { 0xff7c8698 };
const juce::Colour kInputColour  { 0xff4f8cc9 };
const juce::Colour kNoiseColour  { 0xff6b7280 };
const juce::Colour kGainColour   { 0xff44c58a };
const juce::Colour kToneColour   { 0xffe0574f };
const juce::Colour kWarnColour   { 0xffd9a13b };

void styleSlider (juce::Slider& s)
{
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 18);
    s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::textBoxTextColourId, kText);
    s.setColour (juce::Slider::thumbColourId, kGainColour);
    s.setColour (juce::Slider::rotarySliderFillColourId, kGainColour);
    s.setColour (juce::Slider::trackColourId, kGainColour.withAlpha (0.55f));
}

void styleButton (juce::Button& b)
{
    b.setColour (juce::TextButton::buttonColourId, kGrid);
    b.setColour (juce::TextButton::textColourOffId, kText);
    b.setColour (juce::ToggleButton::textColourId, kText);
    b.setColour (juce::ToggleButton::tickColourId, kGainColour);
}

void styleReadout (juce::TextEditor& e)
{
    e.setMultiLine (true);
    e.setReadOnly (true);
    e.setScrollbarsShown (true);
    e.setCaretVisible (false);
    e.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    e.setColour (juce::TextEditor::backgroundColourId, kBackground);
    e.setColour (juce::TextEditor::textColourId, kText);
    e.setColour (juce::TextEditor::outlineColourId, kGrid);
}
} // namespace

// ===========================================================================
void SimpleProgress::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (kGrid);
    g.fillRoundedRectangle (r, 3.0f);

    if (progress_ > 0.0f)
    {
        auto filled = r.withWidth (r.getWidth() * progress_);
        g.setColour (kGainColour.withAlpha (0.7f));
        g.fillRoundedRectangle (filled, 3.0f);
    }

    g.setColour (kText);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (text_, getLocalBounds(), juce::Justification::centred);
}

// ===========================================================================
AnalyserDisplay::AnalyserDisplay (FBKSuppressorProcessor& p) : processor_ (p)
{
    setOpaque (true);
    startTimerHz (25);
}

AnalyserDisplay::~AnalyserDisplay() { stopTimer(); }

void AnalyserDisplay::timerCallback()
{
    if (auto* m = processor_.metering())
    {
        snapshot_ = *m;

        // Ballistics on the display only; the processing itself is untouched by
        // this. It exists so the picture is readable rather than flickering.
        for (int b = 0; b < fbk::kNumBands; ++b)
        {
            const float in = snapshot_.bandInputDb[b];
            const float gain = snapshot_.bandGainDb[b];
            if (! primed_)
            {
                smoothedInput_[b] = in;
                smoothedGain_[b] = gain;
            }
            else
            {
                smoothedInput_[b] += (in > smoothedInput_[b] ? 0.6f : 0.2f) * (in - smoothedInput_[b]);
                smoothedGain_[b] += 0.35f * (gain - smoothedGain_[b]);
            }
        }
        primed_ = true;
    }
    repaint();
}

float AnalyserDisplay::frequencyToX (float hz, juce::Rectangle<float> area) const
{
    const float t = std::log (juce::jlimit (kMinHz, kMaxHz, hz) / kMinHz)
                  / std::log (kMaxHz / kMinHz);
    return area.getX() + t * area.getWidth();
}

void AnalyserDisplay::paint (juce::Graphics& g)
{
    g.fillAll (kPanel);
    auto plot = getLocalBounds().toFloat().reduced (8.0f, 6.0f);

    g.setColour (kGrid);
    g.setFont (juce::FontOptions (10.0f));
    for (float hz : { 50.0f, 100.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = frequencyToX (hz, plot);
        g.setColour (kGrid);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (kDim);
        const juce::String text = hz >= 1000.0f ? juce::String (hz / 1000.0f, 0) + "k"
                                                : juce::String (hz, 0);
        g.drawText (text, juce::Rectangle<float> (x - 16.0f, plot.getBottom() - 12.0f, 32.0f, 12.0f),
                    juce::Justification::centred);
    }

    if (! primed_)
    {
        g.setColour (kDim);
        g.drawText ("waiting for audio", plot, juce::Justification::centred);
        return;
    }

    const auto dbToY = [&plot] (float db)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb));
        return plot.getBottom() - t * plot.getHeight();
    };
    const auto bandX = [&plot] (int b)
    {
        const float t = (static_cast<float> (b) + 0.5f) / static_cast<float> (fbk::kNumBands);
        return plot.getX() + t * plot.getWidth();
    };

    juce::Path input, noise, gain;
    for (int b = 0; b < fbk::kNumBands; ++b)
    {
        const float x = bandX (b);
        const float yIn = dbToY (smoothedInput_[b]);
        const float yNo = dbToY (snapshot_.bandNoiseDb[b]);
        const float t = juce::jlimit (0.0f, 1.0f, (smoothedGain_[b] + 24.0f) / 24.0f);
        const float yGain = plot.getBottom() - t * plot.getHeight();

        if (b == 0)
        {
            input.startNewSubPath (x, yIn);
            noise.startNewSubPath (x, yNo);
            gain.startNewSubPath (x, yGain);
        }
        else
        {
            input.lineTo (x, yIn);
            noise.lineTo (x, yNo);
            gain.lineTo (x, yGain);
        }
    }

    g.setColour (kNoiseColour.withAlpha (0.8f));
    g.strokePath (noise, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
    g.setColour (kGainColour.withAlpha (0.9f));
    g.strokePath (gain, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved));
    g.setColour (kInputColour);
    g.strokePath (input, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));

    for (int i = 0; i < fbk::kMaxTones; ++i)
    {
        const float hz = snapshot_.toneFrequencies[i];
        const float confidence = snapshot_.toneConfidence[i];
        if (hz <= 0.0f || confidence <= 0.02f)
            continue;

        const float x = frequencyToX (hz, plot);
        g.setColour (kToneColour.withAlpha (juce::jlimit (0.15f, 1.0f, confidence)));
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom() - 14.0f);

        if (confidence > 0.4f)
        {
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (juce::String (juce::roundToInt (hz)) + " Hz",
                        juce::Rectangle<float> (x - 28.0f, plot.getY() + 2.0f + (i % 4) * 12.0f,
                                                56.0f, 12.0f),
                        juce::Justification::centred);
        }
    }

    g.setFont (juce::FontOptions (10.0f));
    auto legend = plot.removeFromTop (14.0f).removeFromRight (280.0f);
    const auto entry = [&g, &legend] (juce::Colour c, const juce::String& t)
    {
        auto cell = legend.removeFromLeft (92.0f);
        g.setColour (c);
        g.fillRect (cell.removeFromLeft (10.0f).withSizeKeepingCentre (8.0f, 2.0f));
        g.setColour (kDim);
        g.drawText (t, cell, juce::Justification::centredLeft);
    };
    entry (kInputColour, "input");
    entry (kNoiseColour, "noise floor");
    entry (kGainColour, "mask applied");
}

// ===========================================================================
ProcessPanel::ProcessPanel (FBKSuppressorProcessor& p) : processor_ (p), display_ (p)
{
    addAndMakeVisible (display_);
    addSlider (strength_, fbkparam::strength, "Strength", juce::Slider::RotaryHorizontalVerticalDrag);

    addToggle (fbButton_, fbAttachment_, fbkparam::fbEnabled, "Feedback");
    addSlider (fbSensitivity_, fbkparam::fbSensitivity, "Sensitivity", juce::Slider::LinearHorizontal);
    addSlider (fbDepth_, fbkparam::fbDepth, "Depth", juce::Slider::LinearHorizontal);

    addToggle (nrButton_, nrAttachment_, fbkparam::nrEnabled, "Noise");
    addSlider (nrAmount_, fbkparam::nrAmount, "Amount", juce::Slider::LinearHorizontal);
    addSlider (nrMaxAtten_, fbkparam::nrMaxAtten, "Max cut", juce::Slider::LinearHorizontal);
    addSlider (nrVoiceProtect_, fbkparam::nrVoiceProtect, "Protect", juce::Slider::LinearHorizontal);

    addToggle (humButton_, humAttachment_, fbkparam::humEnabled, "Hum");
    addSlider (humDepth_, fbkparam::humDepth, "Depth", juce::Slider::LinearHorizontal);
    addSlider (humHarmonics_, fbkparam::humHarmonics, "Harmonics", juce::Slider::LinearHorizontal);

    addToggle (drButton_, drAttachment_, fbkparam::drEnabled, "Dereverb");
    addSlider (drAmount_, fbkparam::drAmount, "Amount", juce::Slider::LinearHorizontal);
    addSlider (drRt60_, fbkparam::drRt60, "RT60", juce::Slider::LinearHorizontal);

    addToggle (hpButton_, hpAttachment_, fbkparam::hpEnabled, "Rumble");
    addSlider (hpFreq_, fbkparam::hpFreq, "Freq", juce::Slider::LinearHorizontal);
}

void ProcessPanel::addSlider (BoundSlider& c, const char* paramId,
                              const juce::String& text, juce::Slider::SliderStyle style)
{
    c.slider.setSliderStyle (style);
    styleSlider (c.slider);
    addAndMakeVisible (c.slider);

    c.label.setText (text, juce::dontSendNotification);
    c.label.setFont (juce::FontOptions (12.0f));
    c.label.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (c.label);

    c.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor_.state(), paramId, c.slider);
}

void ProcessPanel::addToggle (juce::ToggleButton& b,
                              std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>& a,
                              const char* paramId, const juce::String& text)
{
    b.setButtonText (text);
    styleButton (b);
    addAndMakeVisible (b);
    a = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor_.state(), paramId, b);
}

void ProcessPanel::resized()
{
    auto area = getLocalBounds().reduced (8);

    display_.setBounds (area.removeFromTop (juce::jmax (140, area.getHeight() / 3)));
    area.removeFromTop (10);

    auto strengthRow = area.removeFromTop (90);
    auto knob = strengthRow.removeFromLeft (120);
    strength_.label.setBounds (knob.removeFromTop (16));
    strength_.slider.setBounds (knob);

    area.removeFromTop (6);

    const auto layoutSection = [] (juce::Rectangle<int> r, juce::ToggleButton& toggle,
                                   std::initializer_list<BoundSlider*> controls)
    {
        toggle.setBounds (r.removeFromTop (22));
        for (auto* c : controls)
        {
            auto row = r.removeFromTop (22);
            c->label.setBounds (row.removeFromLeft (72));
            c->slider.setBounds (row);
            r.removeFromTop (2);
        }
    };

    auto columns = area;
    const int columnWidth = juce::jmax (200, columns.getWidth() / 2 - 8);
    auto left = columns.removeFromLeft (columnWidth);
    columns.removeFromLeft (16);
    auto right = columns;

    layoutSection (left.removeFromTop (94), fbButton_, { &fbSensitivity_, &fbDepth_ });
    left.removeFromTop (8);
    layoutSection (left.removeFromTop (118), nrButton_, { &nrAmount_, &nrMaxAtten_, &nrVoiceProtect_ });

    layoutSection (right.removeFromTop (94), humButton_, { &humDepth_, &humHarmonics_ });
    right.removeFromTop (8);
    layoutSection (right.removeFromTop (94), drButton_, { &drAmount_, &drRt60_ });
    right.removeFromTop (8);
    layoutSection (right.removeFromTop (70), hpButton_, { &hpFreq_ });
}

// ===========================================================================
CalibratePanel::CalibratePanel (FBKSuppressorProcessor& p) : processor_ (p)
{
    instructions_.setText (
        "Calibration measures your voice and your room, so the detector's thresholds "
        "stop being my guesses and start being your numbers. Run each phase once; "
        "nothing changes until you press Apply.",
        juce::dontSendNotification);
    instructions_.setFont (juce::FontOptions (12.0f));
    instructions_.setColour (juce::Label::textColourId, kDim);
    instructions_.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (instructions_);

    const auto setupButton = [this] (juce::TextButton& b, const juce::String& text)
    {
        b.setButtonText (text);
        styleButton (b);
        addAndMakeVisible (b);
    };

    setupButton (noiseButton_, "1. Room noise (15 s)");
    setupButton (voiceButton_, "2. Voice (45 s)");
    setupButton (modesButton_, "3. Room modes (90 s)");
    setupButton (stopButton_, "Finish now");
    setupButton (applyButton_, "Apply profile");
    setupButton (clearButton_, "Revert to defaults");
    setupButton (saveButton_, "Save...");
    setupButton (loadButton_, "Load...");

    noiseButton_.onClick = [this] { startPhase (fbk::CalibrationPhase::roomNoise); };
    voiceButton_.onClick = [this] { startPhase (fbk::CalibrationPhase::voice); };
    modesButton_.onClick = [this] { startPhase (fbk::CalibrationPhase::roomModes); };

    stopButton_.onClick = [this]
    {
        processor_.finishCalibration();
        refreshSummary();
    };

    applyButton_.onClick = [this]
    {
        processor_.applyWorkingProfile();
        statusLabel_.setText (processor_.isProfileApplied()
                                  ? "Profile applied."
                                  : "Nothing to apply - run a phase first.",
                              juce::dontSendNotification);
        refreshSummary();
    };

    clearButton_.onClick = [this]
    {
        processor_.clearAppliedProfile();
        statusLabel_.setText ("Reverted to built-in defaults.", juce::dontSendNotification);
        refreshSummary();
    };

    saveButton_.onClick = [this] { saveProfile(); };
    loadButton_.onClick = [this] { loadProfile(); };

    addAndMakeVisible (progress_);
    styleReadout (summary_);
    addAndMakeVisible (summary_);

    statusLabel_.setFont (juce::FontOptions (11.0f));
    statusLabel_.setColour (juce::Label::textColourId, kGainColour);
    addAndMakeVisible (statusLabel_);

    refreshSummary();
    startTimerHz (5);
}

CalibratePanel::~CalibratePanel() { stopTimer(); }

void CalibratePanel::startPhase (fbk::CalibrationPhase phase)
{
    if (phase == fbk::CalibrationPhase::roomModes)
    {
        auto options = juce::MessageBoxOptions()
                           .withIconType (juce::MessageBoxIconType::WarningIcon)
                           .withTitle ("Room mode profiling")
                           .withMessage (
                               "Cancellation stays fully ON for this. Raise the gain past where "
                               "you would normally run it, slowly, and let the plugin fight the "
                               "feedback - it logs every mode it has to kill.\n\n"
                               "You are protected throughout; that is what makes pushing the gain "
                               "safe. Nothing will be EQ'd or notched, then or later.\n\n"
                               "Bring the gain back down when you are done.")
                           .withButton ("Start")
                           .withButton ("Cancel");

        juce::AlertWindow::showAsync (options, [this, phase] (int result)
        {
            if (result == 1)
            {
                processor_.beginCalibration (phase);
                statusLabel_.setText ("Profiling - raise the gain slowly.", juce::dontSendNotification);
            }
        });
        return;
    }

    processor_.beginCalibration (phase);
    statusLabel_.setText (phase == fbk::CalibrationPhase::roomNoise
                              ? "Measuring the room. Keep everyone quiet, PA at show gain."
                              : "Measuring the voice. Talk and sing across your range, PA down.",
                          juce::dontSendNotification);
}

void CalibratePanel::refreshSummary()
{
    const auto& p = processor_.workingProfile();
    juce::String t;

    if (! p.valid)
    {
        t << "No measurements yet.\n\n"
          << "Phase 1 sets the noise floor and the absolute peak gate in this room's terms.\n"
          << "Phase 2 measures how tone-like your voice looks to the detector, and sets every\n"
          << "         threshold just outside it.\n"
          << "Phase 3 harvests the frequencies your room actually rings at, which then let the\n"
          << "         detector engage faster there.";
        summary_.setText (t, false);
        return;
    }

    t << "PROFILE  (measured at " << juce::String (p.sampleRate / 1000.0, 1) << " kHz)\n";
    t << "applied: " << (processor_.isProfileApplied() ? "yes" : "no") << "\n\n";

    if (p.hasNoise)
    {
        t << "ROOM NOISE\n";
        t << "  broadband floor      " << juce::String (p.broadbandNoiseDbFS, 1) << " dBFS\n";
        t << "  -> peak gate         " << juce::String (p.suggestedAbsoluteFloorDb, 1)
          << " dBFS (default -96.0)\n\n";
    }

    if (p.hasVoice)
    {
        t << "VOICE\n";
        t << "  f0 range             " << juce::String (p.f0LowHz, 0) << " - "
          << juce::String (p.f0HighHz, 0) << " Hz (median "
          << juce::String (p.f0MedianHz, 0) << ")\n";
        t << "  criterion samples    " << p.voiceCriterionSamples << "\n";
        t << "  prominence med/p95   " << juce::String (p.voiceProminenceMedianDb, 1) << " / "
          << juce::String (p.voiceProminenceP95Db, 1) << " dB\n";
        t << "  PNPR p95             " << juce::String (p.voicePnprP95Db, 1) << " dB\n";
        t << "  PHPR p95             " << juce::String (p.voicePhprP95Db, 1) << " dB\n";
        t << "  freq wander p05/med  " << juce::String (p.voiceFsdP05Hz, 2) << " / "
          << juce::String (p.voiceFsdMedianHz, 2) << " Hz\n\n";

        t << "THRESHOLDS  (suggested vs default)\n";
        t << "  prominence gate      " << juce::String (p.suggestedLocalProminenceDb, 1)
          << " dB   vs 12.0\n";
        t << "  PNPR                 " << juce::String (p.suggestedPnprDb, 1) << " dB   vs 10.0\n";
        t << "  PHPR                 " << juce::String (p.suggestedPhprDb, 1) << " dB   vs 8.0\n";
        t << "  freq stability        " << juce::String (p.suggestedFsdMaxHz, 2) << " Hz  vs 2.50\n\n";
    }

    if (p.numModes > 0)
    {
        t << "ROOM MODES  (" << p.numModes << " found, worst first)\n";
        for (int i = 0; i < juce::jmin (p.numModes, 12); ++i)
            t << "  " << juce::String (p.modes[i].freqHz, 1).paddedLeft (' ', 8) << " Hz   "
              << juce::String (p.modes[i].engagedSeconds, 1) << " s engaged, "
              << p.modes[i].hits << " hits\n";
        if (p.numModes > 12)
            t << "  ... and " << (p.numModes - 12) << " more\n";
    }

    summary_.setText (t, false);
}

void CalibratePanel::saveProfile()
{
    chooser_ = std::make_unique<juce::FileChooser> (
        "Save calibration profile",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile ("FBKSuppressor-profile.json"),
        "*.json");

    chooser_->launchAsync (juce::FileBrowserComponent::saveMode
                               | juce::FileBrowserComponent::canSelectFiles
                               | juce::FileBrowserComponent::warnAboutOverwriting,
                           [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        juce::String error;
        statusLabel_.setText (processor_.saveProfile (file, error)
                                  ? "Saved " + file.getFileName()
                                  : error,
                              juce::dontSendNotification);
    });
}

void CalibratePanel::loadProfile()
{
    chooser_ = std::make_unique<juce::FileChooser> (
        "Load calibration profile",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*.json");

    chooser_->launchAsync (juce::FileBrowserComponent::openMode
                               | juce::FileBrowserComponent::canSelectFiles,
                           [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        juce::String error;
        if (processor_.loadProfile (file, error))
            statusLabel_.setText (error.isEmpty() ? "Loaded " + file.getFileName() : error,
                                  juce::dontSendNotification);
        else
            statusLabel_.setText (error, juce::dontSendNotification);

        refreshSummary();
    });
}

void CalibratePanel::timerCallback()
{
    const auto phase = processor_.calibrationPhase();
    const bool running = phase != fbk::CalibrationPhase::idle;

    noiseButton_.setEnabled (! running);
    voiceButton_.setEnabled (! running);
    modesButton_.setEnabled (! running);
    stopButton_.setEnabled (running);

    const auto& p = processor_.workingProfile();
    applyButton_.setEnabled (p.valid && ! running);
    clearButton_.setEnabled (processor_.isProfileApplied());
    saveButton_.setEnabled (p.valid);
    loadButton_.setEnabled (! running);

    if (running)
    {
        const float target = fbk::Calibrator::recommendedSeconds (phase);
        const float elapsed = processor_.calibrationElapsed();
        progress_.setProgress (processor_.calibrationProgress());
        progress_.setText (juce::String (elapsed, 1) + " s of " + juce::String (target, 0) + " s");

        // Stop on its own at the recommended duration; there is nothing to be gained
        // from making the operator watch a clock.
        if (elapsed >= target)
        {
            processor_.finishCalibration();
            statusLabel_.setText ("Phase complete.", juce::dontSendNotification);
            refreshSummary();
        }
    }
    else
    {
        progress_.setProgress (0.0f);
        progress_.setText (p.valid ? "idle - profile present" : "idle");
    }
}

void CalibratePanel::paint (juce::Graphics& g) { g.fillAll (kBackground); }

void CalibratePanel::resized()
{
    auto area = getLocalBounds().reduced (10);

    instructions_.setBounds (area.removeFromTop (48));
    area.removeFromTop (6);

    auto phaseRow = area.removeFromTop (28);
    const int w = phaseRow.getWidth() / 4 - 4;
    noiseButton_.setBounds (phaseRow.removeFromLeft (w));
    phaseRow.removeFromLeft (5);
    voiceButton_.setBounds (phaseRow.removeFromLeft (w));
    phaseRow.removeFromLeft (5);
    modesButton_.setBounds (phaseRow.removeFromLeft (w));
    phaseRow.removeFromLeft (5);
    stopButton_.setBounds (phaseRow);

    area.removeFromTop (6);
    progress_.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    auto actionRow = area.removeFromBottom (28);
    const int aw = actionRow.getWidth() / 4 - 4;
    applyButton_.setBounds (actionRow.removeFromLeft (aw));
    actionRow.removeFromLeft (5);
    clearButton_.setBounds (actionRow.removeFromLeft (aw));
    actionRow.removeFromLeft (5);
    saveButton_.setBounds (actionRow.removeFromLeft (aw));
    actionRow.removeFromLeft (5);
    loadButton_.setBounds (actionRow);

    area.removeFromBottom (4);
    statusLabel_.setBounds (area.removeFromBottom (18));
    area.removeFromBottom (4);

    summary_.setBounds (area);
}

// ===========================================================================
DiagnosticsPanel::DiagnosticsPanel (FBKSuppressorProcessor& p) : processor_ (p)
{
    const auto setupHeader = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        l.setColour (juce::Label::textColourId, kText);
        addAndMakeVisible (l);
    };
    const auto setupButton = [this] (juce::TextButton& b, const juce::String& text)
    {
        b.setButtonText (text);
        styleButton (b);
        addAndMakeVisible (b);
    };
    const auto setupBody = [this] (juce::Label& l, juce::Colour c)
    {
        l.setFont (juce::FontOptions (11.0f));
        l.setColour (juce::Label::textColourId, c);
        addAndMakeVisible (l);
    };

    setupHeader (loggingHeader_, "Logging");

    telemetryButton_.setButtonText ("Telemetry (band energies + every detection criterion, ~25 Hz)");
    styleButton (telemetryButton_);
    addAndMakeVisible (telemetryButton_);
    telemetryAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor_.state(), fbkparam::telemetry, telemetryButton_);

    captureButton_.setButtonText ("Keep a rolling 12 s of audio, and save it automatically on an event");
    styleButton (captureButton_);
    captureButton_.setToggleState (processor_.isCaptureEnabled(), juce::dontSendNotification);
    captureButton_.onClick = [this] { processor_.setCaptureEnabled (captureButton_.getToggleState()); };
    addAndMakeVisible (captureButton_);

    eventThreshold_.slider.setSliderStyle (juce::Slider::LinearHorizontal);
    styleSlider (eventThreshold_.slider);
    addAndMakeVisible (eventThreshold_.slider);
    eventThreshold_.label.setText ("Event at", juce::dontSendNotification);
    eventThreshold_.label.setFont (juce::FontOptions (12.0f));
    eventThreshold_.label.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (eventThreshold_.label);
    eventThreshold_.attachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor_.state(), fbkparam::eventThreshold, eventThreshold_.slider);

    setupButton (folderButton_, "Choose folder...");
    setupButton (saveCaptureButton_, "Save last 12 s now");
    saveCaptureButton_.onClick = [this]
    {
        auto folder = processor_.diagnosticsFolder();
        if (folder == juce::File{})
            folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
        const auto file = folder.getChildFile ("fbk-manual-" + stamp + ".wav");

        juce::String error;
        loggingStatus_.setText (processor_.writeCapture (file, error)
                                    ? "Saved " + file.getFileName()
                                    : error,
                                juce::dontSendNotification);
    };

    folderButton_.onClick = [this] { chooseFolder(); };

    setupBody (folderLabel_, kDim);
    setupBody (loggingStatus_, kGainColour);

    setupHeader (sweepHeader_, "Feedback path measurement");

    sweepLevelSlider_.setSliderStyle (juce::Slider::LinearHorizontal);
    sweepLevelSlider_.setRange (-40.0, -6.0, 0.5);
    sweepLevelSlider_.setValue (-26.0, juce::dontSendNotification);
    sweepLevelSlider_.setTextValueSuffix (" dBFS");
    styleSlider (sweepLevelSlider_);
    addAndMakeVisible (sweepLevelSlider_);

    sweepLevelLabel_.setText ("Sweep level", juce::dontSendNotification);
    sweepLevelLabel_.setFont (juce::FontOptions (12.0f));
    sweepLevelLabel_.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (sweepLevelLabel_);

    setupButton (sweepButton_, "Measure (emits a sweep to the PA)");
    setupButton (sweepAbortButton_, "Stop");
    setupButton (exportIrButton_, "Export impulse response...");

    sweepButton_.setColour (juce::TextButton::buttonColourId, kWarnColour.withAlpha (0.35f));
    sweepButton_.onClick = [this] { confirmAndStartSweep(); };
    sweepAbortButton_.onClick = [this] { processor_.abortSweep(); };
    exportIrButton_.onClick = [this] { exportImpulse(); };

    addAndMakeVisible (sweepProgress_);
    setupBody (sweepSummary_, kDim);

    startTimerHz (5);
}

DiagnosticsPanel::~DiagnosticsPanel() { stopTimer(); }

void DiagnosticsPanel::chooseFolder()
{
    chooser_ = std::make_unique<juce::FileChooser> (
        "Choose a folder for telemetry and event captures",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));

    chooser_->launchAsync (juce::FileBrowserComponent::openMode
                               | juce::FileBrowserComponent::canSelectDirectories,
                           [this] (const juce::FileChooser& fc)
    {
        const auto folder = fc.getResult();
        if (folder == juce::File{})
            return;
        processor_.setDiagnosticsFolder (folder);
        folderLabel_.setText (folder.getFullPathName(), juce::dontSendNotification);
    });
}

void DiagnosticsPanel::confirmAndStartSweep()
{
    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::WarningIcon)
                       .withTitle ("Emit a measurement sweep?")
                       .withMessage (
                           "This sends a full-band sine sweep out of this channel and into the PA "
                           "for about 4.5 seconds, then records what the microphone hears.\n\n"
                           "Normal processing stops while it runs. Do not do this with an audience "
                           "in the room, and check the level first - start low.\n\n"
                           "The result is the impulse response of the whole loop, which is what "
                           "makes realistic feedback simulation possible later.")
                       .withButton ("Emit sweep")
                       .withButton ("Cancel");

    juce::AlertWindow::showAsync (options, [this] (int result)
    {
        if (result == 1)
            processor_.startSweep (static_cast<float> (sweepLevelSlider_.getValue()));
    });
}

void DiagnosticsPanel::exportImpulse()
{
    chooser_ = std::make_unique<juce::FileChooser> (
        "Export impulse response",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile ("FBKSuppressor-feedback-path.wav"),
        "*.wav");

    chooser_->launchAsync (juce::FileBrowserComponent::saveMode
                               | juce::FileBrowserComponent::canSelectFiles
                               | juce::FileBrowserComponent::warnAboutOverwriting,
                           [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        juce::String error;
        sweepSummary_.setText (processor_.exportImpulseResponse (file, error)
                                   ? "Exported " + file.getFileName()
                                   : error,
                               juce::dontSendNotification);
    });
}

void DiagnosticsPanel::timerCallback()
{
    auto& writer = processor_.diagnostics();
    const auto folder = processor_.diagnosticsFolder();

    folderLabel_.setText (folder == juce::File{}
                              ? "No folder chosen - nothing is being written to disk."
                              : folder.getFullPathName(),
                          juce::dontSendNotification);

    juce::String status;
    if (folder != juce::File{})
    {
        status << writer.framesWritten() << " telemetry rows, "
               << writer.eventsWritten() << " event captures";
        if (auto* ch = processor_.primaryChannel())
            if (ch->telemetry().droppedFrames() > 0)
                status << "   (" << ch->telemetry().droppedFrames() << " frames dropped)";
        const auto message = writer.lastMessage();
        if (message.isNotEmpty())
            status << "   -   " << message;
    }
    loggingStatus_.setText (status, juce::dontSendNotification);

    const bool sweeping = processor_.isSweeping();
    sweepButton_.setEnabled (! sweeping);
    sweepAbortButton_.setEnabled (sweeping);
    exportIrButton_.setEnabled (! sweeping);
    sweepLevelSlider_.setEnabled (! sweeping);

    sweepProgress_.setProgress (processor_.sweepProgress());
    sweepProgress_.setText (sweeping
                                ? "emitting sweep - " + juce::String (juce::roundToInt (
                                      processor_.sweepProgress() * 100.0f)) + "%"
                                : "idle");
    sweepSummary_.setText (processor_.sweepSummary(), juce::dontSendNotification);
}

void DiagnosticsPanel::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    // Separator between the two halves, so the sweep controls read as a distinct
    // and more consequential thing than the logging switches above them.
    g.setColour (kGrid);
    g.fillRect (10, separatorY_, getWidth() - 20, 1);
}

void DiagnosticsPanel::resized()
{
    auto area = getLocalBounds().reduced (10);

    loggingHeader_.setBounds (area.removeFromTop (22));
    area.removeFromTop (4);
    telemetryButton_.setBounds (area.removeFromTop (24));
    captureButton_.setBounds (area.removeFromTop (24));

    auto thresholdRow = area.removeFromTop (24);
    eventThreshold_.label.setBounds (thresholdRow.removeFromLeft (72));
    eventThreshold_.slider.setBounds (thresholdRow.removeFromLeft (
        juce::jmin (320, thresholdRow.getWidth())));

    area.removeFromTop (6);
    auto folderRow = area.removeFromTop (26);
    folderButton_.setBounds (folderRow.removeFromLeft (130));
    folderRow.removeFromLeft (6);
    saveCaptureButton_.setBounds (folderRow.removeFromLeft (150));
    area.removeFromTop (2);
    folderLabel_.setBounds (area.removeFromTop (16));
    loggingStatus_.setBounds (area.removeFromTop (16));

    area.removeFromTop (12);
    separatorY_ = area.getY();
    area.removeFromTop (10);

    sweepHeader_.setBounds (area.removeFromTop (22));
    area.removeFromTop (4);

    auto levelRow = area.removeFromTop (24);
    sweepLevelLabel_.setBounds (levelRow.removeFromLeft (80));
    sweepLevelSlider_.setBounds (levelRow.removeFromLeft (juce::jmin (320, levelRow.getWidth())));

    area.removeFromTop (6);
    auto sweepRow = area.removeFromTop (26);
    sweepButton_.setBounds (sweepRow.removeFromLeft (230));
    sweepRow.removeFromLeft (6);
    sweepAbortButton_.setBounds (sweepRow.removeFromLeft (70));
    sweepRow.removeFromLeft (6);
    exportIrButton_.setBounds (sweepRow.removeFromLeft (190));

    area.removeFromTop (6);
    sweepProgress_.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);
    sweepSummary_.setBounds (area.removeFromTop (18));
}

// ===========================================================================
FBKSuppressorEditor::FBKSuppressorEditor (FBKSuppressorProcessor& p)
    : AudioProcessorEditor (&p), processor_ (p)
{
    titleLabel_.setText ("FBKSuppressor", juce::dontSendNotification);
    titleLabel_.setFont (juce::FontOptions (20.0f));
    titleLabel_.setColour (juce::Label::textColourId, kText);
    addAndMakeVisible (titleLabel_);

    latencyLabel_.setFont (juce::FontOptions (12.0f));
    latencyLabel_.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (latencyLabel_);

    statusLabel_.setFont (juce::FontOptions (11.0f));
    statusLabel_.setColour (juce::Label::textColourId, kDim);
    statusLabel_.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLabel_);

    bypassButton_.setButtonText ("Bypass");
    styleButton (bypassButton_);
    addAndMakeVisible (bypassButton_);
    bypassAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor_.state(), fbkparam::bypass, bypassButton_);

    qualityButton_.setButtonText ("Quality mode (+latency)");
    styleButton (qualityButton_);
    addAndMakeVisible (qualityButton_);
    qualityAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor_.state(), fbkparam::qualityMode, qualityButton_);

    tabs_.setOutline (0);
    tabs_.setTabBarDepth (26);
    tabs_.setColour (juce::TabbedComponent::backgroundColourId, kBackground);
    tabs_.addTab ("Process", kBackground, new ProcessPanel (processor_), true);
    tabs_.addTab ("Calibrate", kBackground, new CalibratePanel (processor_), true);
    tabs_.addTab ("Diagnostics", kBackground, new DiagnosticsPanel (processor_), true);
    addAndMakeVisible (tabs_);

    setResizable (true, true);
    setResizeLimits (760, 560, 1600, 1100);
    setSize (900, 680);

    startTimerHz (10);
}

FBKSuppressorEditor::~FBKSuppressorEditor() { stopTimer(); }

void FBKSuppressorEditor::timerCallback()
{
    const int latency = processor_.getLatencySamples();
    const double sr = processor_.currentSampleRate();
    const double ms = sr > 0.0 ? 1000.0 * latency / sr : 0.0;

    latencyLabel_.setText (latency == 0
                               ? "Latency: 0 samples (0.00 ms)"
                               : "Latency: " + juce::String (latency) + " samples ("
                                     + juce::String (ms, 2) + " ms)",
                           juce::dontSendNotification);
    latencyLabel_.setColour (juce::Label::textColourId, latency == 0 ? kGainColour : kToneColour);

    juce::String status;
    status << juce::String (sr / 1000.0, 1) << " kHz   "
           << processor_.numActiveChannels() << " ch   CPU "
           << juce::String (processor_.cpuLoad() * 100.0f, 1) << "%";

    if (processor_.isProfileApplied())
        status << "   profile";

    if (auto* m = processor_.metering())
    {
        if (m->confirmedTones > 0)
            status << "   tones " << m->confirmedTones;
        if (m->humActive)
            status << "   hum " << juce::String (m->humFundamentalHz, 0) << " Hz";
    }

    if (processor_.isSweeping())
        status << "   SWEEPING";

    statusLabel_.setText (status, juce::dontSendNotification);
}

void FBKSuppressorEditor::paint (juce::Graphics& g) { g.fillAll (kBackground); }

void FBKSuppressorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto header = area.removeFromTop (44);
    titleLabel_.setBounds (header.removeFromLeft (190));
    auto headerRight = header.removeFromRight (320);
    latencyLabel_.setBounds (headerRight.removeFromTop (22));
    statusLabel_.setBounds (headerRight);
    auto headerMid = header.reduced (8, 10);
    bypassButton_.setBounds (headerMid.removeFromLeft (86));
    qualityButton_.setBounds (headerMid.removeFromLeft (186));

    area.removeFromTop (4);
    tabs_.setBounds (area);
}
