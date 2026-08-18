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
} // namespace

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

        // Ballistics on the display only. The processing itself is untouched by
        // this; it exists so the picture is readable rather than flickering.
        for (int b = 0; b < fbk::kNumBands; ++b)
        {
            const float in = snapshot_.bandInputDb[b];
            const float g  = snapshot_.bandGainDb[b];
            if (! primed_)
            {
                smoothedInput_[b] = in;
                smoothedGain_[b]  = g;
            }
            else
            {
                smoothedInput_[b] += (in > smoothedInput_[b] ? 0.6f : 0.2f) * (in - smoothedInput_[b]);
                smoothedGain_[b]  += 0.35f * (g - smoothedGain_[b]);
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
    auto bounds = getLocalBounds().toFloat();
    g.fillAll (kPanel);

    auto plot = bounds.reduced (8.0f, 6.0f);

    // --- grid ---------------------------------------------------------------
    g.setColour (kGrid);
    g.setFont (juce::FontOptions (10.0f));
    for (float hz : { 50.0f, 100.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = frequencyToX (hz, plot);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (kDim);
        const juce::String text = hz >= 1000.0f ? juce::String (hz / 1000.0f, 0) + "k"
                                                : juce::String (hz, 0);
        g.drawText (text, juce::Rectangle<float> (x - 16.0f, plot.getBottom() - 12.0f, 32.0f, 12.0f),
                    juce::Justification::centred);
        g.setColour (kGrid);
    }

    const auto dbToY = [&plot] (float db)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb));
        return plot.getBottom() - t * plot.getHeight();
    };

    if (! primed_)
    {
        g.setColour (kDim);
        g.drawText ("waiting for audio", plot, juce::Justification::centred);
        return;
    }

    const auto bandX = [this, &plot] (int b)
    {
        // Band centres are only available through the metering snapshot indirectly,
        // so lay the bands out on the same ERB progression the DSP uses.
        const float t = (static_cast<float> (b) + 0.5f) / static_cast<float> (fbk::kNumBands);
        return plot.getX() + t * plot.getWidth();
    };

    // --- input spectrum -----------------------------------------------------
    juce::Path input, noise;
    for (int b = 0; b < fbk::kNumBands; ++b)
    {
        const float x = bandX (b);
        const float yIn = dbToY (smoothedInput_[b]);
        const float yNo = dbToY (snapshot_.bandNoiseDb[b]);
        if (b == 0) { input.startNewSubPath (x, yIn); noise.startNewSubPath (x, yNo); }
        else        { input.lineTo (x, yIn);          noise.lineTo (x, yNo); }
    }

    g.setColour (kNoiseColour.withAlpha (0.8f));
    g.strokePath (noise, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
    g.setColour (kInputColour);
    g.strokePath (input, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));

    // --- applied mask, on its own scale down the right-hand side -------------
    // 0 dB at the top of the plot, -24 dB at the bottom.
    juce::Path gain;
    for (int b = 0; b < fbk::kNumBands; ++b)
    {
        const float x = bandX (b);
        const float t = juce::jlimit (0.0f, 1.0f, (smoothedGain_[b] + 24.0f) / 24.0f);
        const float y = plot.getBottom() - t * plot.getHeight();
        if (b == 0) gain.startNewSubPath (x, y);
        else        gain.lineTo (x, y);
    }
    g.setColour (kGainColour.withAlpha (0.9f));
    g.strokePath (gain, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved));

    // --- cancelled tones ----------------------------------------------------
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

    // --- legend -------------------------------------------------------------
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
    entry (kGainColour,  "mask applied");
}

// ===========================================================================
FBKSuppressorEditor::FBKSuppressorEditor (FBKSuppressorProcessor& p)
    : AudioProcessorEditor (&p), processor_ (p), display_ (p)
{
    auto styleLabel = [] (juce::Label& l, float size, juce::Colour colour,
                          juce::Justification j = juce::Justification::centredLeft)
    {
        l.setFont (juce::FontOptions (size));
        l.setColour (juce::Label::textColourId, colour);
        l.setJustificationType (j);
    };

    titleLabel_.setText ("FBKSuppressor", juce::dontSendNotification);
    styleLabel (titleLabel_, 20.0f, kText);
    addAndMakeVisible (titleLabel_);

    styleLabel (latencyLabel_, 12.0f, kGainColour, juce::Justification::centredRight);
    addAndMakeVisible (latencyLabel_);

    styleLabel (statusLabel_, 11.0f, kDim, juce::Justification::centredRight);
    addAndMakeVisible (statusLabel_);

    addToggle (bypassButton_,  bypassAttachment_,  fbkparam::bypass,      "Bypass");
    addToggle (qualityButton_, qualityAttachment_, fbkparam::qualityMode, "Quality mode (+latency)");

    addControl (strength_, fbkparam::strength, "Strength", juce::Slider::RotaryHorizontalVerticalDrag);

    addToggle (fbButton_,  fbAttachment_,  fbkparam::fbEnabled,  "Feedback");
    addControl (fbSensitivity_, fbkparam::fbSensitivity, "Sensitivity", juce::Slider::LinearHorizontal);
    addControl (fbDepth_,       fbkparam::fbDepth,       "Depth",       juce::Slider::LinearHorizontal);

    addToggle (nrButton_, nrAttachment_, fbkparam::nrEnabled, "Noise");
    addControl (nrAmount_,       fbkparam::nrAmount,       "Amount",     juce::Slider::LinearHorizontal);
    addControl (nrMaxAtten_,     fbkparam::nrMaxAtten,     "Max cut",    juce::Slider::LinearHorizontal);
    addControl (nrVoiceProtect_, fbkparam::nrVoiceProtect, "Protect",    juce::Slider::LinearHorizontal);

    addToggle (humButton_, humAttachment_, fbkparam::humEnabled, "Hum");
    addControl (humDepth_,     fbkparam::humDepth,     "Depth",     juce::Slider::LinearHorizontal);
    addControl (humHarmonics_, fbkparam::humHarmonics, "Harmonics", juce::Slider::LinearHorizontal);

    addToggle (drButton_, drAttachment_, fbkparam::drEnabled, "Dereverb");
    addControl (drAmount_, fbkparam::drAmount, "Amount", juce::Slider::LinearHorizontal);
    addControl (drRt60_,   fbkparam::drRt60,   "RT60",   juce::Slider::LinearHorizontal);

    addToggle (hpButton_, hpAttachment_, fbkparam::hpEnabled, "Rumble");
    addControl (hpFreq_, fbkparam::hpFreq, "Freq", juce::Slider::LinearHorizontal);

    captureButton_.setButtonText ("Capture last 12 s");
    captureButton_.setColour (juce::ToggleButton::textColourId, kText);
    captureButton_.setToggleState (processor_.isCaptureEnabled(), juce::dontSendNotification);
    captureButton_.onClick = [this]
    {
        processor_.setCaptureEnabled (captureButton_.getToggleState());
    };
    addAndMakeVisible (captureButton_);

    saveCaptureButton_.setButtonText ("Save WAV...");
    saveCaptureButton_.onClick = [this] { saveCapture(); };
    addAndMakeVisible (saveCaptureButton_);

    addAndMakeVisible (display_);

    setResizable (true, true);
    setResizeLimits (700, 520, 1600, 1100);
    setSize (860, 620);

    startTimerHz (10);
}

FBKSuppressorEditor::~FBKSuppressorEditor() { stopTimer(); }

void FBKSuppressorEditor::addControl (Control& c, const char* paramId,
                                      const juce::String& text,
                                      juce::Slider::SliderStyle style)
{
    c.slider.setSliderStyle (style);
    c.slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 18);
    c.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    c.slider.setColour (juce::Slider::textBoxTextColourId, kText);
    c.slider.setColour (juce::Slider::thumbColourId, kGainColour);
    c.slider.setColour (juce::Slider::rotarySliderFillColourId, kGainColour);
    c.slider.setColour (juce::Slider::trackColourId, kGainColour.withAlpha (0.55f));
    addAndMakeVisible (c.slider);

    c.label.setText (text, juce::dontSendNotification);
    c.label.setFont (juce::FontOptions (12.0f));
    c.label.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (c.label);

    c.attachment = std::make_unique<SliderAttachment> (processor_.state(), paramId, c.slider);
}

void FBKSuppressorEditor::addToggle (juce::ToggleButton& b,
                                     std::unique_ptr<ButtonAttachment>& a,
                                     const char* paramId, const juce::String& text)
{
    b.setButtonText (text);
    b.setColour (juce::ToggleButton::textColourId, kText);
    b.setColour (juce::ToggleButton::tickColourId, kGainColour);
    addAndMakeVisible (b);
    a = std::make_unique<ButtonAttachment> (processor_.state(), paramId, b);
}

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

    if (auto* m = processor_.metering())
    {
        if (m->confirmedTones > 0)
            status << "   tones " << m->confirmedTones;
        if (m->humActive)
            status << "   hum " << juce::String (m->humFundamentalHz, 0) << " Hz";
    }

    statusLabel_.setText (status, juce::dontSendNotification);
}

void FBKSuppressorEditor::saveCapture()
{
    chooser_ = std::make_unique<juce::FileChooser> (
        "Save the last 12 seconds (channel 1 = input, channel 2 = processed)",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile ("FBKSuppressor-capture.wav"),
        "*.wav");

    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting;

    chooser_->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File{})
            return;

        juce::String error;
        if (! processor_.writeCapture (file, error))
        {
            auto options = juce::MessageBoxOptions()
                               .withIconType (juce::MessageBoxIconType::WarningIcon)
                               .withTitle ("Capture failed")
                               .withMessage (error)
                               .withButton ("OK");
            juce::AlertWindow::showAsync (options, nullptr);
        }
    });
}

void FBKSuppressorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
}

void FBKSuppressorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    // --- header -------------------------------------------------------------
    auto header = area.removeFromTop (44);
    titleLabel_.setBounds (header.removeFromLeft (200));
    auto headerRight = header.removeFromRight (320);
    latencyLabel_.setBounds (headerRight.removeFromTop (22));
    statusLabel_.setBounds (headerRight);
    auto headerMid = header.reduced (8, 10);
    bypassButton_.setBounds (headerMid.removeFromLeft (90));
    qualityButton_.setBounds (headerMid.removeFromLeft (190));

    area.removeFromTop (6);

    // --- analyser -----------------------------------------------------------
    display_.setBounds (area.removeFromTop (juce::jmax (150, area.getHeight() / 3)));
    area.removeFromTop (10);

    // --- strength -----------------------------------------------------------
    auto strengthRow = area.removeFromTop (96);
    auto knobArea = strengthRow.removeFromLeft (120);
    strength_.label.setBounds (knobArea.removeFromTop (16));
    strength_.slider.setBounds (knobArea);

    auto captureArea = strengthRow.removeFromRight (180);
    captureArea = captureArea.withSizeKeepingCentre (180, 60);
    captureButton_.setBounds (captureArea.removeFromTop (28));
    captureArea.removeFromTop (4);
    saveCaptureButton_.setBounds (captureArea.removeFromTop (26));

    area.removeFromTop (6);

    // --- stage sections -----------------------------------------------------
    const auto layoutSection = [] (juce::Rectangle<int> r, juce::ToggleButton& toggle,
                                   std::initializer_list<Control*> controls)
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
    const int columnWidth = columns.getWidth() / 2 - 8;

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
