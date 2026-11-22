#include "PluginEditor.h"
#include "DrumMaps.h"
#include "MidiExporter.h"
#include "PatternPresets.h"

namespace {
const juce::Colour chassis { 0xffbfbcb2 }, panel { 0xffdedacf }, ink { 0xff272724 };
const juce::Colour key { 0xffcbc7bb }, border { 0xff878278 }, orange { 0xffed883e };
const juce::Colour legendOrange { 0xffa64716 }, muteRed { 0xffdf8b82 }, soloAmber { 0xffefbe62 };
const juce::Colour playheadRing { 0xff1d1c19 };
constexpr int trackWidth = 244;
constexpr int trackHeight = 665;
constexpr int windowGutter = 24;
constexpr int windowChromeHeight = 94;
constexpr int minWindowWidth = 980;
constexpr int maxWindowWidth = 2560;

const std::array<double, 5> rates { 1.0 / 3.0, .5, 1.0, 2.0, 3.0 };
const std::array<const char*, 5> rateLabels { "1/3", "1/2", "1x", "2x", "3x" };
const std::array<const char*, SliderRhythmMidiProcessor::controlCount> controlNames {
    "Pattern fader", "DIV", "Velocity", "Shuffle/Swing", "MOD amount", "Mute", "MOD type",
    "MIDI note", "MIDI channel", "Pattern length", "Step grid", "Solo" };

const char* playbackModeLabel (sliderseq::PlaybackMode mode) noexcept {
    switch (mode) {
        case sliderseq::PlaybackMode::gen: return "GEN";
        case sliderseq::PlaybackMode::genPlusThru: return "GEN + THRU";
        case sliderseq::PlaybackMode::thru: return "THRU";
    }
    return "GEN";
}

class DrumMachineLookAndFeel : public juce::LookAndFeel_V4 {
public:
    DrumMachineLookAndFeel() : juce::LookAndFeel_V4 (getLightColourScheme()) {
        setColour (juce::Label::textColourId, ink);
        setColour (juce::TextButton::buttonColourId, key);
        setColour (juce::TextButton::buttonOnColourId, orange);
        setColour (juce::TextButton::textColourOffId, ink);
        setColour (juce::TextButton::textColourOnId, ink);
        setColour (juce::ComboBox::backgroundColourId, panel);
        setColour (juce::ComboBox::outlineColourId, border);
        setColour (juce::ComboBox::textColourId, ink);
        setColour (juce::PopupMenu::backgroundColourId, panel);
        setColour (juce::PopupMenu::textColourId, ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, orange);
        setColour (juce::PopupMenu::highlightedTextColourId, ink);
        setColour (juce::Slider::textBoxTextColourId, ink);
        setColour (juce::Slider::textBoxOutlineColourId, border);
        setColour (juce::ScrollBar::thumbColourId, border);
        setColour (juce::AlertWindow::backgroundColourId, panel);
        setColour (juce::AlertWindow::textColourId, ink);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float position, float startAngle, float endAngle, juce::Slider&) override {
        const juce::Point<float> centre (x + width * .5f, y + height * .5f);
        const float radius = std::min (width, height) * .33f;
        if (radius < 2.0f)
            return;

        const auto at = [centre] (float angle, float distance) {
            return centre + juce::Point<float> (std::sin (angle), -std::cos (angle)) * distance;
        };
        const float pointerAngle = juce::jmap (position, startAngle, endAngle);
        for (int i = 0; i <= 10; ++i) {
            const float angle = juce::jmap (static_cast<float> (i) / 10, startAngle, endAngle);
            g.setColour (angle <= pointerAngle ? legendOrange : border);
            g.drawLine ({ at (angle, radius + 3.0f), at (angle, radius + 5.5f) }, 1.1f);
        }

        const juce::Rectangle<float> cap (centre.x - radius, centre.y - radius, radius * 2, radius * 2);
        g.setColour (ink.withAlpha (.28f));
        g.fillEllipse (cap.translated (0, 2.0f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe4e1d7), cap.getTopLeft(),
                                                 juce::Colour (0xff918d83), cap.getBottomLeft(), false));
        g.fillEllipse (cap);
        g.setColour (ink.withAlpha (.65f));
        g.drawEllipse (cap, 1.0f);
        g.setColour (ink);
        g.drawLine ({ at (pointerAngle, radius * .27f), at (pointerAngle, radius * .83f) }, 2.2f);
    }
};

class CompactButtonLookAndFeel final : public DrumMachineLookAndFeel {
public:
    juce::Font getTextButtonFont (juce::TextButton&, int) override {
        return juce::Font (juce::FontOptions (11.0f, juce::Font::bold));
    }
};

class PatternFaderLookAndFeel final : public DrumMachineLookAndFeel {
public:
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float, const juce::Slider::SliderStyle,
                           juce::Slider&) override {
        const auto left = static_cast<float> (x);
        const auto top = static_cast<float> (y);
        const auto centre = left + width * .5f;
        const auto travelTop = top + 9.0f;
        const auto travelBottom = top + height - 9.0f;

        g.setColour (juce::Colour (0xff353431));
        g.fillRoundedRectangle (centre - 5.5f, top, 11.0f, static_cast<float> (height), 4.0f);
        g.setColour (juce::Colour (0xff716c62));
        g.drawRoundedRectangle (centre - 5.5f, top, 11.0f, static_cast<float> (height), 4.0f, 1.0f);

        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        for (int mark = 0; mark < 12; ++mark) {
            const auto markY = travelTop + (travelBottom - travelTop) * mark / 11.0f;
            const bool fill = mark < 3;
            g.setColour (fill ? legendOrange : ink);
            g.drawText (juce::String (12 - mark), x + 1, static_cast<int> (markY - 7), 18, 14, juce::Justification::centredRight);
            g.drawHorizontalLine (static_cast<int> (std::round (markY)), centre - 20.0f, centre - 10.0f);
            g.drawHorizontalLine (static_cast<int> (std::round (markY)), centre + 10.0f, centre + 20.0f);
        }

        const auto capY = juce::jlimit (top + 8.5f, top + height - 8.5f, sliderPos);
        const juce::Rectangle<float> cap (centre - 28.0f, capY - 8.5f, 56.0f, 17.0f);
        g.setColour (juce::Colours::black.withAlpha (.6f));
        g.fillRoundedRectangle (cap.translated (0, 3), 3.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffbf7ed), cap.getTopLeft(),
                                                 juce::Colour (0xffaea89b), cap.getBottomLeft(), false));
        g.fillRoundedRectangle (cap, 3.0f);
        g.setColour (juce::Colour (0xff716c62));
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);
        g.setColour (juce::Colour (0xff766f63).withAlpha (.85f));
        for (int ridge = 4; ridge <= 12; ridge += 4)
            g.drawHorizontalLine (static_cast<int> (cap.getY()) + ridge, cap.getX() + 5, cap.getRight() - 5);
    }
};

class LearnableSlider final : public juce::Slider {
public:
    std::function<bool()> interceptLearn;
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
private:
    bool learningPress = false;
};

class LearnableButton : public juce::TextButton {
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
protected:
    bool isLearningPress() const { return learningPress; }
private:
    bool learningPress = false;
};

class LearnableComboBox final : public juce::ComboBox {
public:
    std::function<bool()> interceptLearn;
    void mouseDown (const juce::MouseEvent& event) override {
        if (interceptLearn && interceptLearn()) return;
        juce::ComboBox::mouseDown (event);
    }
};

class StepButton final : public juce::TextButton {
public:
    StepButton() : juce::TextButton ("") { setClickingTogglesState (false); }
    void setPlayheadActive (bool active) {
        if (playheadActive == active) return;
        playheadActive = active;
        repaint();
    }
    void paintOverChildren (juce::Graphics& g) override {
        if (! playheadActive) return;
        g.setColour (playheadRing);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.75f), 2.5f, 2.0f);
    }
private:
    bool playheadActive = false;
};

void label (juce::Label& component, const juce::String& text, juce::Component& parent) {
    component.setText (text, juce::dontSendNotification);
    component.setColour (juce::Label::textColourId, ink);
    component.setJustificationType (juce::Justification::centred);
    parent.addAndMakeVisible (component);
}

void rotary (juce::Slider& slider, double minimum, double maximum, double initial, juce::Component& parent) {
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 48, 18);
    slider.setRange (minimum, maximum, 1);
    slider.setValue (initial, juce::dontSendNotification);
    slider.setColour (juce::Slider::rotarySliderFillColourId, legendOrange);
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, border);
    slider.setColour (juce::Slider::thumbColourId, orange);
    parent.addAndMakeVisible (slider);
}
} // namespace

struct SliderRhythmMidiEditor::LookAndFeelResources {
    DrumMachineLookAndFeel main;
    CompactButtonLookAndFeel compactButton;
    PatternFaderLookAndFeel patternFader;
};

class SliderRhythmMidiEditor::MidiDragHandle final : public juce::Component, public juce::SettableTooltipClient {
public:
    std::function<juce::File()> onCreateFile;
    std::function<void()> onRightClick;
    std::function<juce::String()> getLabelText;

    MidiDragHandle() {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        setTooltip ("Drag into Ableton / Bitwig / Logic to export a MIDI clip (right-click to set bars)");
    }

    void paint (juce::Graphics& g) override {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (dragging ? orange : key);
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (border);
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
        g.setColour (ink);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        const auto text = getLabelText ? getLabelText() : juce::String ("DRAG .MID");
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
};

class SliderRhythmMidiEditor::TrackStrip final : public juce::Component {
public:
    std::function<bool(int, SliderRhythmMidiProcessor::Control)> onControlLearn;
    std::function<void(const sliderseq::Pattern&)> onCopyPattern;
    std::function<std::optional<sliderseq::Pattern>()> onGetClipboard;
    std::function<void()> onTracksModified;

    TrackStrip (SliderRhythmMidiProcessor& owner, int number) : processor (owner), index (number) {
        addAndMakeVisible (titleButton);
        titleButton.setTooltip ("Track menu: rename, clear, copy/paste slot, duplicate track, MOD shape, gate, choke, velocity scale");
        titleButton.onClick = [this] { showTrackOptionsMenu(); };
        label (velocityCaption, "VELOCITY", *this); label (grooveCaption, "SHUFFLE / SWING", *this);
        label (noteCaption, "MIDI NOTE", *this); label (channelCaption, "CH", *this);
        label (lengthCaption, "LENGTH", *this); label (modCaption, "MOD AMT", *this);
        rotary (velocity, 1, 127, 100, *this); rotary (groove, 0, 100, 50, *this);
        rotary (note, 0, 127, 36, *this); rotary (channel, 1, 16, 10, *this);
        rotary (modAmount, 0, 100, 50, *this); rotary (length, 1, 64, 16, *this);
        grid.setTooltip ("Set total steps without changing the duration of each step");

        slot.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::slot); };
        velocity.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::velocity); };
        groove.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::groove); };
        note.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::note); };
        channel.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::channel); };
        modAmount.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::modAmount); };
        length.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::length); };
        grid.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::grid); };
        mod.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::modMode); };
        muteButton.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::mute); };
        soloButton.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::solo); };

        slot.setSliderStyle (juce::Slider::LinearVertical);
        slot.setLookAndFeel (&lookAndFeel->patternFader);
        slot.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 18);
        slot.setRange (1, 12, 1); slot.setColour (juce::Slider::thumbColourId, orange);
        addAndMakeVisible (slot);

        for (int i = 0; i < 5; ++i) {
            rateButtons[static_cast<size_t> (i)].setButtonText (rateLabels[static_cast<size_t> (i)]);
            rateButtons[static_cast<size_t> (i)].setLookAndFeel (&lookAndFeel->compactButton);
            rateButtons[static_cast<size_t> (i)].interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::rate); };
            addAndMakeVisible (rateButtons[static_cast<size_t> (i)]);
            rateButtons[static_cast<size_t> (i)].onClick = [this, i] {
                processor.setHostParamValue (index, "rate", static_cast<float> (i));
                refresh();
            };
        }

        muteButton.setLookAndFeel (&lookAndFeel->compactButton);
        soloButton.setLookAndFeel (&lookAndFeel->compactButton);
        muteButton.setTooltip ("Mute track");
        soloButton.setTooltip ("Solo track (exclusive; Cmd/Ctrl+click for multi-track solo)");
        addAndMakeVisible (mod); addAndMakeVisible (muteButton); addAndMakeVisible (soloButton);
        addAndMakeVisible (patternName); addAndMakeVisible (grid);

        mod.onClick = [this] {
            const auto current = processor.snapshot();
            const int cur = static_cast<int> (current->sequence.tracks[static_cast<size_t> (index)].modShape);
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setModShape;
            op.track = index;
            op.arg0 = (cur + 1) % static_cast<int> (sliderseq::ModShape::count);
            processor.applyUiOp (op);
            refresh();
        };

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

        for (const auto gridSize : { 16, 12, 32, 48, 64 }) grid.addItem (juce::String (gridSize), gridSize);

        slotAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "slot"), slot);
        velocityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "velocity"), velocity);
        grooveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "groove"), groove);
        modAmountAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.parameters(), SliderRhythmMidiProcessor::paramId (index, "mod_amt"), modAmount);

        slot.onValueChange = [this] { refresh(); };
        note.onValueChange = [this] {
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setMidiNote;
            op.track = index;
            op.arg0 = static_cast<int> (note.getValue());
            processor.applyUiOp (op);
        };
        channel.onValueChange = [this] {
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setMidiChannel;
            op.track = index;
            op.arg0 = static_cast<int> (channel.getValue());
            processor.applyUiOp (op);
        };
        length.onValueChange = [this] {
            const auto current = processor.snapshot();
            const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setPatternLength;
            op.track = index;
            op.slotIndex = t.slot;
            op.arg0 = static_cast<int> (length.getValue());
            processor.applyUiOp (op);
        };
        grid.onChange = [this] {
            const auto current = processor.snapshot();
            const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::resizePattern;
            op.track = index;
            op.slotIndex = t.slot;
            op.arg0 = grid.getSelectedId();
            processor.applyUiOp (op);
            refresh();
        };
        patternName.onClick = [this] { showPatternMenu(); };
        patternName.setTooltip ("Load a beat or save this slot as a preset");
        for (int i = 0; i < 64; ++i) {
            steps[static_cast<size_t> (i)] = std::make_unique<StepButton>();
            auto& button = *steps[static_cast<size_t> (i)];
            button.onClick = [this, i] {
                const auto current = processor.snapshot();
                const auto& t = current->sequence.tracks[static_cast<size_t> (index)];
                const bool bit = t.patterns[static_cast<size_t> (t.slot)].steps[static_cast<size_t> (i)];
                SliderRhythmMidiProcessor::StateOp op;
                op.kind = SliderRhythmMidiProcessor::OpKind::setStep;
                op.track = index;
                op.slotIndex = t.slot;
                op.arg0 = i;
                op.barg = ! bit;
                processor.applyUiOp (op);
                refresh();
            };
            addAndMakeVisible (button);
        }
        refresh();
    }

    ~TrackStrip() override {
        if (activeDialog != nullptr)
            activeDialog->exitModalState (0);
        slotAttachment.reset();
        velocityAttachment.reset();
        grooveAttachment.reset();
        modAmountAttachment.reset();
        slot.setLookAndFeel (nullptr);
        muteButton.setLookAndFeel (nullptr);
        soloButton.setLookAndFeel (nullptr);
        for (auto& button : rateButtons)
            button.setLookAndFeel (nullptr);
    }

    void updatePlayhead() {
        const int nowStep = processor.getActiveStep (index);
        if (nowStep == displayedPlayheadStep) return;
        if (displayedPlayheadStep >= 0 && displayedPlayheadStep < 64)
            steps[static_cast<size_t> (displayedPlayheadStep)]->setPlayheadActive (false);
        if (nowStep >= 0 && nowStep < 64)
            steps[static_cast<size_t> (nowStep)]->setPlayheadActive (true);
        displayedPlayheadStep = nowStep;
    }

    void refresh() {
        const auto state = processor.snapshot();
        const auto& track = state->sequence.tracks[static_cast<size_t> (index)];
        const auto& pattern = track.patterns[static_cast<size_t> (track.slot)];
        titleButton.setButtonText (juce::String (track.name) + " / " + juce::String (index + 1) + "  ▾");
        velocity.setValue (track.velocity, juce::dontSendNotification);
        groove.setValue (track.groove, juce::dontSendNotification);
        note.setValue (track.midiNote, juce::dontSendNotification);
        channel.setValue (track.midiChannel, juce::dontSendNotification);
        modAmount.setValue (track.modAmount, juce::dontSendNotification);
        const int activeLen = sliderseq::effectivePatternLength (pattern.length, pattern.grid, track.loopStepsOverride);
        lengthCaption.setText (track.loopStepsOverride > 0 ? "CAP " + juce::String (activeLen) : "LENGTH",
                               juce::dontSendNotification);
        length.setRange (1, std::max (2, pattern.grid), 1);
        length.setEnabled (pattern.grid > 1);
        length.setValue (pattern.length, juce::dontSendNotification);
        slot.setValue (track.slot + 1, juce::dontSendNotification);
        grid.setSelectedId (pattern.grid, juce::dontSendNotification);
        patternName.setButtonText (juce::String (track.slot + 1) + "  " + juce::String (pattern.name) + "  ▾");
        for (int i = 0; i < 5; ++i)
            rateButtons[static_cast<size_t> (i)].setColour (
                juce::TextButton::buttonColourId, std::abs (track.rate - rates[static_cast<size_t> (i)]) < 1e-6 ? orange : key);
        mod.setButtonText ("MOD: " + juce::String (sliderseq::modShapeLabel (track.modShape)));
        muteButton.setColour (juce::TextButton::buttonColourId, track.muted ? muteRed : key);
        soloButton.setColour (juce::TextButton::buttonColourId, track.solo ? soloAmber : key);
        for (int i = 0; i < 64; ++i) {
            auto& button = *steps[static_cast<size_t> (i)];
            button.setEnabled (i < pattern.grid);
            const bool enabled = pattern.steps[static_cast<size_t> (i)];
            const bool downbeat = i % std::max (1, pattern.subdivision / 4) == 0;
            button.setColour (juce::TextButton::buttonColourId, enabled ? orange : downbeat ? key.darker (.15f) : key);
            button.setAlpha (i >= activeLen ? .35f : 1.0f);
        }
        updatePlayhead();
    }

    void paint (juce::Graphics& g) override {
        g.setColour (panel); g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (3), 12);
        g.setColour (border); g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (3), 12, 1);
    }

    void resized() override {
        const int w = getWidth(), usable = w - 16;
        titleButton.setBounds (8, 8, usable, 28);
        const int knobWidth = usable / 2;
        velocityCaption.setBounds (8, 38, knobWidth, 17);
        grooveCaption.setBounds (8 + knobWidth, 38, usable - knobWidth, 17);
        velocity.setBounds (8 + (knobWidth - 80) / 2, 55, 80, 76);
        groove.setBounds (8 + knobWidth + (usable - knobWidth - 80) / 2, 55, 80, 76);
        slot.setBounds ((w - 96) / 2, 133, 96, 176);
        const int rateGap = 3, rateWidth = (usable - 4 * rateGap) / 5;
        for (int i = 0; i < 5; ++i)
            rateButtons[static_cast<size_t> (i)].setBounds (8 + i * (rateWidth + rateGap), 316, rateWidth, 28);
        mod.setBounds (8, 354, usable, 30);
        const int controlColumn = usable / 3;
        modCaption.setBounds (8, 389, controlColumn, 16);
        noteCaption.setBounds (8 + 2 * controlColumn, 389, usable - 2 * controlColumn, 16);
        modAmount.setBounds (8 + (controlColumn - 58) / 2, 405, 58, 68);
        const int msGap = 4;
        const int msW = std::max (24, (controlColumn - msGap) / 2);
        const int msX = 8 + controlColumn + (controlColumn - (2 * msW + msGap)) / 2;
        muteButton.setBounds (msX, 417, msW, 36);
        soloButton.setBounds (msX + msW + msGap, 417, msW, 36);
        note.setBounds (8 + 2 * controlColumn + (usable - 2 * controlColumn - 58) / 2, 405, 58, 68);
        channelCaption.setBounds (8, 477, controlColumn, 17);
        lengthCaption.setBounds (8 + controlColumn, 477, controlColumn, 17);
        channel.setBounds (8 + (controlColumn - 54) / 2, 495, 54, 64);
        length.setBounds (8 + controlColumn + (controlColumn - 54) / 2, 495, 54, 64);
        grid.setBounds (8 + 2 * controlColumn, 503, usable - 2 * controlColumn, 26);
        patternName.setBounds (8, 560, usable, 25);
        const int stepGap = 2, stepWidth = (usable - 15 * stepGap) / 16;
        for (int i = 0; i < 64; ++i)
            steps[static_cast<size_t> (i)]->setBounds (8 + (i % 16) * (stepWidth + stepGap), 594 + (i / 16) * 17, stepWidth, 14);
    }

private:
    bool intercept (SliderRhythmMidiProcessor::Control control) {
        return onControlLearn && onControlLearn (index, control);
    }
    void setTrack (const std::function<void(sliderseq::Track&)>& change) {
        processor.edit ([this, &change] (auto& s) { change (s.sequence.tracks[static_cast<size_t> (index)]); });
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
        const int targetSlot = track.slot;
        const auto& pattern = track.patterns[static_cast<size_t> (targetSlot)];
        const auto clipboard = onGetClipboard ? onGetClipboard() : std::nullopt;

        juce::PopupMenu menu;
        menu.addItem (10, "Rename Track...");
        menu.addItem (11, "Clear Current Slot Steps");
        menu.addItem (12, "Copy Current Slot");
        menu.addItem (13, "Paste Into Current Slot", clipboard.has_value());

        juce::PopupMenu dupMenu;
        for (int dst = 0; dst < sliderseq::maxTracks; ++dst) {
            if (dst == index) continue;
            const auto& dt = state->sequence.tracks[static_cast<size_t> (dst)];
            dupMenu.addItem (500 + dst, "To Track " + juce::String (dst + 1) + " (" + juce::String (dt.name) + ")");
        }
        menu.addSubMenu ("Duplicate Entire Track", dupMenu);
        menu.addSeparator();

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
        const int targetSlot = track.slot;
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
    int displayedPlayheadStep = -1;
    juce::SharedResourcePointer<SliderRhythmMidiEditor::LookAndFeelResources> lookAndFeel;
    juce::Component::SafePointer<juce::AlertWindow> activeDialog;
    juce::TextButton titleButton;
    juce::Label velocityCaption, grooveCaption, noteCaption, channelCaption, lengthCaption, modCaption;
    LearnableSlider velocity, groove, slot, note, channel, modAmount, length;
    std::array<LearnableButton, 5> rateButtons;
    LearnableButton mod { "MOD" }, muteButton { "M" }, soloButton { "S" };
    juce::TextButton patternName;
    LearnableComboBox grid;
    std::array<std::unique_ptr<StepButton>, 64> steps;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> slotAttachment, velocityAttachment, grooveAttachment, modAmountAttachment;
};

SliderRhythmMidiEditor::SliderRhythmMidiEditor (SliderRhythmMidiProcessor& owner)
    : AudioProcessorEditor (&owner), processor (owner) {
    setLookAndFeel (&lookAndFeel->main);

    for (auto* btn : { &addTrack, &removeTrack, &unmuteAll, &unsoloAll, &learn, &noteLearnToggle, &triggerModeToggle, &playbackMode }) {
        btn->setLookAndFeel (&lookAndFeel->compactButton);
        addAndMakeVisible (*btn);
    }
    unmuteAll.setTooltip ("Unmute all tracks");
    unsoloAll.setTooltip ("Clear solo on all tracks");
    noteLearnToggle.setTooltip ("Allow MIDI Note On messages (pads/keys) for MIDI Learn and control");
    triggerModeToggle.setTooltip ("Switch Learn trigger mode for M/S: Toggle (press flips) vs Hold (momentary)");
    playbackMode.setTooltip ("GEN: sequencer only | GEN + THRU: sequencer + live input | THRU: passthrough only");

    dragMidi = std::make_unique<MidiDragHandle>();
    dragMidi->getLabelText = [this] { return "DRAG " + juce::String (exportBars) + "B"; };
    dragMidi->onCreateFile = [this] () -> juce::File {
        const auto snap = processor.snapshot();
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SliderRhythmMidiClips");
        if (dir.createDirectory().failed()) return {};
        const auto file = dir.getChildFile ("SliderRhythm_" + juce::String (exportBars) + "bar_"
                                            + juce::String (juce::Time::currentTimeMillis()) + ".mid");
        if (! midiexporter::writeMidiFile (snap->sequence, file, exportBars, processor.getLastKnownBpm()))
            return {};
        return file;
    };
    dragMidi->onRightClick = [this] {
        juce::PopupMenu m;
        for (int b : { 1, 2, 4, 8 })
            m.addItem (b, "Export " + juce::String (b) + (b == 1 ? " Bar" : " Bars"), true, exportBars == b);
        auto sharedLnf = lookAndFeel;
        m.setLookAndFeel (&sharedLnf->main);
        juce::Component::SafePointer<SliderRhythmMidiEditor> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (dragMidi.get()),
                         [safe, sharedLnf] (int result) {
            if (safe == nullptr || result <= 0) return;
            safe->exportBars = result;
            if (safe->dragMidi) safe->dragMidi->repaint();
        });
    };
    addAndMakeVisible (*dragMidi);
    addAndMakeVisible (status);

    drumMap.addItem ("MAP: GM (Logic / Groove Agent / AD2 GM)", 1);
    drumMap.addItem ("MAP: 12 chromatic pads (editable)", 2);
    drumMap.addItem ("MAP: XLN Addictive Drums 2 Standard", 3);
    drumMap.addItem ("MAP: XLN XO 8 slots", 4);
    drumMap.addItem ("MAP: Ableton Drum Rack / Bitwig Drum Machine (16 pads)", 5);
    drumMap.setTextWhenNothingSelected ("LOAD MIDI MAP...");
    addAndMakeVisible (drumMap);
    drumMap.onChange = [this] {
        const int selected = drumMap.getSelectedId();
        if (selected == 0) return;
        processor.edit ([selected] (auto& s) {
            drummaps::apply (s.sequence, static_cast<drummaps::Preset> (selected - 1));
        });
        rebuildStrips();
    };

    addAndMakeVisible (viewport);
    viewport.setScrollBarsShown (true, true);
    viewport.setViewedComponent (&stripContent, false);

    addTrack.onClick = [this] {
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setTrackCount;
        op.arg0 = std::min (sliderseq::maxTracks, processor.snapshot()->sequence.trackCount + 1);
        processor.applyUiOp (op);
        timerCallback();
    };
    removeTrack.onClick = [this] {
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setTrackCount;
        op.arg0 = std::max (1, processor.snapshot()->sequence.trackCount - 1);
        processor.applyUiOp (op);
        timerCallback();
    };
    unmuteAll.onClick = [this] { processor.unmuteAllTracks(); timerCallback(); };
    unsoloAll.onClick = [this] { processor.unsoloAllTracks(); timerCallback(); };
    learn.onClick = [this] {
        if (choosingLearnTarget || processor.getLearnTarget() >= 0) {
            choosingLearnTarget = false;
            processor.cancelMidiLearn();
        } else {
            choosingLearnTarget = true;
        }
        timerCallback();
    };
    noteLearnToggle.onClick = [this] {
        processor.setNoteLearnEnabled (! processor.isNoteLearnEnabled());
        timerCallback();
    };
    triggerModeToggle.onClick = [this] {
        const auto next = processor.getLearnTriggerMode() == SliderRhythmMidiProcessor::TriggerMode::toggle
            ? SliderRhythmMidiProcessor::TriggerMode::momentary
            : SliderRhythmMidiProcessor::TriggerMode::toggle;
        processor.setLearnTriggerMode (next);
        timerCallback();
    };
    playbackMode.onClick = [this] {
        const auto current = processor.snapshot()->playbackMode;
        const int next = (static_cast<int> (current) + 1) % 3;
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setPlaybackMode;
        op.arg0 = next;
        processor.applyUiOp (op);
        timerCallback();
    };

    setResizable (true, true);
    setResizeLimits (minWindowWidth, 420, maxWindowWidth, 1000);
    rebuildStrips();
    startTimerHz (60);
}

SliderRhythmMidiEditor::~SliderRhythmMidiEditor() {
    stopTimer();
    juce::PopupMenu::dismissAllActiveMenus();
    for (auto* btn : { &addTrack, &removeTrack, &unmuteAll, &unsoloAll, &learn, &noteLearnToggle, &triggerModeToggle, &playbackMode })
        btn->setLookAndFeel (nullptr);
    strips.clear();
    stripContent.removeAllChildren();
    viewport.setViewedComponent (nullptr, false);
    setLookAndFeel (nullptr);
}

void SliderRhythmMidiEditor::rebuildStrips() {
    stripContent.removeAllChildren();
    strips.clear();
    const bool firstLayout = shownTrackCount == 0;
    shownTrackCount = processor.snapshot()->sequence.trackCount;
    for (int i = 0; i < shownTrackCount; ++i) {
        auto strip = std::make_unique<TrackStrip> (processor, i);
        strip->onControlLearn = [this] (int track, SliderRhythmMidiProcessor::Control control) {
            if (! choosingLearnTarget && processor.getLearnTarget() < 0) return false;
            choosingLearnTarget = false;
            processor.armMidiLearn (track, control);
            timerCallback();
            return true;
        };
        strip->onCopyPattern = [this] (const sliderseq::Pattern& p) { patternClipboard = p; };
        strip->onGetClipboard = [this] { return patternClipboard; };
        strip->onTracksModified = [this] {
            lastSeenVersion = 0;
            timerCallback();
        };
        stripContent.addAndMakeVisible (*strip);
        strips.push_back (std::move (strip));
    }
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* display = firstLayout ? displays.getPrimaryDisplay()
                                      : displays.getDisplayForRect (getScreenBounds());
    const int widthLimit = display != nullptr
        ? std::clamp (static_cast<int> (display->userBounds.getWidth()) - 32, minWindowWidth, maxWindowWidth)
        : maxWindowWidth;
    const int width = std::clamp (shownTrackCount * trackWidth + windowGutter,
                                  minWindowWidth, widthLimit);
    const int height = firstLayout ? trackHeight + windowChromeHeight : getHeight();
    setSize (width, height);
    resized();
    lastSeenVersion = processor.stateVersion();
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

    bool anyMuted = false, anySolo = false;
    for (int i = 0; i < state->sequence.trackCount; ++i) {
        anyMuted = anyMuted || state->sequence.tracks[static_cast<size_t> (i)].muted;
        anySolo = anySolo || state->sequence.tracks[static_cast<size_t> (i)].solo;
    }
    unmuteAll.setColour (juce::TextButton::buttonColourId, anyMuted ? muteRed : key);
    unsoloAll.setColour (juce::TextButton::buttonColourId, anySolo ? soloAmber : key);

    const bool noteOn = processor.isNoteLearnEnabled();
    noteLearnToggle.setColour (juce::TextButton::buttonColourId, noteOn ? orange : key);
    const bool isToggle = processor.getLearnTriggerMode() == SliderRhythmMidiProcessor::TriggerMode::toggle;
    triggerModeToggle.setButtonText (isToggle ? "TOGGLE" : "HOLD");
    triggerModeToggle.setColour (juce::TextButton::buttonColourId, isToggle ? key : soloAmber);

    const int armedTarget = processor.getLearnTarget();
    const bool armed = armedTarget >= 0;
    const juce::String learnedControl = armed ? controlNames[static_cast<size_t> (armedTarget % SliderRhythmMidiProcessor::controlCount)] : "";
    const juce::String transport = processor.isHostRecording() ? "HOST REC" : processor.isHostPlaying() ? "HOST PLAY" : processor.hasHostPosition() ? "HOST STOP" : "Waiting transport";
    const juce::String learnHint = noteOn ? "move CC or hit Note" : "move a MIDI CC";
    status.setText (
        choosingLearnTarget ? "Click a control on any track"
        : armed ? "T" + juce::String (armedTarget / SliderRhythmMidiProcessor::controlCount + 1) + " / " + learnedControl + ": " + learnHint
        : state->playbackMode == sliderseq::PlaybackMode::thru ? "MIDI THRU (recorded/live passthrough)"
        : transport + " | OUT " + juce::String (processor.generatedNoteCount()),
        juce::dontSendNotification);

    learn.setButtonText (choosingLearnTarget || armed ? "CANCEL" : "MIDI LEARN");
    learn.setColour (juce::TextButton::buttonColourId, choosingLearnTarget || armed ? orange : key);

    playbackMode.setButtonText (playbackModeLabel (state->playbackMode));
    playbackMode.setColour (juce::TextButton::buttonColourId,
                            state->playbackMode == sliderseq::PlaybackMode::thru ? orange
                            : state->playbackMode == sliderseq::PlaybackMode::genPlusThru ? soloAmber : key);
}

void SliderRhythmMidiEditor::paint (juce::Graphics& g) {
    g.fillAll (chassis);
    g.setColour (orange);
    g.fillRect (12, 70, getWidth() - 24, 3);
    g.setColour (ink);
    g.setFont (juce::FontOptions (21.0f, juce::Font::bold));
    g.drawFittedText (juce::String (JucePlugin_Name).toUpperCase(), 16, 8, 255, 32,
                      juce::Justification::centredLeft, 1, 0.7f);
    g.setFont (juce::FontOptions (11.5f));
    g.drawText ("AU MIDI FX  |  VST3 MIDI Out", 18, 41, 255, 22, juce::Justification::centredLeft);
}

void SliderRhythmMidiEditor::resized() {
    addTrack.setBounds (278, 11, 56, 26);
    removeTrack.setBounds (338, 11, 56, 26);
    unmuteAll.setBounds (402, 11, 68, 26);
    unsoloAll.setBounds (474, 11, 68, 26);
    learn.setBounds (550, 11, 96, 26);
    noteLearnToggle.setBounds (650, 11, 52, 26);
    triggerModeToggle.setBounds (706, 11, 62, 26);
    playbackMode.setBounds (776, 11, 96, 26);
    if (dragMidi) dragMidi->setBounds (878, 11, 90, 26);

    drumMap.setBounds (278, 42, 320, 22);
    status.setBounds (606, 42, std::max (120, getWidth() - 618), 22);
    viewport.setBounds (windowGutter / 2, 77, getWidth() - windowGutter, getHeight() - windowChromeHeight);

    if (shownTrackCount == 0)
        return;

    int available = viewport.getWidth();
    if (viewport.getHeight() < trackHeight)
        available -= viewport.getScrollBarThickness();
    const int stripWidth = std::max (190, std::min (trackWidth, available / shownTrackCount));
    stripContent.setSize (shownTrackCount * stripWidth, trackHeight);
    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (static_cast<int> (i) * stripWidth, 0, stripWidth, trackHeight);
}
