#include "PluginEditor.h"
#include "DrumMaps.h"

namespace {
const juce::Colour background { 0xff0d1014 }, panel { 0xff1b2229 }, brass { 0xffd6c195 }, blue { 0xff58b9e5 };
const std::array<double, 5> rates { 1.0 / 3.0, .5, 1.0, 2.0, 3.0 };
const std::array<const char*, 5> rateLabels { "1/3", "1/2", "1x", "2x", "3x" };
const std::array<const char*, SliderRhythmMidiProcessor::controlCount> controlNames {
    "Pattern fader", "DIV", "Velocity", "Shuffle/Swing", "MOD amount", "Mute", "MOD type",
    "MIDI note", "MIDI channel", "Pattern length", "Step grid" };
class CompactButtonLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    juce::Font getTextButtonFont (juce::TextButton&, int) override {
        return juce::Font (juce::FontOptions (11.0f, juce::Font::bold));
    }
};
CompactButtonLookAndFeel compactButtonLookAndFeel;

class PatternFaderLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float, const juce::Slider::SliderStyle,
                           juce::Slider&) override {
        const auto left = static_cast<float> (x);
        const auto top = static_cast<float> (y);
        const auto centre = left + width * .5f;
        const auto travelTop = top + 9.0f;
        const auto travelBottom = top + height - 9.0f;

        g.setColour (juce::Colour (0xff0b0e12));
        g.fillRoundedRectangle (centre - 5.5f, top, 11.0f, static_cast<float> (height), 4.0f);
        g.setColour (juce::Colour (0xff606b75));
        g.drawRoundedRectangle (centre - 5.5f, top, 11.0f, static_cast<float> (height), 4.0f, 1.0f);

        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        for (int mark = 0; mark < 12; ++mark) {
            const auto markY = travelTop + (travelBottom - travelTop) * mark / 11.0f;
            const bool fill = mark < 3;
            g.setColour (fill ? juce::Colour (0xff58b9e5) : juce::Colour (0xff78838d));
            g.drawText (juce::String (12 - mark), x + 1, static_cast<int> (markY - 7), 18, 14, juce::Justification::centredRight);
            g.drawHorizontalLine (static_cast<int> (std::round (markY)), centre - 20.0f, centre - 10.0f);
            g.drawHorizontalLine (static_cast<int> (std::round (markY)), centre + 10.0f, centre + 20.0f);
        }

        const auto capY = juce::jlimit (top + 8.5f, top + height - 8.5f, sliderPos);
        const juce::Rectangle<float> cap (centre - 28.0f, capY - 8.5f, 56.0f, 17.0f);
        g.setColour (juce::Colours::black.withAlpha (.6f));
        g.fillRoundedRectangle (cap.translated (0, 3), 3.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffd6d9d2), cap.getTopLeft(),
                                                 juce::Colour (0xff777f80), cap.getBottomLeft(), false));
        g.fillRoundedRectangle (cap, 3.0f);
        g.setColour (juce::Colour (0xffb7bdb5));
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);
        g.setColour (juce::Colour (0xff626c6e).withAlpha (.85f));
        for (int ridge = 4; ridge <= 12; ridge += 4)
            g.drawHorizontalLine (static_cast<int> (cap.getY()) + ridge, cap.getX() + 5, cap.getRight() - 5);
    }
};
PatternFaderLookAndFeel patternFaderLookAndFeel;

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
    void mouseDown (const juce::MouseEvent& event) override {
        learningPress = interceptLearn && interceptLearn();
        if (! learningPress) juce::TextButton::mouseDown (event);
    }
    void mouseUp (const juce::MouseEvent& event) override {
        if (! learningPress) juce::TextButton::mouseUp (event);
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

void label (juce::Label& component, const juce::String& text, juce::Component& parent) {
    component.setText (text, juce::dontSendNotification);
    component.setColour (juce::Label::textColourId, brass);
    component.setJustificationType (juce::Justification::centred);
    parent.addAndMakeVisible (component);
}

void rotary (juce::Slider& slider, double minimum, double maximum, double initial, juce::Component& parent) {
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 48, 18);
    slider.setRange (minimum, maximum, 1);
    slider.setValue (initial, juce::dontSendNotification);
    slider.setColour (juce::Slider::rotarySliderFillColourId, brass);
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colours::darkgrey);
    slider.setColour (juce::Slider::thumbColourId, blue);
    parent.addAndMakeVisible (slider);
}
}

class SliderRhythmMidiEditor::TrackStrip final : public juce::Component {
public:
    std::function<bool(int, SliderRhythmMidiProcessor::Control)> onControlLearn;
    class HoldButton final : public LearnableButton, private juce::Timer {
    public:
        std::function<void()> shortPress, longPress;
        HoldButton() : LearnableButton ("M / S") {
            onClick = [this] { if (! held && shortPress) shortPress(); held = false; };
        }
        void mouseDown (const juce::MouseEvent& event) override {
            held = false;
            LearnableButton::mouseDown (event);
            if (! isLearningPress() && event.mods.isLeftButtonDown()) startTimer (1500);
        }
        void mouseUp (const juce::MouseEvent& event) override {
            stopTimer();
            LearnableButton::mouseUp (event);
        }
    private:
        void timerCallback() override { stopTimer(); held = true; if (longPress) longPress(); }
        bool held = false;
    };

    TrackStrip (SliderRhythmMidiProcessor& owner, int number) : processor (owner), index (number) {
        label (title, "", *this); label (velocityCaption, "VELOCITY", *this); label (grooveCaption, "SHUFFLE / SWING", *this);
        label (noteCaption, "MIDI NOTE", *this); label (channelCaption, "CH", *this);
        label (lengthCaption, "LENGTH", *this); label (modCaption, "MOD AMT", *this);
        rotary (velocity, 1, 127, 100, *this); rotary (groove, 0, 100, 50, *this);
        rotary (note, 0, 127, 36, *this); rotary (channel, 1, 16, 10, *this);
        rotary (modAmount, 0, 100, 50, *this); rotary (length, 1, 16, 16, *this);

        slot.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::slot); };
        velocity.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::velocity); };
        groove.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::groove); };
        note.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::note); };
        channel.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::channel); };
        modAmount.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::modAmount); };
        length.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::length); };
        grid.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::grid); };
        mod.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::modMode); };
        muteSolo.interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::mute); };

        slot.setSliderStyle (juce::Slider::LinearVertical);
        slot.setLookAndFeel (&patternFaderLookAndFeel);
        slot.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 18);
        slot.setRange (1, 12, 1);slot.setColour (juce::Slider::thumbColourId, blue);
        addAndMakeVisible (slot);
        for (int i = 0; i < 5; ++i) {
            rateButtons[static_cast<size_t> (i)].setButtonText (rateLabels[static_cast<size_t> (i)]);
            rateButtons[static_cast<size_t> (i)].setLookAndFeel (&compactButtonLookAndFeel);
            rateButtons[static_cast<size_t> (i)].interceptLearn = [this] { return intercept (SliderRhythmMidiProcessor::rate); };
            addAndMakeVisible (rateButtons[static_cast<size_t> (i)]);
            rateButtons[static_cast<size_t> (i)].onClick = [this, i] {
                processor.edit ([this, i] (auto& s) { s.sequence.tracks[static_cast<size_t> (index)].rate = rates[static_cast<size_t> (i)]; });
                refresh();
            };
        }
        addAndMakeVisible (mod);addAndMakeVisible (muteSolo);addAndMakeVisible (patternName);addAndMakeVisible (grid);
        mod.onClick = [this] { processor.edit ([this] (auto& s) { auto& t=s.sequence.tracks[static_cast<size_t> (index)];t.modMode=(t.modMode+1)%3; });refresh(); };
        muteSolo.shortPress = [this] { processor.edit ([this] (auto& s) { auto& t=s.sequence.tracks[static_cast<size_t> (index)];if(t.solo)t.solo=false;else t.muted=!t.muted; });refresh(); };
        muteSolo.longPress = [this] { processor.edit ([this] (auto& s) { for(auto& t:s.sequence.tracks)t.solo=false;auto& t=s.sequence.tracks[static_cast<size_t> (index)];t.solo=true;t.muted=false; });refresh(); };
        for (const auto gridSize : { 16, 12, 32, 48, 64 }) grid.addItem (juce::String (gridSize), gridSize);

        slot.onValueChange = [this] { processor.edit ([this] (auto& s) { s.sequence.tracks[static_cast<size_t> (index)].slot=static_cast<int> (slot.getValue())-1; });refresh(); };
        velocity.onValueChange = [this] { setTrack ([this] (auto& t) { t.velocity=static_cast<int> (velocity.getValue()); }); };
        groove.onValueChange = [this] { setTrack ([this] (auto& t) { t.groove=static_cast<int> (groove.getValue()); }); };
        note.onValueChange = [this] { setTrack ([this] (auto& t) { t.midiNote=static_cast<int> (note.getValue()); }); };
        channel.onValueChange = [this] { setTrack ([this] (auto& t) { t.midiChannel=static_cast<int> (channel.getValue()); }); };
        modAmount.onValueChange = [this] { setTrack ([this] (auto& t) { t.modAmount=static_cast<int> (modAmount.getValue()); }); };
        length.onValueChange = [this] { setTrack ([this] (auto& t) { t.patterns[static_cast<size_t> (t.slot)].length=static_cast<int> (length.getValue()); }); };
        grid.onChange = [this] { setTrack ([this] (auto& t) { t.patterns[static_cast<size_t> (t.slot)].grid=grid.getSelectedId(); });refresh(); };
        patternName.onReturnKey = [this] { savePatternName(); };
        patternName.onFocusLost = [this] { savePatternName(); };
        for (int i=0;i<64;++i) {
            steps[static_cast<size_t> (i)] = std::make_unique<juce::TextButton> ("");
            auto& button = *steps[static_cast<size_t> (i)];
            button.setClickingTogglesState (false);
            button.onClick = [this,i] {
                setTrack ([i] (auto& t) { auto& bit=t.patterns[static_cast<size_t> (t.slot)].steps[static_cast<size_t> (i)];bit=!bit; });
                refresh();
            };
            addAndMakeVisible (button);
        }
        refresh();
    }

    void refresh() {
        const auto state = processor.snapshot();
        const auto& track = state->sequence.tracks[static_cast<size_t> (index)];
        const auto& pattern = track.patterns[static_cast<size_t> (track.slot)];
        title.setText (juce::String (track.name) + " / " + juce::String (index+1), juce::dontSendNotification);
        velocity.setValue (track.velocity, juce::dontSendNotification);groove.setValue (track.groove, juce::dontSendNotification);
        note.setValue (track.midiNote, juce::dontSendNotification);channel.setValue (track.midiChannel, juce::dontSendNotification);
        modAmount.setValue (track.modAmount, juce::dontSendNotification);length.setValue (pattern.length, juce::dontSendNotification);
        slot.setValue (track.slot+1, juce::dontSendNotification);grid.setSelectedId (pattern.grid, juce::dontSendNotification);
        if (! patternName.hasKeyboardFocus (true)) patternName.setText (juce::String (pattern.name), false);
        for (int i=0;i<5;++i) rateButtons[static_cast<size_t> (i)].setColour (juce::TextButton::buttonColourId, std::abs(track.rate-rates[static_cast<size_t> (i)])<1e-6 ? blue : juce::Colours::darkslategrey);
        mod.setButtonText ("MOD: " + juce::String (track.modMode==2?"RANDOM":(index<2?(track.modMode==0?"RAMP UP":"RAMP DOWN"):(track.modMode==0?"8TH":"16TH"))));
        muteSolo.setButtonText (track.solo ? "SOLO" : track.muted ? "MUTE" : "M / S");
        muteSolo.setColour (juce::TextButton::buttonColourId, track.solo ? juce::Colours::goldenrod : track.muted ? juce::Colours::firebrick : juce::Colours::darkslategrey);
        for (int i=0;i<64;++i) {
            auto& button=*steps[static_cast<size_t> (i)];
            button.setEnabled (i<pattern.grid);
            button.setColour (juce::TextButton::buttonColourId, pattern.steps[static_cast<size_t> (i)] ? blue : juce::Colours::darkslategrey);
            button.setAlpha (i*16/pattern.grid >= pattern.length ? .35f : 1.0f);
        }
    }

    void paint (juce::Graphics& g) override {
        g.setColour (panel);g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (3), 12);
        g.setColour (brass.withAlpha (.5f));g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (3), 12, 1);
    }

    void resized() override {
        const int w = getWidth(), usable = w - 16;
        title.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        title.setBounds (8, 8, usable, 28);
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
        muteSolo.setBounds (8 + controlColumn + (controlColumn - 54) / 2, 417, 54, 36);
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
        processor.edit ([this,&change] (auto& s) { change (s.sequence.tracks[static_cast<size_t> (index)]); });
    }
    void savePatternName() {
        const auto value=patternName.getText().trim().substring (0,32).toStdString();
        if (value.empty()) return;
        setTrack ([&] (auto& t) { t.patterns[static_cast<size_t> (t.slot)].name=value; });
    }
    SliderRhythmMidiProcessor& processor;
    int index;
    juce::Label title,velocityCaption,grooveCaption,noteCaption,channelCaption,lengthCaption,modCaption;
    LearnableSlider velocity,groove,slot,note,channel,modAmount,length;
    std::array<LearnableButton,5> rateButtons;
    LearnableButton mod {"MOD"};
    HoldButton muteSolo;
    juce::TextEditor patternName;
    LearnableComboBox grid;
    std::array<std::unique_ptr<juce::TextButton>,64> steps;
};

SliderRhythmMidiEditor::SliderRhythmMidiEditor (SliderRhythmMidiProcessor& owner)
    : AudioProcessorEditor (&owner), processor (owner) {
    setSize (1180,770);
    addAndMakeVisible (addTrack);addAndMakeVisible (removeTrack);addAndMakeVisible (learn);addAndMakeVisible (playbackMode);
    addAndMakeVisible (status);
    drumMap.addItem ("MAP: GM (Logic / Groove Agent / AD2 GM)", 1);
    drumMap.addItem ("MAP: 12 chromatic pads (editable)", 2);
    drumMap.addItem ("MAP: XLN Addictive Drums 2 Standard", 3);
    drumMap.addItem ("MAP: XLN XO 8 slots", 4);
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
    addAndMakeVisible (viewport);viewport.setScrollBarsShown (false, true);viewport.setViewedComponent (&stripContent, false);
    addTrack.onClick = [this] { processor.edit ([] (auto& s) { s.sequence.trackCount=std::min (sliderseq::maxTracks,s.sequence.trackCount+1); });rebuildStrips(); };
    removeTrack.onClick = [this] { processor.edit ([] (auto& s) { s.sequence.trackCount=std::max (1,s.sequence.trackCount-1); });rebuildStrips(); };
    learn.onClick = [this] {
        if (choosingLearnTarget || processor.getLearnTarget() >= 0) {
            choosingLearnTarget = false;
            processor.cancelMidiLearn();
        } else {
            choosingLearnTarget = true;
        }
        timerCallback();
    };
    playbackMode.onClick = [this] {
        processor.edit ([] (auto& s) { s.playRecordedMidi = ! s.playRecordedMidi; });
        timerCallback();
    };
    rebuildStrips();startTimerHz (8);
}

SliderRhythmMidiEditor::~SliderRhythmMidiEditor() {stopTimer();viewport.setViewedComponent (nullptr,false);}

void SliderRhythmMidiEditor::rebuildStrips() {
    stripContent.removeAllChildren();strips.clear();
    shownTrackCount=processor.snapshot()->sequence.trackCount;
    const int available = getWidth() - 24;
    const int preferred = 320 - 20 * shownTrackCount;
    const int stripWidth = std::max (190, std::min ({ 300, preferred, available / shownTrackCount }));
    stripContent.setSize (shownTrackCount * stripWidth, 665);
    for (int i=0;i<shownTrackCount;++i) {
        auto strip=std::make_unique<TrackStrip> (processor,i);
        strip->onControlLearn = [this] (int track, SliderRhythmMidiProcessor::Control control) {
            if (! choosingLearnTarget && processor.getLearnTarget() < 0) return false;
            choosingLearnTarget = false;
            processor.armMidiLearn (track, control);
            timerCallback();
            return true;
        };
        strip->setBounds (i * stripWidth, 0, stripWidth, 665);
        stripContent.addAndMakeVisible (*strip);strips.push_back (std::move (strip));
    }
    resized();
}

void SliderRhythmMidiEditor::timerCallback() {
    const auto state = processor.snapshot();
    if (shownTrackCount!=state->sequence.trackCount)rebuildStrips();
    for (auto& strip:strips)strip->refresh();
    const int armedTarget = processor.getLearnTarget();
    const bool armed = armedTarget >= 0;
    const juce::String learnedControl = armed ? controlNames[static_cast<size_t> (armedTarget % SliderRhythmMidiProcessor::controlCount)] : "";
    const juce::String transport = processor.isHostRecording() ? "HOST RECORDING" : processor.isHostPlaying() ? "HOST PLAYING" : processor.hasHostPosition() ? "HOST STOPPED" : "Waiting for host transport";
    status.setText (choosingLearnTarget ? "Click a control on any track" : armed ? "Track " + juce::String (armedTarget / SliderRhythmMidiProcessor::controlCount + 1) + " / " + learnedControl + ": move a MIDI CC" : state->playRecordedMidi ? "PLAY RECORDED MIDI" : transport + " | OUT " + juce::String (processor.generatedNoteCount()), juce::dontSendNotification);
    learn.setButtonText (choosingLearnTarget || armed ? "CANCEL LEARN" : "MIDI LEARN");
    learn.setColour (juce::TextButton::buttonColourId, choosingLearnTarget || armed ? blue : juce::Colours::darkslategrey);
    playbackMode.setButtonText (state->playRecordedMidi ? "PLAY RECORDED MIDI" : "LIVE GENERATE");
    playbackMode.setColour (juce::TextButton::buttonColourId, state->playRecordedMidi ? blue : juce::Colours::darkslategrey);
}

void SliderRhythmMidiEditor::paint (juce::Graphics& g) {
    g.fillAll (background);
    g.setColour (brass);g.setFont (juce::FontOptions (24.0f,juce::Font::bold));
    g.drawText ("SLIDER RHYTHM MIDI",18,8,350,36,juce::Justification::centredLeft);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("AU MIDI FX / Logic  |  VST3 MIDI OUT / Live",20,43,350,22,juce::Justification::centredLeft);
}

void SliderRhythmMidiEditor::resized() {
    auto top=getLocalBounds().removeFromTop (75);
    addTrack.setBounds (380,14,95,28);removeTrack.setBounds (480,14,95,28);
    learn.setBounds (590,14,165,28);
    playbackMode.setBounds (765,14,205,28);
    drumMap.setBounds (380,47,355,21);
    status.setBounds (745,47,420,21);
    viewport.setBounds (12,77,getWidth()-24,getHeight()-89);
    juce::ignoreUnused (top);
}
