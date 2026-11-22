#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace braun {

// Dieter Rams / Braun dual-surface palette matching abmscribe BraunTheme:
// - Light: SK 4 / T 1000 warm anodized silver-white aluminum
// - Dark: Atelier P4 matte anodized graphite with warm silkscreen ink
struct Palette {
    bool isDark = false;
    juce::Colour chassis;
    juce::Colour panelWell;
    juce::Colour capSurface;
    juce::Colour hairline;
    juce::Colour controlBorder;
    juce::Colour ink;
    juce::Colour mutedInk;
    juce::Colour orange;
    juce::Colour orangeTint;
    juce::Colour cyan;
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
            juce::Colour (0xff6e6a61), // mutedInk (#6E6A61)
            juce::Colour (0xffec560e), // orange (#EC560E)
            juce::Colour (0xfff5dcd0), // orangeTint (#F5DCD0)
            juce::Colour (0xff2b93cf), // cyan (#2B93CF)
            juce::Colour (0xffc9402e), // muteRed
            juce::Colour (0xff2b93cf), // soloCyan
            juce::Colour (0xff131312), // scopeBg (#131312)
            juce::Colour (0xff1f221f), // scopeGrid (#1F221F)
            juce::Colour (0xffb6bab6), // scopeTrace (#B6BAB6)
            juce::Colour (0xffd9d4c9)  // faderSlotBg
        };
    }

    static Palette dark() noexcept {
        return {
            true,
            juce::Colour (0xff161615), // chassis (#161615)
            juce::Colour (0xff1e1e1c), // panelWell (#1E1E1C)
            juce::Colour (0xff272724), // capSurface (#272724)
            juce::Colour (0xff302f2c), // hairline (#302F2C)
            juce::Colour (0xff464540), // controlBorder (#464540)
            juce::Colour (0xffedebe6), // ink (#EDEBE6)
            juce::Colour (0xff9a968d), // mutedInk (#9A968D)
            juce::Colour (0xfff26018), // orange (#F26018)
            juce::Colour (0xff382116), // orangeTint (#382116)
            juce::Colour (0xff3ba4dc), // cyan (#3BA4DC)
            juce::Colour (0xffd95344), // muteRed
            juce::Colour (0xff3ba4dc), // soloCyan
            juce::Colour (0xff131312), // scopeBg (#131312)
            juce::Colour (0xff1f221f), // scopeGrid (#1F221F)
            juce::Colour (0xffb6bab6), // scopeTrace (#B6BAB6)
            juce::Colour (0xff0e0e0d)  // faderSlotBg
        };
    }

    static Palette forMode (bool darkMode) noexcept {
        return darkMode ? dark() : light();
    }
};

inline juce::Font monoFont (float size, bool bold = false) {
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          size, bold ? juce::Font::bold : juce::Font::plain));
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

class BraunLookAndFeel : public juce::LookAndFeel_V4 {
public:
    BraunLookAndFeel() { applyPalette (Palette::light()); }

    void applyPalette (const Palette& p) {
        palette = p;
        setColour (juce::Label::textColourId, p.ink);
        setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::TextButton::buttonColourId, p.capSurface);
        setColour (juce::TextButton::buttonOnColourId, p.orange);
        setColour (juce::TextButton::textColourOffId, p.ink);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::ComboBox::backgroundColourId, p.capSurface);
        setColour (juce::ComboBox::outlineColourId, p.controlBorder);
        setColour (juce::ComboBox::textColourId, p.ink);
        setColour (juce::ComboBox::arrowColourId, p.mutedInk);
        setColour (juce::PopupMenu::backgroundColourId, p.chassis);
        setColour (juce::PopupMenu::textColourId, p.ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, p.orange);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::PopupMenu::headerTextColourId, p.mutedInk);
        setColour (juce::Slider::textBoxTextColourId, p.ink);
        setColour (juce::Slider::textBoxBackgroundColourId, p.panelWell);
        setColour (juce::Slider::textBoxOutlineColourId, p.hairline);
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
        if (buttonHeight <= 17) return monoFont (8.5f, true);
        if (buttonHeight <= 20) return monoFont (9.8f, true);
        return monoFont (10.2f, true);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override {
        return monoFont (11.5f, true);
    }

    juce::Font getLabelFont (juce::Label& label) override {
        if (dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr)
            return monoFont (11.5f, true);
        return monoFont (10.5f, false);
    }

    juce::Font getPopupMenuFont() override {
        return sansFont (12.0f, false);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool isHighlighted, bool isDown) override {
        const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        auto fill = backgroundColour;
        if (isDown) fill = fill.darker (0.08f);
        else if (isHighlighted) fill = palette.isDark ? fill.brighter (0.08f) : fill.darker (0.04f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, 2.0f);
        const bool isAccent = (backgroundColour == palette.orange
                               || backgroundColour == palette.muteRed
                               || backgroundColour == palette.soloCyan);
        g.setColour (isAccent ? backgroundColour.darker (0.15f) : palette.controlBorder);
        g.drawRoundedRectangle (bounds, 2.0f, 1.0f);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool,
                       int, int, int, int, juce::ComboBox& box) override {
        const auto bounds = juce::Rectangle<float> (0.5f, 0.5f, width - 1.0f, height - 1.0f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (bounds, 2.0f);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (bounds, 2.0f, 1.0f);

        const float cx = static_cast<float> (width) - 10.0f;
        const float cy = static_cast<float> (height) * 0.5f;
        juce::Path tri;
        tri.addTriangle (cx - 3.2f, cy - 1.5f, cx + 3.2f, cy - 1.5f, cx, cy + 2.5f);
        g.setColour (palette.mutedInk);
        g.fillPath (tri);
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
        trackArc.addCentredArc (centre.x, centre.y, radius + 2.5f, radius + 2.5f,
                                0.0f, startAngle, endAngle, true);
        g.setColour (palette.hairline);
        g.strokePath (trackArc, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

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
            activeArc.addCentredArc (centre.x, centre.y, radius + 2.5f, radius + 2.5f,
                                     0.0f, std::min (originAngle, angle), std::max (originAngle, angle), true);
            const auto accent = isMidBipolar
                ? (angle < originAngle ? palette.cyan : palette.orange)
                : slider.findColour (juce::Slider::rotarySliderFillColourId);
            g.setColour (accent.isOpaque() ? accent : palette.orange);
            g.strokePath (activeArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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
        g.drawLine ({ at (angle, radius * 0.25f), at (angle, radius * 0.85f) }, 1.8f);
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
    Palette palette = Palette::light();
};

} // namespace braun
