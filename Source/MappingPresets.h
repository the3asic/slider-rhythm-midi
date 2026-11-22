#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
#include "DrumMaps.h"
#include "SequencerEngine.h"

namespace mappingpresets {

struct TrackMapping {
    std::string name = "TRACK";
    int midiNote = 36;
    int midiChannel = 10;
    sliderseq::ModShape modShape = sliderseq::ModShape::rampUp;
    double gateTicks = 24.0;
    uint16_t chokeMask = 0;
    int activeZones = 12;
    int groove = 100;

    bool operator== (const TrackMapping& o) const noexcept {
        return name == o.name
            && midiNote == o.midiNote
            && midiChannel == o.midiChannel
            && modShape == o.modShape
            && std::abs (gateTicks - o.gateTicks) < 0.5
            && chokeMask == o.chokeMask
            && activeZones == o.activeZones
            && groove == o.groove;
    }
};

struct Preset {
    std::string id;       // Stable identifier ("factory:gm", "user:<uuid>", etc.)
    std::string name;     // Display name
    bool isFactory = false;
    int trackCount = 4;
    std::array<TrackMapping, sliderseq::maxTracks> tracks {};
    juce::File file;      // Populated for user presets
};

inline juce::File folder() {
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Slider Rhythm MIDI").getChildFile ("Mapping Presets");
}

inline Preset captureFromState (const sliderseq::State& state, std::string id, std::string name, bool isFactory = false) {
    Preset p;
    p.id = std::move (id);
    p.name = std::move (name);
    p.isFactory = isFactory;
    p.trackCount = std::clamp (state.trackCount, 1, sliderseq::maxTracks);
    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& t = state.tracks[static_cast<size_t> (i)];
        p.tracks[static_cast<size_t> (i)] = {
            t.name, t.midiNote, t.midiChannel, t.modShape, t.gateTicks, t.chokeMask, t.activeZones, t.groove
        };
    }
    return p;
}

inline void applyToState (const Preset& preset, sliderseq::State& state) {
    state.trackCount = std::clamp (preset.trackCount, 1, sliderseq::maxTracks);
    for (int i = 0; i < state.trackCount; ++i) {
        const auto& m = preset.tracks[static_cast<size_t> (i)];
        auto& t = state.tracks[static_cast<size_t> (i)];
        t.name = m.name;
        t.midiNote = std::clamp (m.midiNote, 0, 127);
        t.midiChannel = std::clamp (m.midiChannel, 1, 16);
        t.modShape = m.modShape;
        t.gateTicks = std::clamp (m.gateTicks, 6.0, 288.0);
        t.chokeMask = static_cast<uint16_t> (m.chokeMask & ~(1u << static_cast<unsigned> (i)));
        t.activeZones = std::clamp (m.activeZones, 1, sliderseq::slotsPerTrack);
        t.slot = std::min (t.slot, t.activeZones);
        t.groove = std::clamp (m.groove, 0, 100);
    }
}

inline bool matchesState (const Preset& preset, const sliderseq::State& state) noexcept {
    if (preset.trackCount != state.trackCount)
        return false;
    for (int i = 0; i < preset.trackCount; ++i) {
        const auto& m = preset.tracks[static_cast<size_t> (i)];
        const auto& t = state.tracks[static_cast<size_t> (i)];
        const TrackMapping cur {
            t.name, t.midiNote, t.midiChannel, t.modShape, t.gateTicks, t.chokeMask, t.activeZones, t.groove
        };
        if (! (cur == m))
            return false;
    }
    return true;
}

inline Preset makeFactoryPreset (const char* id, const char* name, drummaps::Preset dm) {
    auto state = sliderseq::makeDefaultState();
    drummaps::apply (state, dm);
    return captureFromState (state, id, name, true);
}

inline Preset makeDefaultKitPreset() {
    return captureFromState (sliderseq::makeDefaultState(), "factory:default4", "Default 4-Piece Kit", true);
}

inline std::vector<Preset> factoryPresets() {
    return {
        makeDefaultKitPreset(),
        makeFactoryPreset ("factory:gm", "GM (Logic / AD2 GM)", drummaps::Preset::gm),
        makeFactoryPreset ("factory:chromatic", "Chromatic 12 Pads", drummaps::Preset::chromatic),
        makeFactoryPreset ("factory:ad2", "XLN Addictive Drums 2", drummaps::Preset::ad2Standard),
        makeFactoryPreset ("factory:xo", "XLN XO (8 Slots)", drummaps::Preset::xo),
        makeFactoryPreset ("factory:drumrack16", "Ableton Drum Rack / Bitwig (16 Pads)", drummaps::Preset::drumRack16)
    };
}

inline std::vector<Preset> loadUserPresets (const juce::File& directory = folder()) {
    std::vector<Preset> out;
    if (! directory.isDirectory()) return out;
    for (const auto& file : directory.findChildFiles (juce::File::findFiles, false, "*.xml")) {
        const auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr || ! xml->hasTagName ("SliderRhythmMappingPreset"))
            continue;
        if (xml->getIntAttribute ("version", 0) != 1)
            continue;
        const auto name = xml->getStringAttribute ("name").trim();
        if (name.isEmpty()) continue;

        Preset p = makeDefaultKitPreset();
        p.isFactory = false;
        p.file = file;
        p.id = xml->getStringAttribute ("id", "user:" + file.getFileNameWithoutExtension()).toStdString();
        p.name = name.toStdString();
        p.trackCount = std::clamp (xml->getIntAttribute ("trackCount", 4), 1, sliderseq::maxTracks);

        for (auto* elem : xml->getChildIterator()) {
            if (! elem->hasTagName ("Track")) continue;
            const int idx = elem->getIntAttribute ("index", -1);
            if (idx < 0 || idx >= sliderseq::maxTracks) continue;
            auto& m = p.tracks[static_cast<size_t> (idx)];
            m.name = elem->getStringAttribute ("name", "TRACK").toStdString();
            m.midiNote = std::clamp (elem->getIntAttribute ("note", 36), 0, 127);
            m.midiChannel = std::clamp (elem->getIntAttribute ("channel", 10), 1, 16);
            m.modShape = sliderseq::clampModShape (elem->getIntAttribute ("modShape", 0));
            m.gateTicks = std::clamp (elem->getDoubleAttribute ("gateTicks", 24.0), 6.0, 288.0);
            m.chokeMask = static_cast<uint16_t> (elem->getIntAttribute ("chokeMask", 0) & 0xffff)
                        & static_cast<uint16_t> (~(1u << static_cast<unsigned> (idx)));
            m.activeZones = std::clamp (elem->getIntAttribute ("activeZones", 12), 1, sliderseq::slotsPerTrack);
            m.groove = std::clamp (elem->getIntAttribute ("groove", 100), 0, 100);
        }
        out.push_back (std::move (p));
    }
    std::sort (out.begin(), out.end(), [] (const Preset& a, const Preset& b) {
        return juce::String (a.name).compareNatural (juce::String (b.name)) < 0;
    });
    return out;
}

inline std::vector<Preset> allPresets (const juce::File& userDir = folder()) {
    auto list = factoryPresets();
    auto user = loadUserPresets (userDir);
    list.insert (list.end(), std::make_move_iterator (user.begin()), std::make_move_iterator (user.end()));
    return list;
}

inline bool saveUserPreset (Preset& preset, const juce::File& directory = folder()) {
    if (directory.createDirectory().failed())
        return false;
    if (preset.id.empty() || preset.isFactory) {
        const auto uid = juce::Uuid().toString();
        preset.id = "user:" + uid.toStdString();
        preset.isFactory = false;
        preset.file = directory.getChildFile (uid + ".xml");
    } else if (preset.file == juce::File {}) {
        const auto safeStem = juce::File::createLegalFileName (juce::String (preset.name)).trim();
        preset.file = directory.getChildFile ((safeStem.isNotEmpty() ? safeStem : juce::Uuid().toString()) + ".xml");
    }

    juce::XmlElement root ("SliderRhythmMappingPreset");
    root.setAttribute ("version", 1);
    root.setAttribute ("id", juce::String (preset.id));
    root.setAttribute ("name", juce::String (preset.name));
    root.setAttribute ("trackCount", std::clamp (preset.trackCount, 1, sliderseq::maxTracks));
    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& m = preset.tracks[static_cast<size_t> (i)];
        auto* elem = root.createNewChildElement ("Track");
        elem->setAttribute ("index", i);
        elem->setAttribute ("name", juce::String (m.name));
        elem->setAttribute ("note", m.midiNote);
        elem->setAttribute ("channel", m.midiChannel);
        elem->setAttribute ("modShape", static_cast<int> (m.modShape));
        elem->setAttribute ("gateTicks", m.gateTicks);
        elem->setAttribute ("chokeMask", static_cast<int> (m.chokeMask));
        elem->setAttribute ("activeZones", m.activeZones);
        elem->setAttribute ("groove", m.groove);
    }
    return preset.file.replaceWithText (root.toString());
}

inline bool deleteUserPreset (const Preset& preset) {
    if (preset.isFactory || ! preset.file.existsAsFile())
        return false;
    return preset.file.moveToTrash();
}

} // namespace mappingpresets
