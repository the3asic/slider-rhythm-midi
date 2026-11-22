#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>

namespace uiprefs {

struct Settings {
    bool darkMode = false;
    int scalePercent = 100;             // 75, 85, 100, 125
    bool verticalWheelControls = true;  // Vertical scroll wheel adjusts knobs/faders/boxes
    bool horizontalWheelControls = false; // Horizontal scroll wheel adjusts knobs/faders/boxes (default OFF for trackpad swipe)
};

inline juce::File settingsFile() {
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
    return s;
}

inline bool save (const Settings& s, const juce::File& file = settingsFile()) {
    if (file.getParentDirectory().createDirectory().failed())
        return false;
    juce::XmlElement xml ("SliderRhythmUiPrefs");
    xml.setAttribute ("version", 1);
    xml.setAttribute ("darkMode", s.darkMode);
    xml.setAttribute ("scalePercent", clampScalePercent (s.scalePercent));
    xml.setAttribute ("verticalWheelControls", s.verticalWheelControls);
    xml.setAttribute ("horizontalWheelControls", s.horizontalWheelControls);
    return file.replaceWithText (xml.toString());
}

} // namespace uiprefs
