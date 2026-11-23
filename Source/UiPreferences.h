#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cstdlib>
#include "SequencerEngine.h"

namespace uiprefs {

struct Settings {
    bool darkMode = false;
    int scalePercent = 100;               // 75, 85, 100, 125
    bool verticalWheelControls = true;    // Vertical scroll wheel adjusts knobs/faders/boxes
    bool horizontalWheelControls = false; // Horizontal scroll wheel adjusts knobs/faders/boxes (default OFF for trackpad swipe)
    sliderseq::InputChannelMatch inputChannelMatch = sliderseq::InputChannelMatch::autoLiveCh1;

    bool operator== (const Settings& o) const noexcept {
        return darkMode == o.darkMode
            && scalePercent == o.scalePercent
            && verticalWheelControls == o.verticalWheelControls
            && horizontalWheelControls == o.horizontalWheelControls
            && inputChannelMatch == o.inputChannelMatch;
    }
};

inline juce::File settingsFile() {
    if (const char* overridePath = std::getenv ("SLIDER_UI_PREFS_PATH"); overridePath != nullptr && overridePath[0] != '\0')
        return juce::File (juce::String::fromUTF8 (overridePath));
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Slider Rhythm MIDI").getChildFile ("ui_preferences.xml");
}

inline int clampScalePercent (int raw) noexcept {
    if (raw <= 80) return 75;
    if (raw <= 92) return 85;
    if (raw <= 112) return 100;
    return 125;
}

inline Settings load (const juce::File& file = settingsFile()) {
    Settings s;
    if (! file.existsAsFile())
        return s;
    const auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("SliderRhythmUiPrefs"))
        return s;
    s.darkMode = xml->getBoolAttribute ("darkMode", false);
    s.scalePercent = clampScalePercent (xml->getIntAttribute ("scalePercent", 100));
    s.verticalWheelControls = xml->getBoolAttribute ("verticalWheelControls", true);
    s.horizontalWheelControls = xml->getBoolAttribute ("horizontalWheelControls", false);
    s.inputChannelMatch = sliderseq::clampInputChannelMatch (xml->getIntAttribute ("inputChannelMatch", 0));
    return s;
}

inline bool save (const Settings& s, const juce::File& file = settingsFile()) {
    if (file.getParentDirectory().createDirectory().failed())
        return false;
    juce::XmlElement xml ("SliderRhythmUiPrefs");
    xml.setAttribute ("version", 2);
    xml.setAttribute ("darkMode", s.darkMode);
    xml.setAttribute ("scalePercent", clampScalePercent (s.scalePercent));
    xml.setAttribute ("verticalWheelControls", s.verticalWheelControls);
    xml.setAttribute ("horizontalWheelControls", s.horizontalWheelControls);
    xml.setAttribute ("inputChannelMatch", static_cast<int> (s.inputChannelMatch));
    return file.replaceWithText (xml.toString());
}

// Field-level read-modify-write so multiple open plugin instances never clobber each other's unrelated preference changes.
template <typename MutateFn>
inline Settings updateAndSave (MutateFn&& mutate, const juce::File& file = settingsFile()) {
    Settings latest = load (file);
    mutate (latest);
    save (latest, file);
    return latest;
}

} // namespace uiprefs
