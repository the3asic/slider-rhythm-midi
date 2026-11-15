#pragma once

#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

class SliderRhythmMidiEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit SliderRhythmMidiEditor (SliderRhythmMidiProcessor&);
    ~SliderRhythmMidiEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class TrackStrip;
    void timerCallback() override;
    void rebuildStrips();

    SliderRhythmMidiProcessor& processor;
    juce::TextButton addTrack { "+ TRACK" }, removeTrack { "- TRACK" }, learn { "MIDI LEARN" }, playbackMode { "LIVE GENERATE" };
    juce::Label status;
    juce::ComboBox drumMap;
    juce::Component stripContent;
    juce::Viewport viewport;
    std::vector<std::unique_ptr<TrackStrip>> strips;
    int shownTrackCount = 0;
    bool choosingLearnTarget = false;
};
