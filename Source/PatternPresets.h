#pragma once

#include <juce_core/juce_core.h>
#include "SequencerEngine.h"

namespace patternpresets {

inline juce::File folder() {
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Slider Rhythm MIDI").getChildFile ("Pattern Presets");
}

inline juce::File trackFolder (const juce::String& trackName, int trackIndex,
                               const juce::File& directory = folder()) {
    const auto name = juce::File::createLegalFileName (trackName.trim()).trimCharactersAtStart (".");
    return directory.getChildFile (name.isNotEmpty() ? name : "TRACK " + juce::String (trackIndex + 1));
}

inline std::vector<sliderseq::Pattern> load (const juce::File& directory = folder()) {
    std::vector<sliderseq::Pattern> presets;
    for (const auto& file : directory.findChildFiles (juce::File::findFiles, false, "*.xml")) {
        const auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr || ! xml->hasTagName ("SliderRhythmPattern"))
            continue;

        const int version = xml->getIntAttribute ("version");
        if (version != 1 && version != 2)
            continue;
        const int grid = xml->getIntAttribute ("grid");
        const int length = xml->getIntAttribute ("length");
        const auto steps = xml->getStringAttribute ("steps");
        const auto name = xml->getStringAttribute ("name");
        const int subdivision = xml->getIntAttribute ("subdivision", grid);
        if (grid < 1 || grid > 64 || length < 1 || length > (version == 1 ? 16 : grid)
            || subdivision < 1 || subdivision > 64
            || steps.length() != 64 || ! steps.containsOnly ("01") || name.isEmpty())
            continue;

        sliderseq::Pattern preset { name.toStdString(), grid,
            version == 1 ? sliderseq::legacyStepCount (grid, length) : length, {}, subdivision };
        for (int i = 0; i < steps.length(); ++i)
            preset.steps[static_cast<size_t> (i)] = steps[i] == '1';
        presets.push_back (std::move (preset));
    }
    std::sort (presets.begin(), presets.end(), [] (const auto& a, const auto& b) {
        return juce::String (a.name).compareNatural (juce::String (b.name)) < 0;
    });
    return presets;
}

inline bool save (const sliderseq::Pattern& preset, const juce::File& directory = folder()) {
    if (directory.createDirectory().failed())
        return false;

    juce::XmlElement xml ("SliderRhythmPattern");
    xml.setAttribute ("version", 2);
    xml.setAttribute ("subdivision", preset.subdivision);
    xml.setAttribute ("name", juce::String (preset.name));
    xml.setAttribute ("grid", preset.grid);
    xml.setAttribute ("length", preset.length);
    juce::String steps;
    for (const auto enabled : preset.steps)
        steps += enabled ? "1" : "0";
    xml.setAttribute ("steps", steps);
    return directory.getChildFile (juce::Uuid().toString() + ".xml").replaceWithText (xml.toString());
}

} // namespace patternpresets
