#include "PluginEditor.h"
#include "DrumMaps.h"
#include "MidiExporter.h"
#include "PatternPresets.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
constexpr int kStripWidth = 188;
constexpr int kStripHeight = 538;
constexpr int kHeaderHeight = 78;
constexpr int kScrollBarH = 12;
constexpr int kMinLogicalW = 1040;
constexpr int kMaxLogicalW = 2400;
constexpr int kDefaultLogicalH = kHeaderHeight + kStripHeight + kScrollBarH;

const std::array<double, 5> kRates { 1.0 / 3.0, 0.5, 1.0, 2.0, 3.0 };
const std::array<const char*, 5> kRateLabels { "1/3", "1/2", "1x", "2x", "3x" };
const std::array<const char*, SliderRhythmMidiProcessor::controlCount> kControlNames {
    "Pattern fader", "DIV", "Velocity", "Swing %", "MOD amount", "Mute", "MOD shape",
    "MIDI note", "MIDI channel", "Pattern length", "Step grid", "Solo", "Global Swing"
};

const char* playbackModeLabel (sliderseq::PlaybackMode mode) noexcept {
    switch (mode) {
        case sliderseq::PlaybackMode::gen: return "GEN";
        case sliderseq::PlaybackMode::genPlusThru: return "GEN+THRU";
        case sliderseq::PlaybackMode::thru: return "THRU";
    }
    return "GEN";
}

juce::String formatSwingText (int swingVal) {
    if (swingVal > 0) return "+" + juce::String (swingVal) + "%";
    return juce::String (swingVal) + "%";
}

// Forward unhandled wheel events up the parent chain so Viewport scrolls smoothly on trackpad swipes.
void forwardWheelToAncestor (juce::Component* self, const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
    for (auto* p = self != nullptr ? self->getParentComponent() : nullptr; p != nullptr; p = p->getParentComponent()) {
        if (auto* vp = dynamic_cast<juce::Viewport*> (p)) {
            vp->mouseWheelMove (event.getEventRelativeTo (vp), wheel);
            return;
        }
    }
    if (auto* p = self != nullptr ? self->getParentComponent() : nullptr)
        p->mouseWheelMove (event.getEventRelativeTo (p), wheel);
}

class BraunZoneFaderLookAndFeel final : public braun::BraunLookAndFeel {
public:
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float, const juce::Slider::SliderStyle,
                           juce::Slider& slider) override {
        const auto& p = currentPalette();
        const int maxZone = std::max (1, static_cast<int> (std::lround (slider.getMaximum())));
        const int curVal = std::clamp (static_cast<int> (std::lround (slider.getValue())), 0, maxZone);
        const bool isEmpty = (curVal == 0);

        const float left = static_cast<float> (x);
        const float top = static_cast<float> (y);
        const float travelTop = top + 10.0f;
        const float travelBottom = top + static_cast<float> (height) - 10.0f;
        const float railX = left + static_cast<float> (width) * 0.62f;

        // Recessed vertical fader slot
        g.setColour (p.faderSlotBg);
        g.fillRoundedRectangle (railX - 2.5f, travelTop - 3.0f, 5.0f, (travelBottom - travelTop) + 6.0f, 1.5f);
        g.setColour (p.hairline);
        g.drawRoundedRectangle (railX - 2.5f, travelTop - 3.0f, 5.0f, (travelBottom - travelTop) + 6.0f, 1.5f, 1.0f);

        // Separator hairline midway between EMPTY (0) and Slot 1
        const float y0 = travelBottom;
        const float y1 = travelBottom - (travelBottom - travelTop) * (1.0f / static_cast<float> (maxZone));
        const float sepY = (y0 + y1) * 0.5f;
        g.setColour (p.hairline);
        g.drawHorizontalLine (static_cast<int> (std::round (sepY)), left + 4.0f, left + static_cast<float> (width) - 6.0f);

        // Calibrated ticks & labels for 0 (EMPTY) .. maxZone
        const bool sparseLabels = maxZone >= 10;
        g.setFont (braun::monoFont (9.0f, false));
        for (int z = 0; z <= maxZone; ++z) {
            const float prop = static_cast<float> (z) / static_cast<float> (maxZone);
            const float markY = travelBottom - (travelBottom - travelTop) * prop;
            const bool isFill = (maxZone >= 10 && z >= maxZone - 2) || (maxZone >= 4 && maxZone < 10 && z == maxZone);
            const bool isCurrent = (z == curVal);

            if (z == 0) {
                g.setColour (isEmpty ? p.orange : p.mutedInk);
                g.setFont (braun::monoFont (8.5f, isEmpty));
                g.drawText (isEmpty ? "0 EMPTY" : "0 EMPTY", x + 2, static_cast<int> (markY - 6),
                            static_cast<int> (railX - left) - 24, 12, juce::Justification::centredLeft);
                g.drawHorizontalLine (static_cast<int> (std::round (markY)), railX - 20.0f, railX - 5.0f);
                g.drawHorizontalLine (static_cast<int> (std::round (markY)), railX + 5.0f, railX + 20.0f);
            } else {
                g.setColour (isFill ? p.orange : (isCurrent ? p.ink : p.mutedInk));
                g.setFont (braun::monoFont (9.0f, isCurrent || isFill));
                const bool nearOddCurrent = sparseLabels && (curVal > 0) && (curVal % 2 != 0)
                                         && ! isCurrent && std::abs (z - curVal) == 1;
                const bool drawNum = ! sparseLabels
                    || isCurrent
                    || (! nearOddCurrent && (z == maxZone || (z % 2 == 0)));
                if (drawNum) {
                    g.drawText (juce::String (z), x + 2, static_cast<int> (markY - 6),
                                static_cast<int> (railX - left) - 28, 12, juce::Justification::centredRight);
                }
                const float tickHalf = isFill ? 22.0f : 16.0f;
                g.drawHorizontalLine (static_cast<int> (std::round (markY)), railX - tickHalf, railX - 5.0f);
                g.drawHorizontalLine (static_cast<int> (std::round (markY)), railX + 5.0f, railX + tickHalf);
            }
        }

        // Active travel accent line from slot 1 up to current cap (when not EMPTY)
        const float capY = juce::jlimit (travelTop, travelBottom, sliderPos);
        if (! isEmpty) {
            g.setColour (p.orange.withAlpha (0.75f));
            g.fillRect (railX - 1.0f, capY, 2.0f, std::max (0.0f, y1 - capY));
        }

        // Tactile flat Dieter Rams rectangular fader cap
        const juce::Rectangle<float> cap (railX - 21.0f, capY - 7.0f, 42.0f, 14.0f);
        g.setColour (isEmpty ? p.panelWell : p.capSurface);
        g.fillRoundedRectangle (cap, 2.0f);
        g.setColour (isEmpty ? p.orange : p.controlBorder);
        g.drawRoundedRectangle (cap, 2.0f, isEmpty ? 1.3f : 1.0f);
        g.setColour (isEmpty ? p.orange : p.ink);
        g.drawHorizontalLine (static_cast<int> (std::round (capY)), cap.getX() + 6.0f, cap.getRight() - 6.0f);
    }
};

class BraunSlider final : public juce::Slider {
public:
    std::function<bool()> interceptLearn;
    std::function<bool()> allowVerticalWheel;
    std::function<bool()> allowHorizontalWheel;

    void mouseDown (const juce::MouseEvent& event) override {
        learningPress = interceptLearn && interceptLearn();
        if (! learningPress) juce::Slider::mouseDown (event);
    }
    void mouseDrag (const juce::MouseEvent& event) override {
        if (! learningPress) juce::Slider::mouseDrag (event);
    }
    void mouseUp (const juce::MouseEvent& event) override {
        if (! learningPress) juce::Slider::mouseUp (event);
        learningPress = false;
    }
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override {
        const bool allowV = allowVerticalWheel ? allowVerticalWheel() : true;
        const bool allowH = allowHorizontalWheel ? allowHorizontalWheel() : false;
        const bool isHorizontal = std::abs (wheel.deltaX) > std::abs (wheel.deltaY);
        if (isHorizontal) {
            if (allowH) {
                auto filtered = wheel;
                filtered.deltaY = 0.0f;
                juce::Slider::mouseWheelMove (event, filtered);
            } else {
                forwardWheelToAncestor (this, event, wheel);
            }
            return;
        }
        if (allowV) {
            auto filtered = wheel;
            filtered.deltaX = 0.0f;
            juce::Slider::mouseWheelMove (event, filtered);
        } else {
            forwardWheelToAncestor (this, event, wheel);
        }
    }

private:
    bool learningPress = false;
};

class BraunButton : public juce::TextButton {
public:
    using juce::TextButton::TextButton;
    std::function<bool()> interceptLearn;
    std::function<void(const juce::MouseEvent&)> onMouseClick;

    void mouseDown (const juce::MouseEvent& event) override {
        learningPress = interceptLearn && interceptLearn();
        if (! learningPress) juce::TextButton::mouseDown (event);
    }
    void mouseUp (const juce::MouseEvent& event) override {
        if (! learningPress) {
            if (onMouseClick && event.mods.isLeftButtonDown() && getLocalBounds().contains (event.getPosition())) {
                onMouseClick (event);
                setState (buttonNormal);
            } else {
                juce::TextButton::mouseUp (event);
            }
        }
        learningPress = false;
    }

private:
    bool learningPress = false;
};

class BraunComboBox final : public juce::ComboBox {
public:
    std::function<bool()> interceptLearn;
    std::function<bool()> allowVerticalWheel;
    std::function<bool()> allowHorizontalWheel;

    void mouseDown (const juce::MouseEvent& event) override {
        if (interceptLearn && interceptLearn()) return;
        juce::ComboBox::mouseDown (event);
    }
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override {
        const bool allowV = allowVerticalWheel ? allowVerticalWheel() : true;
        const bool allowH = allowHorizontalWheel ? allowHorizontalWheel() : false;
        const bool isHorizontal = std::abs (wheel.deltaX) > std::abs (wheel.deltaY);
        if (isHorizontal) {
            if (allowH) juce::ComboBox::mouseWheelMove (event, wheel);
            else forwardWheelToAncestor (this, event, wheel);
            return;
        }
        if (allowV) juce::ComboBox::mouseWheelMove (event, wheel);
        else forwardWheelToAncestor (this, event, wheel);
    }
};

class BraunStepPad final : public juce::Component {
public:
    std::function<void()> onClick;

    void setState (bool enabledInGrid, bool activeStep, bool inActiveLoop, bool downbeatStep,
                   float velocityNorm, bool playheadNow, const braun::Palette& palette) {
        if (inGrid == enabledInGrid && active == activeStep && inLoop == inActiveLoop
            && downbeat == downbeatStep && std::abs (velNorm - velocityNorm) < 0.01f
            && playhead == playheadNow && pal.isDark == palette.isDark)
            return;
        inGrid = enabledInGrid;
        active = activeStep;
        inLoop = inActiveLoop;
        downbeat = downbeatStep;
        velNorm = velocityNorm;
        playhead = playheadNow;
        pal = palette;
        setEnabled (inGrid);
        repaint();
    }

    void setPlayheadOnly (bool playheadNow) {
        if (playhead == playheadNow) return;
        playhead = playheadNow;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& event) override {
        if (inGrid && event.mods.isLeftButtonDown() && onClick)
            onClick();
    }

    void paint (juce::Graphics& g) override {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        if (! inGrid) {
            g.setColour (pal.panelWell.withAlpha (0.45f));
            g.fillRoundedRectangle (bounds, 1.5f);
            g.setColour (pal.hairline.withAlpha (0.4f));
            g.drawRoundedRectangle (bounds, 1.5f, 0.75f);
            return;
        }

        const float alpha = inLoop ? 1.0f : 0.30f;
        if (active) {
            const float intensity = std::clamp (0.38f + 0.62f * velNorm, 0.38f, 1.0f);
            g.setColour (pal.orange.withAlpha (intensity * alpha));
            g.fillRoundedRectangle (bounds, 1.5f);
            g.setColour (pal.orange.withAlpha (0.9f * alpha));
            g.drawRoundedRectangle (bounds, 1.5f, 0.8f);
        } else {
            g.setColour ((downbeat ? pal.panelWell.darker (pal.isDark ? -0.06f : 0.06f) : pal.capSurface).withAlpha (alpha));
            g.fillRoundedRectangle (bounds, 1.5f);
            g.setColour (pal.hairline.withAlpha (alpha));
            g.drawRoundedRectangle (bounds, 1.5f, 0.8f);
        }

        if (playhead) {
            g.setColour (pal.isDark ? juce::Colours::white : pal.ink);
            g.drawRoundedRectangle (bounds.reduced (0.4f), 1.5f, 1.8f);
        }
    }

private:
    bool inGrid = true;
    bool active = false;
    bool inLoop = true;
    bool downbeat = false;
    float velNorm = 1.0f;
    bool playhead = false;
    braun::Palette pal = braun::Palette::light();
};
} // namespace

struct SliderRhythmMidiEditor::LookAndFeelResources {
    braun::BraunLookAndFeel main;
    BraunZoneFaderLookAndFeel zoneFader;
    std::atomic<bool> verticalWheel { true };
    std::atomic<bool> horizontalWheel { false };

    void applyPalette (const braun::Palette& p) {
        main.applyPalette (p);
        zoneFader.applyPalette (p);
    }
};

// Matte-black #131312 Braun oscilloscope monitor bay showing global note timing offset across 1 bar.
class SliderRhythmMidiEditor::GlobalSwingScope final : public juce::Component {
public:
    void setSwing (int globalSwing, const braun::Palette& palette) {
        if (swing == globalSwing && pal.isDark == palette.isDark) return;
        swing = globalSwing;
        pal = palette;
        repaint();
    }

    void paint (juce::Graphics& g) override {
        const auto b = getLocalBounds().toFloat();
        g.setColour (pal.scopeBg);
        g.fillRoundedRectangle (b, 2.0f);
        g.setColour (pal.controlBorder);
        g.drawRoundedRectangle (b.reduced (0.5f), 2.0f, 1.0f);

        const float padX = 12.0f;
        const float badgeW = 78.0f;
        const float plotRight = std::max (padX + 40.0f, b.getRight() - badgeW - 6.0f);
        const float usableW = std::max (20.0f, plotRight - padX);
        const float midY = b.getCentreY();
        const float topY = b.getY() + 5.0f;
        const float botY = b.getBottom() - 5.0f;

        // Horizontal center line & divider before mode readout badge
        g.setColour (pal.scopeGrid);
        g.drawHorizontalLine (static_cast<int> (std::round (midY)), b.getX() + 4.0f, plotRight + 4.0f);
        g.drawVerticalLine (static_cast<int> (std::round (plotRight + 5.0f)), topY, botY);

        // 8 sixteenth-note pulses + end anchor over 2 beats (0..576 ticks) for unmistakable displacement readability
        constexpr double spanTicks = 576.0;
        for (int s = 0; s <= 8; ++s) {
            const double nominal = s * (spanTicks / 8.0);
            const float baseX = padX + static_cast<float> (nominal / spanTicks) * usableW;
            const bool isQuarter = (s % 4 == 0);
            g.setColour (isQuarter ? pal.scopeTrace.withAlpha (0.45f) : pal.scopeGrid.brighter (0.22f));
            g.drawLine (baseX, topY, baseX, botY, isQuarter ? 1.1f : 0.8f);

            const double offset = sliderseq::grooveOffsetValue (swing, 100, nominal);
            const float shiftedX = padX + static_cast<float> ((nominal + offset) / spanTicks) * usableW;
            const bool moved = std::abs (offset) > 0.5;
            const float dotY = isQuarter ? midY - 6.0f : (s % 2 == 0 ? midY + 4.5f : midY + 1.0f);

            if (moved) {
                g.setColour (pal.cyan.withAlpha (0.90f));
                g.drawLine (baseX, dotY, shiftedX, dotY, 2.0f);
            }
            g.setColour (moved ? pal.cyan : (isQuarter ? pal.orange : pal.scopeTrace.withAlpha (0.72f)));
            const float r = isQuarter ? 2.8f : (moved ? 2.5f : 2.0f);
            g.fillEllipse (shiftedX - r, dotY - r, r * 2.0f, r * 2.0f);
        }

        // Dedicated non-overlapping mode indicator bay on the right
        g.setFont (braun::monoFont (8.5f, true));
        g.setColour (swing == 0 ? pal.scopeTrace.withAlpha (0.65f) : pal.cyan);
        const juce::String modeTxt = swing == 0 ? "STRAIGHT"
                                   : swing < 0 ? "16TH SHUF" : "8TH SWING";
        g.drawText (modeTxt, static_cast<int> (plotRight + 8.0f), 0, static_cast<int> (badgeW - 6.0f), getHeight(),
                    juce::Justification::centred);
    }

private:
    int swing = 0;
    braun::Palette pal = braun::Palette::light();
};

// Matte-black #131312 Braun oscilloscope bay visualizing per-step modulated velocity bars + envelope trace.
class SliderRhythmMidiEditor::VelocityModScope final : public juce::Component {
public:
    void setTrackData (const sliderseq::Track& track, int activeStepIndex, const braun::Palette& palette) {
        const int patIdx = std::clamp (track.slot > 0 ? track.slot - 1 : 0, 0, sliderseq::slotsPerTrack - 1);
        const auto& pat = track.patterns[static_cast<size_t> (patIdx)];
        activeLen = sliderseq::effectivePatternLength (pat.length, pat.grid, track.loopStepsOverride);
        baseVel = std::clamp (static_cast<int> (std::lround (track.velocity * std::clamp (pat.velocityScale, 0.1f, 2.0f))), 1, 127);
        playheadStep = activeStepIndex;
        isEmpty = (track.slot == 0);
        pal = palette;
        for (int s = 0; s < activeLen && s < 64; ++s) {
            stepActive[static_cast<size_t> (s)] = pat.steps[static_cast<size_t> (s)];
            stepVel[static_cast<size_t> (s)] = sliderseq::stepModulatedVelocity (track, pat, s);
        }
        repaint();
    }

    void setPlayheadStep (int step) {
        if (playheadStep == step) return;
        playheadStep = step;
        repaint();
    }

    void paint (juce::Graphics& g) override {
        const auto b = getLocalBounds().toFloat();
        g.setColour (pal.scopeBg);
        g.fillRoundedRectangle (b, 2.0f);
        g.setColour (pal.controlBorder);
        g.drawRoundedRectangle (b.reduced (0.5f), 2.0f, 1.0f);

        const float padX = 5.0f;
        const float padTop = 5.0f;
        const float padBot = 4.0f;
        const float plotW = std::max (10.0f, b.getWidth() - padX * 2.0f);
        const float plotH = std::max (10.0f, b.getHeight() - padTop - padBot);
        const float bottomY = b.getBottom() - padBot;

        // Subtle horizontal scope gridlines (25%, 50%, 75%)
        g.setColour (pal.scopeGrid);
        for (float frac : { 0.25f, 0.50f, 0.75f })
            g.drawHorizontalLine (static_cast<int> (std::round (bottomY - plotH * frac)), b.getX() + 3.0f, b.getRight() - 3.0f);

        // Dashed horizontal baseline for unmodulated velocity
        const float baseY = bottomY - plotH * (static_cast<float> (baseVel) / 127.0f);
        g.setColour (pal.scopeTrace.withAlpha (0.35f));
        for (float dx = padX; dx < b.getRight() - padX; dx += 6.0f)
            g.drawHorizontalLine (static_cast<int> (std::round (baseY)), dx, std::min (dx + 3.0f, b.getRight() - padX));

        const int count = std::clamp (activeLen, 1, 64);
        const float stepW = plotW / static_cast<float> (count);
        const float barW = std::max (1.5f, stepW - (count > 24 ? 0.8f : 2.0f));
        const float alphaDim = isEmpty ? 0.35f : 1.0f;

        juce::Path envelope;
        for (int s = 0; s < count; ++s) {
            const float vNorm = static_cast<float> (stepVel[static_cast<size_t> (s)]) / 127.0f;
            const float barH = std::max (2.0f, plotH * vNorm);
            const float cx = padX + (static_cast<float> (s) + 0.5f) * stepW;
            const float topY = bottomY - barH;

            if (s == playheadStep && ! isEmpty) {
                g.setColour (juce::Colours::white.withAlpha (0.14f));
                g.fillRect (padX + static_cast<float> (s) * stepW, b.getY() + 2.0f, stepW, b.getHeight() - 4.0f);
            }

            const bool hit = stepActive[static_cast<size_t> (s)];
            g.setColour ((hit ? pal.orange : pal.scopeTrace.withAlpha (0.28f)).withAlpha (hit ? 0.88f * alphaDim : 0.28f * alphaDim));
            g.fillRect (cx - barW * 0.5f, topY, barW, barH);

            if (s == 0) envelope.startNewSubPath (cx, topY);
            else envelope.lineTo (cx, topY);
        }

        // Crisp Braun orange modulation envelope trace
        g.setColour (pal.orange.withAlpha (0.95f * alphaDim));
        g.strokePath (envelope, juce::PathStrokeType (1.25f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    int activeLen = 16;
    int baseVel = 100;
    int playheadStep = -1;
    bool isEmpty = false;
    std::array<bool, 64> stepActive {};
    std::array<int, 64> stepVel {};
    braun::Palette pal = braun::Palette::light();
};

// Matte-black #131312 Braun oscilloscope bay showing per-track note timing shift (Global Swing x Track Swing %).
class SliderRhythmMidiEditor::TrackSwingScope final : public juce::Component {
public:
    void setTiming (int globalSwingVal, int trackSwingPercentVal, const braun::Palette& palette) {
        if (globalSwing == globalSwingVal && trackPercent == trackSwingPercentVal && pal.isDark == palette.isDark)
            return;
        globalSwing = globalSwingVal;
        trackPercent = trackSwingPercentVal;
        pal = palette;
        repaint();
    }

    void paint (juce::Graphics& g) override {
        const auto b = getLocalBounds().toFloat();
        g.setColour (pal.scopeBg);
        g.fillRoundedRectangle (b, 2.0f);
        g.setColour (pal.controlBorder);
        g.drawRoundedRectangle (b.reduced (0.5f), 2.0f, 1.0f);

        const float padX = 9.0f;
        const float usableW = std::max (12.0f, b.getWidth() - padX * 2.0f);
        const float midY = b.getCentreY();
        const float topY = b.getY() + 5.0f;
        const float botY = b.getBottom() - 5.0f;

        g.setColour (pal.scopeGrid);
        g.drawHorizontalLine (static_cast<int> (std::round (midY)), b.getX() + 3.0f, b.getRight() - 3.0f);

        // 1 beat zoomed view (0..288 ticks: 1, e, &, a, 2) so per-track Swing % shifts are immediately unmistakable.
        constexpr double spanTicks = 288.0;
        for (int s = 0; s <= 4; ++s) {
            const double nominal = s * (spanTicks / 4.0);
            const float baseX = padX + static_cast<float> (nominal / spanTicks) * usableW;
            const bool isBeat = (s == 0 || s == 4);
            const bool isEighth = (s == 2);
            g.setColour (isBeat ? pal.scopeTrace.withAlpha (0.48f) : pal.scopeGrid.brighter (0.24f));
            g.drawLine (baseX, topY, baseX, botY, isBeat ? 1.15f : 0.85f);

            const double offset = sliderseq::grooveOffsetValue (globalSwing, trackPercent, nominal);
            const float shiftedX = padX + static_cast<float> ((nominal + offset) / spanTicks) * usableW;
            const bool moved = std::abs (offset) > 0.5;
            const float dotY = isBeat ? midY - 6.0f : (isEighth ? midY + 5.0f : midY);

            if (moved) {
                g.setColour (pal.cyan.withAlpha (0.92f));
                g.drawLine (baseX, dotY, shiftedX, dotY, 2.2f);
            }
            g.setColour (moved ? pal.cyan : (isBeat ? pal.orange.withAlpha (trackPercent > 0 && globalSwing != 0 ? 0.9f : 0.55f)
                                                    : pal.scopeTrace.withAlpha (0.65f)));
            const float r = isBeat ? 2.6f : (moved ? 2.6f : 2.0f);
            g.fillEllipse (shiftedX - r, dotY - r, r * 2.0f, r * 2.0f);
        }
    }

private:
    int globalSwing = 0;
    int trackPercent = 100;
    braun::Palette pal = braun::Palette::light();
};

class SliderRhythmMidiEditor::MidiDragHandle final : public juce::Component, public juce::SettableTooltipClient {
public:
    std::function<juce::File()> onCreateFile;
    std::function<void()> onRightClick;
    std::function<juce::String()> getLabelText;

    MidiDragHandle() {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        setTooltip ("Drag into Ableton / Bitwig / Logic to export a MIDI clip (right-click to choose bars)");
    }

    void setPalette (const braun::Palette& p) { pal = p; repaint(); }

    void paint (juce::Graphics& g) override {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (dragging ? pal.orange : pal.capSurface);
        g.fillRoundedRectangle (bounds, 2.0f);
        g.setColour (dragging ? pal.orange : pal.controlBorder);
        g.drawRoundedRectangle (bounds, 2.0f, 1.0f);
        g.setColour (dragging ? juce::Colours::white : pal.ink);
        g.setFont (braun::monoFont (10.0f, true));
        const auto text = getLabelText ? getLabelText() : juce::String ("[ DRAG 4B ]");
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& event) override {
        dragStarted = false;
        if (event.mods.isPopupMenu() && onRightClick) {
            onRightClick();
            return;
        }
        dragging = true;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& event) override {
        if (dragStarted || event.mods.isPopupMenu() || event.getDistanceFromDragStart() < 4)
            return;
        dragStarted = true;
        if (! onCreateFile) return;
        const auto file = onCreateFile();
        if (file.existsAsFile())
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
        dragging = false;
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override {
        dragging = false;
        dragStarted = false;
        repaint();
    }

private:
    bool dragging = false;
    bool dragStarted = false;
    braun::Palette pal = braun::Palette::light();
};

class SliderRhythmMidiEditor::TrackStrip final : public juce::Component {
public:
    std::function<bool(int, SliderRhythmMidiProcessor::Control)> onControlLearn;
    std::function<void(const sliderseq::Pattern&)> onCopyPattern;
    std::function<std::optional<sliderseq::Pattern>()> onGetClipboard;
    std::function<void()> onTracksModified;

    TrackStrip (SliderRhythmMidiProcessor& owner, int number,
                juce::SharedResourcePointer<LookAndFeelResources> sharedLnf)
        : processor (owner), index (number), lookAndFeel (sharedLnf) {
        auto bindWheel = [this] (auto& comp) {
            comp.allowVerticalWheel = [this] { return lookAndFeel->verticalWheel.load (std::memory_order_relaxed); };
            comp.allowHorizontalWheel = [this] { return lookAndFeel->horizontalWheel.load (std::memory_order_relaxed); };
        };

        addAndMakeVisible (titleButton);
        titleButton.setTooltip ("Track menu: rename, clear, copy/paste slot, duplicate track, MOD shape, gate, choke, velocity scale, loop cap");
        titleButton.onClick = [this] { showTrackOptionsMenu(); };

        addAndMakeVisible (zonesButton);
        zonesButton.setTooltip ("Set number of active pattern zones on this track's fader (1-12 above EMPTY)");
        zonesButton.onClick = [this] { showZonesMenu(); };

        muteButton.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::mute); };
        soloButton.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::solo); };
        muteButton.setTooltip ("Mute track");
        soloButton.setTooltip ("Solo track (exclusive; Cmd/Ctrl+click for multi-track solo)");
        addAndMakeVisible (muteButton);
        addAndMakeVisible (soloButton);

        muteButton.onMouseClick = [this] (const juce::MouseEvent&) {
            const auto current = processor.snapshot();
            const bool nextMuted = ! current->sequence.tracks[static_cast<size_t> (index)].muted;
            processor.setHostParamValue (index, "mute", nextMuted ? 1.0f : 0.0f);
            refresh();
        };
        soloButton.onMouseClick = [this] (const juce::MouseEvent& event) {
            if (event.mods.isCommandDown() || event.mods.isCtrlDown())
                processor.toggleTrackSoloAdditive (index);
            else
                processor.soloTrackExclusive (index);
            if (onTracksModified) onTracksModified();
        };

        // Hero configurable-zone vertical fader (0 = EMPTY, 1..activeZones = active patterns)
        slot.setSliderStyle (juce::Slider::LinearVertical);
        slot.setLookAndFeel (&lookAndFeel->zoneFader);
        slot.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slot.setRange (0, 12, 1);
        slot.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::slot); };
        bindWheel (slot);
        slot.onDragStart = [this] {
            if (auto* p = processor.parameters().getParameter (SliderRhythmMidiProcessor::paramId (index, "slot")))
                p->beginChangeGesture();
        };
        slot.onDragEnd = [this] {
            if (auto* p = processor.parameters().getParameter (SliderRhythmMidiProcessor::paramId (index, "slot")))
                p->endChangeGesture();
        };
        slot.onValueChange = [this] {
            if (suppressCallbacks) return;
            const int v = static_cast<int> (std::lround (slot.getValue()));
            if (v > 0) lastNonEmptySlot = v;
            processor.setHostParamValue (index, "slot", static_cast<float> (v));
            refresh();
        };
        addAndMakeVisible (slot);

        // Rate segmented pill row
        for (int i = 0; i < 5; ++i) {
            auto& btn = rateButtons[static_cast<size_t> (i)];
            btn.setButtonText (kRateLabels[static_cast<size_t> (i)]);
            btn.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::rate); };
            btn.onClick = [this, i] {
                processor.setHostParamValue (index, "rate", static_cast<float> (i));
                refresh();
            };
            addAndMakeVisible (btn);
        }

        // Compact flat Braun rotary knobs (no bulky text boxes underneath — crisp value labels drawn cleanly)
        auto setupKnob = [&] (BraunSlider& s, double lo, double hi, double init, SliderRhythmMidiProcessor::Control ctrl) {
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.setRange (lo, hi, 1.0);
            s.setValue (init, juce::dontSendNotification);
            s.interceptLearn = [this, ctrl] { return intercept (ctrl); };
            bindWheel (s);
            addAndMakeVisible (s);
        };
        setupKnob (velocity, 1, 127, 100, SliderRhythmMidiProcessor::velocity);
        setupKnob (modAmount, 0, 100, 50, SliderRhythmMidiProcessor::modAmount);
        setupKnob (swingPct, 0, 100, 100, SliderRhythmMidiProcessor::groove);

        velocityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "velocity"), velocity);
        modAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "mod_amt"), modAmount);
        swingPctAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "groove"), swingPct);

        velocity.onValueChange = [this] { if (! suppressCallbacks) refreshVisualsOnly(); };
        modAmount.onValueChange = [this] { if (! suppressCallbacks) refreshVisualsOnly(); };
        swingPct.onValueChange = [this] { if (! suppressCallbacks) refreshVisualsOnly(); };

        modButton.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::modMode); };
        modButton.onClick = [this] { showModShapeMenu(); };
        addAndMakeVisible (modButton);

        addAndMakeVisible (velocityScope);
        addAndMakeVisible (swingScope);

        patternName.onClick = [this] { showPatternMenu(); };
        patternName.setTooltip ("Load a beat or save this slot as a preset");
        addAndMakeVisible (patternName);

        // Compact numeric drag-boxes for NOTE, CH, LEN + GRID combo
        auto setupDragBox = [&] (BraunSlider& s, double lo, double hi, double init, SliderRhythmMidiProcessor::Control ctrl) {
            s.setSliderStyle (juce::Slider::LinearBarVertical);
            s.setRange (lo, hi, 1.0);
            s.setValue (init, juce::dontSendNotification);
            s.setVelocityBasedMode (false);
            s.setMouseDragSensitivity (160);
            s.interceptLearn = [this, ctrl] { return intercept (ctrl); };
            bindWheel (s);
            addAndMakeVisible (s);
        };
        setupDragBox (note, 0, 127, 36, SliderRhythmMidiProcessor::note);
        setupDragBox (channel, 1, 16, 10, SliderRhythmMidiProcessor::channel);
        setupDragBox (length, 1, 64, 16, SliderRhythmMidiProcessor::length);

        note.onValueChange = [this] {
            if (suppressCallbacks) return;
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setMidiNote;
            op.track = index;
            op.arg0 = static_cast<int> (std::lround (note.getValue()));
            processor.applyUiOp (op);
            repaint();
        };
        channel.onValueChange = [this] {
            if (suppressCallbacks) return;
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setMidiChannel;
            op.track = index;
            op.arg0 = static_cast<int> (std::lround (channel.getValue()));
            processor.applyUiOp (op);
            repaint();
        };
        length.onValueChange = [this] {
            if (suppressCallbacks) return;
            const auto current = processor.snapshot();
            const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
            const int patIdx = activePatternIndex (t);
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setPatternLength;
            op.track = index;
            op.slotIndex = patIdx;
            op.arg0 = static_cast<int> (std::lround (length.getValue()));
            processor.applyUiOp (op);
            refresh();
        };

        grid.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::grid); };
        bindWheel (grid);
        for (const auto gridSize : { 16, 12, 32, 48, 64 }) grid.addItem (juce::String (gridSize), gridSize);
        grid.onChange = [this] {
            if (suppressCallbacks) return;
            const auto current = processor.snapshot();
            const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
            const int patIdx = activePatternIndex (t);
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::resizePattern;
            op.track = index;
            op.slotIndex = patIdx;
            op.arg0 = grid.getSelectedId();
            processor.applyUiOp (op);
            refresh();
        };
        addAndMakeVisible (grid);

        for (int i = 0; i < 64; ++i) {
            steps[static_cast<size_t> (i)] = std::make_unique<BraunStepPad>();
            auto& pad = *steps[static_cast<size_t> (i)];
            pad.onClick = [this, i] {
                const auto current = processor.snapshot();
                const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
                const int patIdx = activePatternIndex (t);
                const bool bit = t.patterns[static_cast<size_t> (patIdx)].steps[static_cast<size_t> (i)];
                SliderRhythmMidiProcessor::StateOp op;
                op.kind = SliderRhythmMidiProcessor::OpKind::setStep;
                op.track = index;
                op.slotIndex = patIdx;
                op.arg0 = i;
                op.barg = ! bit;
                processor.applyUiOp (op);
                refresh();
            };
            addAndMakeVisible (pad);
        }

        applyTheme();
        refresh();
    }

    ~TrackStrip() override {
        if (activeDialog != nullptr)
            activeDialog->exitModalState (0);
        velocityAttachment.reset();
        modAmountAttachment.reset();
        swingPctAttachment.reset();
        slot.setLookAndFeel (nullptr);
    }

    void applyTheme() {
        const auto& p = lookAndFeel->main.currentPalette();
        velocity.setColour (juce::Slider::rotarySliderFillColourId, p.orange);
        modAmount.setColour (juce::Slider::rotarySliderFillColourId, p.orange);
        swingPct.setColour (juce::Slider::rotarySliderFillColourId, p.cyan);
        for (auto* dragBox : { &note, &channel, &length }) {
            dragBox->setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
            dragBox->setColour (juce::Slider::backgroundColourId, p.capSurface);
            dragBox->setColour (juce::Slider::textBoxTextColourId, p.ink);
            dragBox->setColour (juce::Slider::textBoxOutlineColourId, p.controlBorder);
        }
        refresh();
    }

    void updatePlayhead() {
        const int nowStep = processor.getActiveStep (index);
        if (nowStep == displayedPlayheadStep) return;
        if (displayedPlayheadStep >= 0 && displayedPlayheadStep < 64)
            steps[static_cast<size_t> (displayedPlayheadStep)]->setPlayheadOnly (false);
        if (nowStep >= 0 && nowStep < 64)
            steps[static_cast<size_t> (nowStep)]->setPlayheadOnly (true);
        velocityScope.setPlayheadStep (nowStep);
        displayedPlayheadStep = nowStep;
    }

    void refreshVisualsOnly() {
        const auto state = processor.snapshot();
        const auto& track = state->sequence.tracks[static_cast<size_t> (index)];
        const auto& p = lookAndFeel->main.currentPalette();
        cachedVelText = juce::String (static_cast<int> (std::lround (velocity.getValue())));
        cachedAmtText = juce::String (static_cast<int> (std::lround (modAmount.getValue())));
        cachedSwingText = juce::String (static_cast<int> (std::lround (swingPct.getValue()))) + "%";

        sliderseq::Track previewTrack = track;
        previewTrack.velocity = static_cast<int> (std::lround (velocity.getValue()));
        previewTrack.modAmount = static_cast<int> (std::lround (modAmount.getValue()));
        previewTrack.groove = static_cast<int> (std::lround (swingPct.getValue()));
        velocityScope.setTrackData (previewTrack, processor.getActiveStep (index), p);
        swingScope.setTiming (state->sequence.globalSwing, previewTrack.groove, p);
        repaint();
    }

    void refresh() {
        const auto state = processor.snapshot();
        const auto& track = state->sequence.tracks[static_cast<size_t> (index)];
        if (track.slot > 0) lastNonEmptySlot = track.slot;
        const int patIdx = activePatternIndex (track);
        const auto& pattern = track.patterns[static_cast<size_t> (patIdx)];
        const auto& p = lookAndFeel->main.currentPalette();

        suppressCallbacks = true;
        const juce::String numPrefix = juce::String (index + 1).paddedLeft ('0', 2) + " ";
        titleButton.setButtonText (numPrefix + juce::String (track.name));
        zonesButton.setButtonText ("ZONES: " + juce::String (track.activeZones));

        const int clampedZones = std::clamp (track.activeZones, 1, sliderseq::slotsPerTrack);
        slot.setRange (0, clampedZones, 1);
        slot.setValue (std::clamp (track.slot, 0, clampedZones), juce::dontSendNotification);

        velocity.setValue (track.velocity, juce::dontSendNotification);
        modAmount.setValue (track.modAmount, juce::dontSendNotification);
        swingPct.setValue (track.groove, juce::dontSendNotification);
        note.setValue (track.midiNote, juce::dontSendNotification);
        channel.setValue (track.midiChannel, juce::dontSendNotification);

        const int activeLen = sliderseq::effectivePatternLength (pattern.length, pattern.grid, track.loopStepsOverride);
        length.setRange (1, std::max (2, pattern.grid), 1);
        length.setEnabled (pattern.grid > 1);
        length.setValue (pattern.length, juce::dontSendNotification);
        grid.setSelectedId (pattern.grid, juce::dontSendNotification);
        suppressCallbacks = false;

        const auto midDot = juce::String::fromUTF8 (" \xc2\xb7 ");
        const auto dropTri = juce::String::fromUTF8 (" \xe2\x96\xbe");
        cachedSpecLine = braun::midiNoteName (track.midiNote) + midDot + juce::String (track.midiNote)
                       + midDot + "CH" + juce::String (track.midiChannel);
        cachedIsEmpty = (track.slot == 0);
        cachedLoopCap = track.loopStepsOverride;
        cachedSlotBadge = cachedIsEmpty
            ? juce::String ("EMPTY")
            : ("SLOT " + juce::String (track.slot).paddedLeft ('0', 2));
        cachedVelText = juce::String (track.velocity);
        cachedAmtText = juce::String (track.modAmount);
        cachedSwingText = juce::String (track.groove) + "%";

        modButton.setButtonText (juce::String (sliderseq::modShapeLabel (track.modShape)) + dropTri);
        patternName.setButtonText (cachedIsEmpty
            ? ("EMPTY  [SLOT " + juce::String (patIdx + 1) + " PAUSED]")
            : (juce::String (patIdx + 1) + " " + juce::String (pattern.name) + dropTri));

        for (int i = 0; i < 5; ++i) {
            const bool activeRate = std::abs (track.rate - kRates[static_cast<size_t> (i)]) < 1e-6;
            rateButtons[static_cast<size_t> (i)].setColour (juce::TextButton::buttonColourId, activeRate ? p.orange : p.capSurface);
            rateButtons[static_cast<size_t> (i)].setColour (juce::TextButton::textColourOffId, activeRate ? juce::Colours::white : p.ink);
        }

        muteButton.setColour (juce::TextButton::buttonColourId, track.muted ? p.muteRed : p.capSurface);
        muteButton.setColour (juce::TextButton::textColourOffId, track.muted ? juce::Colours::white : p.ink);
        soloButton.setColour (juce::TextButton::buttonColourId, track.solo ? p.soloCyan : p.capSurface);
        soloButton.setColour (juce::TextButton::textColourOffId, track.solo ? juce::Colours::white : p.ink);

        const int nowStep = processor.getActiveStep (index);
        displayedPlayheadStep = nowStep;
        velocityScope.setTrackData (track, nowStep, p);
        swingScope.setTiming (state->sequence.globalSwing, track.groove, p);

        const int beatDiv = std::max (1, pattern.subdivision / 4);
        for (int i = 0; i < 64; ++i) {
            const bool inGrid = (i < pattern.grid);
            const bool enabled = pattern.steps[static_cast<size_t> (i)];
            const bool inLoop = (i < activeLen) && ! cachedIsEmpty;
            const bool downbeat = (i % beatDiv == 0);
            const float velNorm = inGrid
                ? static_cast<float> (sliderseq::stepModulatedVelocity (track, pattern, i)) / 127.0f
                : 0.0f;
            steps[static_cast<size_t> (i)]->setState (inGrid, enabled, inLoop, downbeat, velNorm, i == nowStep, p);
        }

        repaint();
    }

    void paint (juce::Graphics& g) override {
        const auto& p = lookAndFeel->main.currentPalette();
        const int w = getWidth();
        const int h = getHeight();

        // Pure Braun architectural surface with crisp 1px structural hairlines (no chunky drop shadows)
        g.fillAll (cachedIsEmpty ? p.panelWell : p.chassis);
        g.setColour (p.hairline);
        g.drawVerticalLine (w - 1, 0.0f, static_cast<float> (h));

        for (int hy : { 44, 206, 235, 345, 402 })
            g.drawHorizontalLine (hy, 0.0f, static_cast<float> (w - 1));

        // Track spec subline under title
        g.setFont (braun::monoFont (9.5f, false));
        g.setColour (p.mutedInk);
        g.drawText (cachedSpecLine, 8, 25, w - 70, 15, juce::Justification::centredLeft);

        if (cachedLoopCap > 0) {
            g.setFont (braun::monoFont (9.0f, true));
            g.setColour (p.cyan);
            g.drawText ("CAP " + juce::String (cachedLoopCap), w - 62, 25, 54, 15, juce::Justification::centredRight);
        }

        // Slot status badge (top right of fader section, with vector LED dot when EMPTY)
        g.setFont (braun::monoFont (9.5f, true));
        g.setColour (cachedIsEmpty ? p.orange : p.mutedInk);
        if (cachedIsEmpty) {
            g.fillEllipse (static_cast<float> (w - 55), 54.5f, 5.5f, 5.5f);
            g.drawText ("EMPTY", w - 47, 49, 39, 17, juce::Justification::centredRight);
        } else {
            g.drawText (cachedSlotBadge, w - 88, 49, 80, 17, juce::Justification::centredRight);
        }

        // Velocity / MOD / AMT micro-labels & numeric readouts
        const int usable = w - 16;
        const int knobColW = 44;
        const int modX = 8 + knobColW + 4;
        const int modW = std::max (56, usable - 2 * knobColW - 8);
        const int amtX = w - 8 - knobColW;

        g.setFont (braun::monoFont (8.5f, true));
        g.setColour (p.mutedInk);
        g.drawText ("VEL", 8, 239, knobColW, 11, juce::Justification::centred);
        g.drawText ("MOD", modX, 239, modW, 11, juce::Justification::centred);
        g.drawText ("AMT", amtX, 239, knobColW, 11, juce::Justification::centred);

        g.setFont (braun::monoFont (9.5f, true));
        g.setColour (p.ink);
        g.drawText (cachedVelText, 8, 280, knobColW, 12, juce::Justification::centred);
        g.drawText (cachedAmtText, amtX, 280, knobColW, 12, juce::Justification::centred);

        // SWING % micro-label & readout
        g.setFont (braun::monoFont (8.5f, true));
        g.setColour (p.mutedInk);
        g.drawText ("SWING %", 8, 349, 54, 11, juce::Justification::centred);
        g.setFont (braun::monoFont (9.5f, true));
        g.setColour (p.ink);
        g.drawText (cachedSwingText, 8, 386, 54, 12, juce::Justification::centred);

        // Compact NOTE / CH / LEN / GRID labels
        const int pGap = 3;
        const int gridW = std::max (48, usable * 30 / 100);
        const int boxW = std::max (28, (usable - gridW - 3 * pGap) / 3);
        const int xNote = 8;
        const int xCh = xNote + boxW + pGap;
        const int xLen = xCh + boxW + pGap;
        const int xGrid = xLen + boxW + pGap;
        g.setFont (braun::monoFont (8.0f, true));
        g.setColour (p.mutedInk);
        g.drawText ("NOTE", xNote, 430, boxW, 11, juce::Justification::centred);
        g.drawText ("CH", xCh, 430, boxW, 11, juce::Justification::centred);
        g.drawText (cachedLoopCap > 0 ? "CAP" : "LEN", xLen, 430, boxW, 11, juce::Justification::centred);
        g.drawText ("GRID", xGrid, 430, (w - 8) - xGrid, 11, juce::Justification::centred);
    }

    void resized() override {
        const int w = getWidth();
        const int usable = w - 16;

        // Cell A: Track Header
        titleButton.setBounds (8, 5, usable - 58, 19);
        muteButton.setBounds (w - 62, 5, 25, 19);
        soloButton.setBounds (w - 33, 5, 25, 19);

        // Cell B: Configurable-Zone Vertical Fader
        zonesButton.setBounds (8, 49, 78, 17);
        slot.setBounds (8, 69, usable, 132);

        // Cell C: Rate Segmented Pills
        const int rateGap = 2;
        const int rateW = (usable - 4 * rateGap) / 5;
        for (int i = 0; i < 5; ++i)
            rateButtons[static_cast<size_t> (i)].setBounds (8 + i * (rateW + rateGap), 211, rateW, 19);

        // Cell D: VEL / MOD / AMT + VelocityModScope
        const int knobColW = 44;
        const int modX = 8 + knobColW + 4;
        const int modW = std::max (56, usable - 2 * knobColW - 8);
        const int amtX = w - 8 - knobColW;
        velocity.setBounds (8 + (knobColW - 28) / 2, 250, 28, 28);
        modButton.setBounds (modX, 254, modW, 22);
        modAmount.setBounds (amtX + (knobColW - 28) / 2, 250, 28, 28);
        velocityScope.setBounds (8, 294, usable, 45);

        // Cell E: SWING % + TrackSwingScope
        swingPct.setBounds (23, 360, 24, 24);
        swingScope.setBounds (64, 354, w - 72, 38);

        // Cell F: Pattern Preset, Compact NOTE/CH/LEN/GRID, 64-step Matrix
        patternName.setBounds (8, 407, usable, 19);
        const int pGap = 3;
        const int gridW = std::max (48, usable * 30 / 100);
        const int boxW = std::max (28, (usable - gridW - 3 * pGap) / 3);
        const int xNote = 8;
        const int xCh = xNote + boxW + pGap;
        const int xLen = xCh + boxW + pGap;
        const int xGrid = xLen + boxW + pGap;
        note.setBounds (xNote, 442, boxW, 19);
        channel.setBounds (xCh, 442, boxW, 19);
        length.setBounds (xLen, 442, boxW, 19);
        grid.setBounds (xGrid, 442, (w - 8) - xGrid, 19);

        const int stepGap = 2;
        const int stepW = std::max (6, (usable - 15 * stepGap) / 16);
        const int rowW = 16 * stepW + 15 * stepGap;
        const int padStartX = 8 + (usable - rowW) / 2;
        for (int i = 0; i < 64; ++i)
            steps[static_cast<size_t> (i)]->setBounds (
                padStartX + (i % 16) * (stepW + stepGap),
                468 + (i / 16) * 16,
                stepW, 13);
    }

private:
    int activePatternIndex (const sliderseq::Track& track) const noexcept {
        if (track.slot > 0) return std::clamp (track.slot - 1, 0, sliderseq::slotsPerTrack - 1);
        return std::clamp (lastNonEmptySlot - 1, 0, sliderseq::slotsPerTrack - 1);
    }

    bool intercept (SliderRhythmMidiProcessor::Control control) {
        return onControlLearn && onControlLearn (index, control);
    }

    void setTrack (const std::function<void(sliderseq::Track&)>& change) {
        processor.edit ([this, &change] (auto& s) { change (s.sequence.tracks[static_cast<size_t> (index)]); });
    }

    void showZonesMenu() {
        const int curZones = processor.snapshot()->sequence.tracks[static_cast<size_t> (index)].activeZones;
        juce::PopupMenu menu;
        menu.addSectionHeader ("Fader Active Zones (above EMPTY)");
        for (int z = 1; z <= sliderseq::slotsPerTrack; ++z)
            menu.addItem (z, juce::String (z) + (z == 1 ? " Zone (+ EMPTY)" : " Zones (+ EMPTY)"), true, curZones == z);
        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<TrackStrip> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&zonesButton),
                            [safe, sharedLnf] (int result) {
            if (safe == nullptr || result <= 0) return;
            safe->processor.setTrackActiveZones (safe->index, result);
            safe->refresh();
        });
    }

    void showModShapeMenu() {
        const auto cur = processor.snapshot()->sequence.tracks[static_cast<size_t> (index)].modShape;
        juce::PopupMenu menu;
        for (int m = 0; m < static_cast<int> (sliderseq::ModShape::count); ++m) {
            const auto shape = static_cast<sliderseq::ModShape> (m);
            menu.addItem (m + 1, sliderseq::modShapeLabel (shape), true, cur == shape);
        }
        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<TrackStrip> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&modButton),
                            [safe, sharedLnf] (int result) {
            if (safe == nullptr || result <= 0) return;
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setModShape;
            op.track = safe->index;
            op.arg0 = result - 1;
            safe->processor.applyUiOp (op);
            safe->refresh();
        });
    }

    void renameTrackDialog (const juce::String& currentName) {
        auto sharedLnf = lookAndFeel;
        auto dialog = std::make_unique<juce::AlertWindow> (
            "Rename Track", "Enter name for Track " + juce::String (index + 1) + ".",
            juce::MessageBoxIconType::NoIcon, this);
        dialog->setLookAndFeel (&sharedLnf->main);
        dialog->addTextEditor ("name", currentName, "Track name");
        dialog->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        auto* window = dialog.release();
        activeDialog = window;
        juce::Component::SafePointer<TrackStrip> safe (this);
        window->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe, sharedLnf, window] (int result) {
                if (window != nullptr) window->setLookAndFeel (nullptr);
                if (safe == nullptr || result != 1) return;
                const auto newName = window->getTextEditorContents ("name").trim().substring (0, 24);
                if (newName.isEmpty()) return;
                safe->setTrack ([newName] (auto& t) { t.name = newName.toStdString(); });
                safe->refresh();
            }), true);
    }

    void showTrackOptionsMenu() {
        const auto state = processor.snapshot();
        const auto& track = state->sequence.tracks[static_cast<size_t> (index)];
        const int targetSlot = activePatternIndex (track);
        const auto& pattern = track.patterns[static_cast<size_t> (targetSlot)];
        const auto clipboard = onGetClipboard ? onGetClipboard() : std::nullopt;

        juce::PopupMenu menu;
        menu.addItem (10, "Rename Track...");
        menu.addItem (11, "Clear Active Pattern Steps");
        menu.addItem (12, "Copy Active Pattern");
        menu.addItem (13, "Paste Into Active Pattern", clipboard.has_value());

        juce::PopupMenu dupMenu;
        for (int dst = 0; dst < sliderseq::maxTracks; ++dst) {
            if (dst == index) continue;
            const auto& dt = state->sequence.tracks[static_cast<size_t> (dst)];
            dupMenu.addItem (500 + dst, "To Track " + juce::String (dst + 1) + " (" + juce::String (dt.name) + ")");
        }
        menu.addSubMenu ("Duplicate Entire Track", dupMenu);
        menu.addSeparator();

        juce::PopupMenu zonesMenu;
        for (int z = 1; z <= sliderseq::slotsPerTrack; ++z)
            zonesMenu.addItem (700 + z, juce::String (z) + " Zones (+ EMPTY)", true, track.activeZones == z);
        menu.addSubMenu ("Fader Active Zones", zonesMenu);

        juce::PopupMenu modMenu;
        for (int m = 0; m < static_cast<int> (sliderseq::ModShape::count); ++m) {
            const auto shape = static_cast<sliderseq::ModShape> (m);
            modMenu.addItem (100 + m, sliderseq::modShapeLabel (shape), true, track.modShape == shape);
        }
        menu.addSubMenu ("MOD Shape", modMenu);

        juce::PopupMenu gateMenu;
        const std::array<std::pair<const char*, double>, 7> gateChoices {{
            { "12 ticks (1/64)", 12.0 }, { "24 ticks (default)", 24.0 }, { "48 ticks (1/24)", 48.0 },
            { "72 ticks (1/16)", 72.0 }, { "144 ticks (1/8 / Open HH)", 144.0 },
            { "216 ticks (3/16)", 216.0 }, { "288 ticks (1/4)", 288.0 }
        }};
        for (size_t g = 0; g < gateChoices.size(); ++g)
            gateMenu.addItem (200 + static_cast<int> (g), gateChoices[g].first, true,
                              std::abs (track.gateTicks - gateChoices[g].second) < 1.0);
        menu.addSubMenu ("Gate Length", gateMenu);

        juce::PopupMenu chokeMenu;
        for (int other = 0; other < state->sequence.trackCount; ++other) {
            if (other == index) continue;
            const auto& ot = state->sequence.tracks[static_cast<size_t> (other)];
            const bool checked = (track.chokeMask & static_cast<uint16_t> (1u << static_cast<unsigned> (other))) != 0u;
            const auto labelText = juce::String (other + 1) + ": " + juce::String (ot.name)
                + " (Ch " + juce::String (ot.midiChannel) + " / Note " + juce::String (ot.midiNote) + ")";
            chokeMenu.addItem (300 + other, labelText, true, checked);
        }
        if (chokeMenu.getNumItems() > 0) {
            chokeMenu.addSeparator();
            chokeMenu.addItem (399, "Clear All Choke Targets", track.chokeMask != 0u);
        }
        menu.addSubMenu ("Choke On Same Step", chokeMenu, chokeMenu.getNumItems() > 0);

        juce::PopupMenu scaleMenu;
        const std::array<std::pair<const char*, float>, 4> scaleChoices {{
            { "50% (Ghost)", 0.50f }, { "72% (Fill)", 0.72f }, { "100% (Normal)", 1.00f }, { "125% (Accent)", 1.25f }
        }};
        for (size_t s = 0; s < scaleChoices.size(); ++s)
            scaleMenu.addItem (400 + static_cast<int> (s), scaleChoices[s].first, true,
                               std::abs (pattern.velocityScale - scaleChoices[s].second) < 0.02f);
        menu.addSubMenu ("Slot " + juce::String (targetSlot + 1) + " Velocity Scale", scaleMenu);

        juce::PopupMenu loopMenu;
        const std::array<int, 7> loopChoices { 0, 2, 4, 8, 12, 16, 32 };
        for (size_t l = 0; l < loopChoices.size(); ++l) {
            const int v = loopChoices[l];
            loopMenu.addItem (600 + static_cast<int> (l),
                              v == 0 ? "Follow Pattern (0)" : juce::String (v) + " Steps",
                              true, track.loopStepsOverride == v);
        }
        menu.addSubMenu ("Loop Steps Override", loopMenu);

        juce::Component::SafePointer<TrackStrip> safe (this);
        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        const uint16_t currentMask = track.chokeMask;
        const juce::String currentTrackName (track.name);
        const auto currentPattern = pattern;
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&titleButton),
                            [safe, sharedLnf, targetSlot, currentMask, currentTrackName, currentPattern, clipboard, gateChoices, scaleChoices, loopChoices] (int result) {
            if (safe == nullptr || result == 0) return;
            SliderRhythmMidiProcessor::StateOp op;
            op.track = safe->index;
            if (result == 10) {
                safe->renameTrackDialog (currentTrackName);
                return;
            } else if (result == 11) {
                safe->setTrack ([targetSlot] (auto& t) { t.patterns[static_cast<size_t> (targetSlot)].steps.fill (false); });
            } else if (result == 12) {
                if (safe->onCopyPattern) safe->onCopyPattern (currentPattern);
                return;
            } else if (result == 13 && clipboard.has_value()) {
                safe->loadBeat (targetSlot, *clipboard);
                return;
            } else if (result >= 500 && result < 500 + sliderseq::maxTracks) {
                const int dst = result - 500;
                const int src = safe->index;
                safe->processor.edit ([src, dst] (auto& s) {
                    s.sequence.tracks[static_cast<size_t> (dst)] = s.sequence.tracks[static_cast<size_t> (src)];
                    s.sequence.tracks[static_cast<size_t> (dst)].chokeMask &= static_cast<uint16_t> (~(1u << static_cast<unsigned> (dst)));
                    if (dst + 1 > s.sequence.trackCount) s.sequence.trackCount = dst + 1;
                });
                if (safe->onTracksModified) safe->onTracksModified();
                return;
            } else if (result >= 701 && result <= 700 + sliderseq::slotsPerTrack) {
                safe->processor.setTrackActiveZones (safe->index, result - 700);
            } else if (result >= 100 && result < 100 + static_cast<int> (sliderseq::ModShape::count)) {
                op.kind = SliderRhythmMidiProcessor::OpKind::setModShape;
                op.arg0 = result - 100;
                safe->processor.applyUiOp (op);
            } else if (result >= 200 && result < 200 + static_cast<int> (gateChoices.size())) {
                op.kind = SliderRhythmMidiProcessor::OpKind::setGateTicks;
                op.darg = gateChoices[static_cast<size_t> (result - 200)].second;
                safe->processor.applyUiOp (op);
            } else if (result >= 300 && result < 300 + sliderseq::maxTracks) {
                const int other = result - 300;
                op.kind = SliderRhythmMidiProcessor::OpKind::setChokeMask;
                op.arg0 = static_cast<int> (currentMask ^ static_cast<uint16_t> (1u << static_cast<unsigned> (other)));
                safe->processor.applyUiOp (op);
            } else if (result == 399) {
                op.kind = SliderRhythmMidiProcessor::OpKind::setChokeMask;
                op.arg0 = 0;
                safe->processor.applyUiOp (op);
            } else if (result >= 400 && result < 400 + static_cast<int> (scaleChoices.size())) {
                op.kind = SliderRhythmMidiProcessor::OpKind::setPatternVelocityScale;
                op.slotIndex = targetSlot;
                op.darg = scaleChoices[static_cast<size_t> (result - 400)].second;
                safe->processor.applyUiOp (op);
            } else if (result >= 600 && result < 600 + static_cast<int> (loopChoices.size())) {
                safe->processor.setHostParamValue (safe->index, "loop_steps",
                                                   static_cast<float> (loopChoices[static_cast<size_t> (result - 600)]));
            }
            safe->refresh();
        });
    }

    void showPatternMenu() {
        const auto state = processor.snapshot();
        const auto& track = state->sequence.tracks[static_cast<size_t> (index)];
        const int targetSlot = activePatternIndex (track);
        auto current = track.patterns[static_cast<size_t> (targetSlot)];
        const auto destination = patternpresets::trackFolder (juce::String (track.name), index);

        juce::PopupMenu menu;
        menu.addItem (1, "Save Current Beat As...");
        menu.addSeparator();

        std::vector<sliderseq::Pattern> choices;
        const auto addChoice = [&choices] (juce::PopupMenu& section, const sliderseq::Pattern& beat) {
            choices.push_back (beat);
            section.addItem (static_cast<int> (choices.size()) + 1, juce::String (beat.name));
        };

        juce::PopupMenu factory;
        static const auto defaults = sliderseq::makeDefaultState();
        for (size_t voice = 0; voice < 4; ++voice) {
            juce::PopupMenu beats;
            for (const auto& beat : defaults.tracks[voice].patterns)
                addChoice (beats, beat);
            factory.addSubMenu (juce::String (defaults.tracks[voice].name), beats);
        }
        menu.addSubMenu ("Factory Beats", factory);

        juce::PopupMenu user;
        const auto addSavedGroup = [&] (const juce::String& title, const juce::File& directory) {
            juce::PopupMenu beats;
            for (const auto& beat : patternpresets::load (directory))
                addChoice (beats, beat);
            if (beats.getNumItems() > 0)
                user.addSubMenu (title, beats);
        };

        const auto root = patternpresets::folder();
        auto groups = root.findChildFiles (juce::File::findDirectories, false);
        std::sort (groups.begin(), groups.end(), [] (const auto& a, const auto& b) {
            return a.getFileName().compareNatural (b.getFileName()) < 0;
        });
        for (const auto& group : groups)
            addSavedGroup (group.getFileName(), group);
        addSavedGroup ("Unsorted (older presets)", root);
        menu.addSubMenu ("My Beats", user, user.getNumItems() > 0);

        juce::Component::SafePointer<TrackStrip> safe (this);
        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patternName),
                            [safe, sharedLnf, targetSlot, destination, current = std::move (current), choices = std::move (choices)] (int result) {
            if (safe == nullptr || result == 0)
                return;
            if (result == 1)
                safe->saveBeatAs (targetSlot, current, destination);
            else
                safe->loadBeat (targetSlot, choices[static_cast<size_t> (result - 2)]);
        });
    }

    void loadBeat (int targetSlot, const sliderseq::Pattern& beat) {
        setTrack ([targetSlot, &beat] (auto& track) {
            track.patterns[static_cast<size_t> (targetSlot)] = beat;
        });
        refresh();
    }

    void saveBeatAs (int targetSlot, sliderseq::Pattern beat, const juce::File& destination) {
        auto sharedLnf = lookAndFeel;
        auto dialog = std::make_unique<juce::AlertWindow> (
            "Save Beat Preset", "Name the beat from slot " + juce::String (targetSlot + 1) + ".",
            juce::MessageBoxIconType::NoIcon, this);
        dialog->setLookAndFeel (&sharedLnf->main);
        dialog->addTextEditor ("name", juce::String (beat.name), "Preset name");
        dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        auto* window = dialog.release();
        activeDialog = window;
        juce::Component::SafePointer<TrackStrip> safe (this);
        window->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe, sharedLnf, window, targetSlot, destination, beat = std::move (beat)] (int result) mutable {
                if (window != nullptr)
                    window->setLookAndFeel (nullptr);
                if (safe == nullptr || result != 1)
                    return;
                const auto name = window->getTextEditorContents ("name").trim().substring (0, 48);
                if (name.isEmpty())
                    return;
                beat.name = name.toStdString();
                if (! patternpresets::save (beat, destination)) {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        "Could not save beat", "Check write access to the Pattern Presets folder.");
                    return;
                }
                safe->loadBeat (targetSlot, beat);
            }), true);
    }

    SliderRhythmMidiProcessor& processor;
    int index;
    int lastNonEmptySlot = 1;
    int displayedPlayheadStep = -1;
    bool suppressCallbacks = false;
    bool cachedIsEmpty = false;
    int cachedLoopCap = 0;
    juce::String cachedSpecLine, cachedSlotBadge, cachedVelText, cachedAmtText, cachedSwingText;

    juce::SharedResourcePointer<SliderRhythmMidiEditor::LookAndFeelResources> lookAndFeel;
    juce::Component::SafePointer<juce::AlertWindow> activeDialog;

    juce::TextButton titleButton, zonesButton, patternName;
    BraunButton muteButton { "M" }, soloButton { "S" }, modButton { "RAMP UP" };
    std::array<BraunButton, 5> rateButtons;
    BraunSlider slot, velocity, modAmount, swingPct, note, channel, length;
    BraunComboBox grid;
    VelocityModScope velocityScope;
    TrackSwingScope swingScope;
    std::array<std::unique_ptr<BraunStepPad>, 64> steps;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> velocityAttachment, modAmountAttachment, swingPctAttachment;
};

// Scalable root architectural canvas holding the Braun 2-row header + strip viewport.
class SliderRhythmMidiEditor::RootCanvas final : public juce::Component {
public:
    RootCanvas (SliderRhythmMidiProcessor& proc,
                juce::SharedResourcePointer<LookAndFeelResources> sharedLnf,
                juce::Viewport& vp)
        : processor (proc), lookAndFeel (sharedLnf), viewport (vp) {
        auto bindWheel = [this] (auto& comp) {
            comp.allowVerticalWheel = [this] { return lookAndFeel->verticalWheel.load (std::memory_order_relaxed); };
            comp.allowHorizontalWheel = [this] { return lookAndFeel->horizontalWheel.load (std::memory_order_relaxed); };
        };

        for (auto* b : { &presetPrev, &presetNameBtn, &presetNext, &presetSaveBtn,
                         &scaleBtn, &themeBtn, &settingsBtn,
                         &addTrack, &removeTrack, &unmuteAll, &unsoloAll,
                         &learn, &noteLearnToggle, &triggerModeToggle, &playbackMode }) {
            addAndMakeVisible (*b);
        }

        presetPrev.setButtonText ("<");
        presetNext.setButtonText (">");
        presetSaveBtn.setButtonText ("SAVE");
        presetNameBtn.setTooltip ("Load, save, revert, or delete Mapping Presets (* means modified)");
        presetSaveBtn.setTooltip ("Save current mapping to active user preset (or Save As for factory presets)");
        scaleBtn.setTooltip ("Change UI zoom scale (75% / 85% / 100% / 125%)");
        themeBtn.setTooltip ("Switch between Braun SK 4 Light and Braun Atelier P4 Dark");
        settingsBtn.setButtonText ("CFG");
        settingsBtn.setTooltip ("Scroll wheel direction controls (Vertical / Horizontal), theme, and UI scale");

        addTrack.setButtonText ("+TRK");
        removeTrack.setButtonText ("-TRK");
        unmuteAll.setButtonText ("UNMUTE");
        unsoloAll.setButtonText ("UNSOLO");
        learn.setButtonText ("LEARN");
        noteLearnToggle.setButtonText ("NOTE");
        triggerModeToggle.setButtonText ("TGL");
        playbackMode.setButtonText ("GEN");

        unmuteAll.setTooltip ("Unmute all tracks");
        unsoloAll.setTooltip ("Clear solo on all tracks");
        noteLearnToggle.setTooltip ("Allow MIDI Note On messages (pads/keys) for MIDI Learn and control");
        triggerModeToggle.setTooltip ("Learn trigger mode for M/S: TGL (press flips) vs HLD (momentary)");
        playbackMode.setTooltip ("GEN: sequencer only | GEN+THRU: sequencer + live input | THRU: passthrough only");

        // Global Swing bipolar master knob (-100 Shuffle <-> 0 Straight <-> +100 Swing)
        globalSwingDial.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        globalSwingDial.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        globalSwingDial.setRange (-100, 100, 1);
        globalSwingDial.setDoubleClickReturnValue (true, 0.0);
        bindWheel (globalSwingDial);
        addAndMakeVisible (globalSwingDial);
        globalSwingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::globalSwingParamId(), globalSwingDial);

        addAndMakeVisible (globalSwingScope);

        dragMidi = std::make_unique<MidiDragHandle>();
        addAndMakeVisible (*dragMidi);
        addAndMakeVisible (viewport);
    }

    ~RootCanvas() override {
        globalSwingAttachment.reset();
    }

    void paint (juce::Graphics& g) override {
        const auto& p = lookAndFeel->main.currentPalette();
        const int w = getWidth();
        const int h = getHeight();

        g.fillAll (p.chassis);

        // Header architectural 1px hairlines
        g.setColour (p.hairline);
        g.drawHorizontalLine (32, 0.0f, static_cast<float> (w));
        g.drawHorizontalLine (kHeaderHeight - 1, 0.0f, static_cast<float> (w));

        // Row 1 Brand + Subtitle
        g.setColour (p.ink);
        g.setFont (braun::sansFont (13.5f, true));
        g.drawFittedText (juce::String (JucePlugin_Name).toUpperCase(), 10, 6, 188, 20,
                          juce::Justification::centredLeft, 1, 0.72f);

        g.setColour (p.hairline);
        g.drawVerticalLine (203, 6.0f, 26.0f);

        g.setColour (p.mutedInk);
        g.setFont (braun::monoFont (9.0f, false));
        g.drawText ("MAPPING PRESET", 211, 6, 90, 20, juce::Justification::centredLeft);

        // Row 1 Live status LED + text
        const int statusX = 594;
        const int statusRight = std::max (statusX + 60, w - 186);
        g.setColour (statusAccent ? p.orange : p.mutedInk);
        g.fillEllipse (static_cast<float> (statusX), 13.0f, 6.0f, 6.0f);
        g.setFont (braun::monoFont (9.5f, statusAccent));
        g.setColour (statusAccent ? p.orange : p.ink);
        g.drawFittedText (statusText, statusX + 10, 6, statusRight - (statusX + 12), 20,
                          juce::Justification::centredLeft, 1, 0.85f);

        // Row 2 Global Swing labels + value (non-overlapping with dial at x=64..96)
        g.setFont (braun::monoFont (8.5f, true));
        g.setColour (p.mutedInk);
        g.drawText ("GLOBAL", 10, 38, 52, 11, juce::Justification::centredLeft);
        g.drawText ("SWING", 10, 49, 52, 11, juce::Justification::centredLeft);
        g.setFont (braun::monoFont (7.5f, false));
        g.drawText ("SHUF/SWG", 10, 61, 52, 11, juce::Justification::centredLeft);

        g.setFont (braun::monoFont (11.5f, true));
        g.setColour (globalSwingValue != 0 ? p.orange : p.ink);
        g.drawText (formatSwingText (globalSwingValue), 98, 46, 44, 18, juce::Justification::centredRight);

        // Vertical hairline separating Global Swing monitor bay from live utility buttons
        g.setColour (p.hairline);
        g.drawVerticalLine (438, 37.0f, static_cast<float> (kHeaderHeight - 5));

        // Subtle outer border
        g.setColour (p.hairline);
        g.drawRect (0, 0, w, h, 1);
    }

    void resized() override {
        const int w = getWidth();
        const int h = getHeight();

        // Row 1: Mapping Preset bar + Right utility cluster
        presetPrev.setBounds (303, 6, 20, 20);
        presetNameBtn.setBounds (325, 6, 188, 20);
        presetNext.setBounds (515, 6, 20, 20);
        presetSaveBtn.setBounds (538, 6, 46, 20);

        settingsBtn.setBounds (w - 42, 6, 34, 20);
        themeBtn.setBounds (w - 122, 6, 76, 20);
        scaleBtn.setBounds (w - 174, 6, 48, 20);

        // Row 2: Global Swing dial + Oscilloscope bay + Live Action Buttons
        globalSwingDial.setBounds (64, 40, 32, 32);
        globalSwingScope.setBounds (148, 39, 282, 33);

        int bx = 446;
        const int by = 44;
        const int bh = 23;
        const int gap = 4;
        auto place = [&] (juce::Component& c, int bw) {
            c.setBounds (bx, by, bw, bh);
            bx += bw + gap;
        };
        place (addTrack, 46);
        place (removeTrack, 46);
        bx += 4;
        place (unmuteAll, 60);
        place (unsoloAll, 60);
        bx += 4;
        place (learn, 58);
        place (noteLearnToggle, 48);
        place (triggerModeToggle, 42);
        bx += 4;
        place (playbackMode, 76);
        if (dragMidi) place (*dragMidi, 74);

        viewport.setBounds (1, kHeaderHeight, std::max (100, w - 2), std::max (100, h - kHeaderHeight - 1));
    }

    void applyTheme (const braun::Palette& p) {
        globalSwingDial.setColour (juce::Slider::rotarySliderFillColourId, p.orange);
        if (dragMidi) dragMidi->setPalette (p);
        repaint();
    }

    SliderRhythmMidiProcessor& processor;
    juce::SharedResourcePointer<LookAndFeelResources> lookAndFeel;
    juce::Viewport& viewport;

    juce::TextButton presetPrev, presetNameBtn, presetNext, presetSaveBtn;
    juce::TextButton scaleBtn, themeBtn, settingsBtn;
    juce::TextButton addTrack, removeTrack, unmuteAll, unsoloAll;
    juce::TextButton learn, noteLearnToggle, triggerModeToggle, playbackMode;
    BraunSlider globalSwingDial;
    GlobalSwingScope globalSwingScope;
    std::unique_ptr<MidiDragHandle> dragMidi;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> globalSwingAttachment;

    juce::String statusText;
    bool statusAccent = false;
    int globalSwingValue = 0;
};

SliderRhythmMidiEditor::SliderRhythmMidiEditor (SliderRhythmMidiProcessor& owner)
    : AudioProcessorEditor (&owner), processor (owner), uiPrefs (uiprefs::load()) {
    lookAndFeel->verticalWheel.store (uiPrefs.verticalWheelControls, std::memory_order_relaxed);
    lookAndFeel->horizontalWheel.store (uiPrefs.horizontalWheelControls, std::memory_order_relaxed);
    lookAndFeel->applyPalette (braun::Palette::forMode (uiPrefs.darkMode));
    setLookAndFeel (&lookAndFeel->main);

    viewport.setScrollBarsShown (true, true);
    viewport.setScrollBarThickness (kScrollBarH);
    viewport.setViewedComponent (&stripContent, false);

    rootCanvas = std::make_unique<RootCanvas> (processor, lookAndFeel, viewport);
    addAndMakeVisible (*rootCanvas);

    rootCanvas->globalSwingDial.interceptLearn = [this] {
        if (! choosingLearnTarget && processor.getLearnTarget() < 0) return false;
        choosingLearnTarget = false;
        processor.armMidiLearn (0, SliderRhythmMidiProcessor::globalSwing);
        timerCallback();
        return true;
    };
    rootCanvas->globalSwingDial.onValueChange = [this] {
        const int val = static_cast<int> (std::lround (rootCanvas->globalSwingDial.getValue()));
        rootCanvas->globalSwingValue = val;
        rootCanvas->globalSwingScope.setSwing (val, lookAndFeel->main.currentPalette());
        rootCanvas->repaint();
        for (auto& strip : strips) strip->refreshVisualsOnly();
    };

    rootCanvas->presetPrev.onClick = [this] { stepMappingPreset (-1); };
    rootCanvas->presetNext.onClick = [this] { stepMappingPreset (+1); };
    rootCanvas->presetNameBtn.onClick = [this] { showMappingPresetMenu(); };
    rootCanvas->presetSaveBtn.onClick = [this] {
        const auto snap = processor.snapshot();
        if (! snap->activeMappingPreset.isFactory && snap->activeMappingPreset.file.existsAsFile()) {
            auto updated = mappingpresets::captureFromState (
                snap->sequence, snap->activeMappingPreset.id, snap->activeMappingPreset.name, false);
            updated.file = snap->activeMappingPreset.file;
            if (mappingpresets::saveUserPreset (updated))
                processor.setActiveMappingPresetBaseline (updated);
            timerCallback();
        } else {
            saveMappingPresetDialog (true);
        }
    };

    rootCanvas->scaleBtn.onClick = [this] {
        juce::PopupMenu m;
        for (int s : { 75, 85, 100, 125 })
            m.addItem (s, juce::String (s) + "%", true, uiPrefs.scalePercent == s);
        auto sharedLnf = lookAndFeel;
        m.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<SliderRhythmMidiEditor> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&rootCanvas->scaleBtn),
                         [safe, sharedLnf] (int result) {
            if (safe == nullptr || result <= 0) return;
            safe->uiPrefs.scalePercent = uiprefs::clampScalePercent (result);
            uiprefs::save (safe->uiPrefs);
            safe->applyThemeAndScale (true);
        });
    };

    rootCanvas->themeBtn.onClick = [this] {
        uiPrefs.darkMode = ! uiPrefs.darkMode;
        uiprefs::save (uiPrefs);
        applyThemeAndScale (false);
    };

    rootCanvas->settingsBtn.onClick = [this] { showSettingsMenu(); };

    rootCanvas->dragMidi->getLabelText = [this] { return "DRAG " + juce::String (exportBars) + "B"; };
    rootCanvas->dragMidi->onCreateFile = [this] () -> juce::File {
        const auto snap = processor.snapshot();
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SliderRhythmMidiClips");
        if (dir.createDirectory().failed()) return {};
        const auto file = dir.getChildFile ("SliderRhythm_" + juce::String (exportBars) + "bar_"
                                            + juce::String (juce::Time::currentTimeMillis()) + ".mid");
        if (! midiexporter::writeMidiFile (snap->sequence, file, exportBars, processor.getLastKnownBpm()))
            return {};
        return file;
    };
    rootCanvas->dragMidi->onRightClick = [this] {
        juce::PopupMenu m;
        for (int b : { 1, 2, 4, 8 })
            m.addItem (b, "Export " + juce::String (b) + (b == 1 ? " Bar" : " Bars"), true, exportBars == b);
        auto sharedLnf = lookAndFeel;
        m.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<SliderRhythmMidiEditor> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (rootCanvas->dragMidi.get()),
                         [safe, sharedLnf] (int result) {
            if (safe == nullptr || result <= 0) return;
            safe->exportBars = result;
            if (safe->rootCanvas && safe->rootCanvas->dragMidi)
                safe->rootCanvas->dragMidi->repaint();
        });
    };

    rootCanvas->addTrack.onClick = [this] {
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setTrackCount;
        op.arg0 = std::min (sliderseq::maxTracks, processor.snapshot()->sequence.trackCount + 1);
        processor.applyUiOp (op);
        timerCallback();
    };
    rootCanvas->removeTrack.onClick = [this] {
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setTrackCount;
        op.arg0 = std::max (1, processor.snapshot()->sequence.trackCount - 1);
        processor.applyUiOp (op);
        timerCallback();
    };
    rootCanvas->unmuteAll.onClick = [this] { processor.unmuteAllTracks(); timerCallback(); };
    rootCanvas->unsoloAll.onClick = [this] { processor.unsoloAllTracks(); timerCallback(); };
    rootCanvas->learn.onClick = [this] {
        if (choosingLearnTarget || processor.getLearnTarget() >= 0) {
            choosingLearnTarget = false;
            processor.cancelMidiLearn();
        } else {
            choosingLearnTarget = true;
        }
        timerCallback();
    };
    rootCanvas->noteLearnToggle.onClick = [this] {
        processor.setNoteLearnEnabled (! processor.isNoteLearnEnabled());
        timerCallback();
    };
    rootCanvas->triggerModeToggle.onClick = [this] {
        const auto next = processor.getLearnTriggerMode() == SliderRhythmMidiProcessor::TriggerMode::toggle
            ? SliderRhythmMidiProcessor::TriggerMode::momentary
            : SliderRhythmMidiProcessor::TriggerMode::toggle;
        processor.setLearnTriggerMode (next);
        timerCallback();
    };
    rootCanvas->playbackMode.onClick = [this] {
        const auto current = processor.snapshot()->playbackMode;
        const int next = (static_cast<int> (current) + 1) % 3;
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setPlaybackMode;
        op.arg0 = next;
        processor.applyUiOp (op);
        timerCallback();
    };

    setResizable (true, true);
    rebuildStrips();
    applyThemeAndScale (true);
    startTimerHz (60);
}

SliderRhythmMidiEditor::~SliderRhythmMidiEditor() {
    stopTimer();
    if (activeDialog != nullptr)
        activeDialog->exitModalState (0);
    juce::PopupMenu::dismissAllActiveMenus();
    strips.clear();
    stripContent.removeAllChildren();
    viewport.setViewedComponent (nullptr, false);
    rootCanvas.reset();
    setLookAndFeel (nullptr);
}

void SliderRhythmMidiEditor::applyThemeAndScale (bool resizeWindow) {
    const auto p = braun::Palette::forMode (uiPrefs.darkMode);
    lookAndFeel->applyPalette (p);
    lookAndFeel->verticalWheel.store (uiPrefs.verticalWheelControls, std::memory_order_relaxed);
    lookAndFeel->horizontalWheel.store (uiPrefs.horizontalWheelControls, std::memory_order_relaxed);

    const float scale = static_cast<float> (uiPrefs.scalePercent) / 100.0f;
    setResizeLimits (static_cast<int> (std::lround (kMinLogicalW * scale)),
                     static_cast<int> (std::lround (440 * scale)),
                     static_cast<int> (std::lround (kMaxLogicalW * scale)),
                     static_cast<int> (std::lround (920 * scale)));

    if (rootCanvas) {
        rootCanvas->setTransform (juce::AffineTransform::scale (scale));
        rootCanvas->applyTheme (p);
        rootCanvas->scaleBtn.setButtonText (juce::String (uiPrefs.scalePercent) + "%");
        rootCanvas->themeBtn.setButtonText (uiPrefs.darkMode ? "DARK / LGT" : "LIGHT / DRK");
    }
    for (auto& strip : strips) strip->applyTheme();

    if (resizeWindow) {
        const int visibleTracks = std::clamp (shownTrackCount, 6, 8);
        const int targetLogicalW = std::clamp (visibleTracks * kStripWidth + 2, kMinLogicalW, 1520);
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* display = displays.getPrimaryDisplay();
        const int maxScreenW = display != nullptr ? static_cast<int> (display->userBounds.getWidth()) - 40 : 1600;
        const int maxScreenH = display != nullptr ? static_cast<int> (display->userBounds.getHeight()) - 60 : 960;
        const int physW = std::min (maxScreenW, static_cast<int> (std::lround (targetLogicalW * scale)));
        const int physH = std::min (maxScreenH, static_cast<int> (std::lround (kDefaultLogicalH * scale)));
        setSize (std::max (static_cast<int> (std::lround (kMinLogicalW * scale)), physW),
                 std::max (static_cast<int> (std::lround (440 * scale)), physH));
    }

    resized();
    if (const auto snap = processor.snapshot())
        refreshHeaderState (*snap);
    repaint();
}

void SliderRhythmMidiEditor::rebuildStrips() {
    stripContent.removeAllChildren();
    strips.clear();
    shownTrackCount = processor.snapshot()->sequence.trackCount;
    for (int i = 0; i < shownTrackCount; ++i) {
        auto strip = std::make_unique<TrackStrip> (processor, i, lookAndFeel);
        strip->onControlLearn = [this] (int track, SliderRhythmMidiProcessor::Control control) {
            if (! choosingLearnTarget && processor.getLearnTarget() < 0) return false;
            choosingLearnTarget = false;
            processor.armMidiLearn (track, control);
            timerCallback();
            return true;
        };
        strip->onCopyPattern = [this] (const sliderseq::Pattern& pat) { patternClipboard = pat; };
        strip->onGetClipboard = [this] { return patternClipboard; };
        strip->onTracksModified = [this] {
            lastSeenVersion = 0;
            timerCallback();
        };
        stripContent.addAndMakeVisible (*strip);
        strips.push_back (std::move (strip));
    }
    resized();
    lastSeenVersion = processor.stateVersion();
}

void SliderRhythmMidiEditor::stepMappingPreset (int delta) {
    const auto list = mappingpresets::allPresets();
    if (list.empty()) return;
    const auto curId = processor.snapshot()->activeMappingPreset.id;
    int idx = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].id == curId) { idx = static_cast<int> (i); break; }
    }
    const int next = (idx + delta + static_cast<int> (list.size())) % static_cast<int> (list.size());
    processor.loadMappingPreset (list[static_cast<size_t> (next)]);
    rebuildStrips();
    timerCallback();
}

void SliderRhythmMidiEditor::showMappingPresetMenu() {
    const auto snap = processor.snapshot();
    const auto& active = snap->activeMappingPreset;
    const bool dirty = ! mappingpresets::matchesState (active, snap->sequence);
    const auto factories = mappingpresets::factoryPresets();
    const auto users = mappingpresets::loadUserPresets();

    juce::PopupMenu menu;
    menu.addSectionHeader ("Factory Mapping Presets");
    for (size_t i = 0; i < factories.size(); ++i)
        menu.addItem (100 + static_cast<int> (i), juce::String (factories[i].name), true,
                      active.id == factories[i].id);

    if (! users.empty()) {
        menu.addSeparator();
        menu.addSectionHeader ("User Mapping Presets");
        for (size_t i = 0; i < users.size(); ++i)
            menu.addItem (300 + static_cast<int> (i), juce::String (users[i].name), true,
                          active.id == users[i].id);
    }

    menu.addSeparator();
    menu.addItem (1, ! active.isFactory ? "Save \"" + juce::String (active.name) + "\"" : "Save Preset As...");
    menu.addItem (2, "Save Mapping Preset As...");
    menu.addItem (3, "Revert to \"" + juce::String (active.name) + "\"", dirty);
    menu.addItem (4, "Delete \"" + juce::String (active.name) + "\" (Move to Trash)", ! active.isFactory && active.file.existsAsFile());

    auto sharedLnf = lookAndFeel;
    menu.setLookAndFeel (&sharedLnf->main);
    juce::Component::SafePointer<SliderRhythmMidiEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&rootCanvas->presetNameBtn),
                        [safe, sharedLnf, factories, users, active] (int result) {
        if (safe == nullptr || result == 0) return;
        if (result == 1) {
            if (! active.isFactory && active.file.existsAsFile()) {
                const auto cur = safe->processor.snapshot();
                auto updated = mappingpresets::captureFromState (cur->sequence, active.id, active.name, false);
                updated.file = active.file;
                if (mappingpresets::saveUserPreset (updated))
                    safe->processor.setActiveMappingPresetBaseline (updated);
                safe->timerCallback();
            } else {
                safe->saveMappingPresetDialog (true);
            }
        } else if (result == 2) {
            safe->saveMappingPresetDialog (true);
        } else if (result == 3) {
            safe->processor.loadMappingPreset (active);
            safe->rebuildStrips();
            safe->timerCallback();
        } else if (result == 4) {
            if (mappingpresets::deleteUserPreset (active)) {
                safe->processor.setActiveMappingPresetBaseline (mappingpresets::makeDefaultKitPreset());
                safe->timerCallback();
            }
        } else if (result >= 100 && result < 100 + static_cast<int> (factories.size())) {
            safe->processor.loadMappingPreset (factories[static_cast<size_t> (result - 100)]);
            safe->rebuildStrips();
            safe->timerCallback();
        } else if (result >= 300 && result < 300 + static_cast<int> (users.size())) {
            safe->processor.loadMappingPreset (users[static_cast<size_t> (result - 300)]);
            safe->rebuildStrips();
            safe->timerCallback();
        }
    });
}

void SliderRhythmMidiEditor::saveMappingPresetDialog (bool) {
    const auto snap = processor.snapshot();
    auto sharedLnf = lookAndFeel;
    auto dialog = std::make_unique<juce::AlertWindow> (
        "Save Mapping Preset", "Save current track layout, notes, channels, zones, choke, and swing % as a Mapping Preset.",
        juce::MessageBoxIconType::NoIcon, this);
    dialog->setLookAndFeel (&sharedLnf->main);
    dialog->addTextEditor ("name", juce::String (snap->activeMappingPreset.name) + " Custom", "Preset name");
    dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    auto* window = dialog.release();
    activeDialog = window;
    juce::Component::SafePointer<SliderRhythmMidiEditor> safe (this);
    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe, sharedLnf, window] (int result) {
            if (window != nullptr) window->setLookAndFeel (nullptr);
            if (safe == nullptr || result != 1) return;
            const auto name = window->getTextEditorContents ("name").trim().substring (0, 48);
            if (name.isEmpty()) return;
            const auto cur = safe->processor.snapshot();
            auto preset = mappingpresets::captureFromState (cur->sequence, {}, name.toStdString(), false);
            if (! mappingpresets::saveUserPreset (preset)) {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                    "Could not save mapping preset", "Check write access to the Mapping Presets folder.");
                return;
            }
            safe->processor.setActiveMappingPresetBaseline (preset);
            safe->timerCallback();
        }), true);
}

void SliderRhythmMidiEditor::showSettingsMenu() {
    juce::PopupMenu menu;
    menu.addSectionHeader ("Scroll Wheel Controls");
    menu.addItem (1, "Vertical Scroll -> Knobs & Faders", true, uiPrefs.verticalWheelControls);
    menu.addItem (2, "Horizontal Scroll -> Knobs & Faders", true, uiPrefs.horizontalWheelControls);
    menu.addSeparator();
    menu.addSectionHeader ("Appearance (Braun Theme)");
    menu.addItem (10, "Braun SK 4 (Light)", true, ! uiPrefs.darkMode);
    menu.addItem (11, "Braun Atelier P4 (Dark)", true, uiPrefs.darkMode);
    menu.addSeparator();
    menu.addSectionHeader ("UI Scale");
    for (int s : { 75, 85, 100, 125 })
        menu.addItem (100 + s, juce::String (s) + "%", true, uiPrefs.scalePercent == s);

    auto sharedLnf = lookAndFeel;
    menu.setLookAndFeel (&sharedLnf->main);
    juce::Component::SafePointer<SliderRhythmMidiEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&rootCanvas->settingsBtn),
                        [safe, sharedLnf] (int result) {
        if (safe == nullptr || result == 0) return;
        bool resizeNeeded = false;
        if (result == 1) safe->uiPrefs.verticalWheelControls = ! safe->uiPrefs.verticalWheelControls;
        else if (result == 2) safe->uiPrefs.horizontalWheelControls = ! safe->uiPrefs.horizontalWheelControls;
        else if (result == 10) safe->uiPrefs.darkMode = false;
        else if (result == 11) safe->uiPrefs.darkMode = true;
        else if (result >= 175 && result <= 225) {
            safe->uiPrefs.scalePercent = uiprefs::clampScalePercent (result - 100);
            resizeNeeded = true;
        }
        uiprefs::save (safe->uiPrefs);
        safe->applyThemeAndScale (resizeNeeded);
    });
}

void SliderRhythmMidiEditor::refreshHeaderState (const SliderRhythmMidiProcessor::Snapshot& state) {
    if (! rootCanvas) return;
    const auto& p = lookAndFeel->main.currentPalette();

    const bool dirty = ! mappingpresets::matchesState (state.activeMappingPreset, state.sequence);
    const juce::String presetLabel = juce::String (state.activeMappingPreset.name) + (dirty ? " *" : "");
    rootCanvas->presetNameBtn.setButtonText (presetLabel);
    rootCanvas->presetNameBtn.setColour (juce::TextButton::textColourOffId, dirty ? p.orange : p.ink);
    rootCanvas->presetSaveBtn.setColour (juce::TextButton::buttonColourId, dirty ? p.orangeTint : p.capSurface);
    rootCanvas->presetSaveBtn.setColour (juce::TextButton::textColourOffId, dirty ? p.orange : p.ink);

    rootCanvas->globalSwingValue = state.sequence.globalSwing;
    rootCanvas->globalSwingDial.setValue (state.sequence.globalSwing, juce::dontSendNotification);
    rootCanvas->globalSwingScope.setSwing (state.sequence.globalSwing, p);

    bool anyMuted = false, anySolo = false;
    for (int i = 0; i < state.sequence.trackCount; ++i) {
        anyMuted = anyMuted || state.sequence.tracks[static_cast<size_t> (i)].muted;
        anySolo = anySolo || state.sequence.tracks[static_cast<size_t> (i)].solo;
    }
    rootCanvas->unmuteAll.setColour (juce::TextButton::buttonColourId, anyMuted ? p.muteRed : p.capSurface);
    rootCanvas->unmuteAll.setColour (juce::TextButton::textColourOffId, anyMuted ? juce::Colours::white : p.ink);
    rootCanvas->unsoloAll.setColour (juce::TextButton::buttonColourId, anySolo ? p.soloCyan : p.capSurface);
    rootCanvas->unsoloAll.setColour (juce::TextButton::textColourOffId, anySolo ? juce::Colours::white : p.ink);

    const bool noteOn = processor.isNoteLearnEnabled();
    rootCanvas->noteLearnToggle.setColour (juce::TextButton::buttonColourId, noteOn ? p.orange : p.capSurface);
    rootCanvas->noteLearnToggle.setColour (juce::TextButton::textColourOffId, noteOn ? juce::Colours::white : p.ink);

    const bool isToggle = processor.getLearnTriggerMode() == SliderRhythmMidiProcessor::TriggerMode::toggle;
    rootCanvas->triggerModeToggle.setButtonText (isToggle ? "TGL" : "HLD");
    rootCanvas->triggerModeToggle.setColour (juce::TextButton::buttonColourId, isToggle ? p.capSurface : p.orangeTint);
    rootCanvas->triggerModeToggle.setColour (juce::TextButton::textColourOffId, isToggle ? p.ink : p.orange);

    const int armedTarget = processor.getLearnTarget();
    const bool armed = armedTarget >= 0;
    const juce::String learnedControl = armed ? kControlNames[static_cast<size_t> (armedTarget % SliderRhythmMidiProcessor::controlCount)] : "";
    const juce::String transport = processor.isHostRecording() ? "HOST REC"
                                 : processor.isHostPlaying() ? "HOST PLAY"
                                 : processor.hasHostPosition() ? "HOST STOP"
                                 : "IDLE";
    const juce::String learnHint = noteOn ? "move CC/Note" : "move CC";
    rootCanvas->statusAccent = choosingLearnTarget || armed || processor.isHostPlaying() || processor.isHostRecording();
    rootCanvas->statusText = choosingLearnTarget ? "CLICK ANY CONTROL TO LEARN"
                           : armed ? ("LEARN T" + juce::String (armedTarget / SliderRhythmMidiProcessor::controlCount + 1)
                                      + " " + learnedControl + ": " + learnHint)
                           : state.playbackMode == sliderseq::PlaybackMode::thru ? "MIDI THRU"
                           : (transport + juce::String::fromUTF8 (" \xc2\xb7 OUT ") + juce::String (processor.generatedNoteCount()));

    rootCanvas->learn.setButtonText (choosingLearnTarget || armed ? "CANCEL" : "LEARN");
    rootCanvas->learn.setColour (juce::TextButton::buttonColourId, (choosingLearnTarget || armed) ? p.orange : p.capSurface);
    rootCanvas->learn.setColour (juce::TextButton::textColourOffId, (choosingLearnTarget || armed) ? juce::Colours::white : p.ink);

    rootCanvas->playbackMode.setButtonText (playbackModeLabel (state.playbackMode));
    const bool modeOrange = state.playbackMode == sliderseq::PlaybackMode::thru;
    const bool modeCyan = state.playbackMode == sliderseq::PlaybackMode::genPlusThru;
    rootCanvas->playbackMode.setColour (juce::TextButton::buttonColourId,
                                        modeOrange ? p.orange : (modeCyan ? p.soloCyan : p.capSurface));
    rootCanvas->playbackMode.setColour (juce::TextButton::textColourOffId,
                                        (modeOrange || modeCyan) ? juce::Colours::white : p.ink);
    rootCanvas->repaint();
}

void SliderRhythmMidiEditor::timerCallback() {
    const uint64_t currentVersion = processor.stateVersion();
    const bool stateChanged = currentVersion != lastSeenVersion;
    const auto state = processor.snapshot();

    if (shownTrackCount != state->sequence.trackCount) {
        rebuildStrips();
    } else if (stateChanged) {
        for (auto& strip : strips) strip->refresh();
        lastSeenVersion = currentVersion;
    } else {
        for (auto& strip : strips) strip->updatePlayhead();
    }

    refreshHeaderState (*state);
}

void SliderRhythmMidiEditor::paint (juce::Graphics& g) {
    g.fillAll (lookAndFeel->main.currentPalette().chassis);
}

void SliderRhythmMidiEditor::resized() {
    if (! rootCanvas) return;
    const float scale = static_cast<float> (uiPrefs.scalePercent) / 100.0f;
    const int logicalW = std::max (kMinLogicalW, static_cast<int> (std::lround (getWidth() / scale)));
    const int logicalH = std::max (440, static_cast<int> (std::lround (getHeight() / scale)));
    rootCanvas->setBounds (0, 0, logicalW, logicalH);

    if (shownTrackCount == 0) return;

    const int vpW = std::max (kStripWidth, viewport.getWidth());
    const int stripW = shownTrackCount <= 6
        ? std::max (kStripWidth, vpW / shownTrackCount)
        : kStripWidth;
    stripContent.setSize (shownTrackCount * stripW, kStripHeight);
    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (static_cast<int> (i) * stripW, 0, stripW, kStripHeight);
}
