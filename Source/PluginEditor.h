#pragma once

#include "BraunTheme.h"
#include "MappingPresets.h"
#include "PluginProcessor.h"
#include "UiPreferences.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include <vector>

class SliderRhythmMidiEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit SliderRhythmMidiEditor (SliderRhythmMidiProcessor&);
    ~SliderRhythmMidiEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class RootCanvas;
    class TrackStrip;
    class MidiDragHandle;
    class GlobalSwingScope;
    class VelocityModScope;
    class TrackSwingScope;
    struct LookAndFeelResources;

    void timerCallback() override;
    void rebuildStrips();
    void applyThemeAndScale (bool resizeWindow);
    void refreshHeaderState (const SliderRhythmMidiProcessor::Snapshot& state);
    void showMappingPresetMenu();
    void stepMappingPreset (int delta);
    void saveMappingPresetDialog (bool forceSaveAs);
    void showSettingsMenu();

    SliderRhythmMidiProcessor& processor;
    juce::SharedResourcePointer<LookAndFeelResources> lookAndFeel;
    uiprefs::Settings uiPrefs;
    juce::Component::SafePointer<juce::AlertWindow> activeDialog;

    std::unique_ptr<RootCanvas> rootCanvas;
    juce::Component stripContent;
    juce::Viewport viewport;
    std::vector<std::unique_ptr<TrackStrip>> strips;
    std::optional<sliderseq::Pattern> patternClipboard;

    int shownTrackCount = 0;
    int exportBars = 4;
    uint64_t lastSeenVersion = 0;
    bool choosingLearnTarget = false;
};
