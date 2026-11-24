#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "SliderRhythmFonts.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace braun {

// Dieter Rams / Braun dual-surface palette:
// - Dark (default): Atelier P4 matte anodized graphite + emissive smoked-glass LED displays
// - Light: SK 4 / ET 66 warm anodized silver-white aluminum + recessed reflective mineral LCD displays
struct Palette {
    bool isDark = true;
    juce::Colour chassis;
    juce::Colour panelWell;
    juce::Colour capSurface;
    juce::Colour hairline;
    juce::Colour controlBorder;
    juce::Colour ink;
    juce::Colour mutedInk;
    juce::Colour dimInk;
    juce::Colour orange;
    juce::Colour orangeTint;
    juce::Colour cyan;
    juce::Colour cyanTint;
    juce::Colour muteRed;
    juce::Colour soloCyan;
    juce::Colour scopeBg;
    juce::Colour scopeGrid;
    juce::Colour scopeTrace;
    juce::Colour scopeBarOff;
    juce::Colour faderSlotBg;
    // Recessed dot-matrix display bay tokens
    juce::Colour lcdBg;
    juce::Colour lcdWell;
    juce::Colour lcdBezel;
    juce::Colour lcdInnerShadow;
    juce::Colour lcdWordmark;
    juce::Colour lcdAccent;
    juce::Colour lcdCyan;
    juce::Colour lcdNeutral;
    bool lcdSquareDots = false;
    bool lcdGlowHalo = true;

    static Palette light() noexcept {
        return {
            false,
            juce::Colour (0xfff2efe9), // chassis (#F2EFE9)
            juce::Colour (0xffeae6de), // panelWell (#EAE6DE)
            juce::Colour (0xfff7f5f0), // capSurface (#F7F5F0)
            juce::Colour (0xffd3cec3), // hairline (#D3CEC3)
            juce::Colour (0xffbfb9ad), // controlBorder (#BFB9AD)
            juce::Colour (0xff1a1917), // ink (#1A1917)
            juce::Colour (0xff66625a), // mutedInk (#66625A)
            juce::Colour (0xff9d978c), // dimInk (#9D978C)
            juce::Colour (0xffec560e), // orange (#EC560E)
            juce::Colour (0xfff5dcd0), // orangeTint (#F5DCD0)
            juce::Colour (0xff1676ae), // cyan (#1676AE)
            juce::Colour (0xffd7ebf5), // cyanTint (#D7EBF5)
            juce::Colour (0xffc9402e), // muteRed
            juce::Colour (0xff1676ae), // soloCyan
            juce::Colour (0xffdee1d8), // scopeBg (#DEE1D8 reflective mineral LCD)
            juce::Colour (0xffc9cdc3), // scopeGrid (#C9CDC3)
            juce::Colour (0xff6a6e66), // scopeTrace (#6A6E66)
            juce::Colour (0xffb8bcb2), // scopeBarOff (#B8BCB2)
            juce::Colour (0xffd9d4c9), // faderSlotBg
            juce::Colour (0xffdee1d8), // lcdBg (#DEE1D8)
            juce::Colour (0xffcfd2c8), // lcdWell (#CFD2C8)
            juce::Colour (0xffbec1b7), // lcdBezel (#BEC1B7)
            juce::Colour (0x3882867c), // lcdInnerShadow (subtle top recess hairline)
            juce::Colour (0xff1e1f1c), // lcdWordmark (#1E1F1C deep carbon ink)
            juce::Colour (0xffda480a), // lcdAccent (#DA480A Braun orange pigment)
            juce::Colour (0xff116ea6), // lcdCyan (#116EA6 deep technical cyan)
            juce::Colour (0xff444640), // lcdNeutral (#444640 slate ink for 0% / SLOT)
            true,                      // lcdSquareDots (reflective rounded-square pixels)
            false                      // lcdGlowHalo (no glow on reflective ink)
        };
    }

    static Palette dark() noexcept {
        return {
            true,
            juce::Colour (0xff161615), // chassis (#161615)
            juce::Colour (0xff1c1c1a), // panelWell (#1C1C1A)
            juce::Colour (0xff272724), // capSurface (#272724)
            juce::Colour (0xff302f2c), // hairline (#302F2C)
            juce::Colour (0xff464540), // controlBorder (#464540)
            juce::Colour (0xffedebe6), // ink (#EDEBE6)
            juce::Colour (0xff9a968d), // mutedInk (#9A968D)
            juce::Colour (0xff605e58), // dimInk (#605E58)
            juce::Colour (0xfff26018), // orange (#F26018)
            juce::Colour (0xff361e12), // orangeTint (#361E12)
            juce::Colour (0xff4dc4e6), // cyan (#4DC4E6)
            juce::Colour (0xff162e36), // cyanTint (#162E36)
            juce::Colour (0xffd95344), // muteRed
            juce::Colour (0xff3ba4dc), // soloCyan
            juce::Colour (0xff0f100f), // scopeBg (#0F100F)
            juce::Colour (0xff212320), // scopeGrid (#212320)
            juce::Colour (0xffb6bab6), // scopeTrace (#B6BAB6)
            juce::Colour (0xff383a37), // scopeBarOff (#383A37)
            juce::Colour (0xff0d0d0c), // faderSlotBg
            juce::Colour (0xff0d0e0d), // lcdBg (#0D0E0D smoked glass)
            juce::Colour (0xff1c1e1c), // lcdWell (#1C1E1C unlit LED wells)
            juce::Colour (0xff302f2c), // lcdBezel (#302F2C)
            juce::Colours::transparentBlack, // lcdInnerShadow
            juce::Colour (0xfff26018), // lcdWordmark (#F26018 emissive orange)
            juce::Colour (0xfff26018), // lcdAccent (#F26018)
            juce::Colour (0xff4dc4e6), // lcdCyan (#4DC4E6)
            juce::Colour (0xffb8b4aa), // lcdNeutral (#B8B4AA warm off-white LED)
            false,                     // lcdSquareDots (emissive circular LED dots)
            true                       // lcdGlowHalo (subtle emissive LED halo)
        };
    }

    static Palette forMode (bool darkMode) noexcept {
        return darkMode ? dark() : light();
    }
};

// Unified 4-level chassis silkscreen typography scale (Geist Medium, SIL OFL):
// - L1 Micro   : passive labels (NOTE/CH/VEL/SWING/MACRO/LEN/GRID/GATE/CHOK/VSCL), fader ticks, scope badges
// - L2 Control : all bracket buttons, toolbar actions, status readouts, secondary knob readouts
// - L3 Value   : primary editable parameter values (NOTE, CH, LEN, GRID, LOOP, GATE, CHOK, VSCL, active preset)
// - L4 Title   : track header names and popup menu items
inline constexpr float kFontMicro   = 10.0f;
inline constexpr float kFontControl = 11.2f;
inline constexpr float kFontValue   = 12.2f;
inline constexpr float kFontTitle   = 13.0f;

inline juce::Typeface::Ptr silkscreenTypeface() {
    static const juce::Typeface::Ptr tf = juce::Typeface::createSystemTypefaceFor (
        SliderRhythmFonts::GeistMedium_ttf, SliderRhythmFonts::GeistMedium_ttfSize);
    return tf;
}

inline juce::Font silkscreenFont (float size) {
    return juce::Font (juce::FontOptions (silkscreenTypeface()).withHeight (size));
}

inline juce::Font fontMicro()   { return silkscreenFont (kFontMicro); }
inline juce::Font fontControl() { return silkscreenFont (kFontControl); }
inline juce::Font fontValue()   { return silkscreenFont (kFontValue); }
inline juce::Font fontTitle()   { return silkscreenFont (kFontTitle); }

inline juce::String midiNoteName (int midiNote) {
    static constexpr std::array<const char*, 12> pitchNames {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    const int n = std::clamp (midiNote, 0, 127);
    const int octave = (n / 12) - 2; // Ableton/Logic C3 = 60, C1 = 36.
    return juce::String (pitchNames[static_cast<size_t> (n % 12)]) + juce::String (octave);
}

// Strictly parses valid MIDI note text without signed overflow:
// - Pure integer in 0..127 with optional leading '+' ("36", "+36")
// - Pitch name ("C1", "F#1", "Bb2", "C-2") whose MIDI number lies in 0..127
// - Combined display label ("C1 36") where pitch and trailing integer match exactly
// Returns std::nullopt on malformed, out-of-range, or contradictory inputs.
inline std::optional<int> tryParseMidiNoteText (const juce::String& text) noexcept {
    const auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return std::nullopt;

    auto parseBoundedUnsigned = [] (const juce::String& s, int start, int end, int64_t& out) noexcept -> bool {
        if (start >= end) return false;
        int64_t value = 0;
        for (int pos = start; pos < end; ++pos) {
            const auto ch = s[pos];
            if (ch < '0' || ch > '9') return false;
            if (value <= 1000000)
                value = value * 10 + static_cast<int64_t> (ch - '0');
        }
        out = value;
        return true;
    };

    // Pure integer entry (optional leading '+', digits only, strictly within 0..127).
    {
        const int start = (trimmed[0] == '+') ? 1 : 0;
        int64_t val = 0;
        if (parseBoundedUnsigned (trimmed, start, trimmed.length(), val)) {
            if (val >= 0 && val <= 127)
                return static_cast<int> (val);
            return std::nullopt;
        }
    }

    // Pitch name at start (e.g. "C1", "F#1", "Bb2", "C-2", or "C1 36").
    const auto up = trimmed.toUpperCase();
    int baseSemi = -1;
    switch (up[0]) {
        case 'C': baseSemi = 0; break;
        case 'D': baseSemi = 2; break;
        case 'E': baseSemi = 4; break;
        case 'F': baseSemi = 5; break;
        case 'G': baseSemi = 7; break;
        case 'A': baseSemi = 9; break;
        case 'B': baseSemi = 11; break;
        default: break;
    }
    if (baseSemi < 0 || up.length() < 2)
        return std::nullopt;

    int pos = 1;
    if (up[pos] == '#') {
        baseSemi += 1;
        ++pos;
    } else if (up[pos] == 'B') {
        baseSemi -= 1;
        ++pos;
    }

    int64_t octSign = 1;
    if (pos < up.length() && up[pos] == '-') {
        octSign = -1;
        ++pos;
    }
    const int octStart = pos;
    while (pos < up.length() && up[pos] >= '0' && up[pos] <= '9')
        ++pos;

    int64_t octMag = 0;
    if (! parseBoundedUnsigned (up, octStart, pos, octMag))
        return std::nullopt;

    const int64_t oct = octSign * octMag;
    if (oct < -2 || oct > 9)
        return std::nullopt;
    const int64_t midiNote = (oct + 2) * 12 + baseSemi;
    if (midiNote < 0 || midiNote > 127)
        return std::nullopt;

    if (pos == up.length())
        return static_cast<int> (midiNote);

    // Optional whitespace + matching MIDI note number (exact format of "C1 36").
    if (! juce::CharacterFunctions::isWhitespace (up[pos]))
        return std::nullopt;
    while (pos < up.length() && juce::CharacterFunctions::isWhitespace (up[pos]))
        ++pos;
    int64_t trailing = 0;
    if (! parseBoundedUnsigned (up, pos, up.length(), trailing))
        return std::nullopt;
    if (trailing != midiNote)
        return std::nullopt;
    return static_cast<int> (midiNote);
}

inline int parseMidiNoteText (const juce::String& text, int fallback = 36) noexcept {
    if (const auto parsed = tryParseMidiNoteText (text))
        return *parsed;
    return std::clamp (fallback, 0, 127);
}

inline void drawDropdownCaret (juce::Graphics& g, float cx, float cy, juce::Colour colour) {
    juce::Path tri;
    tri.addTriangle (cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 2.5f);
    g.setColour (colour);
    g.fillPath (tri);
}

class BraunLookAndFeel : public juce::LookAndFeel_V4 {
public:
    BraunLookAndFeel() { applyPalette (Palette::dark()); }

    void applyPalette (const Palette& p) {
        palette = p;
        setColour (juce::Label::textColourId, p.ink);
        setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        setColour (juce::TextButton::buttonOnColourId, p.orange);
        setColour (juce::TextButton::textColourOffId, p.ink);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::textColourId, p.ink);
        setColour (juce::ComboBox::arrowColourId, p.mutedInk);
        setColour (juce::PopupMenu::backgroundColourId, p.chassis);
        setColour (juce::PopupMenu::textColourId, p.ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, p.orange);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::PopupMenu::headerTextColourId, p.mutedInk);
        setColour (juce::Slider::textBoxTextColourId, p.ink);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ScrollBar::thumbColourId, p.controlBorder);
        setColour (juce::ScrollBar::trackColourId, p.panelWell);
        setColour (juce::AlertWindow::backgroundColourId, p.chassis);
        setColour (juce::AlertWindow::textColourId, p.ink);
        setColour (juce::AlertWindow::outlineColourId, p.controlBorder);
        setColour (juce::TextEditor::backgroundColourId, p.capSurface);
        setColour (juce::TextEditor::textColourId, p.ink);
        setColour (juce::TextEditor::outlineColourId, p.controlBorder);
        setColour (juce::TextEditor::focusedOutlineColourId, p.orange);
    }

    const Palette& currentPalette() const noexcept { return palette; }

    juce::Font getTextButtonFont (juce::TextButton& button, int) override {
        if (static_cast<bool> (button.getProperties().getWithDefault ("plainText", false)))
            return fontValue();
        return fontControl();
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override {
        return fontValue();
    }

    juce::Font getLabelFont (juce::Label& label) override {
        if (dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr)
            return fontValue();
        return fontControl();
    }

    juce::Font getPopupMenuFont() override {
        return fontTitle();
    }

    bool isAccentFill (const juce::Colour& c) const noexcept {
        return c == palette.orange || c == palette.muteRed || c == palette.soloCyan
            || c == palette.orangeTint || c == palette.cyanTint;
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool isHighlighted, bool isDown) override {
        const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        const bool accent = isAccentFill (backgroundColour);

        if (accent) {
            auto fill = backgroundColour;
            if (isDown) fill = fill.darker (0.10f);
            else if (isHighlighted) fill = palette.isDark ? fill.brighter (0.08f) : fill.darker (0.05f);
            g.setColour (fill);
            g.fillRoundedRectangle (bounds, 1.5f);
            return;
        }

        // Borderless architectural button: transparent idle, subtle matte well on hover/press.
        if (isDown) {
            g.setColour (palette.panelWell);
            g.fillRoundedRectangle (bounds, 1.5f);
        } else if (isHighlighted) {
            g.setColour (palette.capSurface.withAlpha (0.82f));
            g.fillRoundedRectangle (bounds, 1.5f);
        }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button,
                         bool isMouseOverButton, bool isButtonDown) override {
        const auto& props = button.getProperties();
        if (static_cast<bool> (props.getWithDefault ("customPainted", false)))
            return;

        const bool plainText = static_cast<bool> (props.getWithDefault ("plainText", false));
        const bool dropdown = static_cast<bool> (props.getWithDefault ("dropdown", false));
        const bool useBrackets = ! plainText && static_cast<bool> (props.getWithDefault ("brackets", true));
        const bool alignLeft = static_cast<bool> (props.getWithDefault ("alignLeft", false));

        const auto font = getTextButtonFont (button, button.getHeight());
        g.setFont (font);

        auto textColour = button.findColour (button.getToggleState()
            ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
        if (! button.isEnabled())
            textColour = textColour.withMultipliedAlpha (0.45f);
        else if ((isMouseOverButton || isButtonDown) && textColour == palette.mutedInk)
            textColour = palette.ink;

        g.setColour (textColour);
        const auto raw = button.getButtonText();
        const auto area = button.getLocalBounds().reduced (2, 0);

        if (! useBrackets) {
            if (dropdown) {
                const auto textArea = area.withTrimmedRight (12);
                g.drawFittedText (raw, textArea, alignLeft ? juce::Justification::centredLeft : juce::Justification::centred, 1, 0.80f);
                drawDropdownCaret (g, static_cast<float> (area.getRight() - 6), area.toFloat().getCentreY(), palette.mutedInk);
            } else {
                g.drawFittedText (raw, area, alignLeft ? juce::Justification::centredLeft : juce::Justification::centred, 1, 0.80f);
            }
            return;
        }

        if (! dropdown) {
            const juce::String bracketed = "[ " + raw + " ]";
            g.drawFittedText (bracketed, area, alignLeft ? juce::Justification::centredLeft : juce::Justification::centred, 1, 0.78f);
            return;
        }

        // Bracketed dropdown: "[ LABEL  v ]" with measured symmetric caret gap for proportional silkscreen fonts.
        const juce::String leftPart = "[ " + raw;
        const juce::String rightPart = "]";
        const float leftW = juce::GlyphArrangement::getStringWidth (font, leftPart);
        const float rightW = juce::GlyphArrangement::getStringWidth (font, rightPart);
        constexpr float caretSlot = 14.0f;
        const float maxW = static_cast<float> (std::max (20, area.getWidth()));
        const float availLeftW = std::max (8.0f, maxW - caretSlot - rightW);
        const float renderedLeftW = std::min (leftW, availLeftW);
        const float totalW = renderedLeftW + caretSlot + rightW;
        const float startX = alignLeft
            ? static_cast<float> (area.getX())
            : (area.toFloat().getCentreX() - totalW * 0.5f);

        const juce::Rectangle<int> leftRect (
            static_cast<int> (std::floor (startX)), area.getY(),
            static_cast<int> (std::ceil (renderedLeftW)) + 1, area.getHeight());
        g.drawFittedText (leftPart, leftRect, juce::Justification::centredLeft, 1, 0.78f);

        const float caretX = startX + renderedLeftW + caretSlot * 0.50f;
        const auto caretCol = (textColour == juce::Colours::white) ? juce::Colours::white : palette.mutedInk;
        drawDropdownCaret (g, caretX, area.toFloat().getCentreY() + 0.5f, caretCol);

        const juce::Rectangle<int> rightRect (
            static_cast<int> (std::floor (startX + renderedLeftW + caretSlot)), area.getY(),
            static_cast<int> (std::ceil (rightW)) + 2, area.getHeight());
        g.drawText (rightPart, rightRect, juce::Justification::centredLeft, false);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                       int, int, int, int, juce::ComboBox& box) override {
        if (isButtonDown || box.isMouseOver()) {
            g.setColour (isButtonDown ? palette.panelWell : palette.capSurface.withAlpha (0.75f));
            g.fillRect (1, 1, std::max (0, width - 2), std::max (0, height - 2));
        }
        drawDropdownCaret (g, static_cast<float> (width) - 9.0f, static_cast<float> (height) * 0.5f + 0.5f, palette.mutedInk);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override {
        label.setBounds (2, 0, std::max (8, box.getWidth() - 16), box.getHeight());
        label.setFont (getComboBoxFont (box));
        label.setJustificationType (juce::Justification::centred);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle style, juce::Slider& slider) override {
        if (style == juce::Slider::LinearBarVertical || style == juce::Slider::LinearBar) {
            if (slider.isMouseOverOrDragging()) {
                g.setColour (palette.capSurface.withAlpha (0.75f));
                g.fillRect (x + 1, y + 1, std::max (0, width - 2), std::max (0, height - 2));
            }
            return;
        }
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider& slider) override {
        const float size = static_cast<float> (std::min (width, height));
        const float radius = std::max (6.0f, size * 0.40f);
        const juce::Point<float> centre (x + width * 0.5f, y + height * 0.5f);
        const float angle = juce::jmap (sliderPos, startAngle, endAngle);

        // Background track ring
        juce::Path trackArc;
        trackArc.addCentredArc (centre.x, centre.y, radius + 2.6f, radius + 2.6f,
                                0.0f, startAngle, endAngle, true);
        g.setColour (palette.hairline);
        g.strokePath (trackArc, juce::PathStrokeType (1.9f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Active arc (supports negative-to-positive bipolar knobs AND 0..100 midpoint-bipolar macro knobs)
        const bool isMidBipolar = static_cast<bool> (slider.getProperties().getWithDefault ("bipolarMid", false));
        const bool isBipolar = isMidBipolar || (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0);
        const float originAngle = isMidBipolar
            ? juce::jmap (0.5f, startAngle, endAngle)
            : (isBipolar
                ? juce::jmap (static_cast<float> ((0.0 - slider.getMinimum()) / (slider.getMaximum() - slider.getMinimum())),
                              startAngle, endAngle)
                : startAngle);

        if (std::abs (angle - originAngle) > 0.01f) {
            juce::Path activeArc;
            activeArc.addCentredArc (centre.x, centre.y, radius + 2.6f, radius + 2.6f,
                                     0.0f, std::min (originAngle, angle), std::max (originAngle, angle), true);
            const auto accent = isMidBipolar
                ? (angle < originAngle ? palette.cyan : palette.orange)
                : slider.findColour (juce::Slider::rotarySliderFillColourId);
            g.setColour (accent.isOpaque() ? accent : palette.orange);
            g.strokePath (activeArc, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Flat Dieter Rams tactile cap
        const juce::Rectangle<float> cap (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);
        g.setColour (palette.capSurface);
        g.fillEllipse (cap);
        g.setColour (palette.controlBorder);
        g.drawEllipse (cap, 1.0f);

        // Crisp pointer line
        const auto at = [centre] (float a, float d) {
            return centre + juce::Point<float> (std::sin (a), -std::cos (a)) * d;
        };
        g.setColour (palette.ink);
        g.drawLine ({ at (angle, radius * 0.24f), at (angle, radius * 0.85f) }, 1.8f);
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override {
        g.setColour (palette.panelWell);
        g.fillRect (x, y, width, height);
        const auto thumbColour = (isMouseOver || isMouseDown) ? palette.mutedInk : palette.controlBorder;
        g.setColour (thumbColour);
        if (isScrollbarVertical)
            g.fillRoundedRectangle (static_cast<float> (x + 2), static_cast<float> (thumbStartPosition + 1),
                                    static_cast<float> (std::max (2, width - 4)), static_cast<float> (std::max (8, thumbSize - 2)), 2.0f);
        else
            g.fillRoundedRectangle (static_cast<float> (thumbStartPosition + 1), static_cast<float> (y + 2),
                                    static_cast<float> (std::max (8, thumbSize - 2)), static_cast<float> (std::max (2, height - 4)), 2.0f);
    }

private:
    Palette palette = Palette::dark();
};

} // namespace braun
