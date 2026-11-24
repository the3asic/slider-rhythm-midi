#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "SliderRhythmFonts.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace braun {

// Dieter Rams / Braun dual-surface palette matching abmscribe BraunTheme:
// - Dark (default): Atelier P4 matte anodized graphite with warm silkscreen ink
// - Light: SK 4 / T 1000 warm anodized silver-white aluminum
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
    juce::Colour faderSlotBg;

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
            juce::Colour (0xff1f86c2), // cyan (#1F86C2)
            juce::Colour (0xffd7ebf5), // cyanTint (#D7EBF5)
            juce::Colour (0xffc9402e), // muteRed
            juce::Colour (0xff1f86c2), // soloCyan
            juce::Colour (0xff111211), // scopeBg (#111211)
            juce::Colour (0xff212320), // scopeGrid (#212320)
            juce::Colour (0xffb6bab6), // scopeTrace (#B6BAB6)
            juce::Colour (0xffd9d4c9)  // faderSlotBg
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
            juce::Colour (0xff0d0d0c)  // faderSlotBg
        };
    }

    static Palette forMode (bool darkMode) noexcept {
        return darkMode ? dark() : light();
    }
};

inline juce::Typeface::Ptr dotTypeface() {
    static const juce::Typeface::Ptr tf = juce::Typeface::createSystemTypefaceFor (
        SliderRhythmFonts::DotoStatic_ttf, SliderRhythmFonts::DotoStatic_ttfSize);
    return tf;
}

inline juce::Typeface::Ptr bodyTypeface() {
    static const juce::Typeface::Ptr tf = juce::Typeface::createSystemTypefaceFor (
        SliderRhythmFonts::DepartureMonoRegular_otf, SliderRhythmFonts::DepartureMonoRegular_otfSize);
    return tf;
}

// Geometric 6x10 round dot-matrix display font (Doto, SIL OFL) for hero titles, track indices, slot readouts, and swing %.
inline juce::Font dotFont (float size) {
    return juce::Font (juce::FontOptions (dotTypeface()).withHeight (size));
}

// Crisp hardware pixel-monospace font (Departure Mono, SIL OFL) for controls, labels, values, and track titles.
inline juce::Font monoFont (float size, bool = false) {
    return juce::Font (juce::FontOptions (bodyTypeface()).withHeight (size));
}

inline juce::Font sansFont (float size, bool bold = false) {
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::String midiNoteName (int midiNote) {
    static constexpr std::array<const char*, 12> pitchNames {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    const int n = std::clamp (midiNote, 0, 127);
    const int octave = (n / 12) - 2; // Ableton/Logic C3 = 60, C1 = 36.
    return juce::String (pitchNames[static_cast<size_t> (n % 12)]) + juce::String (octave);
}

// Parses numeric MIDI notes ("36"), pitch names ("C1", "F#1", "Bb2", "C-2"), or combined labels ("C1 36").
inline int parseMidiNoteText (const juce::String& text) {
    const auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return 36;

    // Pure integer entry (e.g. "36", "42")
    const auto checkPureInt = [] (const juce::String& s, int& out) -> bool {
        if (s.isEmpty()) return false;
        int pos = (s[0] == '+' || s[0] == '-') ? 1 : 0;
        if (pos >= s.length()) return false;
        for (; pos < s.length(); ++pos)
            if (! juce::CharacterFunctions::isDigit (s[pos]))
                return false;
        out = std::clamp (s.getIntValue(), 0, 127);
        return true;
    };

    int directInt = 0;
    if (checkPureInt (trimmed, directInt))
        return directInt;

    // Pitch name at start (e.g. "C1", "F#1 42", "Bb2", "C-2")
    const auto up = trimmed.toUpperCase();
    int baseSemi = -1;
    switch (static_cast<char> (up[0])) {
        case 'C': baseSemi = 0; break;
        case 'D': baseSemi = 2; break;
        case 'E': baseSemi = 4; break;
        case 'F': baseSemi = 5; break;
        case 'G': baseSemi = 7; break;
        case 'A': baseSemi = 9; break;
        case 'B': baseSemi = 11; break;
        default: break;
    }
    if (baseSemi >= 0 && up.length() >= 2) {
        int pos = 1;
        if (up[pos] == '#') {
            baseSemi += 1;
            ++pos;
        } else if (up[pos] == 'B') {
            baseSemi -= 1;
            ++pos;
        }
        int octSign = 1;
        if (pos < up.length() && up[pos] == '-') {
            octSign = -1;
            ++pos;
        }
        int octDigitsStart = pos;
        while (pos < up.length() && juce::CharacterFunctions::isDigit (up[pos]))
            ++pos;
        if (pos > octDigitsStart) {
            const int oct = octSign * up.substring (octDigitsStart, pos).getIntValue();
            return std::clamp ((oct + 2) * 12 + baseSemi, 0, 127);
        }
    }

    return std::clamp (trimmed.getTrailingIntValue(), 0, 127);
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

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override {
        if (buttonHeight <= 19) return monoFont (10.5f);
        if (buttonHeight <= 23) return monoFont (11.2f);
        return monoFont (11.8f);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override {
        return monoFont (12.0f);
    }

    juce::Font getLabelFont (juce::Label& label) override {
        if (dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr)
            return label.getWidth() <= 60 ? monoFont (10.5f) : monoFont (12.0f);
        return monoFont (11.0f);
    }

    juce::Font getPopupMenuFont() override {
        return sansFont (12.5f, false);
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

        // Bracketed dropdown: "[ LABEL  v ]" with crisp drawn vector caret centered in the right gap before ']'.
        const juce::String display = "[ " + raw + "   ]";
        const float fullW = juce::GlyphArrangement::getStringWidth (font, display);
        const float maxW = static_cast<float> (std::max (12, area.getWidth()));
        const float hScale = fullW > maxW ? std::max (0.74f, maxW / fullW) : 1.0f;
        const float renderedW = fullW * hScale;
        const float closeW = juce::GlyphArrangement::getStringWidth (font, "  ]") * hScale;
        const float startX = alignLeft
            ? static_cast<float> (area.getX())
            : (area.toFloat().getCentreX() - renderedW * 0.5f);

        g.drawFittedText (display, area, alignLeft ? juce::Justification::centredLeft : juce::Justification::centred, 1, 0.78f);
        const float caretX = std::min (static_cast<float> (area.getRight() - 8), startX + renderedW - closeW * 0.65f);
        const auto caretCol = (textColour == juce::Colours::white) ? juce::Colours::white : palette.mutedInk;
        drawDropdownCaret (g, caretX, area.toFloat().getCentreY() + 0.5f, caretCol);
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
