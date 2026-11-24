#include "PluginEditor.h"
#include "DotMatrixFont.h"
#include "DrumMaps.h"
#include "MidiExporter.h"
#include "PatternPresets.h"
#include "TrackPresets.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace {
constexpr int kStripWidth = 224;
constexpr int kMinStripHeight = 656;
constexpr int kHeaderHeight = 84;
constexpr int kScrollBarH = 12;
constexpr int kMinLogicalW = 860;
constexpr int kMinLogicalH = 540;
constexpr int kMaxLogicalW = 2600;
constexpr int kMaxLogicalH = 1600;
// Fixed 224 px track strips: default window fits exactly 5 strips edge-to-edge (+2 px outer border).
constexpr int kDefaultLogicalW = kStripWidth * 5 + 2;
constexpr int kDefaultLogicalH = kHeaderHeight + 719 + 1;

const std::array<double, 5> kRates { 1.0 / 3.0, 0.5, 1.0, 2.0, 3.0 };
const std::array<const char*, 5> kRateLabels { "1/3", "1/2", "1x", "2x", "3x" };
// Accurate beat fractions at 288 PPQ (1/96 = 12 .. 1/4 = 288).
constexpr std::array<double, 8> kGateTicks { 12.0, 18.0, 24.0, 48.0, 72.0, 144.0, 216.0, 288.0 };
constexpr std::array<const char*, 8> kGateShortLabels { "1/96", "1/64", "1/48", "1/24", "1/16", "1/8", "3/16", "1/4" };
constexpr std::array<const char*, 8> kGateMenuLabels {
    "12 ticks (1/96)", "18 ticks (1/64)", "24 ticks (default / 1/48)",
    "48 ticks (1/24)", "72 ticks (1/16)", "144 ticks (1/8 / Open HH)",
    "216 ticks (3/16)", "288 ticks (1/4)"
};
const std::array<const char*, SliderRhythmMidiProcessor::controlCount> kControlNames {
    "Pattern fader", "DIV", "Velocity", "Swing %", "MOD amount", "Mute", "MOD shape",
    "MIDI note", "MIDI channel", "Pattern length", "Step grid", "Solo", "Global Swing", "Macro"
};

int nearestGateIndex (double gateTicks) noexcept {
    int best = 2;
    double bestDiff = 1e9;
    for (int i = 0; i < static_cast<int> (kGateTicks.size()); ++i) {
        const double d = std::abs (gateTicks - kGateTicks[static_cast<size_t> (i)]);
        if (d < bestDiff) { bestDiff = d; best = i; }
    }
    return best;
}

juce::String formatChokeLabel (uint16_t chokeMask, int trackCount) {
    int count = 0;
    int firstIdx = -1;
    const int limit = std::clamp (trackCount, 0, sliderseq::maxTracks);
    for (int i = 0; i < limit; ++i) {
        if ((chokeMask & static_cast<uint16_t> (1u << static_cast<unsigned> (i))) != 0u) {
            if (firstIdx < 0) firstIdx = i;
            ++count;
        }
    }
    if (count == 0) return "OFF";
    if (count == 1) return "T" + juce::String (firstIdx + 1).paddedLeft ('0', 2);
    return juce::String (count) + "T";
}

juce::String formatSwingText (int swingVal) {
    if (swingVal > 0) return "+" + juce::String (swingVal) + "%";
    return juce::String (swingVal) + "%";
}

juce::String formatMacroStatus (const sliderseq::Track& track, const sliderseq::Pattern& pattern, int& colourKind) {
    // colourKind: 0 = muted/neutral, 1 = orange (active/positive), 2 = cyan (negative bipolar)
    const int mv = std::clamp (track.macroValue, 0, 100);
    const auto eff = sliderseq::computeEffectiveTrackForPattern (track, pattern);
    const int effLen = sliderseq::effectivePatternLength (pattern.length, pattern.grid, eff.loopStepsOverride);
    const double rateMul = track.rate > 0.0 ? (eff.rate / track.rate) : 1.0;
    const bool doubledRate = rateMul > 1.5;
    colourKind = 0;

    switch (track.macroMode) {
        case sliderseq::MacroMode::build: {
            if (mv <= 0) return "0% IDLE";
            colourKind = 1;
            if (doubledRate) {
                const juce::String rTag = std::abs (rateMul - 2.0) < 0.05
                    ? "2x" : (juce::String (rateMul, 1) + "x");
                return "+" + juce::String (mv) + "% " + rTag + " L" + juce::String (effLen);
            }
            if (eff.loopStepsOverride > 0 && eff.loopStepsOverride != track.loopStepsOverride)
                return "+" + juce::String (mv) + "% LOOP " + juce::String (effLen);
            return "+" + juce::String (mv) + "% VEL " + juce::String (eff.velocity);
        }
        case sliderseq::MacroMode::density: {
            const int pct = (mv - 50) * 2;
            if (std::abs (pct) < 2) return "0% ORIG";
            if (pct < 0) {
                colourKind = 2;
                return juce::String (pct) + "% THIN";
            }
            colourKind = 1;
            if (pct > 82) return "+" + juce::String (pct) + "% GHOST+3x";
            if (pct > 55) return "+" + juce::String (pct) + "% GHOST+2x";
            return "+" + juce::String (pct) + "% GHOST";
        }
        case sliderseq::MacroMode::roll: {
            if (mv < 5) return "OFF";
            colourKind = 1;
            if (doubledRate) return "2x " + juce::String (effLen) + "-STEP ROLL";
            if (effLen == 1) return "1-STEP ROLL";
            if (effLen == 2) return "2-STEP STUT";
            if (effLen == 3 || effLen == 6) return juce::String (effLen) + "-STEP POLY";
            return juce::String (effLen) + "-STEP LOOP";
        }
        case sliderseq::MacroMode::human: {
            const int pct = (mv - 50) * 2;
            if (std::abs (pct) < 2) return "0% GRID";
            if (pct < 0) {
                colourKind = 2;
                return juce::String (pct) + "% TIGHT";
            }
            colourKind = 1;
            return "+" + juce::String (pct) + "% LOOSE";
        }
        case sliderseq::MacroMode::velocity:
            colourKind = 1;
            return "VEL " + juce::String (eff.velocity);
        case sliderseq::MacroMode::swing:
            colourKind = mv > 0 ? 1 : 0;
            return "SWING " + juce::String (eff.groove) + "%";
        case sliderseq::MacroMode::loopSteps:
            colourKind = eff.loopStepsOverride > 0 ? 1 : 0;
            return eff.loopStepsOverride > 0 ? ("LOOP " + juce::String (eff.loopStepsOverride) + " ST") : "LOOP OFF";
        case sliderseq::MacroMode::rate: {
            colourKind = 1;
            const int idx = std::clamp (static_cast<int> (std::lround (mv * 4.0 / 100.0)), 0, 4);
            return "RATE " + juce::String (kRateLabels[static_cast<size_t> (idx)]);
        }
        case sliderseq::MacroMode::modAmount:
            colourKind = mv > 0 ? 1 : 0;
            return "MOD " + juce::String (eff.modAmount) + "%";
        case sliderseq::MacroMode::count:
            break;
    }
    return "0% IDLE";
}

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
    juce::Slider::SliderLayout getSliderLayout (juce::Slider& slider) override {
        auto layout = juce::LookAndFeel_V4::getSliderLayout (slider);
        const int pad = std::min (14, std::max (8, slider.getHeight() / 8));
        layout.sliderBounds = slider.getLocalBounds().reduced (0, pad);
        return layout;
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float, const juce::Slider::SliderStyle,
                           juce::Slider& slider) override {
        const auto& p = currentPalette();
        const int activeZones = std::clamp (static_cast<int> (slider.getProperties().getWithDefault ("activeZones", 12)),
                                            1, sliderseq::slotsPerTrack);
        const auto topMode = sliderseq::clampEndZoneMode (static_cast<int> (slider.getProperties().getWithDefault ("topMode", 2)));
        const auto bottomMode = sliderseq::clampEndZoneMode (static_cast<int> (slider.getProperties().getWithDefault ("bottomMode", 1)));

        const int minS = sliderseq::trackMinSlot (bottomMode);
        const int maxS = sliderseq::trackMaxSlot (activeZones, topMode);
        const int curVal = std::clamp (static_cast<int> (std::lround (slider.getValue())), minS, maxS);
        const bool inBot = sliderseq::isTrackInBottomZone (curVal, bottomMode);
        const bool inTop = sliderseq::isTrackInTopZone (curVal, activeZones, topMode);
        const bool inGen = ! inBot && ! inTop;

        const float left = static_cast<float> (x);
        const float top = static_cast<float> (y);
        // Exact 1:1 alignment with JUCE sliderBounds (sliderPos = top + height * (1 - prop)).
        const float travelTop = top;
        const float travelBottom = top + static_cast<float> (height);
        // Rail sits at local X = 40 inside the 64px fader box (aligned to the exact 112px horizontal center of each 224px strip).
        const float railX = left + std::min (static_cast<float> (width) * 0.625f, static_cast<float> (width) - 24.0f);
        const float span = static_cast<float> (std::max (1, maxS - minS));

        auto yForVal = [&] (int v) noexcept {
            const float prop = static_cast<float> (v - minS) / span;
            return travelBottom - (travelBottom - travelTop) * prop;
        };

        g.setColour (p.faderSlotBg);
        g.fillRoundedRectangle (railX - 3.0f, travelTop - 3.0f, 6.0f, (travelBottom - travelTop) + 6.0f, 1.5f);
        g.setColour (p.hairline);
        g.drawRoundedRectangle (railX - 3.0f, travelTop - 3.0f, 6.0f, (travelBottom - travelTop) + 6.0f, 1.5f, 1.0f);

        if (bottomMode != sliderseq::EndZoneMode::off) {
            const float sepBot = (yForVal (0) + yForVal (1)) * 0.5f;
            g.setColour (p.hairline);
            g.drawHorizontalLine (static_cast<int> (std::round (sepBot)), left + 2.0f, left + static_cast<float> (width) - 2.0f);
        }

        if (topMode != sliderseq::EndZoneMode::off) {
            const float sepTop = (yForVal (activeZones) + yForVal (activeZones + 1)) * 0.5f;
            g.setColour (p.hairline);
            g.drawHorizontalLine (static_cast<int> (std::round (sepTop)), left + 2.0f, left + static_cast<float> (width) - 2.0f);
        }

        const int totalSteps = maxS - minS;
        const float stepPx = (travelBottom - travelTop) / static_cast<float> (std::max (1, totalSteps));
        const bool sparseLabels = (totalSteps >= 10) && (stepPx < 13.5f);
        const int numAreaW = std::max (15, static_cast<int> (railX - left) - 24);
        for (int z = minS; z <= maxS; ++z) {
            const float markY = yForVal (z);
            const bool isCurrent = (z == curVal);
            const bool isBotTick = sliderseq::isTrackInBottomZone (z, bottomMode);
            const bool isTopTick = sliderseq::isTrackInTopZone (z, activeZones, topMode);

            if (isBotTick) {
                const auto col = bottomMode == sliderseq::EndZoneMode::thru ? p.cyan : p.orange;
                g.setColour (isCurrent ? col : col.withAlpha (0.85f));
                g.setFont (braun::monoFont (9.6f));
                g.drawText ("0", x, static_cast<int> (markY - 6), numAreaW, 12, juce::Justification::centredRight);
                g.drawHorizontalLine (static_cast<int> (std::round (markY)), railX - 16.0f, railX - 6.0f);
                g.drawHorizontalLine (static_cast<int> (std::round (markY)), railX + 6.0f, railX + 16.0f);
            } else if (isTopTick) {
                const auto col = topMode == sliderseq::EndZoneMode::thru ? p.cyan : p.orange;
                g.setColour (isCurrent ? col : col.withAlpha (0.85f));
                g.setFont (braun::monoFont (9.6f));
                g.drawText ("T", x, static_cast<int> (markY - 6), numAreaW, 12, juce::Justification::centredRight);
                g.drawHorizontalLine (static_cast<int> (std::round (markY)), railX - 16.0f, railX - 6.0f);
                g.drawHorizontalLine (static_cast<int> (std::round (markY)), railX + 6.0f, railX + 16.0f);
            } else {
                const bool isFill = (activeZones >= 10 && z >= activeZones - 2)
                                 || (activeZones >= 4 && activeZones < 10 && z == activeZones);
                g.setColour (isCurrent ? p.ink : (isFill ? p.orange : p.mutedInk));
                g.setFont (braun::monoFont (9.6f));
                const bool nearOddCurrent = sparseLabels && inGen && (curVal % 2 != 0)
                                         && ! isCurrent && std::abs (z - curVal) == 1;
                const bool drawNum = ! sparseLabels
                    || isCurrent
                    || (! nearOddCurrent && (z == 1 || z == activeZones || (z % 2 == 0)));
                if (drawNum) {
                    g.drawText (juce::String (z), x, static_cast<int> (markY - 6),
                                numAreaW, 12, juce::Justification::centredRight);
                }
                const float tickHalf = isFill ? 16.0f : (isCurrent ? 14.0f : 11.5f);
                g.drawHorizontalLine (static_cast<int> (std::round (markY)), railX - tickHalf, railX - 6.0f);
                g.drawHorizontalLine (static_cast<int> (std::round (markY)), railX + 6.0f, railX + tickHalf);
            }
        }

        const float capY = juce::jlimit (travelTop, travelBottom, sliderPos);
        const float y1 = yForVal (1);
        if (inGen) {
            g.setColour (p.orange);
            g.fillRect (railX - 1.5f, std::min (capY, y1), 3.0f, std::abs (y1 - capY) + 1.0f);
        }

        const juce::Rectangle<float> cap (railX - 19.0f, capY - 7.5f, 38.0f, 15.0f);
        const bool isThruCap = (inTop && topMode == sliderseq::EndZoneMode::thru)
                            || (inBot && bottomMode == sliderseq::EndZoneMode::thru);
        const bool isBlockCap = (inTop && topMode == sliderseq::EndZoneMode::block)
                             || (inBot && bottomMode == sliderseq::EndZoneMode::block);

        g.setColour ((isThruCap || isBlockCap) ? p.panelWell : p.capSurface);
        g.fillRoundedRectangle (cap, 2.0f);
        const auto capAccent = isThruCap ? p.cyan : (isBlockCap ? p.orange : p.controlBorder);
        g.setColour (capAccent);
        g.drawRoundedRectangle (cap, 2.0f, (isThruCap || isBlockCap) ? 1.4f : 1.0f);
        const auto slitCol = isThruCap ? p.cyan : p.orange;
        g.setColour (slitCol);
        g.fillRect (cap.getX() + 6.0f, capY - 1.0f, cap.getWidth() - 12.0f, 2.0f);
    }
};

class BraunSlider final : public juce::Slider {
public:
    std::function<bool()> interceptLearn;
    std::function<bool()> allowVerticalWheel;
    std::function<bool()> allowHorizontalWheel;
    std::function<void()> onRightClick;

    void mouseDown (const juce::MouseEvent& event) override {
        if (event.mods.isPopupMenu() && onRightClick) {
            learningPress = true;
            onRightClick();
            return;
        }
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
        // Never allow trackpad inertial coasting to keep stepping knobs/faders; pass inertia cleanly to Viewport.
        if (wheel.isInertial) {
            forwardWheelToAncestor (this, event, wheel);
            return;
        }
        const bool allowV = allowVerticalWheel ? allowVerticalWheel() : true;
        const bool allowH = allowHorizontalWheel ? allowHorizontalWheel() : false;
        const int64_t nowMs = juce::Time::currentTimeMillis();
        if (nowMs - lastWheelTimeMs > 180)
            lockedHorizontal = std::abs (wheel.deltaX) > std::abs (wheel.deltaY);
        lastWheelTimeMs = nowMs;

        if (lockedHorizontal) {
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
    bool lockedHorizontal = false;
    int64_t lastWheelTimeMs = 0;
};

class BraunButton : public juce::TextButton {
public:
    using juce::TextButton::TextButton;
    std::function<bool()> interceptLearn;
    std::function<void(const juce::ModifierKeys&)> onActivated;

    void mouseDown (const juce::MouseEvent& event) override {
        lastMouseMods = event.mods;
        learningPress = interceptLearn && interceptLearn();
        if (! learningPress) juce::TextButton::mouseDown (event);
    }
    void mouseUp (const juce::MouseEvent& event) override {
        lastMouseMods = event.mods;
        if (! learningPress) {
            juce::Component::SafePointer<BraunButton> safeSelf (this);
            juce::TextButton::mouseUp (event);
            if (safeSelf == nullptr) return;
        }
        learningPress = false;
    }
    void clicked (const juce::ModifierKeys& modifiers) override {
        if (onActivated) {
            juce::Component::SafePointer<BraunButton> safeSelf (this);
            const auto mods = modifiers.getRawFlags() != 0 ? modifiers : lastMouseMods;
            onActivated (mods);
            if (safeSelf == nullptr) return;
        }
        juce::TextButton::clicked (modifiers);
    }

private:
    bool learningPress = false;
    juce::ModifierKeys lastMouseMods;
};

class ThemeToggleButton final : public juce::TextButton {
public:
    void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override {
        getLookAndFeel().drawButtonBackground (
            g, *this, findColour (juce::TextButton::buttonColourId),
            shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
        const auto* lnf = dynamic_cast<const braun::BraunLookAndFeel*> (&getLookAndFeel());
        const auto p = lnf != nullptr ? lnf->currentPalette() : braun::Palette::dark();
        const auto tb = getLocalBounds();
        g.setFont (braun::monoFont (10.8f));
        const int segW = tb.getWidth() / 3;
        g.setColour (p.isDark ? p.orange : p.mutedInk);
        g.drawText ("DARK", tb.getX(), tb.getY(), segW + 8, tb.getHeight(), juce::Justification::centredRight);
        g.setColour (p.dimInk);
        g.drawText ("/", tb.getX() + segW + 8, tb.getY(), 12, tb.getHeight(), juce::Justification::centred);
        g.setColour (p.isDark ? p.mutedInk : p.orange);
        g.drawText ("LGT", tb.getX() + segW + 20, tb.getY(), std::max (20, tb.getWidth() - (segW + 20)), tb.getHeight(),
                    juce::Justification::centredLeft);
    }
};

class TrackTitleButton final : public juce::TextButton {
public:
    void setTitleParts (const juce::String& numberText, const juce::String& nameText) {
        if (trackNum == numberText && trackName == nameText) return;
        trackNum = numberText;
        trackName = nameText;
        setButtonText (trackNum + " " + trackName);
        repaint();
    }

    void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override {
        getLookAndFeel().drawButtonBackground (
            g, *this, findColour (juce::TextButton::buttonColourId),
            shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
        const auto* lnf = dynamic_cast<const braun::BraunLookAndFeel*> (&getLookAndFeel());
        const auto p = lnf != nullptr ? lnf->currentPalette() : braun::Palette::dark();
        const auto b = getLocalBounds();
        // Unified 2.0 px physical pitch dot-matrix track index pill (Spleen 6x12).
        const juce::Rectangle<int> idxBox (1, 1, 32, std::max (18, b.getHeight() - 2));
        dotmatrix::paintDisplayPanel (g, idxBox, dotmatrix::kFont6x12, trackNum, p.orange, p.hairline,
                                      juce::Justification::centred, true);

        g.setFont (braun::monoFont (12.4f));
        g.setColour (p.ink);
        const int nameX = idxBox.getRight() + 5;
        const int nameW = std::max (20, b.getWidth() - nameX - 12);
        g.drawFittedText (trackName, nameX, 1, nameW, b.getHeight() - 1,
                          juce::Justification::centredLeft, 1, 0.78f);
        braun::drawDropdownCaret (g, static_cast<float> (b.getRight() - 6), b.toFloat().getCentreY() + 0.5f,
                                  (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown) ? p.ink : p.mutedInk);
    }

private:
    juce::String trackNum;
    juce::String trackName;
};

class BraunComboBox final : public juce::ComboBox {
public:
    std::function<bool()> interceptLearn;
    std::function<bool()> allowVerticalWheel;
    std::function<bool()> allowHorizontalWheel;

    BraunComboBox() {
        setScrollWheelEnabled (true);
    }

    void mouseDown (const juce::MouseEvent& event) override {
        if (interceptLearn && interceptLearn()) return;
        juce::ComboBox::mouseDown (event);
    }
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override {
        if (wheel.isInertial) {
            forwardWheelToAncestor (this, event, wheel);
            return;
        }
        const bool allowV = allowVerticalWheel ? allowVerticalWheel() : true;
        const bool allowH = allowHorizontalWheel ? allowHorizontalWheel() : false;
        const int64_t nowMs = juce::Time::currentTimeMillis();
        if (nowMs - lastWheelTimeMs > 180)
            lockedHorizontal = std::abs (wheel.deltaX) > std::abs (wheel.deltaY);
        lastWheelTimeMs = nowMs;

        if (lockedHorizontal) {
            if (allowH) {
                // JUCE ComboBox::mouseWheelMove only inspects deltaY; translate horizontal axis into deltaY when enabled.
                auto mapped = wheel;
                mapped.deltaY = wheel.deltaX;
                mapped.deltaX = 0.0f;
                juce::ComboBox::mouseWheelMove (event, mapped);
            } else {
                forwardWheelToAncestor (this, event, wheel);
            }
            return;
        }
        if (allowV) {
            auto filtered = wheel;
            filtered.deltaX = 0.0f;
            juce::ComboBox::mouseWheelMove (event, filtered);
        } else {
            forwardWheelToAncestor (this, event, wheel);
        }
    }

private:
    bool lockedHorizontal = false;
    int64_t lastWheelTimeMs = 0;
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
            const auto offBase = downbeat
                ? (pal.isDark ? pal.panelWell.brighter (0.06f) : pal.panelWell.darker (0.06f))
                : pal.capSurface;
            g.setColour (offBase.withAlpha (alpha));
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
    braun::Palette pal = braun::Palette::dark();
};
} // namespace

struct SliderRhythmMidiEditor::LookAndFeelResources {
    braun::BraunLookAndFeel main;
    BraunZoneFaderLookAndFeel zoneFader;
    const std::vector<trackpresets::Bank> factoryTrackBanks = trackpresets::factoryBanks();
    std::mutex prefsMutex;
    uiprefs::Settings sharedPrefs = uiprefs::load();
    std::vector<trackpresets::Bank> cachedUserTrackBanks = trackpresets::loadUserBanks();
    std::atomic<uint64_t> prefsGeneration { 1 };
    std::atomic<bool> verticalWheel { true };
    std::atomic<bool> horizontalWheel { false };

    LookAndFeelResources() {
        verticalWheel.store (sharedPrefs.verticalWheelControls, std::memory_order_relaxed);
        horizontalWheel.store (sharedPrefs.horizontalWheelControls, std::memory_order_relaxed);
        applyPalette (braun::Palette::forMode (sharedPrefs.darkMode));
    }

    std::vector<trackpresets::Bank> userTrackBanks() {
        std::lock_guard<std::mutex> lock (prefsMutex);
        return cachedUserTrackBanks;
    }

    std::vector<trackpresets::Bank> reloadUserTrackBanks() {
        auto fresh = trackpresets::loadUserBanks();
        std::lock_guard<std::mutex> lock (prefsMutex);
        cachedUserTrackBanks = std::move (fresh);
        return cachedUserTrackBanks;
    }

    uiprefs::Settings currentPrefs() {
        std::lock_guard<std::mutex> lock (prefsMutex);
        return sharedPrefs;
    }

    template <typename MutateFn>
    uiprefs::Settings updatePrefs (MutateFn&& mutate, bool* savedOk = nullptr) {
        std::lock_guard<std::mutex> lock (prefsMutex);
        bool ok = false;
        const auto next = uiprefs::updateAndSave (std::forward<MutateFn> (mutate), uiprefs::settingsFile(), &ok);
        if (savedOk != nullptr) *savedOk = ok;
        if (! ok)
            return sharedPrefs;
        sharedPrefs = next;
        verticalWheel.store (sharedPrefs.verticalWheelControls, std::memory_order_relaxed);
        horizontalWheel.store (sharedPrefs.horizontalWheelControls, std::memory_order_relaxed);
        applyPalette (braun::Palette::forMode (sharedPrefs.darkMode));
        prefsGeneration.fetch_add (1, std::memory_order_release);
        return sharedPrefs;
    }

    void applyPalette (const braun::Palette& p) {
        main.applyPalette (p);
        zoneFader.applyPalette (p);
    }
};

// Matte-black #131312 Braun oscilloscope monitor bay showing global note timing offset across 2 beats.
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
        g.fillRect (b);
        g.setColour (pal.hairline);
        g.drawRect (b.reduced (0.5f), 1.0f);

        const float padX = 10.0f;
        const bool showBadge = b.getWidth() >= 120.0f;
        const float badgeW = showBadge ? 64.0f : 0.0f;
        const float plotRight = showBadge
            ? std::max (padX + 40.0f, b.getRight() - badgeW - 6.0f)
            : (b.getRight() - padX);
        const float usableW = std::max (20.0f, plotRight - padX);
        const float midY = b.getCentreY();
        const float topY = b.getY() + 5.0f;
        const float botY = b.getBottom() - 5.0f;

        g.setColour (pal.scopeGrid);
        g.drawHorizontalLine (static_cast<int> (std::round (midY)), b.getX() + 4.0f, plotRight + 4.0f);
        if (showBadge)
            g.drawVerticalLine (static_cast<int> (std::round (plotRight + 5.0f)), topY, botY);

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
            const float dotY = isQuarter ? midY - 5.5f : (s % 2 == 0 ? midY + 4.5f : midY + 1.0f);

            if (moved) {
                g.setColour (pal.cyan.withAlpha (0.92f));
                g.drawLine (baseX, dotY, shiftedX, dotY, 2.0f);
            }
            g.setColour (moved ? pal.cyan : (isQuarter ? pal.orange : pal.scopeTrace.withAlpha (0.72f)));
            const float r = isQuarter ? 2.8f : (moved ? 2.5f : 2.0f);
            g.fillEllipse (shiftedX - r, dotY - r, r * 2.0f, r * 2.0f);
        }

        if (showBadge) {
            g.setFont (braun::monoFont (9.8f));
            g.setColour (swing == 0 ? pal.scopeTrace.withAlpha (0.65f) : pal.cyan);
            const juce::String modeTxt = swing == 0 ? "STRAIGHT"
                                       : swing < 0 ? "16TH SHF" : "8TH SWG";
            g.drawFittedText (modeTxt, static_cast<int> (plotRight + 7.0f), 0, static_cast<int> (badgeW - 5.0f), getHeight(),
                              juce::Justification::centred, 1, 0.85f);
        }
    }

private:
    int swing = 0;
    braun::Palette pal = braun::Palette::dark();
};

// Matte-black #131312 Braun oscilloscope bay visualizing modulated velocity, DENSITY ghosts/thinning, ratchets, & loop cap.
class SliderRhythmMidiEditor::VelocityModScope final : public juce::Component {
public:
    void setTrackData (const sliderseq::Track& track, int activePatIdx, int activeStepIndex, const braun::Palette& palette) {
        const int patIdx = std::clamp (activePatIdx, 0, sliderseq::slotsPerTrack - 1);
        const auto& pat = track.patterns[static_cast<size_t> (patIdx)];
        const auto eff = sliderseq::computeEffectiveTrackForPattern (track, pat);
        baseLen = sliderseq::effectivePatternLength (pat.length, pat.grid, track.loopStepsOverride);
        rollStepOffset = eff.rollStepOffset;
        activeLen = sliderseq::effectivePatternLength (pat.length, pat.grid, eff.loopStepsOverride);
        baseVel = std::clamp (static_cast<int> (std::lround (eff.velocity * std::clamp (pat.velocityScale, 0.1f, 2.0f))), 1, 127);
        playheadStep = activeStepIndex;
        isGenerating = sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode);
        pal = palette;

        const double stepTicks = pat.stepTicks();
        const double period = std::max (1.0, activeLen * stepTicks);
        for (int s = 0; s < activeLen && s < 64; ++s) {
            const int srcStep = (s + eff.rollStepOffset) % std::max (1, baseLen);
            const auto eval = sliderseq::evaluateMacroStep (pat.steps[static_cast<size_t> (srcStep)],
                                                            s, activeLen, stepTicks, track.midiNote, eff.densityDelta);
            stepEval[static_cast<size_t> (s)] = eval;
            const double phase = s * stepTicks;
            // Exact single-round, single-clamp parity with SequencerEngine audio output.
            envelopeVel[static_cast<size_t> (s)] = sliderseq::computeFinalStepVelocity (
                eff.velocity, track.modShape, eff.modAmount, track.midiNote, phase, period, pat.velocityScale, 1.0f);
            stepVel[static_cast<size_t> (s)] = sliderseq::computeFinalStepVelocity (
                eff.velocity, track.modShape, eff.modAmount, track.midiNote, phase, period, pat.velocityScale, eval.velocityFactor);
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
        g.fillRect (b);
        g.setColour (pal.hairline);
        g.drawRect (b.reduced (0.5f), 1.0f);

        const float padX = 5.0f;
        const float padTop = 5.0f;
        const float padBot = 4.0f;
        const float plotW = std::max (10.0f, b.getWidth() - padX * 2.0f);
        const float plotH = std::max (10.0f, b.getHeight() - padTop - padBot);
        const float bottomY = b.getBottom() - padBot;

        g.setColour (pal.scopeGrid);
        for (float frac : { 0.25f, 0.50f, 0.75f })
            g.drawHorizontalLine (static_cast<int> (std::round (bottomY - plotH * frac)), b.getX() + 3.0f, b.getRight() - 3.0f);

        const float baseY = bottomY - plotH * (static_cast<float> (baseVel) / 127.0f);
        g.setColour (pal.scopeTrace.withAlpha (0.35f));
        for (float dx = padX; dx < b.getRight() - padX; dx += 6.0f)
            g.drawHorizontalLine (static_cast<int> (std::round (baseY)), dx, std::min (dx + 3.0f, b.getRight() - padX));

        const int count = std::clamp (activeLen, 1, 64);
        const int localPlayhead = (playheadStep >= 0 && baseLen > 0)
            ? ((playheadStep - rollStepOffset + baseLen * 4) % baseLen)
            : -1;
        const float stepW = plotW / static_cast<float> (count);
        const float barW = std::max (1.5f, stepW - (count > 24 ? 0.8f : 2.0f));
        const float alphaDim = isGenerating ? 1.0f : 0.35f;

        juce::Path envelope;
        for (int s = 0; s < count; ++s) {
            const auto& ev = stepEval[static_cast<size_t> (s)];
            const float envNorm = static_cast<float> (envelopeVel[static_cast<size_t> (s)]) / 127.0f;
            const float vNorm = static_cast<float> (stepVel[static_cast<size_t> (s)]) / 127.0f;
            const float barH = std::max (2.0f, plotH * vNorm);
            const float cx = padX + (static_cast<float> (s) + 0.5f) * stepW;
            const float topY = bottomY - barH;
            const float envY = bottomY - std::max (2.0f, plotH * envNorm);

            if (s == localPlayhead && isGenerating) {
                g.setColour (juce::Colours::white.withAlpha (0.14f));
                g.fillRect (padX + static_cast<float> (s) * stepW, b.getY() + 2.0f, stepW, b.getHeight() - 4.0f);
            }

            if (ev.isGhost) {
                g.setColour (pal.cyan.withAlpha (0.85f * alphaDim));
                g.fillRect (cx - barW * 0.5f, topY, barW, barH);
            } else if (ev.isThinned) {
                g.setColour (pal.cyan.withAlpha (0.45f * alphaDim));
                g.drawRect (cx - barW * 0.5f, topY, barW, barH, 0.8f);
            } else if (ev.fires) {
                g.setColour (pal.orange.withAlpha (0.88f * alphaDim));
                g.fillRect (cx - barW * 0.5f, topY, barW, barH);
                if (ev.ratchetCount > 1) {
                    g.setColour (juce::Colours::white.withAlpha (0.85f * alphaDim));
                    g.fillRect (cx - barW * 0.5f, std::max (b.getY() + 2.0f, topY - 2.5f), barW, 1.8f);
                }
            } else {
                g.setColour (pal.scopeTrace.withAlpha (0.24f * alphaDim));
                g.fillRect (cx - barW * 0.5f, envY, barW, bottomY - envY);
            }

            if (s == 0) envelope.startNewSubPath (cx, envY);
            else envelope.lineTo (cx, envY);
        }

        g.setColour (pal.orange.withAlpha (0.92f * alphaDim));
        g.strokePath (envelope, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    int baseLen = 16;
    int rollStepOffset = 0;
    int activeLen = 16;
    int baseVel = 100;
    int playheadStep = -1;
    bool isGenerating = true;
    std::array<sliderseq::MacroStepEval, 64> stepEval {};
    std::array<int, 64> stepVel {};
    std::array<int, 64> envelopeVel {};
    braun::Palette pal = braun::Palette::light();
};

// Matte-black #131312 Braun oscilloscope bay showing per-track note timing shift (Global Swing x effective Track Swing %).
class SliderRhythmMidiEditor::TrackSwingScope final : public juce::Component {
public:
    void setTiming (int globalSwingVal, int trackSwingPercentVal, bool invertSignVal, const braun::Palette& palette) {
        if (globalSwing == globalSwingVal && trackPercent == trackSwingPercentVal
            && invertSign == invertSignVal && pal.isDark == palette.isDark)
            return;
        globalSwing = globalSwingVal;
        trackPercent = trackSwingPercentVal;
        invertSign = invertSignVal;
        pal = palette;
        repaint();
    }

    void paint (juce::Graphics& g) override {
        const auto b = getLocalBounds().toFloat();
        g.setColour (pal.scopeBg);
        g.fillRect (b);
        g.setColour (pal.hairline);
        g.drawRect (b.reduced (0.5f), 1.0f);

        const float padX = 9.0f;
        const float usableW = std::max (12.0f, b.getWidth() - padX * 2.0f);
        const float midY = b.getCentreY();
        const float topY = b.getY() + 5.0f;
        const float botY = b.getBottom() - 5.0f;

        g.setColour (pal.scopeGrid);
        g.drawHorizontalLine (static_cast<int> (std::round (midY)), b.getX() + 3.0f, b.getRight() - 3.0f);

        const int signedGlobal = invertSign ? -globalSwing : globalSwing;
        constexpr double spanTicks = 288.0;
        for (int s = 0; s <= 4; ++s) {
            const double nominal = s * (spanTicks / 4.0);
            const float baseX = padX + static_cast<float> (nominal / spanTicks) * usableW;
            const bool isBeat = (s == 0 || s == 4);
            const bool isEighth = (s == 2);
            g.setColour (isBeat ? pal.scopeTrace.withAlpha (0.48f) : pal.scopeGrid.brighter (0.24f));
            g.drawLine (baseX, topY, baseX, botY, isBeat ? 1.15f : 0.85f);

            const double offset = sliderseq::grooveOffsetValue (signedGlobal, trackPercent, nominal);
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
    bool invertSign = false;
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
        g.setColour (dragging ? pal.orange : pal.orangeTint);
        g.fillRoundedRectangle (bounds, 1.5f);
        g.setColour (dragging ? juce::Colours::white : pal.orange);
        g.setFont (braun::monoFont (11.0f));
        const auto text = "[ " + (getLabelText ? getLabelText() : juce::String ("DRAG 4B")) + " ]";
        g.drawFittedText (text, getLocalBounds().reduced (2, 0), juce::Justification::centred, 1, 0.82f);
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
        if (file.existsAsFile() && file.getSize() > 0)
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
    std::function<void()> onCancelLearn;
    std::function<juce::Component*()> onRequestDialogParent;
    std::function<void(const sliderseq::Pattern&)> onCopyPattern;
    std::function<std::optional<sliderseq::Pattern>()> onGetClipboard;
    std::function<void()> onTracksModified;

    TrackStrip (SliderRhythmMidiProcessor& owner, int number,
                juce::SharedResourcePointer<LookAndFeelResources> sharedLnf,
                const SliderRhythmMidiProcessor::Snapshot& initialSnapshot)
        : processor (owner), index (number), lookAndFeel (sharedLnf) {
        auto bindWheel = [this] (auto& comp) {
            comp.allowVerticalWheel = [this] { return lookAndFeel->verticalWheel.load (std::memory_order_relaxed); };
            comp.allowHorizontalWheel = [this] { return lookAndFeel->horizontalWheel.load (std::memory_order_relaxed); };
        };

        addAndMakeVisible (titleButton);
        titleButton.setTooltip ("12-slot Track Bank Preset selector (preserves MIDI note & channel) + rename, copy/paste slot, duplicate track");
        titleButton.onClick = [this] { cancelChoosingLearn(); showTrackPresetAndOptionsMenu(); };

        addAndMakeVisible (zonesButton);
        zonesButton.setTooltip ("Set number of active pattern zones N on this track's fader (1-12). DAW automation tXX_slot spans 0..13 (0=BOT, 1..N=Slots, N+1..13=TOP).");
        zonesButton.onClick = [this] { cancelChoosingLearn(); showZonesMenu(); };

        addAndMakeVisible (midRouteBtn);
        midRouteBtn.setTooltip ("Middle 1..N zone incoming MIDI policy: GEN (replace input) vs GEN+THRU (layer over input)");
        midRouteBtn.onClick = [this] {
            cancelChoosingLearn();
            const auto cur = processor.snapshot()->sequence.tracks[static_cast<size_t> (index)].midMode;
            const auto next = cur == sliderseq::MidRouteMode::gen
                ? sliderseq::MidRouteMode::genPlusThru : sliderseq::MidRouteMode::gen;
            processor.setTrackMidMode (index, next);
            refreshFromProcessor();
        };

        addAndMakeVisible (topRouteBtn);
        topRouteBtn.setTooltip ("Top end-zone above N: THRU (pass input) / BLK (block input) / OFF (no top detent)");
        topRouteBtn.onClick = [this] {
            cancelChoosingLearn();
            const auto cur = processor.snapshot()->sequence.tracks[static_cast<size_t> (index)].topMode;
            const auto next = cur == sliderseq::EndZoneMode::thru ? sliderseq::EndZoneMode::block
                            : cur == sliderseq::EndZoneMode::block ? sliderseq::EndZoneMode::off
                            : sliderseq::EndZoneMode::thru;
            processor.setTrackTopMode (index, next);
            refreshFromProcessor();
        };

        addAndMakeVisible (botRouteBtn);
        botRouteBtn.setTooltip ("Bottom 0 end-zone below 1: BLOCK (silence + block input) / THRU (bypass input) / OFF (no 0 detent)");
        botRouteBtn.onClick = [this] {
            cancelChoosingLearn();
            const auto cur = processor.snapshot()->sequence.tracks[static_cast<size_t> (index)].bottomMode;
            const auto next = cur == sliderseq::EndZoneMode::block ? sliderseq::EndZoneMode::thru
                            : cur == sliderseq::EndZoneMode::thru ? sliderseq::EndZoneMode::off
                            : sliderseq::EndZoneMode::block;
            processor.setTrackBottomMode (index, next);
            refreshFromProcessor();
        };

        muteButton.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::mute); };
        soloButton.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::solo); };
        muteButton.setTooltip ("Mute track (post-routing: mutes both GEN and THRU for this track)");
        soloButton.setTooltip ("Solo track (exclusive; Cmd/Ctrl+click for multi-track solo; post-routing)");
        addAndMakeVisible (muteButton);
        addAndMakeVisible (soloButton);

        // Standard button activation works for both mouse clicks and keyboard/accessibility activation.
        muteButton.onActivated = [this] (const juce::ModifierKeys&) {
            const auto current = processor.snapshot();
            const bool nextMuted = ! current->sequence.tracks[static_cast<size_t> (index)].muted;
            processor.setHostParamValue (index, "mute", nextMuted ? 1.0f : 0.0f);
            refreshFromProcessor();
        };
        soloButton.onActivated = [this] (const juce::ModifierKeys& mods) {
            if (mods.isCommandDown() || mods.isCtrlDown())
                processor.toggleTrackSoloAdditive (index);
            else
                processor.soloTrackExclusive (index);
            if (onTracksModified) onTracksModified();
        };

        // Hero 3-zone vertical fader with non-nested host automation gestures
        slot.setSliderStyle (juce::Slider::LinearVertical);
        slot.setLookAndFeel (&lookAndFeel->zoneFader);
        slot.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slot.setRange (0, 13, 1);
        slot.setDoubleClickReturnValue (true, static_cast<double> (sliderseq::defaultTrackSlot));
        slot.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::slot); };
        bindWheel (slot);
        slot.onDragStart = [this] {
            slotDragging = true;
            if (auto* p = processor.parameters().getParameter (SliderRhythmMidiProcessor::paramId (index, "slot")))
                p->beginChangeGesture();
        };
        slot.onDragEnd = [this] {
            if (slotDragging) {
                slotDragging = false;
                if (auto* p = processor.parameters().getParameter (SliderRhythmMidiProcessor::paramId (index, "slot")))
                    p->endChangeGesture();
            }
        };
        slot.onValueChange = [this] {
            if (suppressCallbacks) return;
            const int v = static_cast<int> (std::lround (slot.getValue()));
            // Do not wrap nested begin/end change gestures while drag gesture is already open.
            processor.setHostParamValue (index, "slot", static_cast<float> (v), ! slotDragging);
            refreshFromProcessor();
        };
        addAndMakeVisible (slot);

        for (int i = 0; i < 5; ++i) {
            auto& btn = rateButtons[static_cast<size_t> (i)];
            btn.setButtonText (kRateLabels[static_cast<size_t> (i)]);
            btn.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::rate); };
            btn.onClick = [this, i] {
                processor.setHostParamValue (index, "rate", static_cast<float> (i));
                refreshFromProcessor();
            };
            addAndMakeVisible (btn);
        }

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
        setupKnob (macroDial, 0, 100, 0, SliderRhythmMidiProcessor::macro);
        macroDial.setTooltip ("Per-track performance Macro knob (MIDI-learnable & DAW-automatable; double-click resets to neutral)");

        velocityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "velocity"), velocity);
        modAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "mod_amt"), modAmount);
        swingPctAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "groove"), swingPct);
        macroAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "macro"), macroDial);

        velocity.onValueChange = [this] { if (! suppressCallbacks) refreshVisualsOnly(); };
        modAmount.onValueChange = [this] { if (! suppressCallbacks) refreshVisualsOnly(); };
        swingPct.onValueChange = [this] { if (! suppressCallbacks) refreshVisualsOnly(); };
        macroDial.onValueChange = [this] { if (! suppressCallbacks) refreshVisualsOnly(); };

        modButton.getProperties().set ("dropdown", true);
        modButton.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::modMode); };
        modButton.onClick = [this] { showModShapeMenu(); };
        addAndMakeVisible (modButton);

        addAndMakeVisible (macroModeBtn);
        macroModeBtn.getProperties().set ("dropdown", true);
        macroModeBtn.setTooltip ("Choose performance Macro behavior or direct single-parameter target for this track's hardware knob");
        macroModeBtn.onClick = [this] { cancelChoosingLearn(); showMacroModeMenu(); };

        addAndMakeVisible (velocityScope);
        addAndMakeVisible (swingScope);

        patternName.getProperties().set ("dropdown", true);
        patternName.getProperties().set ("alignLeft", true);
        patternName.onClick = [this] { cancelChoosingLearn(); showPatternMenu(); };
        patternName.setTooltip ("Load a single-slot beat or save this slot as a preset");
        addAndMakeVisible (patternName);

        clearPatternBtn.setButtonText ("CLR");
        clearPatternBtn.setTooltip ("Clear all steps in the active pattern slot");
        clearPatternBtn.onClick = [this] {
            cancelChoosingLearn();
            const auto current = processor.snapshot();
            const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
            const int patIdx = sliderseq::resolveEditingPatternIndex (t);
            setTrack ([patIdx] (auto& tr) { tr.patterns[static_cast<size_t> (patIdx)].steps.fill (false); });
            refreshFromProcessor();
        };
        addAndMakeVisible (clearPatternBtn);

        auto setupDragBox = [&] (BraunSlider& s, double lo, double hi, double init, std::optional<SliderRhythmMidiProcessor::Control> ctrl) {
            s.setSliderStyle (juce::Slider::LinearBarVertical);
            s.setRange (lo, hi, 1.0);
            s.setValue (init, juce::dontSendNotification);
            s.setVelocityBasedMode (false);
            s.setMouseDragSensitivity (160);
            if (ctrl.has_value())
                s.interceptLearn = [this, c = *ctrl] { return intercept (c); };
            bindWheel (s);
            addAndMakeVisible (s);
        };

        // On-face Performance Row: LOOP | GATE | CHOK | VSCL
        setupDragBox (loopSteps, 0, 64, 0, std::nullopt);
        loopStepsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "loop_steps"), loopSteps);
        // Set custom formatters AFTER SliderAttachment so APVTS does not overwrite "OFF" with "0".
        loopSteps.textFromValueFunction = [] (double v) {
            const int n = static_cast<int> (std::lround (v));
            return n <= 0 ? juce::String ("OFF") : juce::String (n);
        };
        loopSteps.valueFromTextFunction = [] (const juce::String& text) {
            const auto s = text.trim().toUpperCase();
            if (s.isEmpty() || s == "OFF") return 0.0;
            return static_cast<double> (std::clamp (s.getIntValue(), 0, 64));
        };
        loopSteps.updateText();
        loopSteps.setTooltip ("Loop Steps Override (0=OFF follows pattern length; drag 1-64 or right-click for quick lengths)");
        loopSteps.onValueChange = [this] { if (! suppressCallbacks) refreshVisualsOnly(); };
        loopSteps.onRightClick = [this] { cancelChoosingLearn(); showLoopStepsMenu(); };

        setupDragBox (gateBox, 0, static_cast<double> (kGateTicks.size() - 1), 2, std::nullopt);
        gateBox.setTextBoxIsEditable (false);
        gateBox.setDoubleClickReturnValue (true, 2.0);
        gateBox.textFromValueFunction = [] (double v) {
            const int gi = std::clamp (static_cast<int> (std::lround (v)), 0, static_cast<int> (kGateShortLabels.size() - 1));
            return juce::String (kGateShortLabels[static_cast<size_t> (gi)]);
        };
        gateBox.updateText();
        gateBox.setTooltip ("Note Gate Length (drag 1/96..1/4, double-click for 1/48 default, or right-click for exact menu)");
        gateBox.onValueChange = [this] {
            if (suppressCallbacks) return;
            const int gi = std::clamp (static_cast<int> (std::lround (gateBox.getValue())), 0, static_cast<int> (kGateTicks.size() - 1));
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setGateTicks;
            op.track = index;
            op.darg = kGateTicks[static_cast<size_t> (gi)];
            processor.applyUiOp (op);
            refreshFromProcessor();
        };
        gateBox.onRightClick = [this] { cancelChoosingLearn(); showGateMenu(); };

        chokeBtn.getProperties().set ("plainText", true);
        chokeBtn.getProperties().set ("dropdown", true);
        chokeBtn.setTooltip ("Choke On Same Step: choose tracks silenced when this track hits on the same step");
        chokeBtn.onClick = [this] { cancelChoosingLearn(); showChokeMenu(); };
        addAndMakeVisible (chokeBtn);

        setupDragBox (velScaleBox, 10, 200, 100, std::nullopt);
        velScaleBox.textFromValueFunction = [] (double v) {
            return juce::String (static_cast<int> (std::lround (v))) + "%";
        };
        velScaleBox.valueFromTextFunction = [] (const juce::String& text) {
            const auto digits = text.retainCharacters ("0123456789");
            if (digits.isEmpty()) return 100.0;
            return static_cast<double> (std::clamp (digits.getIntValue(), 10, 200));
        };
        velScaleBox.updateText();
        velScaleBox.setTooltip ("Active Slot Velocity Scale (10%-200%; drag, type %, or right-click for quick values)");
        velScaleBox.onValueChange = [this] {
            if (suppressCallbacks) return;
            const auto current = processor.snapshot();
            const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
            const int patIdx = sliderseq::resolveEditingPatternIndex (t);
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setPatternVelocityScale;
            op.track = index;
            op.slotIndex = patIdx;
            op.darg = std::clamp (velScaleBox.getValue(), 10.0, 200.0) / 100.0;
            processor.applyUiOp (op);
            refreshFromProcessor();
        };
        velScaleBox.onRightClick = [this] { cancelChoosingLearn(); showVelScaleMenu(); };

        // On-face MIDI & Pattern Row: NOTE | CH | LEN | GRID
        setupDragBox (note, 0, 127, 36, SliderRhythmMidiProcessor::note);
        note.textFromValueFunction = [] (double v) {
            const int n = std::clamp (static_cast<int> (std::lround (v)), 0, 127);
            return braun::midiNoteName (n) + " " + juce::String (n);
        };
        note.valueFromTextFunction = [this] (const juce::String& text) {
            if (const auto parsed = braun::tryParseMidiNoteText (text))
                return static_cast<double> (*parsed);
            return note.getValue();
        };
        note.updateText();
        note.setTooltip ("Track MIDI Note (drag or type pitch name like C1 / F#1 or number 0-127)");

        setupDragBox (channel, 1, 16, 10, SliderRhythmMidiProcessor::channel);
        channel.setTooltip ("Track MIDI Output Channel (1-16; right-click to apply this channel to all tracks)");
        channel.onRightClick = [this] { cancelChoosingLearn(); showChannelMenu(); };

        setupDragBox (length, 1, 64, 16, SliderRhythmMidiProcessor::length);
        length.setTooltip ("Active pattern step length (1..grid)");

        note.onValueChange = [this] {
            if (suppressCallbacks) return;
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setMidiNote;
            op.track = index;
            op.arg0 = static_cast<int> (std::lround (note.getValue()));
            processor.applyUiOp (op);
            refreshFromProcessor();
        };
        channel.onValueChange = [this] {
            if (suppressCallbacks) return;
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setMidiChannel;
            op.track = index;
            op.arg0 = static_cast<int> (std::lround (channel.getValue()));
            processor.applyUiOp (op);
            refreshFromProcessor();
        };
        length.onValueChange = [this] {
            if (suppressCallbacks) return;
            const auto current = processor.snapshot();
            const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
            const int patIdx = sliderseq::resolveEditingPatternIndex (t);
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setPatternLength;
            op.track = index;
            op.slotIndex = patIdx;
            op.arg0 = static_cast<int> (std::lround (length.getValue()));
            processor.applyUiOp (op);
            refreshFromProcessor();
        };

        grid.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::grid); };
        bindWheel (grid);
        for (const auto gridSize : { 16, 12, 32, 48, 64 }) grid.addItem (juce::String (gridSize), gridSize);
        grid.onChange = [this] {
            if (suppressCallbacks) return;
            const auto current = processor.snapshot();
            const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
            const int patIdx = sliderseq::resolveEditingPatternIndex (t);
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::resizePattern;
            op.track = index;
            op.slotIndex = patIdx;
            op.arg0 = grid.getSelectedId();
            processor.applyUiOp (op);
            refreshFromProcessor();
        };
        addAndMakeVisible (grid);

        for (int i = 0; i < 64; ++i) {
            steps[static_cast<size_t> (i)] = std::make_unique<BraunStepPad>();
            auto& pad = *steps[static_cast<size_t> (i)];
            pad.onClick = [this, i] {
                cancelChoosingLearn();
                const auto current = processor.snapshot();
                const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
                const int patIdx = sliderseq::resolveEditingPatternIndex (t);
                const bool bit = t.patterns[static_cast<size_t> (patIdx)].steps[static_cast<size_t> (i)];
                SliderRhythmMidiProcessor::StateOp op;
                op.kind = SliderRhythmMidiProcessor::OpKind::setStep;
                op.track = index;
                op.slotIndex = patIdx;
                op.arg0 = i;
                op.barg = ! bit;
                processor.applyUiOp (op);
                refreshFromProcessor();
            };
            addAndMakeVisible (pad);
        }

        applyTheme (initialSnapshot);
    }

    ~TrackStrip() override {
        if (slotDragging) {
            slotDragging = false;
            if (auto* p = processor.parameters().getParameter (SliderRhythmMidiProcessor::paramId (index, "slot")))
                p->endChangeGesture();
        }
        if (activeDialog != nullptr)
            activeDialog->exitModalState (0);
        velocityAttachment.reset();
        modAmountAttachment.reset();
        swingPctAttachment.reset();
        macroAttachment.reset();
        loopStepsAttachment.reset();
        slot.setLookAndFeel (nullptr);
    }

    void applyTheme (const SliderRhythmMidiProcessor::Snapshot& snap) {
        const auto& p = lookAndFeel->main.currentPalette();
        velocity.setColour (juce::Slider::rotarySliderFillColourId, p.orange);
        modAmount.setColour (juce::Slider::rotarySliderFillColourId, p.orange);
        swingPct.setColour (juce::Slider::rotarySliderFillColourId, p.cyan);
        macroDial.setColour (juce::Slider::rotarySliderFillColourId, p.orange);
        for (auto* dragBox : { &loopSteps, &gateBox, &velScaleBox, &note, &channel, &length }) {
            dragBox->setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
            dragBox->setColour (juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
            dragBox->setColour (juce::Slider::textBoxTextColourId, p.ink);
            dragBox->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            dragBox->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        }
        refresh (snap);
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
        const int patIdx = sliderseq::resolveEditingPatternIndex (track);
        const auto& pat = track.patterns[static_cast<size_t> (patIdx)];

        sliderseq::Track preview = track;
        preview.velocity = static_cast<int> (std::lround (velocity.getValue()));
        preview.modAmount = static_cast<int> (std::lround (modAmount.getValue()));
        preview.groove = static_cast<int> (std::lround (swingPct.getValue()));
        preview.macroValue = static_cast<int> (std::lround (macroDial.getValue()));
        preview.loopStepsOverride = static_cast<int> (std::lround (loopSteps.getValue()));

        const auto eff = sliderseq::computeEffectiveTrackForPattern (preview, pat);
        cachedVelText = juce::String (preview.velocity);
        cachedAmtText = juce::String (preview.modAmount);
        cachedSwingText = juce::String (preview.groove) + "%";
        cachedMacroStatus = formatMacroStatus (preview, pat, cachedMacroColourKind);
        loopSteps.setColour (juce::Slider::textBoxTextColourId, preview.loopStepsOverride > 0 ? p.cyan : p.mutedInk);

        velocityScope.setTrackData (preview, patIdx, processor.getActiveStep (index), p);
        swingScope.setTiming (state->sequence.globalSwing, eff.groove, preview.invertSwingSign, p);
        repaint();
    }

    void refreshFromProcessor() {
        const auto snap = processor.snapshot();
        refresh (*snap);
    }

    void refresh (const SliderRhythmMidiProcessor::Snapshot& state) {
        const auto& track = state.sequence.tracks[static_cast<size_t> (index)];
        const int patIdx = sliderseq::resolveEditingPatternIndex (track);
        const auto& pattern = track.patterns[static_cast<size_t> (patIdx)];
        const auto& p = lookAndFeel->main.currentPalette();
        const auto eff = sliderseq::computeEffectiveTrackForPattern (track, pattern);

        const int minS = sliderseq::trackMinSlot (track.bottomMode);
        const int maxS = sliderseq::trackMaxSlot (track.activeZones, track.topMode);
        const bool inBot = sliderseq::isTrackInBottomZone (track.slot, track.bottomMode);
        const bool inTop = sliderseq::isTrackInTopZone (track.slot, track.activeZones, track.topMode);
        const bool inGen = ! inBot && ! inTop;

        suppressCallbacks = true;
        cachedTrackNum = juce::String (index + 1).paddedLeft ('0', 2);
        cachedTrackName = juce::String (track.name);
        titleButton.setTitleParts (cachedTrackNum, cachedTrackName);
        zonesButton.setButtonText ("ZONES:" + juce::String (track.activeZones));
        zonesButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        zonesButton.setColour (juce::TextButton::textColourOffId, p.mutedInk);

        const bool isLayer = (track.midMode == sliderseq::MidRouteMode::genPlusThru);
        midRouteBtn.setButtonText (isLayer ? "MID:+TH" : "MID:GEN");
        midRouteBtn.setColour (juce::TextButton::buttonColourId, isLayer ? p.cyanTint : juce::Colours::transparentBlack);
        midRouteBtn.setColour (juce::TextButton::textColourOffId, isLayer ? p.cyan : p.mutedInk);

        topRouteBtn.setButtonText ("TOP:" + juce::String (sliderseq::endZoneShortLabel (track.topMode)));
        const bool topActive = (track.topMode != sliderseq::EndZoneMode::off);
        const bool topThru = (track.topMode == sliderseq::EndZoneMode::thru);
        topRouteBtn.setColour (juce::TextButton::buttonColourId,
                               inTop ? (topThru ? p.soloCyan : p.orange) : juce::Colours::transparentBlack);
        topRouteBtn.setColour (juce::TextButton::textColourOffId,
                               inTop ? juce::Colours::white : (topActive ? (topThru ? p.cyan : p.orange) : p.dimInk));

        const juce::String botLbl = track.bottomMode == sliderseq::EndZoneMode::block ? "0:BLOCK"
                                  : track.bottomMode == sliderseq::EndZoneMode::thru ? "0:THRU" : "0:OFF";
        botRouteBtn.setButtonText (botLbl);
        const bool botActive = (track.bottomMode != sliderseq::EndZoneMode::off);
        const bool botThru = (track.bottomMode == sliderseq::EndZoneMode::thru);
        botRouteBtn.setColour (juce::TextButton::buttonColourId,
                               inBot ? (botThru ? p.soloCyan : p.orange) : juce::Colours::transparentBlack);
        botRouteBtn.setColour (juce::TextButton::textColourOffId,
                               inBot ? juce::Colours::white : (botActive ? (botThru ? p.cyan : p.orange) : p.dimInk));

        slot.getProperties().set ("activeZones", track.activeZones);
        slot.getProperties().set ("topMode", static_cast<int> (track.topMode));
        slot.getProperties().set ("bottomMode", static_cast<int> (track.bottomMode));
        slot.setRange (minS, std::max (minS + 1, maxS), 1);
        slot.setEnabled (maxS > minS);
        slot.setDoubleClickReturnValue (true, static_cast<double> (std::clamp (sliderseq::defaultTrackSlot, 1, track.activeZones)));
        slot.setValue (std::clamp (track.slot, minS, maxS), juce::dontSendNotification);

        velocity.setValue (track.velocity, juce::dontSendNotification);
        modAmount.setValue (track.modAmount, juce::dontSendNotification);
        swingPct.setValue (track.groove, juce::dontSendNotification);

        const bool macroBipolar = sliderseq::isMacroBipolar (track.macroMode);
        macroDial.getProperties().set ("bipolarMid", macroBipolar);
        macroDial.setDoubleClickReturnValue (true, static_cast<double> (sliderseq::neutralMacroValueForTrack (track.macroMode, track)));
        macroDial.setValue (track.macroValue, juce::dontSendNotification);

        note.setValue (track.midiNote, juce::dontSendNotification);
        channel.setValue (track.midiChannel, juce::dontSendNotification);

        loopSteps.setValue (track.loopStepsOverride, juce::dontSendNotification);
        gateBox.setValue (nearestGateIndex (track.gateTicks), juce::dontSendNotification);
        const int velScalePct = std::clamp (static_cast<int> (std::lround (pattern.velocityScale * 100.0f)), 10, 200);
        velScaleBox.setValue (velScalePct, juce::dontSendNotification);

        const int activeLen = sliderseq::effectivePatternLength (pattern.length, pattern.grid, eff.loopStepsOverride);
        length.setRange (1, std::max (2, pattern.grid), 1);
        length.setEnabled (pattern.grid > 1);
        length.setValue (pattern.length, juce::dontSendNotification);
        grid.setSelectedId (pattern.grid, juce::dontSendNotification);
        suppressCallbacks = false;

        loopSteps.setColour (juce::Slider::textBoxTextColourId, track.loopStepsOverride > 0 ? p.cyan : p.mutedInk);
        gateBox.setColour (juce::Slider::textBoxTextColourId, p.ink);
        const auto chokeText = formatChokeLabel (track.chokeMask, state.sequence.trackCount);
        chokeBtn.setButtonText (chokeText);
        chokeBtn.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        chokeBtn.setColour (juce::TextButton::textColourOffId, chokeText == "OFF" ? p.mutedInk : p.orange);
        velScaleBox.setColour (juce::Slider::textBoxTextColourId, velScalePct != 100 ? p.orange : p.ink);
        clearPatternBtn.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        clearPatternBtn.setColour (juce::TextButton::textColourOffId, p.mutedInk);

        cachedInNonGenZone = ! inGen;
        cachedBadgeKind = 0;
        if (inTop) {
            cachedBadgeKind = topThru ? 2 : 1;
            cachedSlotHero = "T";
            cachedSlotSub = topThru ? "THRU" : "BLK";
            cachedSlotBadge = topThru ? "T THRU" : "T BLK";
        } else if (inBot) {
            cachedBadgeKind = botThru ? 2 : 1;
            cachedSlotHero = "0";
            cachedSlotSub = botThru ? "THRU" : "BLK";
            cachedSlotBadge = botThru ? "0 THRU" : "0 BLK";
        } else {
            cachedSlotHero = juce::String (track.slot).paddedLeft ('0', 2);
            cachedSlotSub = "SLOT";
            cachedSlotBadge = "SLOT " + cachedSlotHero;
        }

        cachedVelText = juce::String (track.velocity);
        cachedAmtText = juce::String (track.modAmount);
        cachedSwingText = juce::String (track.groove) + "%";
        cachedMacroStatus = formatMacroStatus (track, pattern, cachedMacroColourKind);

        modButton.setButtonText (juce::String (sliderseq::modShapeLabel (track.modShape)));
        macroModeBtn.setButtonText (juce::String (sliderseq::macroModeLabel (track.macroMode)));
        patternName.setButtonText (! inGen
            ? (cachedSlotBadge + " / S" + juce::String (patIdx + 1))
            : (juce::String (patIdx + 1) + " " + juce::String (pattern.name)));

        for (int i = 0; i < 5; ++i) {
            const bool activeRate = std::abs (track.rate - kRates[static_cast<size_t> (i)]) < 1e-6;
            rateButtons[static_cast<size_t> (i)].setColour (juce::TextButton::buttonColourId, activeRate ? p.orange : juce::Colours::transparentBlack);
            rateButtons[static_cast<size_t> (i)].setColour (juce::TextButton::textColourOffId, activeRate ? juce::Colours::white : p.mutedInk);
        }

        muteButton.setColour (juce::TextButton::buttonColourId, track.muted ? p.muteRed : juce::Colours::transparentBlack);
        muteButton.setColour (juce::TextButton::textColourOffId, track.muted ? juce::Colours::white : p.mutedInk);
        soloButton.setColour (juce::TextButton::buttonColourId, track.solo ? p.soloCyan : juce::Colours::transparentBlack);
        soloButton.setColour (juce::TextButton::textColourOffId, track.solo ? juce::Colours::white : p.mutedInk);

        const int nowStep = processor.getActiveStep (index);
        displayedPlayheadStep = nowStep;
        velocityScope.setTrackData (track, patIdx, nowStep, p);
        swingScope.setTiming (state.sequence.globalSwing, eff.groove, track.invertSwingSign, p);

        const int baseLen = sliderseq::effectivePatternLength (pattern.length, pattern.grid, track.loopStepsOverride);
        const int beatDiv = std::max (1, pattern.subdivision / 4);
        for (int i = 0; i < 64; ++i) {
            const bool inGrid = (i < pattern.grid);
            const bool enabled = pattern.steps[static_cast<size_t> (i)];
            bool inLoop = false;
            if (inGen && i < baseLen) {
                const int rel = (i - eff.rollStepOffset + baseLen * 4) % std::max (1, baseLen);
                inLoop = (rel < activeLen);
            }
            const bool downbeat = (i % beatDiv == 0);
            const float velNorm = inGrid
                ? static_cast<float> (sliderseq::stepModulatedVelocity (track, pattern, i)) / 127.0f
                : 0.0f;
            steps[static_cast<size_t> (i)]->setState (inGrid, enabled, inLoop, downbeat, velNorm, i == nowStep, p);
        }

        repaint();
    }

    struct LayoutMetrics {
        int headerY1 = 52;
        int faderY1 = 285;
        int rateY1 = 313;
        int velScopeY = 375;
        int velScopeH = 57;
        int velY1 = 438;
        int swingY1 = 498;
        int macroY1 = 548;
        int patBarY1 = 576;
        int rowAY1 = 610;
        int rowBY1 = 644;
        int padStartY = 650;
        int padH = 13;
        int padGap = 3;
    };

    static LayoutMetrics computeLayout (int h) noexcept {
        LayoutMetrics m;
        const int extraH = std::max (0, h - kMinStripHeight);
        constexpr int headerH = 52;
        constexpr int rateH = 28;
        const int velScopeH = std::clamp (46 + extraH * 18 / 100, 46, 108);
        const int velH = 62 + velScopeH + 6;
        const int swingH = std::clamp (54 + extraH * 10 / 100, 54, 88);
        constexpr int macroH = 50;
        constexpr int patBarH = 28;
        constexpr int rowAH = 34;
        constexpr int rowBH = 34;
        const int padH = std::clamp (12 + extraH * 3 / 100, 12, 18);
        constexpr int padGap = 3;
        const int matrixH = 4 * padH + 3 * padGap;
        const int footerH = patBarH + rowAH + rowBH + 6 + matrixH + 8;
        const int faderH = std::max (191, h - (headerH + rateH + velH + swingH + macroH + footerH));

        m.headerY1 = headerH;
        m.faderY1 = m.headerY1 + faderH;
        m.rateY1 = m.faderY1 + rateH;
        m.velScopeY = m.rateY1 + 62;
        m.velScopeH = velScopeH;
        m.velY1 = m.rateY1 + velH;
        m.swingY1 = m.velY1 + swingH;
        m.macroY1 = m.swingY1 + macroH;
        m.patBarY1 = m.macroY1 + patBarH;
        m.rowAY1 = m.patBarY1 + rowAH;
        m.rowBY1 = m.rowAY1 + rowBH;
        m.padStartY = m.rowBY1 + 6;
        m.padH = padH;
        m.padGap = padGap;
        return m;
    }

    void paint (juce::Graphics& g) override {
        const auto& p = lookAndFeel->main.currentPalette();
        const int w = getWidth();
        const int h = getHeight();
        const auto m = computeLayout (h);

        g.fillAll (cachedInNonGenZone ? p.panelWell : p.chassis);
        g.setColour (p.hairline);
        g.drawVerticalLine (w - 1, 0.0f, static_cast<float> (h));

        for (int hy : { m.headerY1, m.faderY1, m.rateY1, m.velY1, m.swingY1, m.macroY1, m.patBarY1, m.rowAY1, m.rowBY1 })
            g.drawHorizontalLine (hy, 0.0f, static_cast<float> (w - 1));

        // 1. Header Row 2: Flush NOTE | CH bar directly beneath track name & preset selector.
        constexpr int headerRow2Y = 28;
        const int noteChSplitX = 136;
        g.setColour (p.hairline);
        g.drawHorizontalLine (headerRow2Y, 0.0f, static_cast<float> (w - 1));
        g.drawVerticalLine (noteChSplitX, static_cast<float> (headerRow2Y), static_cast<float> (m.headerY1));
        g.setFont (braun::monoFont (9.4f));
        g.setColour (p.mutedInk);
        g.drawText ("NOTE", 8, headerRow2Y, 34, m.headerY1 - headerRow2Y, juce::Justification::centredLeft);
        g.drawText ("CH", noteChSplitX + 8, headerRow2Y, 20, m.headerY1 - headerRow2Y, juce::Justification::centredLeft);

        // 2. Centered Fader Bay right-side Hero Dot-Matrix Slot Display (unified 2.0 px physical LED pitch).
        const int fy0 = m.headerY1;
        const int fy1 = m.faderY1;
        const int slotY = fy0 + 28;
        const int slotH = std::max (120, fy1 - slotY - 6);
        const int dispW = 80;
        const int dispH = 74;
        const int dispX = w - dispW - 6;
        const int dispY = slotY + std::max (0, (slotH - dispH) / 2);
        const auto heroCol = cachedBadgeKind == 2 ? p.cyan : p.orange;
        const auto subCol = cachedBadgeKind == 2 ? p.cyan
                          : cachedBadgeKind == 1 ? p.orange
                          : juce::Colour (0xffb8b4aa);
        dotmatrix::paintSlotDisplayPanel (g, { dispX, dispY, dispW, dispH },
                                          cachedSlotHero, cachedSlotSub, heroCol, subCol, p.hairline);

        // 4. Velocity & Mod labels/values
        const int vy0 = m.rateY1;
        const int usable = w - 16;
        const int knobColW = 50;
        const int modX = 8 + knobColW + 2;
        const int modW = std::max (56, usable - 2 * knobColW - 4);
        const int amtX = w - 8 - knobColW;

        g.setFont (braun::monoFont (9.6f));
        g.setColour (p.mutedInk);
        g.drawText ("VEL", 8, vy0 + 3, knobColW, 12, juce::Justification::centred);
        g.drawText ("MOD", modX, vy0 + 3, modW, 12, juce::Justification::centred);
        g.drawText ("AMT", amtX, vy0 + 3, knobColW, 12, juce::Justification::centred);

        g.setFont (braun::monoFont (10.5f));
        g.setColour (p.ink);
        g.drawText (cachedVelText, 8, vy0 + 46, knobColW, 13, juce::Justification::centred);
        g.drawText (cachedAmtText, amtX, vy0 + 46, knobColW, 13, juce::Justification::centred);

        // 5. Swing % label & value
        const int sy0 = m.velY1;
        const int syH = m.swingY1 - sy0;
        const int swLabelY = sy0 + std::max (2, (syH - 50) / 2);
        g.setFont (braun::monoFont (9.6f));
        g.setColour (p.mutedInk);
        g.drawText ("SWING %", 6, swLabelY, 60, 12, juce::Justification::centred);
        g.setFont (braun::monoFont (10.5f));
        g.setColour (p.ink);
        g.drawText (cachedSwingText, 6, swLabelY + 38, 60, 12, juce::Justification::centred);

        // 6. Macro label & readout
        const int my0 = m.swingY1;
        g.setFont (braun::monoFont (9.6f));
        g.setColour (p.mutedInk);
        g.drawText ("MACRO", 6, my0 + 3, 52, 12, juce::Justification::centred);

        g.setFont (braun::monoFont (10.4f));
        const auto mCol = cachedMacroColourKind == 1 ? p.orange
                        : cachedMacroColourKind == 2 ? p.cyan : p.mutedInk;
        g.setColour (mCol);
        g.drawFittedText (cachedMacroStatus, 62, my0 + 28, w - 70, 18, juce::Justification::centredLeft, 1, 0.85f);

        // 7 & 8. Spacious 3-column flush hairline parameter rows: LEN | GRID | LOOP and GATE | CHOK | VSCL
        const int colW = w / 3;
        auto drawTriGrid = [&] (int y0, int y1, const std::array<const char*, 3>& labels) {
            g.setFont (braun::monoFont (9.4f));
            for (int c = 0; c < 3; ++c) {
                const int cx0 = c * colW;
                const int cx1 = (c == 2) ? (w - 1) : ((c + 1) * colW);
                if (c > 0) {
                    g.setColour (p.hairline);
                    g.drawVerticalLine (cx0, static_cast<float> (y0), static_cast<float> (y1));
                }
                g.setColour (p.mutedInk);
                g.drawText (labels[static_cast<size_t> (c)], cx0, y0 + 2, cx1 - cx0, 12, juce::Justification::centred);
            }
        };
        drawTriGrid (m.patBarY1, m.rowAY1, { "LEN", "GRID", "LOOP" });
        drawTriGrid (m.rowAY1, m.rowBY1, { "GATE", "CHOK", "VSCL" });
    }

    void resized() override {
        const int w = getWidth();
        const int h = getHeight();
        const int usable = w - 16;
        const auto m = computeLayout (h);

        // 1. Header (Row 1: Title/Bank Preset + M/S; Row 2: inline NOTE | CH)
        titleButton.setBounds (4, 3, std::max (60, w - 84), 23);
        muteButton.setBounds (w - 78, 4, 36, 21);
        soloButton.setBounds (w - 40, 4, 36, 21);
        note.setBounds (42, 30, 92, 20);
        channel.setBounds (164, 30, w - 168, 20);

        // 2. Centered Hero Fader Bay (rail dead-center at w / 2 = 112 px; giant LED slot screen on right)
        const int fy0 = m.headerY1;
        const int fy1 = m.faderY1;
        zonesButton.setBounds (4, fy0 + 5, 84, 20);
        midRouteBtn.setBounds (w - 88, fy0 + 5, 84, 20);

        const int slotW = 64;
        const int slotX = (w / 2) - 40; // Rail sits at slotX + 40 == w / 2 (exact horizontal center!)
        const int slotY = fy0 + 28;
        const int slotH = std::max (120, fy1 - slotY - 6);
        slot.setBounds (slotX, slotY, slotW, slotH);

        const int endBtnW = std::max (58, slotX - 3);
        const int pad = std::min (14, std::max (8, slotH / 8));
        topRouteBtn.setBounds (2, slotY + pad - 10, endBtnW, 20);
        botRouteBtn.setBounds (2, slotY + slotH - pad - 10, endBtnW, 20);

        // 3. Rate Bar
        const int ry0 = m.faderY1;
        const int rateGap = 2;
        const int rateW = (usable - 4 * rateGap) / 5;
        for (int i = 0; i < 5; ++i)
            rateButtons[static_cast<size_t> (i)].setBounds (8 + i * (rateW + rateGap), ry0 + 3, rateW, 22);

        // 4. Velocity & Mod
        const int vy0 = m.rateY1;
        const int knobColW = 50;
        const int modX = 8 + knobColW + 2;
        const int modW = std::max (56, usable - 2 * knobColW - 4);
        const int amtX = w - 8 - knobColW;
        velocity.setBounds (8 + (knobColW - 28) / 2, vy0 + 17, 28, 28);
        modButton.setBounds (modX, vy0 + 19, modW, 24);
        modAmount.setBounds (amtX + (knobColW - 28) / 2, vy0 + 17, 28, 28);
        velocityScope.setBounds (8, m.velScopeY, usable, m.velScopeH);

        // 5. Track Swing
        const int sy0 = m.velY1;
        const int syH = m.swingY1 - sy0;
        const int swLabelY = sy0 + std::max (2, (syH - 50) / 2);
        swingPct.setBounds (24, swLabelY + 13, 24, 24);
        swingScope.setBounds (68, sy0 + 6, std::max (60, w - 76), std::max (34, syH - 12));

        // 6. Macro
        const int my0 = m.swingY1;
        macroDial.setBounds (19, my0 + 18, 26, 26);
        macroModeBtn.setBounds (60, my0 + 5, std::max (60, w - 68), 21);

        // 7. Pattern Slot Bar + 1-click CLR
        const int clrW = 48;
        patternName.setBounds (6, m.macroY1 + 3, std::max (60, w - 14 - clrW), 22);
        clearPatternBtn.setBounds (w - 6 - clrW, m.macroY1 + 3, clrW, 22);

        // 8. Spacious 3-Column Parameter Rows: LEN | GRID | LOOP & GATE | CHOK | VSCL
        const int colW = w / 3;
        const int col2W = (w - 1) - (colW * 2 + 1);
        const int rowAValY = m.patBarY1 + 14;
        const int rowBValY = m.rowAY1 + 14;
        const int valH = 19;
        length.setBounds (1, rowAValY, colW - 2, valH);
        grid.setBounds (colW + 1, rowAValY, colW - 2, valH);
        loopSteps.setBounds (colW * 2 + 1, rowAValY, col2W, valH);

        gateBox.setBounds (1, rowBValY, colW - 2, valH);
        chokeBtn.setBounds (colW + 1, rowBValY, colW - 2, valH);
        velScaleBox.setBounds (colW * 2 + 1, rowBValY, col2W, valH);

        // 9. 4x16 Step Matrix
        const int stepGap = 2;
        const int stepW = std::max (6, (usable - 15 * stepGap) / 16);
        const int rowW = 16 * stepW + 15 * stepGap;
        const int padStartX = 8 + (usable - rowW) / 2;
        for (int i = 0; i < 64; ++i)
            steps[static_cast<size_t> (i)]->setBounds (
                padStartX + (i % 16) * (stepW + stepGap),
                m.padStartY + (i / 16) * (m.padH + m.padGap),
                stepW, m.padH);
    }

    void mouseDown (const juce::MouseEvent&) override {
        cancelChoosingLearn();
    }

private:
    void cancelChoosingLearn() {
        if (onCancelLearn) onCancelLearn();
    }

    juce::Component* dialogParent() {
        return onRequestDialogParent ? onRequestDialogParent() : static_cast<juce::Component*> (this);
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
        menu.addSectionHeader ("Fader Active Pattern Zones");
        for (int z = 1; z <= sliderseq::slotsPerTrack; ++z)
            menu.addItem (z, juce::String (z) + (z == 1 ? " Zone" : " Zones"), true, curZones == z);
        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<TrackStrip> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&zonesButton),
                            [safe, sharedLnf] (int result) {
            if (safe == nullptr || result <= 0) return;
            safe->processor.setTrackActiveZones (safe->index, result);
            safe->refreshFromProcessor();
        });
    }

    void showMacroModeMenu() {
        const auto cur = processor.snapshot()->sequence.tracks[static_cast<size_t> (index)].macroMode;
        juce::PopupMenu menu;
        menu.addSectionHeader ("Performance Macros");
        menu.addItem (1 + static_cast<int> (sliderseq::MacroMode::build), "BUILD (Energy + Shorten + 2x Roll)", true, cur == sliderseq::MacroMode::build);
        menu.addItem (1 + static_cast<int> (sliderseq::MacroMode::density), "DENSITY (Thin <-> Orig <-> Ghost + Ratchet)", true, cur == sliderseq::MacroMode::density);
        menu.addItem (1 + static_cast<int> (sliderseq::MacroMode::roll), "ROLL / POLY (8 / 6 / 4 / 3 / 2 / 1 / 2x)", true, cur == sliderseq::MacroMode::roll);
        menu.addItem (1 + static_cast<int> (sliderseq::MacroMode::human), "HUMAN (Tight Machine <-> Loose Groove)", true, cur == sliderseq::MacroMode::human);
        menu.addSeparator();
        menu.addSectionHeader ("Direct Parameter Map");
        menu.addItem (1 + static_cast<int> (sliderseq::MacroMode::velocity), "-> Velocity (1-127)", true, cur == sliderseq::MacroMode::velocity);
        menu.addItem (1 + static_cast<int> (sliderseq::MacroMode::swing), "-> Track Swing % (0-100%)", true, cur == sliderseq::MacroMode::swing);
        menu.addItem (1 + static_cast<int> (sliderseq::MacroMode::loopSteps), "-> Loop Steps (OFF / 64..1)", true, cur == sliderseq::MacroMode::loopSteps);
        menu.addItem (1 + static_cast<int> (sliderseq::MacroMode::rate), "-> Rate (1/3 .. 3x)", true, cur == sliderseq::MacroMode::rate);
        menu.addItem (1 + static_cast<int> (sliderseq::MacroMode::modAmount), "-> Mod Amount (0-100%)", true, cur == sliderseq::MacroMode::modAmount);

        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<TrackStrip> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&macroModeBtn),
                            [safe, sharedLnf] (int result) {
            if (safe == nullptr || result <= 0) return;
            const auto mode = sliderseq::clampMacroMode (result - 1);
            safe->processor.setTrackMacroMode (safe->index, mode, true);
            safe->refreshFromProcessor();
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
            safe->refreshFromProcessor();
        });
    }

    void renameTrackDialog (const juce::String& currentName) {
        auto sharedLnf = lookAndFeel;
        auto dialog = std::make_unique<juce::AlertWindow> (
            "Rename Track", "Enter name for Track " + juce::String (index + 1) + ".",
            juce::MessageBoxIconType::NoIcon, dialogParent());
        dialog->setLookAndFeel (&sharedLnf->main);
        dialog->addTextEditor ("name", currentName, "Track name");
        dialog->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        auto* window = dialog.release();
        activeDialog = window;
        juce::Component::SafePointer<TrackStrip> safe (this);
        juce::Component::SafePointer<juce::AlertWindow> safeWin (window);
        window->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe, sharedLnf, safeWin] (int result) {
                if (safeWin != nullptr) safeWin->setLookAndFeel (nullptr);
                if (safe == nullptr || safeWin == nullptr || result != 1) return;
                const auto newName = safeWin->getTextEditorContents ("name").trim().substring (0, 24);
                if (newName.isEmpty()) return;
                safe->setTrack ([newName] (auto& t) { t.name = newName.toStdString(); });
                safe->refreshFromProcessor();
            }), true);
    }

    void showTrackPresetAndOptionsMenu() {
        const auto state = processor.snapshot();
        const auto& track = state->sequence.tracks[static_cast<size_t> (index)];
        const int targetSlot = sliderseq::resolveEditingPatternIndex (track);
        const auto& pattern = track.patterns[static_cast<size_t> (targetSlot)];
        const auto clipboard = onGetClipboard ? onGetClipboard() : std::nullopt;
        const auto& factories = lookAndFeel->factoryTrackBanks;
        const auto users = lookAndFeel->reloadUserTrackBanks();

        static constexpr int kFactoryTrackMenuBase = 100;
        static constexpr int kDuplicateTrackMenuBase = 500;
        static constexpr int kUserTrackMenuBase = 10000;

        int matchedUserIdx = -1;
        juce::PopupMenu menu;
        menu.addSectionHeader ("Track Bank Presets (12 Slots)");
        for (size_t i = 0; i < factories.size(); ++i)
            menu.addItem (kFactoryTrackMenuBase + static_cast<int> (i), juce::String (factories[i].name), true,
                          trackpresets::bankMatchesTrack (factories[i], track));

        if (! users.empty()) {
            menu.addSeparator();
            menu.addSectionHeader ("User Track Presets");
            for (size_t i = 0; i < users.size(); ++i) {
                const bool matched = trackpresets::bankMatchesTrack (users[i], track);
                if (matched && matchedUserIdx < 0)
                    matchedUserIdx = static_cast<int> (i);
                menu.addItem (kUserTrackMenuBase + static_cast<int> (i), juce::String (users[i].name), true, matched);
            }
        }

        menu.addSeparator();
        menu.addItem (1, "Save Track Preset As...");
        const bool canDelete = matchedUserIdx >= 0;
        const juce::String deleteLabel = canDelete
            ? ("Delete \"" + juce::String (users[static_cast<size_t> (matchedUserIdx)].name) + "\" (Move to Trash)")
            : "Delete Track Preset (Move to Trash)";
        menu.addItem (2, deleteLabel, canDelete);

        menu.addSeparator();
        menu.addSectionHeader ("Track Utilities");
        menu.addItem (10, "Rename Track...");
        menu.addItem (12, "Copy Active Pattern");
        menu.addItem (13, "Paste Into Active Pattern", clipboard.has_value());

        juce::PopupMenu dupMenu;
        for (int dst = 0; dst < sliderseq::maxTracks; ++dst) {
            if (dst == index) continue;
            const auto& dt = state->sequence.tracks[static_cast<size_t> (dst)];
            dupMenu.addItem (kDuplicateTrackMenuBase + dst, "To Track " + juce::String (dst + 1) + " (" + juce::String (dt.name) + ")");
        }
        menu.addSubMenu ("Duplicate Entire Track", dupMenu);

        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<TrackStrip> safe (this);
        const juce::String currentTrackName (track.name);
        const auto currentPattern = pattern;
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&titleButton),
                            [safe, sharedLnf, users, matchedUserIdx, targetSlot, currentTrackName, currentPattern, clipboard] (int result) {
            if (safe == nullptr || result == 0) return;
            if (result == 1) {
                safe->saveTrackPresetDialog();
                return;
            }
            if (result == 2 && matchedUserIdx >= 0 && matchedUserIdx < static_cast<int> (users.size())) {
                if (trackpresets::deleteUserBank (users[static_cast<size_t> (matchedUserIdx)])) {
                    sharedLnf->reloadUserTrackBanks();
                    if (safe->onTracksModified) safe->onTracksModified();
                    else safe->refreshFromProcessor();
                }
                return;
            }
            if (result == 10) {
                safe->renameTrackDialog (currentTrackName);
                return;
            }
            if (result == 12) {
                if (safe->onCopyPattern) safe->onCopyPattern (currentPattern);
                return;
            }
            if (result == 13 && clipboard.has_value()) {
                safe->loadBeat (targetSlot, *clipboard);
                return;
            }
            if (result >= kFactoryTrackMenuBase && result < kFactoryTrackMenuBase + static_cast<int> (sharedLnf->factoryTrackBanks.size())) {
                const auto bank = sharedLnf->factoryTrackBanks[static_cast<size_t> (result - kFactoryTrackMenuBase)];
                safe->setTrack ([&bank] (auto& t) {
                    trackpresets::applyToTrack (bank, t);
                    t.name = bank.name;
                });
                safe->refreshFromProcessor();
                return;
            }
            if (result >= kDuplicateTrackMenuBase && result < kDuplicateTrackMenuBase + sliderseq::maxTracks) {
                const int dst = result - kDuplicateTrackMenuBase;
                const int src = safe->index;
                safe->processor.edit ([src, dst] (auto& s) {
                    s.sequence.tracks[static_cast<size_t> (dst)] = s.sequence.tracks[static_cast<size_t> (src)];
                    s.sequence.tracks[static_cast<size_t> (dst)].chokeMask &= static_cast<uint16_t> (~(1u << static_cast<unsigned> (dst)));
                    if (dst + 1 > s.sequence.trackCount) s.sequence.trackCount = dst + 1;
                });
                if (safe->onTracksModified) safe->onTracksModified();
                return;
            }
            if (result >= kUserTrackMenuBase && result < kUserTrackMenuBase + static_cast<int> (users.size())) {
                const auto bank = users[static_cast<size_t> (result - kUserTrackMenuBase)];
                safe->setTrack ([&bank] (auto& t) {
                    trackpresets::applyToTrack (bank, t);
                    t.name = bank.name;
                });
                safe->refreshFromProcessor();
            }
        });
    }

    void saveTrackPresetDialog() {
        const auto snap = processor.snapshot();
        const auto& track = snap->sequence.tracks[static_cast<size_t> (index)];
        auto sharedLnf = lookAndFeel;
        auto dialog = std::make_unique<juce::AlertWindow> (
            "Save Track Preset",
            "Save all 12 pattern slots from Track " + juce::String (index + 1) + " (" + juce::String (track.name) + ").",
            juce::MessageBoxIconType::NoIcon, dialogParent());
        dialog->setLookAndFeel (&sharedLnf->main);
        dialog->addTextEditor ("name", juce::String (track.name) + " Bank", "Track preset name");
        dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        auto* window = dialog.release();
        activeDialog = window;
        juce::Component::SafePointer<TrackStrip> safe (this);
        juce::Component::SafePointer<juce::AlertWindow> safeWin (window);
        window->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe, sharedLnf, safeWin] (int result) {
                if (safeWin != nullptr) safeWin->setLookAndFeel (nullptr);
                if (safe == nullptr || safeWin == nullptr || result != 1) return;
                const auto name = safeWin->getTextEditorContents ("name").trim().substring (0, 32);
                if (name.isEmpty()) return;
                const auto cur = safe->processor.snapshot();
                auto bank = trackpresets::captureFromTrack (
                    cur->sequence.tracks[static_cast<size_t> (safe->index)], {}, name.toStdString(), false);
                if (! trackpresets::saveUserBank (bank)) {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        "Could not save track preset", "Check write access to the Track Presets folder.");
                    return;
                }
                sharedLnf->reloadUserTrackBanks();
                if (safe->onTracksModified) safe->onTracksModified();
                else safe->refreshFromProcessor();
            }), true);
    }

    void showLoopStepsMenu() {
        const int cur = processor.snapshot()->sequence.tracks[static_cast<size_t> (index)].loopStepsOverride;
        const std::array<int, 7> choices { 0, 2, 4, 8, 12, 16, 32 };
        juce::PopupMenu menu;
        menu.addSectionHeader ("Loop Steps Override");
        for (size_t i = 0; i < choices.size(); ++i) {
            const int v = choices[i];
            menu.addItem (1 + static_cast<int> (i), v == 0 ? "Follow Pattern (OFF)" : (juce::String (v) + " Steps"),
                          true, cur == v);
        }
        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<TrackStrip> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&loopSteps),
                            [safe, sharedLnf, choices] (int result) {
            if (safe == nullptr || result <= 0) return;
            safe->processor.setHostParamValue (safe->index, "loop_steps",
                                               static_cast<float> (choices[static_cast<size_t> (result - 1)]));
            safe->refreshFromProcessor();
        });
    }

    void showGateMenu() {
        const double cur = processor.snapshot()->sequence.tracks[static_cast<size_t> (index)].gateTicks;
        juce::PopupMenu menu;
        menu.addSectionHeader ("Note Gate Length");
        for (size_t g = 0; g < kGateTicks.size(); ++g)
            menu.addItem (1 + static_cast<int> (g), kGateMenuLabels[g], true,
                          std::abs (cur - kGateTicks[g]) < 0.5);
        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<TrackStrip> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&gateBox),
                            [safe, sharedLnf] (int result) {
            if (safe == nullptr || result <= 0) return;
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setGateTicks;
            op.track = safe->index;
            op.darg = kGateTicks[static_cast<size_t> (result - 1)];
            safe->processor.applyUiOp (op);
            safe->refreshFromProcessor();
        });
    }

    void showChokeMenu() {
        const auto state = processor.snapshot();
        const auto& track = state->sequence.tracks[static_cast<size_t> (index)];
        const uint16_t currentMask = track.chokeMask;
        juce::PopupMenu menu;
        menu.addSectionHeader ("Choke On Same Step");
        for (int other = 0; other < state->sequence.trackCount; ++other) {
            if (other == index) continue;
            const auto& ot = state->sequence.tracks[static_cast<size_t> (other)];
            const bool checked = (currentMask & static_cast<uint16_t> (1u << static_cast<unsigned> (other))) != 0u;
            const auto labelText = juce::String (other + 1) + ": " + juce::String (ot.name)
                + " (" + braun::midiNoteName (ot.midiNote) + " / CH" + juce::String (ot.midiChannel) + ")";
            menu.addItem (100 + other, labelText, true, checked);
        }
        if (menu.getNumItems() > 1) {
            menu.addSeparator();
            menu.addItem (399, "Clear All Choke Targets", currentMask != 0u);
        }
        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<TrackStrip> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&chokeBtn),
                            [safe, sharedLnf, currentMask] (int result) {
            if (safe == nullptr || result == 0) return;
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setChokeMask;
            op.track = safe->index;
            if (result == 399)
                op.arg0 = 0;
            else if (result >= 100 && result < 100 + sliderseq::maxTracks)
                op.arg0 = static_cast<int> (currentMask ^ static_cast<uint16_t> (1u << static_cast<unsigned> (result - 100)));
            else
                return;
            safe->processor.applyUiOp (op);
            safe->refreshFromProcessor();
        });
    }

    void showVelScaleMenu() {
        const auto state = processor.snapshot();
        const auto& track = state->sequence.tracks[static_cast<size_t> (index)];
        const int targetSlot = sliderseq::resolveEditingPatternIndex (track);
        const float curScale = track.patterns[static_cast<size_t> (targetSlot)].velocityScale;
        const std::array<std::pair<const char*, float>, 4> choices {{
            { "50% (Ghost)", 0.50f }, { "72% (Fill)", 0.72f }, { "100% (Normal)", 1.00f }, { "125% (Accent)", 1.25f }
        }};
        juce::PopupMenu menu;
        menu.addSectionHeader ("Slot " + juce::String (targetSlot + 1) + " Velocity Scale");
        for (size_t i = 0; i < choices.size(); ++i)
            menu.addItem (1 + static_cast<int> (i), choices[i].first, true,
                          std::abs (curScale - choices[i].second) < 0.02f);
        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<TrackStrip> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&velScaleBox),
                            [safe, sharedLnf, targetSlot, choices] (int result) {
            if (safe == nullptr || result <= 0) return;
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setPatternVelocityScale;
            op.track = safe->index;
            op.slotIndex = targetSlot;
            op.darg = choices[static_cast<size_t> (result - 1)].second;
            safe->processor.applyUiOp (op);
            safe->refreshFromProcessor();
        });
    }

    void showChannelMenu() {
        const int curCh = processor.snapshot()->sequence.tracks[static_cast<size_t> (index)].midiChannel;
        juce::PopupMenu menu;
        menu.addItem (1, "Apply CH " + juce::String (curCh) + " to All Tracks");
        juce::PopupMenu allChMenu;
        for (int ch = 1; ch <= 16; ++ch)
            allChMenu.addItem (100 + ch, "CH " + juce::String (ch), true, ch == curCh);
        menu.addSubMenu ("Set All Tracks to Channel", allChMenu);

        auto sharedLnf = lookAndFeel;
        menu.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<TrackStrip> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&channel),
                            [safe, sharedLnf, curCh] (int result) {
            if (safe == nullptr || result == 0) return;
            const int targetCh = (result == 1) ? curCh
                               : (result >= 101 && result <= 116) ? (result - 100) : 0;
            if (targetCh < 1 || targetCh > 16) return;
            safe->processor.edit ([targetCh] (auto& s) {
                for (auto& tr : s.sequence.tracks)
                    tr.midiChannel = targetCh;
            });
            if (safe->onTracksModified) safe->onTracksModified();
            else safe->refreshFromProcessor();
        });
    }

    void showPatternMenu() {
        const auto state = processor.snapshot();
        const auto& track = state->sequence.tracks[static_cast<size_t> (index)];
        const int targetSlot = sliderseq::resolveEditingPatternIndex (track);
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
        for (const auto& bank : lookAndFeel->factoryTrackBanks) {
            juce::PopupMenu beats;
            for (const auto& beat : bank.patterns)
                addChoice (beats, beat);
            factory.addSubMenu (juce::String (bank.name), beats);
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
        refreshFromProcessor();
    }

    void saveBeatAs (int targetSlot, sliderseq::Pattern beat, const juce::File& destination) {
        auto sharedLnf = lookAndFeel;
        auto dialog = std::make_unique<juce::AlertWindow> (
            "Save Beat Preset", "Name the beat from slot " + juce::String (targetSlot + 1) + ".",
            juce::MessageBoxIconType::NoIcon, dialogParent());
        dialog->setLookAndFeel (&sharedLnf->main);
        dialog->addTextEditor ("name", juce::String (beat.name), "Preset name");
        dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        auto* window = dialog.release();
        activeDialog = window;
        juce::Component::SafePointer<TrackStrip> safe (this);
        juce::Component::SafePointer<juce::AlertWindow> safeWin (window);
        window->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe, sharedLnf, safeWin, targetSlot, destination, beat = std::move (beat)] (int result) mutable {
                if (safeWin != nullptr)
                    safeWin->setLookAndFeel (nullptr);
                if (safe == nullptr || safeWin == nullptr || result != 1)
                    return;
                const auto name = safeWin->getTextEditorContents ("name").trim().substring (0, 48);
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
    int displayedPlayheadStep = -1;
    bool suppressCallbacks = false;
    bool slotDragging = false;
    bool cachedInNonGenZone = false;
    int cachedBadgeKind = 0;
    int cachedMacroColourKind = 0;
    juce::String cachedTrackNum, cachedTrackName, cachedSlotHero, cachedSlotSub, cachedSlotBadge, cachedVelText, cachedAmtText, cachedSwingText, cachedMacroStatus;

    juce::SharedResourcePointer<SliderRhythmMidiEditor::LookAndFeelResources> lookAndFeel;
    juce::Component::SafePointer<juce::AlertWindow> activeDialog;

    TrackTitleButton titleButton;
    juce::TextButton zonesButton, midRouteBtn, topRouteBtn, botRouteBtn, macroModeBtn, chokeBtn, patternName, clearPatternBtn;
    BraunButton muteButton { "M" }, soloButton { "S" }, modButton { "RAMP UP" };
    std::array<BraunButton, 5> rateButtons;
    BraunSlider slot, velocity, modAmount, swingPct, macroDial, loopSteps, gateBox, velScaleBox, note, channel, length;
    BraunComboBox grid;
    VelocityModScope velocityScope;
    TrackSwingScope swingScope;
    std::array<std::unique_ptr<BraunStepPad>, 64> steps;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> velocityAttachment, modAmountAttachment, swingPctAttachment, macroAttachment, loopStepsAttachment;
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

        for (juce::TextButton* b : { static_cast<juce::TextButton*> (&presetPrev), &presetNameBtn, &presetNext, &presetSaveBtn,
                                     &scaleBtn, static_cast<juce::TextButton*> (&themeBtn), &settingsBtn,
                                     &addTrack, &removeTrack, &unmuteAll, &unsoloAll,
                                     &learn, &noteLearnToggle, &triggerModeToggle, &playbackMode, &unmappedModeBtn, &inputMatchBtn }) {
            addAndMakeVisible (*b);
        }

        presetPrev.setButtonText ("<");
        presetNext.setButtonText (">");
        presetSaveBtn.setButtonText ("SAVE");
        presetNameBtn.getProperties().set ("plainText", true);
        presetNameBtn.setTooltip ("Load, save, revert, or delete Mapping Presets (* means modified)");
        presetSaveBtn.setTooltip ("Save current mapping to active user preset (or Save As for factory presets)");
        scaleBtn.setTooltip ("Change UI zoom scale (75% / 85% / 100% / 125%)");
        themeBtn.setTooltip ("Switch between Braun Atelier P4 Dark and Braun SK 4 Light");
        settingsBtn.setButtonText ("CFG");
        settingsBtn.setTooltip ("Scroll wheel preferences, input channel match, theme, and UI scale");

        addTrack.setButtonText ("+TRK");
        removeTrack.setButtonText ("-TRK");
        unmuteAll.setButtonText ("UNMUTE");
        unsoloAll.setButtonText ("UNSOLO");
        learn.setButtonText ("LEARN");
        noteLearnToggle.setButtonText ("NOTE");
        triggerModeToggle.setButtonText ("TGL");
        playbackMode.setButtonText ("ACTIVE");
        unmappedModeBtn.setButtonText ("OTH:THRU");
        inputMatchBtn.setButtonText ("IN:AUTO");

        unmuteAll.setTooltip ("Unmute all tracks");
        unsoloAll.setTooltip ("Clear solo on all tracks");
        noteLearnToggle.setTooltip ("Allow MIDI Note On messages (pads/keys) for Mute/Solo/Slot/Rate/Mod MIDI Learn");
        triggerModeToggle.setTooltip ("Learn trigger mode for M/S: TGL (press flips) vs HLD (momentary)");
        playbackMode.setTooltip ("ACTIVE: per-track 3-zone routing | ALL THRU: global bypass (all MIDI passes through)");
        unmappedModeBtn.setTooltip ("Notes not owned by any track: OTH:THRU (pass through) vs OTH:BLK (block notes; sustain release & panic always pass)");
        inputMatchBtn.setTooltip ("Incoming MIDI channel matching: IN:AUTO (exact ch + Ableton Live CH1 fallback) / IN:OMNI (any ch by note) / IN:STR (exact ch + note only)");

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

    struct HeaderLayout {
        int wordW = 296;
        int div1X = 312;
        int presetLabelX = 320;
        int presetLabelW = 48;
        int presetPrevX = 370;
        int presetNameW = 160;
        int presetSaveW = 58;
        int div2X = 658;
        int statusX = 668;
        int swingDisplayW = 68;
        int scopeX = 174;
        int scopeW = 178;
        int swingDivX = 358;
    };

    static HeaderLayout computeHeaderLayout (int w) noexcept {
        HeaderLayout hl;
        if (w >= 1100)
            return hl;
        hl.wordW = std::clamp (w - 788, 162, 296);
        hl.div1X = 10 + hl.wordW + 6;
        hl.presetLabelX = hl.div1X + 8;
        hl.presetLabelW = w >= 1000 ? 48 : 42;
        hl.presetPrevX = hl.presetLabelX + hl.presetLabelW + 2;
        hl.presetNameW = std::clamp (w - 880, 100, 160);
        hl.presetSaveW = w >= 980 ? 58 : 52;
        const int saveRight = hl.presetPrevX + 30 + 2 + hl.presetNameW + 2 + 30 + 2 + hl.presetSaveW;
        hl.div2X = saveRight + 6;
        hl.statusX = hl.div2X + 10;
        hl.scopeW = std::clamp (w - 906, 84, 178);
        hl.swingDivX = hl.scopeX + hl.scopeW + 6;
        return hl;
    }

    void paint (juce::Graphics& g) override {
        const auto& p = lookAndFeel->main.currentPalette();
        const int w = getWidth();
        const int h = getHeight();
        const auto hl = computeHeaderLayout (w);

        g.fillAll (p.chassis);

        g.setColour (p.panelWell);
        g.fillRect (0, 0, w, 38);
        g.setColour (p.chassis);
        g.fillRect (0, 38, w, kHeaderHeight - 38);

        g.setColour (p.hairline);
        g.drawHorizontalLine (38, 0.0f, static_cast<float> (w));
        g.drawHorizontalLine (kHeaderHeight - 1, 0.0f, static_cast<float> (w));

        // Top bar wordmark: true recessed dot-matrix LED strip (Spleen 5x8 @ unified 2.0 px pitch).
        const juce::Rectangle<int> wordBox (10, 7, hl.wordW, 24);
        const int availWordCols = std::max (1, static_cast<int> (std::floor (static_cast<float> (wordBox.getWidth() - 6) / dotmatrix::kDotPitch)));
        const auto fullTitle = juce::String (JucePlugin_Name).toUpperCase();
        const auto displayTitle = dotmatrix::kFont5x8.measureCols (fullTitle) <= availWordCols
            ? fullTitle : juce::String ("SLIDER RHYTHM");
        dotmatrix::paintDisplayPanel (g, wordBox, dotmatrix::kFont5x8, displayTitle, p.orange, p.hairline,
                                      juce::Justification::centred, true);

        g.setColour (p.hairline);
        g.drawVerticalLine (hl.div1X, 8.0f, 30.0f);

        g.setColour (p.mutedInk);
        g.setFont (braun::monoFont (10.0f));
        g.drawText ("PRESET", hl.presetLabelX, 8, hl.presetLabelW, 22, juce::Justification::centredLeft);

        g.setColour (p.hairline);
        g.drawVerticalLine (hl.div2X, 8.0f, 30.0f);

        const int statusRight = std::max (hl.statusX + 48, w - 216);
        g.setColour (statusAccent ? p.orange : p.mutedInk);
        g.fillEllipse (static_cast<float> (hl.statusX), 15.5f, 7.0f, 7.0f);
        g.setFont (braun::monoFont (10.8f));
        g.setColour (statusAccent ? p.ink : p.mutedInk);
        g.drawFittedText (statusText, hl.statusX + 13, 8, std::max (32, statusRight - (hl.statusX + 16)), 22,
                          juce::Justification::centredLeft, 1, 0.80f);

        g.setColour (p.hairline);
        g.drawVerticalLine (w - 208, 8.0f, 30.0f);

        // Global swing row: silkscreen label + rotary dial + unified 2.0 px dot-matrix readout (Spleen 6x12).
        g.setFont (braun::monoFont (9.6f));
        g.setColour (p.mutedInk);
        g.drawText ("GLOBAL", 12, 46, 52, 14, juce::Justification::centredLeft);
        g.drawText ("SWING", 12, 60, 52, 14, juce::Justification::centredLeft);

        const auto swingCol = globalSwingValue != 0 ? p.orange : juce::Colour (0xffd5d3cd);
        dotmatrix::paintDisplayPanel (g, { 102, 45, hl.swingDisplayW, 32 }, dotmatrix::kFont6x12,
                                      formatSwingText (globalSwingValue), swingCol, p.hairline,
                                      juce::Justification::centred, true);

        g.setColour (p.hairline);
        g.drawVerticalLine (hl.swingDivX, 44.0f, static_cast<float> (kHeaderHeight - 6));

        g.setColour (p.hairline);
        g.drawRect (0, 0, w, h, 1);
    }

    void resized() override {
        const int w = getWidth();
        const int h = getHeight();
        const auto hl = computeHeaderLayout (w);

        int px = hl.presetPrevX;
        presetPrev.setBounds (px, 7, 30, 24);
        px += 32;
        presetNameBtn.setBounds (px, 7, hl.presetNameW, 24);
        px += hl.presetNameW + 2;
        presetNext.setBounds (px, 7, 30, 24);
        px += 32;
        presetSaveBtn.setBounds (px, 7, hl.presetSaveW, 24);

        scaleBtn.setBounds (w - 202, 7, 62, 24);
        themeBtn.setBounds (w - 138, 7, 82, 24);
        settingsBtn.setBounds (w - 54, 7, 48, 24);

        globalSwingDial.setBounds (66, 45, 32, 32);
        globalSwingScope.setBounds (hl.scopeX, 45, hl.scopeW, 32);

        const int barStart = hl.swingDivX + 8;
        const int barAvail = std::max (200, w - barStart - 8);
        const std::array<std::pair<juce::Component*, int>, 11> items {{
            { &addTrack, 54 }, { &removeTrack, 54 },
            { &unmuteAll, 66 }, { &unsoloAll, 66 },
            { &learn, 60 }, { &noteLearnToggle, 52 }, { &triggerModeToggle, 44 },
            { &playbackMode, 68 }, { &unmappedModeBtn, 78 }, { &inputMatchBtn, 74 },
            { dragMidi.get(), 78 }
        }};
        int baseSum = 0;
        for (const auto& item : items) baseSum += item.second;
        int bx = barStart;
        const int by = 49;
        const int bh = 24;
        if (barAvail >= baseSum) {
            const int extraGap = (barAvail - baseSum) / static_cast<int> (items.size());
            for (const auto& item : items) {
                const int bw = item.second + extraGap;
                if (item.first != nullptr)
                    item.first->setBounds (bx, by, bw, bh);
                bx += bw;
            }
        } else {
            const int barEnd = barStart + barAvail;
            for (size_t i = 0; i < items.size(); ++i) {
                const int bw = (i + 1 == items.size())
                    ? std::max (24, barEnd - bx)
                    : std::max (24, items[i].second * barAvail / baseSum);
                if (items[i].first != nullptr)
                    items[i].first->setBounds (bx, by, bw, bh);
                bx += bw;
            }
        }

        viewport.setBounds (1, kHeaderHeight, std::max (100, w - 2), std::max (100, h - kHeaderHeight - 1));
    }

    void mouseDown (const juce::MouseEvent&) override {
        if (onCancelLearn) onCancelLearn();
    }

    void applyTheme (const braun::Palette& p) {
        globalSwingDial.setColour (juce::Slider::rotarySliderFillColourId, p.orange);
        if (dragMidi) dragMidi->setPalette (p);
        themeBtn.repaint();
        repaint();
    }

    std::function<void()> onCancelLearn;
    SliderRhythmMidiProcessor& processor;
    juce::SharedResourcePointer<LookAndFeelResources> lookAndFeel;
    juce::Viewport& viewport;

    juce::TextButton presetPrev, presetNameBtn, presetNext, presetSaveBtn;
    juce::TextButton scaleBtn, settingsBtn;
    ThemeToggleButton themeBtn;
    juce::TextButton addTrack, removeTrack, unmuteAll, unsoloAll;
    juce::TextButton learn, noteLearnToggle, triggerModeToggle, playbackMode, unmappedModeBtn, inputMatchBtn;
    BraunSlider globalSwingDial;
    GlobalSwingScope globalSwingScope;
    std::unique_ptr<MidiDragHandle> dragMidi;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> globalSwingAttachment;

    juce::String statusText;
    bool statusAccent = false;
    int globalSwingValue = 0;
};

SliderRhythmMidiEditor::SliderRhythmMidiEditor (SliderRhythmMidiProcessor& owner)
    : AudioProcessorEditor (&owner), processor (owner), uiPrefs (lookAndFeel->currentPrefs()) {
    lastSeenPrefsGen = lookAndFeel->prefsGeneration.load (std::memory_order_acquire);
    lookAndFeel->applyPalette (braun::Palette::forMode (uiPrefs.darkMode));
    setLookAndFeel (&lookAndFeel->main);

    viewport.setScrollBarsShown (true, true);
    viewport.setScrollBarThickness (kScrollBarH);
    viewport.setViewedComponent (&stripContent, false);

    rootCanvas = std::make_unique<RootCanvas> (processor, lookAndFeel, viewport);
    rootCanvas->onCancelLearn = [this] {
        if (! choosingLearnTarget) return;
        choosingLearnTarget = false;
        lastSeenVersion = 0;
        timerCallback();
    };
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
            if (mappingpresets::saveUserPreset (updated)) {
                processor.setActiveMappingPresetBaseline (updated);
                lastSeenVersion = 0;
                timerCallback();
            } else {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                    "Could not overwrite mapping preset",
                    "Failed writing " + updated.file.getFullPathName() + ". Check folder permissions.");
            }
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
            const int clamped = uiprefs::clampScalePercent (result);
            bool ok = false;
            safe->uiPrefs = sharedLnf->updatePrefs ([clamped] (auto& s) { s.scalePercent = clamped; }, &ok);
            if (! ok) return;
            safe->lastSeenPrefsGen = sharedLnf->prefsGeneration.load (std::memory_order_acquire);
            safe->applyThemeAndScale (true);
        });
    };

    rootCanvas->themeBtn.onClick = [this] {
        const bool nextDark = ! uiPrefs.darkMode;
        bool ok = false;
        uiPrefs = lookAndFeel->updatePrefs ([nextDark] (auto& s) { s.darkMode = nextDark; }, &ok);
        if (! ok) return;
        lastSeenPrefsGen = lookAndFeel->prefsGeneration.load (std::memory_order_acquire);
        applyThemeAndScale (false);
    };

    rootCanvas->settingsBtn.onClick = [this] { showSettingsMenu(); };

    rootCanvas->dragMidi->getLabelText = [this] { return "DRAG " + juce::String (exportBars) + "B"; };
    rootCanvas->dragMidi->onCreateFile = [this] () -> juce::File {
        const auto snap = processor.snapshot();
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SliderRhythmMidiClips");
        if (dir.createDirectory().failed()) return {};
        const auto file = dir.getChildFile ("SliderRhythm_" + juce::String (exportBars) + "bar_"
                                            + juce::String (juce::Time::currentTimeMillis()) + "_"
                                            + juce::Uuid().toString().substring (0, 8) + ".mid");
        if (! midiexporter::writeMidiFile (snap->sequence, file, exportBars, processor.getLastKnownBpm(),
                                           processor.getLastTimeSigNumerator(), processor.getLastTimeSigDenominator()))
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
        lastSeenVersion = 0;
        timerCallback();
    };
    rootCanvas->removeTrack.onClick = [this] {
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setTrackCount;
        op.arg0 = std::max (1, processor.snapshot()->sequence.trackCount - 1);
        processor.applyUiOp (op);
        lastSeenVersion = 0;
        timerCallback();
    };
    rootCanvas->unmuteAll.onClick = [this] { processor.unmuteAllTracks(); lastSeenVersion = 0; timerCallback(); };
    rootCanvas->unsoloAll.onClick = [this] { processor.unsoloAllTracks(); lastSeenVersion = 0; timerCallback(); };
    rootCanvas->learn.onClick = [this] {
        if (choosingLearnTarget || processor.getLearnTarget() >= 0) {
            choosingLearnTarget = false;
            processor.cancelMidiLearn();
        } else {
            choosingLearnTarget = true;
        }
        lastSeenVersion = 0;
        timerCallback();
    };
    rootCanvas->noteLearnToggle.onClick = [this] {
        processor.setNoteLearnEnabled (! processor.isNoteLearnEnabled());
        lastSeenVersion = 0;
        timerCallback();
    };
    rootCanvas->triggerModeToggle.onClick = [this] {
        const auto next = processor.getLearnTriggerMode() == SliderRhythmMidiProcessor::TriggerMode::toggle
            ? SliderRhythmMidiProcessor::TriggerMode::momentary
            : SliderRhythmMidiProcessor::TriggerMode::toggle;
        processor.setLearnTriggerMode (next);
        lastSeenVersion = 0;
        timerCallback();
    };
    rootCanvas->playbackMode.onClick = [this] {
        const auto current = processor.snapshot()->playbackMode;
        const auto next = (current == sliderseq::PlaybackMode::thru)
            ? sliderseq::PlaybackMode::gen
            : sliderseq::PlaybackMode::thru;
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setPlaybackMode;
        op.arg0 = static_cast<int> (next);
        processor.applyUiOp (op);
        lastSeenVersion = 0;
        timerCallback();
    };
    rootCanvas->unmappedModeBtn.onClick = [this] {
        const auto cur = processor.snapshot()->sequence.unmappedMode;
        const auto next = cur == sliderseq::UnmappedRouteMode::thru
            ? sliderseq::UnmappedRouteMode::block : sliderseq::UnmappedRouteMode::thru;
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setUnmappedMode;
        op.arg0 = static_cast<int> (next);
        processor.applyUiOp (op);
        lastSeenVersion = 0;
        timerCallback();
    };
    rootCanvas->inputMatchBtn.onClick = [this] {
        const auto cur = processor.snapshot()->sequence.inputChannelMatch;
        const auto next = cur == sliderseq::InputChannelMatch::autoLiveCh1 ? sliderseq::InputChannelMatch::omni
                        : cur == sliderseq::InputChannelMatch::omni ? sliderseq::InputChannelMatch::strict
                        : sliderseq::InputChannelMatch::autoLiveCh1;
        processor.setInputChannelMatch (next);
        uiPrefs = lookAndFeel->updatePrefs ([next] (auto& s) { s.inputChannelMatch = next; });
        lastSeenPrefsGen = lookAndFeel->prefsGeneration.load (std::memory_order_acquire);
        lastSeenVersion = 0;
        timerCallback();
    };

    setResizable (true, true);
    const auto initialSnap = processor.snapshot();
    rebuildStrips (*initialSnap);
    applyThemeAndScale (true);
    startTimerHz (60);
}

SliderRhythmMidiEditor::~SliderRhythmMidiEditor() {
    stopTimer();
    if (activeDialog != nullptr)
        activeDialog->exitModalState (0);
    // Do NOT call PopupMenu::dismissAllActiveMenus() globally so other open instances keep their menus intact.
    strips.clear();
    stripContent.removeAllChildren();
    viewport.setViewedComponent (nullptr, false);
    rootCanvas.reset();
    setLookAndFeel (nullptr);
}

void SliderRhythmMidiEditor::applyThemeAndScale (bool resizeWindow) {
    const auto p = braun::Palette::forMode (uiPrefs.darkMode);
    lookAndFeel->applyPalette (p);

    const float scale = static_cast<float> (uiPrefs.scalePercent) / 100.0f;
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* display = displays.getDisplayForRect (getScreenBounds());
    if (display == nullptr) display = displays.getPrimaryDisplay();
    const int maxScreenW = display != nullptr ? std::max (640, static_cast<int> (display->userBounds.getWidth()) - 40) : 1600;
    const int maxScreenH = display != nullptr ? std::max (480, static_cast<int> (display->userBounds.getHeight()) - 60) : 960;

    const int minPhysW = std::min (maxScreenW, static_cast<int> (std::lround (kMinLogicalW * scale)));
    const int minPhysH = std::min (maxScreenH, static_cast<int> (std::lround (kMinLogicalH * scale)));
    setResizeLimits (minPhysW, minPhysH,
                     static_cast<int> (std::lround (kMaxLogicalW * scale)),
                     static_cast<int> (std::lround (kMaxLogicalH * scale)));

    if (rootCanvas) {
        rootCanvas->setTransform (juce::AffineTransform::scale (scale));
        rootCanvas->applyTheme (p);
        rootCanvas->scaleBtn.setButtonText (juce::String (uiPrefs.scalePercent) + "%");
        rootCanvas->themeBtn.setButtonText (uiPrefs.darkMode ? "DARK / LGT" : "LIGHT / DRK");
        rootCanvas->presetPrev.setColour (juce::TextButton::textColourOffId, p.mutedInk);
        rootCanvas->presetNext.setColour (juce::TextButton::textColourOffId, p.mutedInk);
        rootCanvas->settingsBtn.setColour (juce::TextButton::textColourOffId, p.mutedInk);
    }

    const auto snap = processor.snapshot();
    for (auto& strip : strips) strip->applyTheme (*snap);

    if (resizeWindow) {
        const int targetLogicalW = shownTrackCount <= 5
            ? kDefaultLogicalW
            : std::clamp (std::min (shownTrackCount, 6) * kStripWidth + 2, kDefaultLogicalW, 1346);
        const int physW = std::min (maxScreenW, static_cast<int> (std::lround (targetLogicalW * scale)));
        const int physH = std::min (maxScreenH, static_cast<int> (std::lround (kDefaultLogicalH * scale)));
        setSize (std::max (minPhysW, physW), std::max (minPhysH, physH));
    }

    resized();
    refreshHeaderState (*snap);
    repaint();
}

void SliderRhythmMidiEditor::rebuildStrips (const SliderRhythmMidiProcessor::Snapshot& snap) {
    stripContent.removeAllChildren();
    strips.clear();
    shownTrackCount = snap.sequence.trackCount;
    juce::Component::SafePointer<SliderRhythmMidiEditor> safeEd (this);
    for (int i = 0; i < shownTrackCount; ++i) {
        auto strip = std::make_unique<TrackStrip> (processor, i, lookAndFeel, snap);
        strip->onControlLearn = [this] (int track, SliderRhythmMidiProcessor::Control control) {
            if (! choosingLearnTarget && processor.getLearnTarget() < 0) return false;
            choosingLearnTarget = false;
            processor.armMidiLearn (track, control);
            lastSeenVersion = 0;
            timerCallback();
            return true;
        };
        strip->onCancelLearn = [this] {
            if (! choosingLearnTarget) return;
            choosingLearnTarget = false;
            lastSeenVersion = 0;
            timerCallback();
        };
        strip->onRequestDialogParent = [this] { return rootCanvas.get(); };
        strip->onCopyPattern = [this] (const sliderseq::Pattern& pat) { patternClipboard = pat; };
        strip->onGetClipboard = [this] { return patternClipboard; };
        // Defer structural strip refresh asynchronously so a child button never destroys itself mid-mouseUp.
        strip->onTracksModified = [safeEd] {
            juce::MessageManager::callAsync ([safeEd] {
                if (safeEd == nullptr) return;
                safeEd->lastSeenVersion = 0;
                safeEd->timerCallback();
            });
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
    const auto snap = processor.snapshot();
    rebuildStrips (*snap);
    refreshHeaderState (*snap);
}

void SliderRhythmMidiEditor::showMappingPresetMenu() {
    const auto snap = processor.snapshot();
    const auto& active = snap->activeMappingPreset;
    const bool dirty = ! mappingpresets::matchesState (active, snap->sequence);
    const auto factories = mappingpresets::factoryPresets();
    const auto users = mappingpresets::loadUserPresets();

    static constexpr int kFactoryMappingMenuBase = 100;
    static constexpr int kUserMappingMenuBase = 10000;

    juce::PopupMenu menu;
    menu.addSectionHeader ("Factory Mapping Presets");
    for (size_t i = 0; i < factories.size(); ++i)
        menu.addItem (kFactoryMappingMenuBase + static_cast<int> (i), juce::String (factories[i].name), true,
                      active.id == factories[i].id);

    if (! users.empty()) {
        menu.addSeparator();
        menu.addSectionHeader ("User Mapping Presets");
        for (size_t i = 0; i < users.size(); ++i)
            menu.addItem (kUserMappingMenuBase + static_cast<int> (i), juce::String (users[i].name), true,
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
                if (mappingpresets::saveUserPreset (updated)) {
                    safe->processor.setActiveMappingPresetBaseline (updated);
                    safe->lastSeenVersion = 0;
                    safe->timerCallback();
                } else {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        "Could not overwrite mapping preset",
                        "Failed writing " + updated.file.getFullPathName() + ". Check folder permissions.");
                }
            } else {
                safe->saveMappingPresetDialog (true);
            }
        } else if (result == 2) {
            safe->saveMappingPresetDialog (true);
        } else if (result == 3) {
            safe->processor.loadMappingPreset (active);
            const auto updatedSnap = safe->processor.snapshot();
            safe->rebuildStrips (*updatedSnap);
            safe->refreshHeaderState (*updatedSnap);
        } else if (result == 4) {
            if (mappingpresets::deleteUserPreset (active)) {
                safe->processor.setActiveMappingPresetBaseline (mappingpresets::makeDefaultKitPreset());
                safe->lastSeenVersion = 0;
                safe->timerCallback();
            }
        } else if (result >= kFactoryMappingMenuBase && result < kFactoryMappingMenuBase + static_cast<int> (factories.size())) {
            safe->processor.loadMappingPreset (factories[static_cast<size_t> (result - kFactoryMappingMenuBase)]);
            const auto updatedSnap = safe->processor.snapshot();
            safe->rebuildStrips (*updatedSnap);
            safe->refreshHeaderState (*updatedSnap);
        } else if (result >= kUserMappingMenuBase && result < kUserMappingMenuBase + static_cast<int> (users.size())) {
            safe->processor.loadMappingPreset (users[static_cast<size_t> (result - kUserMappingMenuBase)]);
            const auto updatedSnap = safe->processor.snapshot();
            safe->rebuildStrips (*updatedSnap);
            safe->refreshHeaderState (*updatedSnap);
        }
    });
}

void SliderRhythmMidiEditor::saveMappingPresetDialog (bool) {
    const auto snap = processor.snapshot();
    auto sharedLnf = lookAndFeel;
    // Parent to rootCanvas so the dialog matches the scaled UI consistently with track dialogs.
    auto dialog = std::make_unique<juce::AlertWindow> (
        "Save Mapping Preset", "Save track layout, notes, channels, zones, routing (TOP/MID/0), macro modes, choke, and swing % as a Mapping Preset.",
        juce::MessageBoxIconType::NoIcon, rootCanvas.get());
    dialog->setLookAndFeel (&sharedLnf->main);
    dialog->addTextEditor ("name", juce::String (snap->activeMappingPreset.name) + " Custom", "Preset name");
    dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    auto* window = dialog.release();
    activeDialog = window;
    juce::Component::SafePointer<SliderRhythmMidiEditor> safe (this);
    juce::Component::SafePointer<juce::AlertWindow> safeWin (window);
    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe, sharedLnf, safeWin] (int result) {
            if (safeWin != nullptr) safeWin->setLookAndFeel (nullptr);
            if (safe == nullptr || safeWin == nullptr || result != 1) return;
            const auto name = safeWin->getTextEditorContents ("name").trim().substring (0, 48);
            if (name.isEmpty()) return;
            const auto cur = safe->processor.snapshot();
            auto preset = mappingpresets::captureFromState (cur->sequence, {}, name.toStdString(), false);
            if (! mappingpresets::saveUserPreset (preset)) {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                    "Could not save mapping preset", "Check write access to the Mapping Presets folder.");
                return;
            }
            safe->processor.setActiveMappingPresetBaseline (preset);
            safe->lastSeenVersion = 0;
            safe->timerCallback();
        }), true);
}

void SliderRhythmMidiEditor::showSettingsMenu() {
    const auto curMatch = processor.snapshot()->sequence.inputChannelMatch;
    juce::PopupMenu menu;
    menu.addSectionHeader ("Scroll Wheel Controls");
    menu.addItem (1, "Vertical Scroll -> Knobs & Faders", true, uiPrefs.verticalWheelControls);
    menu.addItem (2, "Horizontal Scroll -> Knobs & Faders", true, uiPrefs.horizontalWheelControls);
    menu.addSeparator();
    menu.addSectionHeader ("MIDI Input Channel Match");
    menu.addItem (20, "Auto (Exact Ch + Ableton Live CH1 Fallback)", true, curMatch == sliderseq::InputChannelMatch::autoLiveCh1);
    menu.addItem (21, "Omni (Match Any Channel by Note)", true, curMatch == sliderseq::InputChannelMatch::omni);
    menu.addItem (22, "Strict (Require Exact Channel + Note)", true, curMatch == sliderseq::InputChannelMatch::strict);
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
        bool ok = true;
        if (result == 1) {
            safe->uiPrefs = sharedLnf->updatePrefs ([] (auto& s) { s.verticalWheelControls = ! s.verticalWheelControls; }, &ok);
        } else if (result == 2) {
            safe->uiPrefs = sharedLnf->updatePrefs ([] (auto& s) { s.horizontalWheelControls = ! s.horizontalWheelControls; }, &ok);
        } else if (result == 10) {
            safe->uiPrefs = sharedLnf->updatePrefs ([] (auto& s) { s.darkMode = false; }, &ok);
        } else if (result == 11) {
            safe->uiPrefs = sharedLnf->updatePrefs ([] (auto& s) { s.darkMode = true; }, &ok);
        } else if (result >= 20 && result <= 22) {
            const auto mode = sliderseq::clampInputChannelMatch (result - 20);
            safe->processor.setInputChannelMatch (mode);
            safe->uiPrefs = sharedLnf->updatePrefs ([mode] (auto& s) { s.inputChannelMatch = mode; }, &ok);
            safe->lastSeenVersion = 0;
            safe->timerCallback();
        } else if (result >= 175 && result <= 225) {
            const int sc = uiprefs::clampScalePercent (result - 100);
            safe->uiPrefs = sharedLnf->updatePrefs ([sc] (auto& s) { s.scalePercent = sc; }, &ok);
            resizeNeeded = true;
        }
        if (! ok) return;
        safe->lastSeenPrefsGen = sharedLnf->prefsGeneration.load (std::memory_order_acquire);
        safe->applyThemeAndScale (resizeNeeded);
    });
}

void SliderRhythmMidiEditor::refreshHeaderStatusText (bool allThru) {
    if (! rootCanvas) return;
    const bool noteOn = processor.isNoteLearnEnabled();
    const int armedTarget = processor.getLearnTarget();
    const bool armed = armedTarget >= 0;
    const juce::String learnedControl = armed ? kControlNames[static_cast<size_t> (armedTarget % SliderRhythmMidiProcessor::controlCount)] : "";
    const juce::String transport = processor.isHostRecording() ? "HOST REC"
                                 : processor.isHostPlaying() ? "HOST PLAY"
                                 : processor.hasHostPosition() ? "HOST STOP"
                                 : "IDLE";
    const auto ctrlEnum = armed ? static_cast<SliderRhythmMidiProcessor::Control> (armedTarget % SliderRhythmMidiProcessor::controlCount)
                                : SliderRhythmMidiProcessor::slot;
    const juce::String learnHint = (noteOn && SliderRhythmMidiProcessor::supportsNoteLearnControl (ctrlEnum))
        ? "move CC/Note" : "move CC";

    const auto midDot = juce::String::fromUTF8 (" \xc2\xb7 ");
    const juce::String bpmSuffix = processor.hasHostPosition()
        ? (midDot + juce::String (static_cast<int> (std::lround (processor.getLastKnownBpm()))) + " BPM")
        : juce::String {};
    const bool nextAccent = choosingLearnTarget || armed || allThru || processor.isHostPlaying() || processor.isHostRecording();
    const juce::String nextText = choosingLearnTarget ? "CLICK ANY CONTROL TO LEARN"
                                : armed ? ("LEARN T" + juce::String (armedTarget / SliderRhythmMidiProcessor::controlCount + 1)
                                           + " " + learnedControl + ": " + learnHint)
                                : allThru ? "ALL THRU BYPASS"
                                : (transport + bpmSuffix + midDot + "OUT " + juce::String (processor.generatedNoteCount()));

    if (rootCanvas->statusAccent != nextAccent || rootCanvas->statusText != nextText) {
        rootCanvas->statusAccent = nextAccent;
        rootCanvas->statusText = nextText;
        const auto hl = RootCanvas::computeHeaderLayout (rootCanvas->getWidth());
        rootCanvas->repaint (hl.div2X, 0, std::max (60, rootCanvas->getWidth() - hl.div2X - 208), 38);
    }
}

void SliderRhythmMidiEditor::refreshHeaderState (const SliderRhythmMidiProcessor::Snapshot& state) {
    if (! rootCanvas) return;
    const auto& p = lookAndFeel->main.currentPalette();

    const bool dirty = ! mappingpresets::matchesState (state.activeMappingPreset, state.sequence);
    const juce::String presetLabel = juce::String (state.activeMappingPreset.name) + (dirty ? " *" : "");
    rootCanvas->presetNameBtn.setButtonText (presetLabel);
    rootCanvas->presetNameBtn.setColour (juce::TextButton::textColourOffId, dirty ? p.orange : p.ink);
    rootCanvas->presetSaveBtn.setColour (juce::TextButton::buttonColourId, dirty ? p.orangeTint : juce::Colours::transparentBlack);
    rootCanvas->presetSaveBtn.setColour (juce::TextButton::textColourOffId, dirty ? p.orange : p.orange);

    rootCanvas->globalSwingValue = state.sequence.globalSwing;
    rootCanvas->globalSwingDial.setValue (state.sequence.globalSwing, juce::dontSendNotification);
    rootCanvas->globalSwingScope.setSwing (state.sequence.globalSwing, p);

    bool anyMuted = false, anySolo = false;
    for (int i = 0; i < state.sequence.trackCount; ++i) {
        anyMuted = anyMuted || state.sequence.tracks[static_cast<size_t> (i)].muted;
        anySolo = anySolo || state.sequence.tracks[static_cast<size_t> (i)].solo;
    }
    rootCanvas->unmuteAll.setColour (juce::TextButton::buttonColourId, anyMuted ? p.muteRed : juce::Colours::transparentBlack);
    rootCanvas->unmuteAll.setColour (juce::TextButton::textColourOffId, anyMuted ? juce::Colours::white : p.ink);
    rootCanvas->unsoloAll.setColour (juce::TextButton::buttonColourId, anySolo ? p.soloCyan : juce::Colours::transparentBlack);
    rootCanvas->unsoloAll.setColour (juce::TextButton::textColourOffId, anySolo ? juce::Colours::white : p.ink);

    const bool noteOn = processor.isNoteLearnEnabled();
    rootCanvas->noteLearnToggle.setColour (juce::TextButton::buttonColourId, noteOn ? p.orange : juce::Colours::transparentBlack);
    rootCanvas->noteLearnToggle.setColour (juce::TextButton::textColourOffId, noteOn ? juce::Colours::white : p.mutedInk);

    const bool isToggle = processor.getLearnTriggerMode() == SliderRhythmMidiProcessor::TriggerMode::toggle;
    rootCanvas->triggerModeToggle.setButtonText (isToggle ? "TGL" : "HLD");
    rootCanvas->triggerModeToggle.setColour (juce::TextButton::buttonColourId, isToggle ? juce::Colours::transparentBlack : p.orangeTint);
    rootCanvas->triggerModeToggle.setColour (juce::TextButton::textColourOffId, isToggle ? p.mutedInk : p.orange);

    const int armedTarget = processor.getLearnTarget();
    const bool armed = armedTarget >= 0;
    const bool allThru = (state.playbackMode == sliderseq::PlaybackMode::thru);
    cachedAllThru = allThru;
    refreshHeaderStatusText (allThru);

    rootCanvas->learn.setButtonText (choosingLearnTarget || armed ? "CANCEL" : "LEARN");
    rootCanvas->learn.setColour (juce::TextButton::buttonColourId, (choosingLearnTarget || armed) ? p.orange : juce::Colours::transparentBlack);
    rootCanvas->learn.setColour (juce::TextButton::textColourOffId, (choosingLearnTarget || armed) ? juce::Colours::white : p.ink);

    rootCanvas->playbackMode.setButtonText (allThru ? "ALL THRU" : "ACTIVE");
    rootCanvas->playbackMode.setColour (juce::TextButton::buttonColourId, allThru ? p.orange : juce::Colours::transparentBlack);
    rootCanvas->playbackMode.setColour (juce::TextButton::textColourOffId, allThru ? juce::Colours::white : p.ink);

    const bool unmappedBlock = (state.sequence.unmappedMode == sliderseq::UnmappedRouteMode::block);
    rootCanvas->unmappedModeBtn.setButtonText (unmappedBlock ? "OTH:BLK" : "OTH:THRU");
    rootCanvas->unmappedModeBtn.setColour (juce::TextButton::buttonColourId, unmappedBlock ? p.orangeTint : juce::Colours::transparentBlack);
    rootCanvas->unmappedModeBtn.setColour (juce::TextButton::textColourOffId, unmappedBlock ? p.orange : p.mutedInk);

    const auto inMatch = state.sequence.inputChannelMatch;
    const juce::String inMatchText = inMatch == sliderseq::InputChannelMatch::omni ? "IN:OMNI"
                                   : inMatch == sliderseq::InputChannelMatch::strict ? "IN:STR" : "IN:AUTO";
    rootCanvas->inputMatchBtn.setButtonText (inMatchText);
    rootCanvas->inputMatchBtn.setColour (juce::TextButton::buttonColourId,
                                         inMatch == sliderseq::InputChannelMatch::omni ? p.cyanTint
                                       : inMatch == sliderseq::InputChannelMatch::strict ? p.orangeTint
                                       : juce::Colours::transparentBlack);
    rootCanvas->inputMatchBtn.setColour (juce::TextButton::textColourOffId,
                                         inMatch == sliderseq::InputChannelMatch::omni ? p.cyan
                                       : inMatch == sliderseq::InputChannelMatch::strict ? p.orange
                                       : p.mutedInk);

    rootCanvas->repaint();
}

void SliderRhythmMidiEditor::timerCallback() {
    // Pick up shared appearance/scale preference updates from sibling plugin instances cleanly.
    const uint64_t prefGen = lookAndFeel->prefsGeneration.load (std::memory_order_acquire);
    if (prefGen != lastSeenPrefsGen) {
        const auto updated = lookAndFeel->currentPrefs();
        const bool scaleChanged = updated.scalePercent != uiPrefs.scalePercent;
        uiPrefs = updated;
        lastSeenPrefsGen = prefGen;
        applyThemeAndScale (scaleChanged);
    }

    const uint64_t currentVersion = processor.stateVersion();
    const bool hostOrAudioDirty = processor.hasPendingHostOrAudioChanges();
    if (currentVersion != lastSeenVersion || hostOrAudioDirty) {
        processor.syncAudioCcToApvts();
        const auto state = processor.snapshot();
        if (shownTrackCount != state->sequence.trackCount) {
            rebuildStrips (*state);
        } else {
            for (auto& strip : strips) strip->refresh (*state);
            lastSeenVersion = processor.stateVersion();
        }
        refreshHeaderState (*state);
    } else {
        // Zero-copy idle/playback path: only update playhead highlights and live status line.
        for (auto& strip : strips) strip->updatePlayhead();
        refreshHeaderStatusText (cachedAllThru);
    }
}

void SliderRhythmMidiEditor::paint (juce::Graphics& g) {
    g.fillAll (lookAndFeel->main.currentPalette().chassis);
}

void SliderRhythmMidiEditor::resized() {
    if (! rootCanvas) return;
    const float requestedScale = static_cast<float> (uiPrefs.scalePercent) / 100.0f;
    // Clamp effective scale by BOTH width and height whenever the host/screen constrains either dimension,
    // guaranteeing header controls (scale, theme, CFG), bottom step pads, and the horizontal scrollbar stay inside.
    float effectiveScale = requestedScale;
    if (getWidth() > 0)
        effectiveScale = std::min (effectiveScale, std::max (0.5f, static_cast<float> (getWidth()) / static_cast<float> (kMinLogicalW)));
    if (getHeight() > 0)
        effectiveScale = std::min (effectiveScale, std::max (0.5f, static_cast<float> (getHeight()) / static_cast<float> (kMinLogicalH)));

    int logicalW = getWidth() > 0
        ? std::max (kMinLogicalW, static_cast<int> (std::floor (static_cast<float> (getWidth()) / effectiveScale)))
        : kMinLogicalW;
    int logicalH = getHeight() > 0
        ? std::max (kMinLogicalH, static_cast<int> (std::floor (static_cast<float> (getHeight()) / effectiveScale)))
        : kMinLogicalH;
    // Guard against 1-ULP float rounding pushing scaled bounds 1 px past the physical window edge.
    if (getWidth() > 0 && static_cast<float> (logicalW) * effectiveScale > static_cast<float> (getWidth()))
        effectiveScale = std::nextafter (static_cast<float> (getWidth()) / static_cast<float> (logicalW), 0.0f);
    if (getHeight() > 0 && static_cast<float> (logicalH) * effectiveScale > static_cast<float> (getHeight()))
        effectiveScale = std::nextafter (static_cast<float> (getHeight()) / static_cast<float> (logicalH), 0.0f);
    if (getWidth() > 0)
        logicalW = std::max (kMinLogicalW, static_cast<int> (std::floor (static_cast<float> (getWidth()) / effectiveScale)));
    if (getHeight() > 0)
        logicalH = std::max (kMinLogicalH, static_cast<int> (std::floor (static_cast<float> (getHeight()) / effectiveScale)));

    rootCanvas->setTransform (juce::AffineTransform::scale (effectiveScale));
    rootCanvas->setBounds (0, 0, logicalW, logicalH);

    if (shownTrackCount == 0) return;

    const int rawVpW = std::max (kStripWidth, viewport.getWidth());
    const int rawVpH = std::max (100, viewport.getHeight());
    const int stripW = kStripWidth;
    const int contentW = shownTrackCount * stripW;
    bool needVScroll = (rawVpH < kMinStripHeight);
    int availW = std::max (kStripWidth, rawVpW - (needVScroll ? kScrollBarH : 0));
    bool needHScroll = (contentW > availW);

    // If horizontal scroll appears and reduces vertical height below kMinStripHeight, re-evaluate once stably.
    if (! needVScroll && needHScroll && (rawVpH - kScrollBarH) < kMinStripHeight) {
        needVScroll = true;
        availW = std::max (kStripWidth, rawVpW - kScrollBarH);
        needHScroll = (contentW > availW);
    }

    const int availH = std::max (100, rawVpH - (needHScroll ? kScrollBarH : 0));
    const int stripH = std::max (kMinStripHeight, availH);

    stripContent.setSize (contentW, stripH);
    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (static_cast<int> (i) * stripW, 0, stripW, stripH);
}
