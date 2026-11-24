#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>
#include "SequencerEngine.h"

namespace trackpresets {

struct Bank {
    std::string id;   // Stable identifier ("factory:kick", "user:<uuid>", etc.)
    std::string name; // Display name ("KICK", "CLAP", "BASS / 808", ...)
    bool isFactory = false;
    std::array<sliderseq::Pattern, sliderseq::slotsPerTrack> patterns {};
    juce::File file;  // Populated for user track presets
};

inline juce::File folder() {
    if (const char* overrideDir = std::getenv ("SLIDER_TRACK_PRESETS_DIR"); overrideDir != nullptr && overrideDir[0] != '\0')
        return juce::File (juce::String::fromUTF8 (overrideDir));
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Slider Rhythm MIDI").getChildFile ("Track Presets");
}

inline bool patternsEqual (const sliderseq::Pattern& a, const sliderseq::Pattern& b) noexcept {
    return a.grid == b.grid
        && a.length == b.length
        && a.subdivision == b.subdivision
        && std::abs (a.velocityScale - b.velocityScale) < 1e-3f
        && a.steps == b.steps;
}

inline bool bankMatchesTrack (const Bank& bank, const sliderseq::Track& track) noexcept {
    for (size_t i = 0; i < sliderseq::slotsPerTrack; ++i)
        if (! patternsEqual (bank.patterns[i], track.patterns[i]))
            return false;
    return true;
}

inline bool isTrackBankEmpty (const sliderseq::Track& track) noexcept {
    for (const auto& pat : track.patterns)
        for (bool on : pat.steps)
            if (on) return false;
    return true;
}

// Replaces all 12 pattern slots on the target track while preserving MIDI note, channel, name, routing, and zones.
inline void applyToTrack (const Bank& bank, sliderseq::Track& track) {
    track.patterns = bank.patterns;
}

inline Bank captureFromTrack (const sliderseq::Track& track, std::string id, std::string name, bool isFactory = false) {
    Bank b;
    b.id = std::move (id);
    b.name = std::move (name);
    b.isFactory = isFactory;
    b.patterns = track.patterns;
    return b;
}

inline std::vector<Bank> factoryBanks() {
    using sliderseq::makePattern;
    const auto defaults = sliderseq::makeDefaultState();

    std::vector<Bank> list;
    list.reserve (11);

    // 1..4: Exact built-in 4-piece kit banks (Slots 1-3 sparse -> Slot 4 starter groove -> Slots 5-9 dense -> Slots 10-12 fills).
    list.push_back ({ "factory:kick", "KICK", true, defaults.tracks[0].patterns, {} });
    list.push_back ({ "factory:snare", "SNARE", true, defaults.tracks[1].patterns, {} });
    list.push_back ({ "factory:closed_hh", "CLOSED HH", true, defaults.tracks[2].patterns, {} });
    list.push_back ({ "factory:open_hh", "OPEN HH", true, defaults.tracks[3].patterns, {} });

    // 5: CLAP
    list.push_back ({ "factory:clap", "CLAP", true, {{
        makePattern ("Bar end", 16, { 12 }),
        makePattern ("Half time", 16, { 8 }),
        makePattern ("Beat two", 16, { 4 }),
        makePattern ("Backbeat 2/4", 16, { 4, 12 }),
        makePattern ("Disco double", 16, { 4, 12, 14 }),
        makePattern ("Skip clap", 16, { 4, 7, 12 }),
        makePattern ("UKG two-step", 16, { 4, 11, 14 }),
        makePattern ("Syncopated drive", 16, { 4, 6, 12, 15 }),
        makePattern ("Four claps", 16, { 0, 4, 8, 12 }),
        makePattern ("Fill buildup", 16, { 4, 8, 10, 12, 14, 15 }, 0.72f),
        makePattern ("Fill 16th rush", 16, { 4, 12, 13, 14, 15 }, 0.72f),
        makePattern ("Fill triplet", 12, { 3, 9, 10, 11 }, 0.72f)
    }}, {} });

    // 6: RIM / SIDESTICK
    list.push_back ({ "factory:rim", "RIM / STICK", true, {{
        makePattern ("Pickup tail", 16, { 15 }),
        makePattern ("Ghost offbeat", 16, { 6, 14 }),
        makePattern ("Clave light", 16, { 3, 10 }),
        makePattern ("House rim", 16, { 3, 6, 14 }),
        makePattern ("Son clave", 16, { 0, 3, 6, 10, 12 }),
        makePattern ("Tresillo", 16, { 0, 3, 6, 8, 11, 14 }),
        makePattern ("UKG skip", 16, { 2, 7, 10, 15 }),
        makePattern ("Broken techno", 16, { 1, 4, 7, 11, 14 }),
        makePattern ("Triplet wood", 12, { 1, 4, 7, 9, 11 }),
        makePattern ("Fill double", 16, { 3, 6, 12, 14, 15 }, 0.72f),
        makePattern ("Fill 32nd roll", 32, { 6, 14, 24, 26, 28, 29, 30, 31 }, 0.72f),
        makePattern ("Fill triplet", 12, { 2, 5, 8, 9, 10, 11 }, 0.72f)
    }}, {} });

    // 7: TOMS
    list.push_back ({ "factory:toms", "TOMS", true, {{
        makePattern ("Turnaround", 16, { 14 }),
        makePattern ("End pair", 16, { 11, 14 }),
        makePattern ("Low pulse", 16, { 3, 11 }),
        makePattern ("Techno rumble", 16, { 3, 6, 11, 14 }),
        makePattern ("Tribal drive", 16, { 0, 3, 6, 10, 14 }),
        makePattern ("Offbeat gallop", 16, { 2, 3, 10, 11, 14 }),
        makePattern ("Syncopated tom", 16, { 0, 5, 7, 10, 13, 15 }),
        makePattern ("Triplet tribal", 12, { 0, 2, 5, 6, 8, 11 }),
        makePattern ("Rolling 16ths", 16, { 0, 2, 3, 6, 8, 10, 11, 14, 15 }),
        makePattern ("Fill 4-hit", 16, { 12, 13, 14, 15 }, 0.72f),
        makePattern ("Fill 8-hit roll", 16, { 8, 9, 10, 11, 12, 13, 14, 15 }, 0.72f),
        makePattern ("Fill triplet", 12, { 6, 7, 8, 9, 10, 11 }, 0.72f)
    }}, {} });

    // 8: SHAKER / TOPS
    list.push_back ({ "factory:shaker", "SHAKER", true, {{
        makePattern ("Offbeat sparse", 16, { 2, 10 }),
        makePattern ("All offbeats", 16, { 2, 6, 10, 14 }),
        makePattern ("Push eighths", 16, { 0, 2, 6, 10, 14 }),
        makePattern ("House shaker", 16, { 0, 2, 3, 6, 8, 10, 11, 14 }),
        makePattern ("Classic 16ths", 16, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }),
        makePattern ("Gap groove", 16, { 1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15 }),
        makePattern ("Syncopated tops", 16, { 0, 2, 3, 5, 7, 8, 10, 11, 13, 15 }),
        makePattern ("Triplet shaker", 12, { 0, 2, 3, 5, 6, 8, 9, 11 }),
        makePattern ("Full triplets", 12, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }),
        makePattern ("Fill 32nd lift", 32, { 0, 4, 6, 8, 12, 14, 16, 20, 24, 26, 28, 29, 30, 31 }, 0.72f),
        makePattern ("Fill rush", 16, { 2, 6, 10, 12, 13, 14, 15 }, 0.72f),
        makePattern ("Fill triplet rush", 12, { 2, 5, 8, 9, 10, 11 }, 0.72f)
    }}, {} });

    // 9: PERC / CONGA
    list.push_back ({ "factory:perc", "PERC / CONGA", true, {{
        makePattern ("Accent 11", 16, { 11 }),
        makePattern ("Two hits", 16, { 6, 14 }),
        makePattern ("Dotted feel", 16, { 0, 6, 12 }),
        makePattern ("Conga groove", 16, { 2, 6, 11, 14 }),
        makePattern ("Bongo bounce", 16, { 0, 3, 7, 10, 14 }),
        makePattern ("Afro 3-2", 16, { 0, 2, 5, 8, 10, 13 }),
        makePattern ("Minimal poly", 16, { 0, 3, 6, 9, 12, 15 }),
        makePattern ("Triplet conga", 12, { 0, 2, 4, 7, 9, 11 }),
        makePattern ("Dense perc", 16, { 0, 2, 3, 5, 6, 9, 10, 11, 14, 15 }),
        makePattern ("Fill conga run", 16, { 6, 10, 12, 13, 14, 15 }, 0.72f),
        makePattern ("Fill 32nd perc", 32, { 4, 12, 20, 24, 26, 27, 28, 29, 30, 31 }, 0.72f),
        makePattern ("Fill triplet", 12, { 3, 6, 8, 9, 10, 11 }, 0.72f)
    }}, {} });

    // 10: CYMBAL / RIDE
    list.push_back ({ "factory:cymbal", "CYMBAL / RIDE", true, {{
        makePattern ("Bar crash", 16, { 0 }),
        makePattern ("Half crash", 16, { 0, 8 }),
        makePattern ("Offbeat bell", 16, { 2, 10 }),
        makePattern ("909 offbeat ride", 16, { 2, 6, 10, 14 }),
        makePattern ("Techno quarters", 16, { 0, 4, 8, 12 }),
        makePattern ("Driving eighths", 16, { 0, 2, 4, 6, 8, 10, 12, 14 }),
        makePattern ("DnB ride", 16, { 0, 2, 4, 7, 8, 10, 12, 15 }),
        makePattern ("Swing triplet ride", 12, { 0, 3, 5, 6, 9, 11 }),
        makePattern ("Relentless 16ths", 16, { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 }),
        makePattern ("Fill bell build", 16, { 0, 4, 8, 10, 12, 14, 15 }, 0.72f),
        makePattern ("Fill wash", 16, { 0, 2, 4, 6, 8, 10, 12, 13, 14, 15 }, 0.72f),
        makePattern ("Fill triplet bell", 12, { 0, 3, 6, 9, 10, 11 }, 0.72f)
    }}, {} });

    // 11: BASS / 808
    list.push_back ({ "factory:bass", "BASS / 808", true, {{
        makePattern ("Whole sub", 16, { 0 }),
        makePattern ("Half sub", 16, { 0, 8 }),
        makePattern ("Trap 808 A", 16, { 0, 6, 10 }),
        makePattern ("Offbeat house", 16, { 2, 6, 10, 14 }),
        makePattern ("Electro bounce", 16, { 0, 3, 6, 10, 14 }),
        makePattern ("Techno rolling", 16, { 1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15 }),
        makePattern ("UKG bass", 16, { 0, 3, 7, 10, 12, 15 }),
        makePattern ("Acid 16th", 16, { 0, 2, 3, 6, 8, 11, 13, 14 }),
        makePattern ("Triplet rolling", 12, { 1, 2, 4, 5, 7, 8, 10, 11 }),
        makePattern ("Fill stutter", 16, { 0, 6, 10, 12, 14, 15 }, 0.72f),
        makePattern ("Fill 32nd 808", 32, { 0, 12, 20, 26, 28, 30, 31 }, 0.72f),
        makePattern ("Fill triplet roll", 12, { 0, 5, 8, 9, 10, 11 }, 0.72f)
    }}, {} });

    return list;
}

inline std::vector<Bank> loadUserBanks (const juce::File& directory = folder()) {
    std::vector<Bank> out;
    if (! directory.isDirectory()) return out;

    auto parseFiniteDouble = [] (const juce::String& raw, double& outVal) -> bool {
        const auto s = raw.trim().toStdString();
        if (s.empty()) return false;
        char* endPtr = nullptr;
        const double parsed = std::strtod (s.c_str(), &endPtr);
        if (endPtr == nullptr || *endPtr != '\0' || ! std::isfinite (parsed))
            return false;
        outVal = parsed;
        return true;
    };

    for (const auto& file : directory.findChildFiles (juce::File::findFiles, false, "*.xml")) {
        const auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr || ! xml->hasTagName ("SliderRhythmTrackPreset"))
            continue;
        if (! xml->hasAttribute ("version"))
            continue;
        const int version = xml->getIntAttribute ("version", 0);
        if (version != 1)
            continue;
        const auto name = xml->getStringAttribute ("name").trim();
        if (name.isEmpty()) continue;

        Bank b;
        b.isFactory = false;
        b.file = file;
        b.id = xml->getStringAttribute ("id", "user:" + file.getFileNameWithoutExtension()).toStdString();
        b.name = name.toStdString();

        std::array<bool, sliderseq::slotsPerTrack> seenSlot {};
        int seenCount = 0;
        bool validBank = true;

        for (auto* child : xml->getChildIterator()) {
            if (! child->hasTagName ("Pattern")) continue;
            if (! child->hasAttribute ("slot") || ! child->hasAttribute ("grid")
                || ! child->hasAttribute ("subdivision") || ! child->hasAttribute ("length")
                || ! child->hasAttribute ("velocityScale") || ! child->hasAttribute ("steps")) {
                validBank = false;
                break;
            }
            const int slotIdx = child->getIntAttribute ("slot", -1);
            if (slotIdx < 0 || slotIdx >= sliderseq::slotsPerTrack || seenSlot[static_cast<size_t> (slotIdx)]) {
                validBank = false;
                break;
            }
            const int grid = child->getIntAttribute ("grid", 0);
            const int subdivision = child->getIntAttribute ("subdivision", 0);
            const int length = child->getIntAttribute ("length", 0);
            double velScale = std::numeric_limits<double>::quiet_NaN();
            if (grid < 1 || grid > 64 || subdivision < 1 || subdivision > 64
                || length < 1 || length > grid
                || ! parseFiniteDouble (child->getStringAttribute ("velocityScale"), velScale)
                || velScale < 0.1 || velScale > 2.0) {
                validBank = false;
                break;
            }
            const auto steps = child->getStringAttribute ("steps");
            if (steps.length() < grid || steps.length() > 64) {
                validBank = false;
                break;
            }
            for (int i = 0; i < steps.length(); ++i) {
                if (steps[i] != '0' && steps[i] != '1') {
                    validBank = false;
                    break;
                }
            }
            if (! validBank) break;

            seenSlot[static_cast<size_t> (slotIdx)] = true;
            ++seenCount;
            auto& pat = b.patterns[static_cast<size_t> (slotIdx)];
            pat = sliderseq::makePattern (slotIdx >= 9 ? "Fill" : "Empty", grid, {}, static_cast<float> (velScale));
            pat.name = child->getStringAttribute ("name", pat.name).toStdString();
            pat.grid = grid;
            pat.subdivision = subdivision;
            pat.length = length;
            pat.velocityScale = static_cast<float> (velScale);
            for (int i = 0; i < 64; ++i)
                pat.steps[static_cast<size_t> (i)] = i < steps.length() && steps[i] == '1';
        }
        if (! validBank || seenCount != sliderseq::slotsPerTrack)
            continue;
        out.push_back (std::move (b));
    }
    std::sort (out.begin(), out.end(), [] (const Bank& a, const Bank& b) {
        return juce::String (a.name).compareNatural (juce::String (b.name)) < 0;
    });
    return out;
}

inline bool saveUserBank (Bank& bank, const juce::File& directory = folder()) {
    if (directory.createDirectory().failed())
        return false;
    if (bank.id.empty() || bank.isFactory) {
        const auto uid = juce::Uuid().toString();
        bank.id = "user:" + uid.toStdString();
        bank.isFactory = false;
        bank.file = directory.getChildFile (uid + ".xml");
    } else if (bank.file == juce::File {}) {
        const auto safeStem = juce::File::createLegalFileName (juce::String (bank.name)).trim();
        bank.file = directory.getChildFile ((safeStem.isNotEmpty() ? safeStem : juce::Uuid().toString()) + ".xml");
    }

    juce::XmlElement root ("SliderRhythmTrackPreset");
    root.setAttribute ("version", 1);
    root.setAttribute ("id", juce::String (bank.id));
    root.setAttribute ("name", juce::String (bank.name));
    for (int s = 0; s < sliderseq::slotsPerTrack; ++s) {
        const auto& pat = bank.patterns[static_cast<size_t> (s)];
        auto* child = root.createNewChildElement ("Pattern");
        child->setAttribute ("slot", s);
        child->setAttribute ("name", juce::String (pat.name));
        child->setAttribute ("grid", pat.grid);
        child->setAttribute ("subdivision", pat.subdivision);
        child->setAttribute ("length", pat.length);
        const double safeVelScale = std::isfinite (pat.velocityScale)
            ? std::clamp (static_cast<double> (pat.velocityScale), 0.1, 2.0)
            : 1.0;
        child->setAttribute ("velocityScale", safeVelScale);
        juce::String steps;
        for (int i = 0; i < 64; ++i)
            steps += pat.steps[static_cast<size_t> (i)] ? "1" : "0";
        child->setAttribute ("steps", steps);
    }
    return bank.file.replaceWithText (root.toString());
}

inline bool deleteUserBank (const Bank& bank) {
    if (bank.isFactory || ! bank.file.existsAsFile())
        return false;
    return bank.file.moveToTrash();
}

// Identifies the displayed bank label for a track's 12-slot pattern array.
// Returns exact factory/user bank name on match, "EMPTY" when all 12 slots have zero active steps, or "CUSTOM".
inline juce::String identifyTrackBankLabel (const sliderseq::Track& track,
                                            const std::vector<Bank>& factories,
                                            const std::vector<Bank>& users) {
    if (isTrackBankEmpty (track))
        return "EMPTY";
    for (const auto& b : factories)
        if (bankMatchesTrack (b, track))
            return juce::String (b.name);
    for (const auto& b : users)
        if (bankMatchesTrack (b, track))
            return juce::String (b.name);
    return "CUSTOM";
}

} // namespace trackpresets
