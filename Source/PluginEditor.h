#pragma once

#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>

class SliderRhythmMidiEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit SliderRhythmMidiEditor (SliderRhythmMidiProcessor&);
    ~SliderRhythmMidiEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class TrackStrip;
    class MidiDragHandle;
    struct LookAndFeelResources;
    void timerCallback() override;
    void rebuildStrips();

    SliderRhythmMidiProcessor& processor;
    juce::SharedResourcePointer<LookAndFeelResources> lookAndFeel;
    juce::TextButton addTrack { "+ TRK" }, removeTrack { "- TRK" };
    juce::TextButton unmuteAll { "UNMUTE" }, unsoloAll { "UNSOLO" };
    juce::TextButton learn { "MIDI LEARN" }, noteLearnToggle { "NOTE" }, triggerModeToggle { "TOGGLE" };
    juce::TextButton playbackMode { "GEN" };
    std::unique_ptr<MidiDragHandle> dragMidi;
    juce::Label status;
    juce::ComboBox drumMap;
    juce::Component stripContent;
    juce::Viewport viewport;
    std::vector<std::unique_ptr<TrackStrip>> strips;
    std::optional<sliderseq::Pattern> patternClipboard;
    int shownTrackCount = 0;
    int exportBars = 4;
    uint64_t lastSeenVersion = 0;
    bool choosingLearnTarget = false;
};
