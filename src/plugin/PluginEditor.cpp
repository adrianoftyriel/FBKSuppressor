#include "PluginEditor.h"

namespace
{
constexpr float kMinHz = 40.0f;
constexpr float kMaxHz = 20000.0f;
constexpr float kMinDb = -90.0f;
constexpr float kMaxDb = -6.0f;

// Console greys over a carbon chassis, with one colour per module so a strip can
// be identified from across a stage without reading anything.
const juce::Colour kBackground   { 0xff282c31 };   // carbon base
const juce::Colour kPanel        { 0xff141619 };   // analyser well
const juce::Colour kPanelTop     { 0xff3c4149 };
const juce::Colour kPanelBottom  { 0xff2a2e34 };
const juce::Colour kPanelEdge    { 0xff454b54 };
const juce::Colour kGrid         { 0xff3a4049 };
const juce::Colour kText         { 0xffeef1f5 };
const juce::Colour kDim          { 0xff9aa3af };
const juce::Colour kSlot         { 0xff111316 };
const juce::Colour kInputColour  { 0xff63b3ff };
const juce::Colour kNoiseColour  { 0xff8d959f };
const juce::Colour kGainColour   { 0xff5ad48f };
const juce::Colour kToneColour   { 0xffff5a4d };
const juce::Colour kWarnColour   { 0xffffb02e };

const juce::Colour kFeedbackHue  { 0xffff5a4d };
const juce::Colour kNoiseHue     { 0xff63b3ff };
const juce::Colour kHumHue       { 0xffffb02e };
const juce::Colour kDereverbHue  { 0xffb48cff };
const juce::Colour kRumbleHue    { 0xff5ad48f };
const juce::Colour kStrengthHue  { 0xffffb02e };

juce::Font uiFont (float height, bool bold = false)
{
    return juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain);
}

juce::Font monoFont (float height)
{
    return juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain);
}

// Small caps with a little air between the letters. JUCE has no letter spacing, so
// the text is drawn a character at a time; it is only ever used on short labels.
float spacedTextWidth (const juce::Font& font, const juce::String& text, float spacing)
{
    float width = 0.0f;
    for (int i = 0; i < text.length(); ++i)
        width += juce::GlyphArrangement::getStringWidth (font, text.substring (i, i + 1)) + spacing;
    return juce::jmax (0.0f, width - spacing);
}

void drawSpacedText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                     float spacing, juce::Justification justification)
{
    const auto font = g.getCurrentFont();
    const float width = spacedTextWidth (font, text, spacing);

    float x = area.getX();
    if (justification.testFlags (juce::Justification::horizontallyCentred))
        x = area.getCentreX() - width * 0.5f;
    else if (justification.testFlags (juce::Justification::right))
        x = area.getRight() - width;

    for (int i = 0; i < text.length(); ++i)
    {
        const auto ch = text.substring (i, i + 1);
        g.drawText (ch, juce::Rectangle<float> (x, area.getY(), 40.0f, area.getHeight()),
                    juce::Justification::centredLeft, false);
        x += juce::GlyphArrangement::getStringWidth (font, ch) + spacing;
    }
}

void styleSlider (juce::Slider& s)
{
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 18);
    s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::textBoxTextColourId, kText);
    s.setColour (juce::Slider::thumbColourId, kWarnColour);
    s.setColour (juce::Slider::rotarySliderFillColourId, kWarnColour);
    s.setColour (juce::Slider::trackColourId, kWarnColour.withAlpha (0.55f));
}

void styleButton (juce::Button& b)
{
    b.setColour (juce::TextButton::buttonColourId, kPanelTop);
    b.setColour (juce::TextButton::textColourOffId, kText);
    b.setColour (juce::ToggleButton::textColourId, kText);
    b.setColour (juce::ToggleButton::tickColourId, kWarnColour);
}

void styleReadout (juce::TextEditor& e)
{
    e.setMultiLine (true);
    e.setReadOnly (true);
    e.setScrollbarsShown (true);
    e.setCaretVisible (false);
    e.setFont (monoFont (12.0f));
    e.setColour (juce::TextEditor::backgroundColourId, kPanel);
    e.setColour (juce::TextEditor::textColourId, kText);
    e.setColour (juce::TextEditor::outlineColourId, kPanelEdge);
}
} // namespace

// ===========================================================================
void CarbonBackground::paint (juce::Graphics& g, juce::Rectangle<int> area)
{
    if (! tile_.isValid())
    {
        // A 2x2 arrangement of 10px tows, each running against its neighbours:
        // that alternation is what makes a weave read as woven rather than
        // hatched.
        tile_ = juce::Image (juce::Image::ARGB, 20, 20, false);
        juce::Graphics tg (tile_);
        tg.fillAll (kBackground);

        const auto tow = [&tg] (int x, int y, bool rising)
        {
            const juce::Graphics::ScopedSaveState save (tg);
            tg.reduceClipRegion (juce::Rectangle<int> (x, y, 10, 10));
            tg.setColour (juce::Colour (rising ? 0xff2e333a : 0xff23272c));
            tg.fillRect (x, y, 10, 10);

            for (float i = -10.0f; i < 12.0f; i += 3.0f)
            {
                const float x0 = static_cast<float> (x) + i;
                const float yTop = static_cast<float> (y);
                const float yBot = yTop + 10.0f;

                tg.setColour (juce::Colours::white.withAlpha (0.055f));
                if (rising) tg.drawLine (x0, yBot, x0 + 10.0f, yTop, 1.0f);
                else        tg.drawLine (x0, yTop, x0 + 10.0f, yBot, 1.0f);

                tg.setColour (juce::Colours::black.withAlpha (0.14f));
                if (rising) tg.drawLine (x0 + 1.5f, yBot, x0 + 11.5f, yTop, 1.0f);
                else        tg.drawLine (x0 + 1.5f, yTop, x0 + 11.5f, yBot, 1.0f);
            }
        };

        tow (0, 0, true);   tow (10, 0, false);
        tow (0, 10, false); tow (10, 10, true);
    }

    g.setTiledImageFill (tile_, 0, 0, 1.0f);
    g.fillRect (area);

    // The chassis catches the light from above, like a rack panel does.
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.05f),
                                             area.toFloat().getCentreX(), area.toFloat().getY(),
                                             juce::Colours::transparentWhite,
                                             area.toFloat().getCentreX(),
                                             area.toFloat().getY() + area.getHeight() * 0.45f,
                                             false));
    g.fillRect (area);
}

// ===========================================================================
FbkLookAndFeel::FbkLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, kBackground);
    setColour (juce::TabbedComponent::backgroundColourId, kBackground);
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, kText);
    setColour (juce::TextButton::buttonColourId, kPanelTop);
    setColour (juce::TextButton::textColourOffId, kText);
    setColour (juce::TextButton::textColourOnId, kText);
    setColour (juce::ToggleButton::textColourId, kText);
    setColour (juce::ToggleButton::tickColourId, kWarnColour);
    setColour (juce::AlertWindow::backgroundColourId, kPanelBottom);
    setColour (juce::AlertWindow::textColourId, kText);
    setColour (juce::PopupMenu::backgroundColourId, kPanelBottom);
    setColour (juce::PopupMenu::textColourId, kText);
    setColour (juce::ScrollBar::thumbColourId, kPanelEdge);
}

juce::Range<float> FbkLookAndFeel::travel (juce::Rectangle<float> area, bool large) noexcept
{
    const float half = capHeight (large) * 0.5f;
    return { area.getY() + half, area.getBottom() - half };
}

void FbkLookAndFeel::drawSlot (juce::Graphics& g, juce::Rectangle<float> slot)
{
    const float r = slot.getWidth() * 0.5f;

    g.setColour (kSlot);
    g.fillRoundedRectangle (slot, r);

    // A groove is dark at the top edge and catches light at the bottom lip.
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.85f),
                                             slot.getCentreX(), slot.getY(),
                                             juce::Colours::transparentBlack,
                                             slot.getCentreX(), slot.getY() + 8.0f, false));
    g.fillRoundedRectangle (slot, r);

    g.setColour (juce::Colour (0xff1d2126).withAlpha (0.9f));
    g.drawRoundedRectangle (slot.reduced (0.5f), r, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.09f));
    g.drawLine (slot.getX(), slot.getBottom() + 0.5f, slot.getRight(), slot.getBottom() + 0.5f, 1.0f);
}

void FbkLookAndFeel::drawFaderCap (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float radius = 4.0f;

    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (r.translated (0.0f, 2.5f).expanded (1.0f, 0.0f), radius);

    // Brushed aluminium: bright at the top, a dark machined band across the
    // middle where the indicator sits, bright again below.
    juce::ColourGradient grad (juce::Colour (0xffc9cfd8), r.getCentreX(), r.getY(),
                               juce::Colour (0xffa8b0bb), r.getCentreX(), r.getBottom(), false);
    grad.addColour (0.42, juce::Colour (0xff8f97a3));
    grad.addColour (0.47, juce::Colour (0xff4a515c));
    grad.addColour (0.52, juce::Colour (0xff2c3138));
    grad.addColour (0.58, juce::Colour (0xff6d7581));
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r, radius);

    g.setColour (juce::Colours::black.withAlpha (0.13f));
    for (float y = r.getY() + 2.0f; y < r.getBottom() - 1.0f; y += 3.0f)
        g.fillRect (r.getX() + 1.5f, y, r.getWidth() - 3.0f, 1.0f);

    const auto line = juce::Rectangle<float> (r.getX() + 3.0f, r.getCentreY() - 1.0f,
                                              r.getWidth() - 6.0f, 2.0f);
    g.setColour (juce::Colour (0xff0e1116));
    g.fillRect (line);
    g.setColour (juce::Colours::white.withAlpha (0.32f));
    g.fillRect (line.withY (line.getBottom()).withHeight (1.0f));

    g.setColour (juce::Colours::white.withAlpha (0.22f));
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
}

void FbkLookAndFeel::drawPanel (juce::Graphics& g, juce::Rectangle<float> r,
                                juce::Colour topRail, bool dimmed)
{
    const float radius = 6.0f;
    const float alpha = dimmed ? 0.72f : 1.0f;

    g.setColour (juce::Colours::black.withAlpha (0.35f * alpha));
    g.fillRoundedRectangle (r.translated (0.0f, 2.0f), radius);

    g.setGradientFill (juce::ColourGradient (kPanelTop.withMultipliedAlpha (alpha),
                                             r.getCentreX(), r.getY(),
                                             kPanelBottom.withMultipliedAlpha (alpha),
                                             r.getCentreX(), r.getBottom(), false));
    g.fillRoundedRectangle (r, radius);

    if (! topRail.isTransparent())
    {
        // The anodised rail along the top edge is the strip's identity: colour
        // first, text second.
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (r.withHeight (4.0f).getSmallestIntegerContainer());
        g.setColour (dimmed ? topRail.withMultipliedSaturation (0.35f).withAlpha (0.55f) : topRail);
        g.fillRoundedRectangle (r.withHeight (10.0f), radius);
    }

    g.setColour (juce::Colours::white.withAlpha (0.10f * alpha));
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    g.setColour (kPanelEdge.withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
}

void FbkLookAndFeel::drawLed (juce::Graphics& g, juce::Rectangle<float> r,
                              juce::Colour colour, bool lit)
{
    if (lit)
    {
        g.setColour (colour.withAlpha (0.35f));
        g.fillEllipse (r.expanded (3.0f));
        g.setColour (colour);
        g.fillEllipse (r);
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.fillEllipse (r.reduced (r.getWidth() * 0.32f).translated (0.0f, -0.5f));
    }
    else
    {
        g.setColour (juce::Colour (0xff3b4046));
        g.fillEllipse (r);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawEllipse (r.reduced (0.5f), 1.0f);
    }
}

void FbkLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPos, float minSliderPos, float maxSliderPos,
                                       juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                          minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const bool large = static_cast<bool> (slider.getProperties()
                                              .getWithDefault (largeFaderProperty, false));
    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
    const float capH = capHeight (large);
    const float capW = large ? 62.0f : 34.0f;
    const float slotW = large ? 13.0f : 9.0f;

    const auto range = travel (area, large);
    const float cx = area.getCentreX();

    const auto slot = juce::Rectangle<float> (cx - slotW * 0.5f, range.getStart(),
                                              slotW, range.getLength());
    drawSlot (g, slot);

    const float proportion = height > 0
        ? juce::jlimit (0.0f, 1.0f, (area.getBottom() - sliderPos) / static_cast<float> (height))
        : 0.0f;
    const float capY = range.getEnd() - proportion * range.getLength();

    // Everything below the cap is lit: the illuminated travel is what makes the
    // setting readable at a glance rather than something you have to squint at.
    const auto accent = slider.findColour (juce::Slider::trackColourId);
    if (capY < slot.getBottom() - 1.0f)
    {
        const auto fill = slot.withTop (juce::jmax (slot.getY(), capY));
        g.setColour (accent.withAlpha (0.25f));
        g.fillRoundedRectangle (fill.expanded (2.5f, 0.0f), slotW * 0.5f + 2.5f);
        g.setGradientFill (juce::ColourGradient (accent, fill.getCentreX(), fill.getBottom(),
                                                 accent.brighter (0.35f), fill.getCentreX(),
                                                 fill.getY(), false));
        g.fillRoundedRectangle (fill, slotW * 0.5f);
    }

    const float tickInner = slotW * 0.5f + 5.0f;
    for (int i = 0; i < 9; ++i)
    {
        const float t = static_cast<float> (i) / 8.0f;
        const float ty = range.getEnd() - t * range.getLength();
        const bool major = (i % 4) == 0;
        const float len = major ? (large ? 14.0f : 10.0f) : (large ? 9.0f : 6.0f);

        g.setColour (kDim.withAlpha (major ? 0.9f : 0.5f));
        g.fillRect (cx - tickInner - len, ty - 0.75f, len, major ? 1.5f : 1.0f);
        g.fillRect (cx + tickInner, ty - 0.75f, len, major ? 1.5f : 1.0f);
    }

    drawFaderCap (g, juce::Rectangle<float> (cx - capW * 0.5f, capY - capH * 0.5f, capW, capH));
}

void FbkLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                           const juce::Colour& backgroundColour,
                                           bool highlighted, bool down)
{
    auto r = button.getLocalBounds().toFloat().reduced (0.5f);
    const bool enabled = button.isEnabled();
    const float alpha = enabled ? 1.0f : 0.45f;

    auto top = juce::Colour (0xff5a616b).withMultipliedAlpha (alpha);
    auto bottom = juce::Colour (0xff3c414a).withMultipliedAlpha (alpha);
    if (down)         { top = top.darker (0.35f); bottom = bottom.darker (0.35f); }
    else if (highlighted) { top = top.brighter (0.08f); bottom = bottom.brighter (0.08f); }

    g.setGradientFill (juce::ColourGradient (top, r.getCentreX(), r.getY(),
                                             bottom, r.getCentreX(), r.getBottom(), false));
    g.fillRoundedRectangle (r, 4.0f);

    // A button that has deliberately been given its own colour - the sweep, which
    // plays tone through the PA - keeps it as a wash over the metal.
    if (! backgroundColour.isTransparent() && backgroundColour != kPanelTop)
    {
        g.setColour (backgroundColour.withMultipliedAlpha (0.85f * alpha));
        g.fillRoundedRectangle (r, 4.0f);
    }

    if (! down)
    {
        g.setColour (juce::Colours::white.withAlpha (0.16f * alpha));
        g.drawLine (r.getX() + 4.0f, r.getY() + 1.0f, r.getRight() - 4.0f, r.getY() + 1.0f, 1.0f);
    }

    g.setColour (juce::Colour (0xff565d67).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

void FbkLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                       bool highlighted, bool down)
{
    auto r = button.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = button.getToggleState();
    const float alpha = button.isEnabled() ? 1.0f : 0.45f;

    drawButtonBackground (g, button, {}, highlighted, down);

    const float ledSize = juce::jmin (9.0f, r.getHeight() - 8.0f);
    const auto led = juce::Rectangle<float> (r.getX() + 10.0f, r.getCentreY() - ledSize * 0.5f,
                                             ledSize, ledSize);
    drawLed (g, led, button.findColour (juce::ToggleButton::tickColourId).withMultipliedAlpha (alpha), on);

    auto text = r.withTrimmedLeft (led.getRight() - r.getX() + 9.0f).withTrimmedRight (8.0f);
    g.setColour (button.findColour (juce::ToggleButton::textColourId)
                     .withMultipliedAlpha (on ? alpha : alpha * 0.82f));
    g.setFont (uiFont (12.0f));
    g.drawText (button.getButtonText(), text, juce::Justification::centredLeft, true);
}

void FbkLookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g,
                                    bool highlighted, bool)
{
    auto r = button.getActiveArea().toFloat().reduced (1.0f, 0.0f);
    const bool front = button.isFrontTab();

    auto top = front ? juce::Colour (0xff5d646f) : juce::Colour (0xff3e434b);
    auto bottom = front ? juce::Colour (0xff464c55) : juce::Colour (0xff2f333a);
    if (highlighted && ! front) { top = top.brighter (0.08f); bottom = bottom.brighter (0.08f); }

    g.setGradientFill (juce::ColourGradient (top, r.getCentreX(), r.getY(),
                                             bottom, r.getCentreX(), r.getBottom(), false));
    g.fillRoundedRectangle (r, 6.0f);
    // Square off the bottom so the selected tab joins the panel below it.
    g.fillRect (r.withTop (r.getBottom() - 8.0f));

    g.setColour (front ? juce::Colour (0xff6b727d) : kPanelEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    if (front)
    {
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.drawLine (r.getX() + 6.0f, r.getY() + 1.0f, r.getRight() - 6.0f, r.getY() + 1.0f, 1.0f);
    }

    g.setColour (front ? juce::Colours::white : kDim);
    g.setFont (uiFont (11.5f, front));
    drawSpacedText (g, button.getButtonText().toUpperCase(), r, 2.0f,
                    juce::Justification::centred);
}

int FbkLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int)
{
    return juce::GlyphArrangement::getStringWidth (uiFont (11.5f, true),
                                                   button.getButtonText().toUpperCase())
         + 2 * button.getButtonText().length() + 44;
}

// ===========================================================================
LedButton::LedButton (juce::Colour accent) : juce::Button ("power"), accent_ (accent)
{
    setClickingTogglesState (true);
}

void LedButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);

    juce::ColourGradient bezel (juce::Colour (0xff4f5660), r.getCentreX(), r.getY(),
                                juce::Colour (0xff31363d), r.getCentreX(), r.getBottom(), false);
    g.setGradientFill (bezel);
    g.fillEllipse (r);
    g.setColour (juce::Colour (0xff626a75).brighter (highlighted ? 0.15f : 0.0f));
    g.drawEllipse (r.reduced (0.5f), 1.0f);

    auto led = r.reduced (r.getWidth() * 0.3f);
    if (down)
        led = led.translated (0.0f, 0.5f);
    FbkLookAndFeel::drawLed (g, led, accent_, getToggleState());
}

// ===========================================================================
FaderStrip::FaderStrip (FBKSuppressorProcessor& processor, const char* paramId,
                        juce::String name, juce::Colour accent, juce::String suffix,
                        int decimals)
    : name_ (std::move (name)), suffix_ (std::move (suffix)), decimals_ (decimals)
{
    slider_.setSliderStyle (juce::Slider::LinearVertical);
    slider_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider_.setColour (juce::Slider::trackColourId, accent);
    slider_.setColour (juce::Slider::backgroundColourId, kSlot);
    slider_.setDoubleClickReturnValue (true, slider_.getValue());
    slider_.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider_);

    attachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.state(), paramId, slider_);

    // Set after the attachment, so double-click returns to the parameter's own
    // default rather than to whatever it happened to be at construction.
    if (auto* param = processor.state().getParameter (paramId))
        slider_.setDoubleClickReturnValue (
            true, slider_.proportionOfLengthToValue (param->getDefaultValue()));
}

juce::String FaderStrip::valueText() const
{
    return juce::String (slider_.getValue(), decimals_) + suffix_;
}

void FaderStrip::resized()
{
    slider_.setBounds (getLocalBounds().withTrimmedBottom (32));
}

void FaderStrip::paint (juce::Graphics& g)
{
    auto labels = getLocalBounds().removeFromBottom (32);

    g.setColour (kDim);
    g.setFont (uiFont (11.0f));
    g.drawText (name_, labels.removeFromTop (15), juce::Justification::centred, false);

    g.setColour (kText);
    g.setFont (monoFont (11.5f));
    g.drawText (valueText(), labels, juce::Justification::centred, false);
}

// ===========================================================================
ModuleStrip::ModuleStrip (FBKSuppressorProcessor& processor, juce::String title,
                          const char* enableParamId, juce::Colour accent,
                          std::initializer_list<FaderSpec> specs)
    : title_ (std::move (title)), accent_ (accent), power_ (accent)
{
    addAndMakeVisible (power_);
    power_.onStateChange = [this] { refreshDimming(); };
    powerAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.state(), enableParamId, power_);

    for (const auto& spec : specs)
        addAndMakeVisible (faders_.add (new FaderStrip (processor, spec.paramId, spec.name,
                                                        accent, spec.suffix, spec.decimals)));

    refreshDimming();
}

void ModuleStrip::setBadge (const juce::String& text)
{
    if (badge_ == text)
        return;

    badge_ = text;
    repaint();
}

void ModuleStrip::refreshDimming()
{
    // A disabled module stays legible - you still need to see what it is set to
    // before you switch it back in - but it must never read as active.
    const float alpha = isOn() ? 1.0f : 0.5f;
    for (auto* fader : faders_)
        fader->setAlpha (alpha);

    repaint();
}

void ModuleStrip::resized()
{
    auto area = getLocalBounds();
    auto head = area.removeFromTop (34);
    power_.setBounds (head.removeFromLeft (29).withSizeKeepingCentre (19, 19));

    area = area.reduced (4, 0).withTrimmedTop (10).withTrimmedBottom (10);
    if (faders_.isEmpty())
        return;

    const int each = area.getWidth() / faders_.size();
    for (auto* fader : faders_)
        fader->setBounds (area.removeFromLeft (each));
}

void ModuleStrip::paint (juce::Graphics& g)
{
    const bool on = isOn();
    FbkLookAndFeel::drawPanel (g, getLocalBounds().toFloat().reduced (0.5f), accent_, ! on);

    auto head = getLocalBounds().withHeight (34).withTrimmedLeft (31).withTrimmedRight (7);

    // The badge takes what it needs and the name gets the rest, clipped rather
    // than allowed to run into it when the window is narrow.
    g.setFont (monoFont (10.5f));
    auto badgeArea = head.removeFromRight (badge_.isEmpty()
        ? 0
        : juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), badge_)) + 8);
    g.setColour (on ? accent_ : kDim.withAlpha (0.7f));
    g.drawText (badge_, badgeArea, juce::Justification::centredRight, false);

    // Narrow windows lose the letter spacing first, then a little size: a name
    // that has been cut in half tells you nothing.
    float spacing = 1.6f;
    float height = 10.5f;
    for (int attempt = 0; attempt < 12; ++attempt)
    {
        if (spacedTextWidth (uiFont (height, true), title_, spacing)
                <= static_cast<float> (head.getWidth()))
            break;

        if (spacing > 0.0f)      spacing = juce::jmax (0.0f, spacing - 0.4f);
        else if (height > 8.5f)  height -= 0.5f;
        else                     break;
    }

    {
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (head);
        g.setColour (on ? kText : kDim);
        g.setFont (uiFont (height, true));
        drawSpacedText (g, title_, head.toFloat(), spacing, juce::Justification::left);
    }

    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.drawLine (4.0f, 34.0f, static_cast<float> (getWidth()) - 4.0f, 34.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawLine (4.0f, 35.0f, static_cast<float> (getWidth()) - 4.0f, 35.0f, 1.0f);
}

// ===========================================================================
StrengthColumn::StrengthColumn (FBKSuppressorProcessor& processor)
{
    slider_.setSliderStyle (juce::Slider::LinearVertical);
    slider_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider_.setColour (juce::Slider::trackColourId, kStrengthHue);
    slider_.getProperties().set (FbkLookAndFeel::largeFaderProperty, true);
    slider_.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider_);

    attachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.state(), fbkparam::strength, slider_);

    if (auto* param = processor.state().getParameter (fbkparam::strength))
        slider_.setDoubleClickReturnValue (
            true, slider_.proportionOfLengthToValue (param->getDefaultValue()));
}

void StrengthColumn::resized()
{
    auto area = getLocalBounds().withTrimmedTop (74).withTrimmedBottom (44);
    slider_.setBounds (area.withSizeKeepingCentre (78, area.getHeight()));
}

void StrengthColumn::paint (juce::Graphics& g)
{
    FbkLookAndFeel::drawPanel (g, getLocalBounds().toFloat().reduced (0.5f), kStrengthHue);

    auto top = getLocalBounds().withHeight (74).withTrimmedTop (14);
    g.setColour (kText);
    g.setFont (uiFont (11.5f, true));
    drawSpacedText (g, "STRENGTH", top.removeFromTop (16).toFloat(), 3.0f,
                    juce::Justification::centred);

    g.setColour (kStrengthHue);
    g.setFont (monoFont (21.0f));
    g.drawText (juce::String (juce::roundToInt (slider_.getValue())) + "%",
                top.removeFromTop (30), juce::Justification::centred, false);

    // The scale is drawn here rather than by the fader so that it lines up with
    // the travel even when the column is resized.
    const auto range = FbkLookAndFeel::travel (slider_.getBounds().toFloat(), true);
    const float labelRight = static_cast<float> (slider_.getX()) - 4.0f;

    g.setColour (kDim);
    g.setFont (monoFont (9.5f));
    for (int i = 0; i <= 4; ++i)
    {
        const float t = static_cast<float> (i) / 4.0f;
        const float y = range.getEnd() - t * range.getLength();
        g.drawText (juce::String (juce::roundToInt (t * 100.0f)),
                    juce::Rectangle<float> (labelRight - 30.0f, y - 7.0f, 30.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }

    g.setColour (kDim);
    g.setFont (uiFont (9.0f));
    auto foot = getLocalBounds().removeFromBottom (40);
    drawSpacedText (g, "MASTER", foot.removeFromTop (14).toFloat(), 1.6f,
                    juce::Justification::centred);
    drawSpacedText (g, "PROCESS DEPTH", foot.removeFromTop (14).toFloat(), 1.6f,
                    juce::Justification::centred);
}

// ===========================================================================
void SimpleProgress::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();

    g.setColour (kSlot);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);

    if (progress_ > 0.0f)
    {
        auto filled = r.reduced (1.5f);
        filled = filled.withWidth (filled.getWidth() * progress_);
        g.setColour (kWarnColour.withAlpha (0.85f));
        g.fillRoundedRectangle (filled, 2.0f);
    }

    g.setColour (kText);
    g.setFont (uiFont (11.0f));
    g.drawText (text_, getLocalBounds(), juce::Justification::centred);
}

// ===========================================================================
AnalyserDisplay::AnalyserDisplay (FBKSuppressorProcessor& p) : processor_ (p)
{
    setOpaque (false);
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
    auto bounds = getLocalBounds().toFloat();

    // A recessed well, so the analyser reads as a screen let into the panel
    // rather than as another panel.
    g.setColour (kPanel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.55f),
                                             bounds.getCentreX(), bounds.getY(),
                                             juce::Colours::transparentBlack,
                                             bounds.getCentreX(), bounds.getY() + 18.0f, false));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (kPanelEdge);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    // Title and legend live on their own strip so nothing overlaps the trace.
    auto area = bounds.reduced (1.0f);
    auto bar = area.removeFromTop (23.0f);

    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.06f),
                                             bar.getCentreX(), bar.getY(),
                                             juce::Colours::transparentWhite,
                                             bar.getCentreX(), bar.getBottom(), false));
    g.fillRect (bar);
    g.setColour (juce::Colour (0xff2b3038));
    g.drawLine (bar.getX() + 6.0f, bar.getBottom(), bar.getRight() - 6.0f, bar.getBottom(), 1.0f);

    g.setColour (kDim);
    g.setFont (uiFont (9.0f, true));
    drawSpacedText (g, "SPECTRUM & MASK", bar.withTrimmedLeft (10.0f), 2.2f,
                    juce::Justification::left);

    g.setFont (monoFont (9.5f));
    auto legend = bar.withTrimmedRight (10.0f);
    const auto entry = [&g, &legend] (juce::Colour colour, const juce::String& text)
    {
        const float width = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 22.0f;
        auto cell = legend.removeFromRight (width);
        g.setColour (kDim);
        g.drawText (text, cell.withTrimmedLeft (18.0f), juce::Justification::centredLeft, false);
        g.setColour (colour);
        g.fillRect (cell.getX() + 2.0f, cell.getCentreY() - 1.0f, 12.0f, 2.0f);
        legend.removeFromRight (6.0f);
    };
    entry (kToneColour, "tones");
    entry (kGainColour, "mask applied");
    entry (kNoiseColour, "noise floor");
    entry (kInputColour, "input");

    auto plot = area.reduced (10.0f, 8.0f).withTrimmedLeft (24.0f);

    g.setFont (monoFont (9.5f));
    for (float hz : { 50.0f, 100.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = frequencyToX (hz, plot);
        g.setColour (kGrid);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom() - 13.0f);
        g.setColour (kDim);
        const juce::String text = hz >= 1000.0f ? juce::String (hz / 1000.0f, 0) + "k"
                                                : juce::String (hz, 0);
        g.drawText (text, juce::Rectangle<float> (x - 16.0f, plot.getBottom() - 12.0f, 32.0f, 12.0f),
                    juce::Justification::centred);
    }

    if (! primed_)
    {
        g.setColour (kDim);
        g.setFont (uiFont (12.0f));
        g.drawText ("waiting for audio", plot, juce::Justification::centred);
        return;
    }

    auto trace = plot.withTrimmedBottom (14.0f);

    const auto dbToY = [&trace] (float db)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb));
        return trace.getBottom() - t * trace.getHeight();
    };

    g.setFont (monoFont (9.0f));
    for (int db = -80; db <= -20; db += 20)
    {
        const float y = dbToY (static_cast<float> (db));
        g.setColour (kGrid.withAlpha (0.55f));
        g.drawHorizontalLine (juce::roundToInt (y), trace.getX(), trace.getRight());
        g.setColour (kDim);
        g.drawText (juce::String (db), juce::Rectangle<float> (trace.getX() - 26.0f, y - 7.0f, 22.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }

    const auto bandX = [&trace] (int b)
    {
        const float t = (static_cast<float> (b) + 0.5f) / static_cast<float> (fbk::kNumBands);
        return trace.getX() + t * trace.getWidth();
    };

    juce::Path input, noise, gain, inputFill;
    for (int b = 0; b < fbk::kNumBands; ++b)
    {
        const float x = bandX (b);
        const float yIn = dbToY (smoothedInput_[b]);
        const float yNo = dbToY (snapshot_.bandNoiseDb[b]);
        const float t = juce::jlimit (0.0f, 1.0f, (smoothedGain_[b] + 24.0f) / 24.0f);
        const float yGain = trace.getBottom() - t * trace.getHeight();

        if (b == 0)
        {
            input.startNewSubPath (x, yIn);
            noise.startNewSubPath (x, yNo);
            gain.startNewSubPath (x, yGain);
            inputFill.startNewSubPath (x, trace.getBottom());
            inputFill.lineTo (x, yIn);
        }
        else
        {
            input.lineTo (x, yIn);
            noise.lineTo (x, yNo);
            gain.lineTo (x, yGain);
            inputFill.lineTo (x, yIn);
        }
    }
    inputFill.lineTo (bandX (fbk::kNumBands - 1), trace.getBottom());
    inputFill.closeSubPath();

    g.setGradientFill (juce::ColourGradient (kInputColour.withAlpha (0.35f),
                                             trace.getCentreX(), trace.getY(),
                                             kInputColour.withAlpha (0.02f),
                                             trace.getCentreX(), trace.getBottom(), false));
    g.fillPath (inputFill);

    g.setColour (kNoiseColour.withAlpha (0.8f));
    g.strokePath (noise, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved));
    g.setColour (kGainColour.withAlpha (0.9f));
    g.strokePath (gain, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved));
    g.setColour (kInputColour);
    g.strokePath (input, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved));

    int labelled = 0;
    for (int i = 0; i < fbk::kMaxTones; ++i)
    {
        const float hz = snapshot_.toneFrequencies[i];
        const float confidence = snapshot_.toneConfidence[i];
        if (hz <= 0.0f || confidence <= 0.02f)
            continue;

        const float x = frequencyToX (hz, plot);
        g.setColour (kToneColour.withAlpha (juce::jlimit (0.15f, 1.0f, confidence)));
        g.drawVerticalLine (juce::roundToInt (x), trace.getY(), trace.getBottom());

        if (confidence > 0.4f)
        {
            // Alternating rows, so two tones close together stay readable.
            const auto chip = juce::Rectangle<float> (x - 31.0f,
                                                      trace.getY() + 2.0f + (labelled % 2) * 18.0f,
                                                      62.0f, 15.0f);
            g.setColour (kPanel.withAlpha (0.92f));
            g.fillRoundedRectangle (chip, 2.0f);
            g.setColour (kToneColour);
            g.drawRoundedRectangle (chip.reduced (0.5f), 2.0f, 0.8f);
            g.setFont (monoFont (9.5f));
            g.drawText (hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz"
                                      : juce::String (juce::roundToInt (hz)) + " Hz",
                        chip, juce::Justification::centred, false);
            ++labelled;
        }
    }
}

// ===========================================================================
ProcessPanel::ProcessPanel (FBKSuppressorProcessor& p) : processor_ (p), display_ (p)
{
    addAndMakeVisible (display_);

    // Order matters: this is the order the signal meets the stages in, left to
    // right, so the surface reads the same way the processing runs.
    modules_.add (new ModuleStrip (p, "FEEDBACK", fbkparam::fbEnabled, kFeedbackHue,
                                   { { fbkparam::fbSensitivity, "Sens", "%", 0 },
                                     { fbkparam::fbDepth, "Depth", "%", 0 } }));

    modules_.add (new ModuleStrip (p, "NOISE", fbkparam::nrEnabled, kNoiseHue,
                                   { { fbkparam::nrAmount, "Amount", "%", 0 },
                                     { fbkparam::nrMaxAtten, "Max cut", " dB", 1 },
                                     { fbkparam::nrVoiceProtect, "Protect", "%", 0 } }));

    modules_.add (new ModuleStrip (p, "HUM", fbkparam::humEnabled, kHumHue,
                                   { { fbkparam::humDepth, "Depth", "%", 0 },
                                     { fbkparam::humHarmonics, "Harm", "", 0 } }));

    modules_.add (new ModuleStrip (p, "DEREVERB", fbkparam::drEnabled, kDereverbHue,
                                   { { fbkparam::drAmount, "Amount", "%", 0 },
                                     { fbkparam::drRt60, "RT60", " s", 2 } }));

    modules_.add (new ModuleStrip (p, "RUMBLE", fbkparam::hpEnabled, kRumbleHue,
                                   { { fbkparam::hpFreq, "Freq", " Hz", 0 } }));

    for (auto* module : modules_)
        addAndMakeVisible (module);

    startTimerHz (8);
}

ProcessPanel::~ProcessPanel() { stopTimer(); }

void ProcessPanel::timerCallback()
{
    const auto* m = processor_.metering();
    if (m == nullptr)
        return;

    const auto badge = [this] (int index, const juce::String& text)
    {
        if (auto* strip = modules_[index])
            strip->setBadge (strip->isOn() ? text : "off");
    };

    badge (0, m->confirmedTones > 0 ? juce::String (m->confirmedTones) + " tones" : "clear");

    // The deepest cut the mask is currently making: the noise stage's headline
    // number, and the one worth watching while you set it.
    float deepestCutDb = 0.0f;
    for (int b = 0; b < fbk::kNumBands; ++b)
        deepestCutDb = juce::jmin (deepestCutDb, m->bandGainDb[b]);
    badge (1, juce::String (deepestCutDb, 1) + " dB");

    badge (2, m->humActive ? juce::String (juce::roundToInt (m->humFundamentalHz)) + " Hz" : "idle");
    badge (3, "on");
    badge (4, "on");
}

void ProcessPanel::paint (juce::Graphics& g)
{
    carbon_.paint (g, getLocalBounds());
}

void ProcessPanel::resized()
{
    auto area = getLocalBounds();

    // A third of the window for the analyser and a half for the modules. What is
    // left over went to the header and the tabs above this panel, so the shares
    // are taken from what arrives here rather than from the window.
    display_.setBounds (area.removeFromTop (juce::jmax (120, juce::roundToInt (area.getHeight() * 0.395f))));
    area.removeFromTop (12);

    if (modules_.isEmpty())
        return;

    const int gap = 8;
    const int each = (area.getWidth() - gap * (modules_.size() - 1)) / modules_.size();
    for (auto* module : modules_)
    {
        module->setBounds (area.removeFromLeft (each));
        area.removeFromLeft (gap);
    }
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

void CalibratePanel::paint (juce::Graphics& g) { carbon_.paint (g, getLocalBounds()); }

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
    carbon_.paint (g, getLocalBounds());

    // Separator between the two halves, so the sweep controls read as a distinct
    // and more consequential thing than the logging switches above them.
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRect (10, separatorY_, getWidth() - 20, 1);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.fillRect (10, separatorY_ + 1, getWidth() - 20, 1);
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
    : AudioProcessorEditor (&p), processor_ (p), strength_ (p)
{
    setLookAndFeel (&lookAndFeel_);

    titleLabel_.setText ("FBKSuppressor", juce::dontSendNotification);
    titleLabel_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    titleLabel_.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (titleLabel_);

    subtitleLabel_.setText ("LIVE FEEDBACK & NOISE CONTROL", juce::dontSendNotification);
    subtitleLabel_.setFont (juce::FontOptions (9.0f));
    subtitleLabel_.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (subtitleLabel_);

    latencyLabel_.setFont (monoFont (11.5f));
    latencyLabel_.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (latencyLabel_);

    statusLabel_.setFont (monoFont (10.0f));
    statusLabel_.setColour (juce::Label::textColourId, kDim);
    statusLabel_.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLabel_);

    bypassButton_.setButtonText ("BYPASS");
    styleButton (bypassButton_);
    bypassButton_.setColour (juce::ToggleButton::tickColourId, kToneColour);
    addAndMakeVisible (bypassButton_);
    bypassAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor_.state(), fbkparam::bypass, bypassButton_);

    qualityButton_.setButtonText ("QUALITY MODE");
    styleButton (qualityButton_);
    addAndMakeVisible (qualityButton_);
    qualityAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor_.state(), fbkparam::qualityMode, qualityButton_);

    tabs_.setOutline (0);
    tabs_.setTabBarDepth (30);
    tabs_.setColour (juce::TabbedComponent::backgroundColourId, juce::Colours::transparentBlack);
    tabs_.addTab ("Process", juce::Colours::transparentBlack, new ProcessPanel (processor_), true);
    tabs_.addTab ("Calibrate", juce::Colours::transparentBlack, new CalibratePanel (processor_), true);
    tabs_.addTab ("Diagnostics", juce::Colours::transparentBlack, new DiagnosticsPanel (processor_), true);
    addAndMakeVisible (tabs_);

    // Strength sits outside the tabs on purpose: the master control must be
    // under your hand whichever tab happens to be open.
    addAndMakeVisible (strength_);

    setResizable (true, true);
    setResizeLimits (940, 660, 1800, 1300);
    setSize (1060, 800);

    startTimerHz (10);
}

FBKSuppressorEditor::~FBKSuppressorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void FBKSuppressorEditor::timerCallback()
{
    const int latency = processor_.getLatencySamples();
    const double sr = processor_.currentSampleRate();
    const double ms = sr > 0.0 ? 1000.0 * latency / sr : 0.0;

    latencyLabel_.setText (latency == 0
                               ? "LATENCY 0 samples / 0.00 ms"
                               : "LATENCY " + juce::String (latency) + " samples / "
                                     + juce::String (ms, 2) + " ms",
                           juce::dontSendNotification);
    latencyLabel_.setColour (juce::Label::textColourId, latency == 0 ? kGainColour : kToneColour);

    juce::String status;
    status << juce::String (sr / 1000.0, 1) << " kHz   "
           << processor_.numActiveChannels() << " ch   CPU "
           << juce::String (processor_.cpuLoad() * 100.0f, 1) << "%";

    if (processor_.isProfileApplied())
        status << "   PROFILE";

    if (auto* m = processor_.metering())
    {
        if (m->confirmedTones > 0)
            status << "   TONES " << m->confirmedTones;
        if (m->humActive)
            status << "   HUM " << juce::String (m->humFundamentalHz, 0) << " Hz";
        if (m->meanAttenuationDb < -0.05f)
            status << "   MASK " << juce::String (m->meanAttenuationDb, 1) << " dB";
    }

    if (processor_.isSweeping())
        status << "   SWEEPING";

    statusLabel_.setText (status, juce::dontSendNotification);
}

void FBKSuppressorEditor::paint (juce::Graphics& g)
{
    carbon_.paint (g, getLocalBounds());
    FbkLookAndFeel::drawPanel (g, brandArea_.toFloat(), {});
}

void FBKSuppressorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    brandArea_ = area.removeFromTop (52);
    auto header = brandArea_.reduced (16, 0);

    auto brand = header.removeFromLeft (200);
    titleLabel_.setBounds (brand.removeFromTop (32).withTrimmedTop (6));
    subtitleLabel_.setBounds (brand.withTrimmedTop (-2));

    header.removeFromLeft (14);
    auto buttons = header.reduced (0, 12);
    bypassButton_.setBounds (buttons.removeFromLeft (104));
    buttons.removeFromLeft (8);
    qualityButton_.setBounds (buttons.removeFromLeft (152));

    auto readouts = header.removeFromRight (juce::jmin (400, header.getWidth())).reduced (0, 8);
    latencyLabel_.setBounds (readouts.removeFromTop (readouts.getHeight() / 2));
    statusLabel_.setBounds (readouts);

    area.removeFromTop (10);

    // Strength runs from the top of the analyser - which is the top of the tab
    // content, not the top of the tab bar - to the bottom of the window.
    auto body = area;
    auto strengthArea = body.removeFromRight (140);
    body.removeFromRight (12);
    tabs_.setBounds (body);
    strength_.setBounds (strengthArea.withTrimmedTop (tabs_.getTabBarDepth() + 4));
}
