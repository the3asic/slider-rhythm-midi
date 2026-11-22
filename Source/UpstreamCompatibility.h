#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "SequencerEngine.h"

// Standalone compatibility & migration reference layer for upstream merges.
// Keeps versioned AU parameter hints, VST3 normalized slot mapping helpers,
// and legacy v1-v4 -> v5 XML routing/groove migration rules isolated from the core engine.
namespace upstreamcompat {

// AUv2 parameter version hints: JUCE sorts AUv2 parameters by (versionHint, hashedParamId).
// Assigning strictly increasing hints per released parameter generation guarantees existing
// Logic Pro / AU host automation indices never shift when new parameters are introduced.
constexpr int kAuHintGen1CoreTrackParams = 1; // tXX_slot, mute, solo, velocity, rate, groove, mod_amt, loop_steps
constexpr int kAuHintGen2GlobalSwing     = 2; // global_swing
constexpr int kAuHintGen3TrackMacro      = 3; // tXX_macro

// Legacy VST3 normalized slot mapping helper (pre-TOP-detent 0..12 range -> v5 0..13 range).
// If an upstream release previously shipped `tXX_slot` with integer range 0..12 and must preserve
// existing DAW automation curves after extending the top detent to 13, apply this conversion when
// decoding legacy automation streams or expose a dedicated `tXX_fader_zone` parameter ID.
inline int legacyV4NormalizedSlotToV5 (float normalized01) noexcept {
    const float clamped = std::clamp (normalized01, 0.0f, 1.0f);
    return std::clamp (static_cast<int> (std::lround (clamped * 12.0f)), 0, 12);
}

inline float v5SlotToLegacyV4Normalized (int slot) noexcept {
    return static_cast<float> (std::clamp (slot, 0, 12)) / 12.0f;
}

// Legacy v1-v4 routing migration result.
struct LegacyRoutingMigration {
    sliderseq::PlaybackMode playbackMode = sliderseq::PlaybackMode::gen;
    sliderseq::UnmappedRouteMode unmappedMode = sliderseq::UnmappedRouteMode::thru;
    sliderseq::EndZoneMode topMode = sliderseq::EndZoneMode::off;
    sliderseq::MidRouteMode midMode = sliderseq::MidRouteMode::gen;
    sliderseq::EndZoneMode bottomMode = sliderseq::EndZoneMode::off;
    bool legacyThruBypassesMuteSolo = false;
};

// Faithfully maps pre-v5 global PlaybackMode (0=GEN, 1=GEN+THRU, 2=THRU) to v5 per-track & unmapped routing.
inline LegacyRoutingMigration migrateLegacyRouting (int savedVersion, int rawPlaybackMode, bool playRecordedMidi) noexcept {
    LegacyRoutingMigration out;
    if (savedVersion >= 5) {
        out.playbackMode = rawPlaybackMode == static_cast<int> (sliderseq::PlaybackMode::thru)
            ? sliderseq::PlaybackMode::thru : sliderseq::PlaybackMode::gen;
        out.unmappedMode = sliderseq::UnmappedRouteMode::thru;
        out.topMode = sliderseq::EndZoneMode::thru;
        out.midMode = sliderseq::MidRouteMode::gen;
        out.bottomMode = sliderseq::EndZoneMode::block;
        return out;
    }

    const bool wasThru = (rawPlaybackMode == static_cast<int> (sliderseq::PlaybackMode::thru)) || playRecordedMidi;
    const bool wasGenPlusThru = (rawPlaybackMode == static_cast<int> (sliderseq::PlaybackMode::genPlusThru));

    out.playbackMode = wasThru ? sliderseq::PlaybackMode::thru : sliderseq::PlaybackMode::gen;
    out.topMode = sliderseq::EndZoneMode::off; // Pre-v5 never had a TOP detent.

    if (wasGenPlusThru) {
        // Legacy GEN+THRU forwarded all unconsumed input regardless of fader slot or track M/S.
        out.unmappedMode = sliderseq::UnmappedRouteMode::thru;
        out.midMode = sliderseq::MidRouteMode::genPlusThru;
        out.bottomMode = savedVersion >= 4 ? sliderseq::EndZoneMode::thru : sliderseq::EndZoneMode::off;
        out.legacyThruBypassesMuteSolo = true;
    } else if (wasThru) {
        out.unmappedMode = sliderseq::UnmappedRouteMode::thru;
        out.midMode = sliderseq::MidRouteMode::gen;
        out.bottomMode = savedVersion >= 4 ? sliderseq::EndZoneMode::block : sliderseq::EndZoneMode::off;
    } else {
        // Legacy GEN blocked all unconsumed incoming MIDI (including unmapped notes).
        out.unmappedMode = sliderseq::UnmappedRouteMode::block;
        out.midMode = sliderseq::MidRouteMode::gen;
        out.bottomMode = savedVersion >= 4 ? sliderseq::EndZoneMode::block : sliderseq::EndZoneMode::off;
    }
    return out;
}

// Legacy v1-v3 groove migration result.
// Preserves per-track signed groove (-100..+100) when opposite directions existed in the same project
// (e.g., track 0 at 75 = 8th Swing while track 1 at 25 = 16th Shuffle), alongside the unified globalSwing decomposition.
struct LegacyGrooveMigration {
    int globalSwing = 0;
    std::array<int, sliderseq::maxTracks> trackSwingPercent {};
    std::array<int, sliderseq::maxTracks> signedPerTrackGroove {}; // -100..+100 exact legacy values
    bool hasMixedSigns = false;
};

inline LegacyGrooveMigration migrateLegacyGrooves (const std::array<int, sliderseq::maxTracks>& legacyGrooves0To100,
                                                   int trackCount) noexcept {
    LegacyGrooveMigration out;
    out.trackSwingPercent.fill (100);
    out.signedPerTrackGroove.fill (0);

    const int count = std::clamp (trackCount, 1, sliderseq::maxTracks);
    int maxAbsDelta = 0;
    int dominantSigned = 0;
    bool sawPos = false, sawNeg = false;

    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const int delta = std::clamp (legacyGrooves0To100[static_cast<size_t> (i)], 0, 100) - 50;
        out.signedPerTrackGroove[static_cast<size_t> (i)] = std::clamp (delta * 2, -100, 100);
        if (i < count) {
            if (delta > 0) sawPos = true;
            if (delta < 0) sawNeg = true;
            if (std::abs (delta) > maxAbsDelta) {
                maxAbsDelta = std::abs (delta);
                dominantSigned = delta;
            }
        }
    }

    out.hasMixedSigns = sawPos && sawNeg;
    if (maxAbsDelta == 0) {
        out.globalSwing = 0;
        out.trackSwingPercent.fill (100);
    } else {
        out.globalSwing = std::clamp (dominantSigned * 2, -100, 100);
        for (int i = 0; i < sliderseq::maxTracks; ++i) {
            const int delta = std::clamp (legacyGrooves0To100[static_cast<size_t> (i)], 0, 100) - 50;
            // Tracks with opposite sign to dominantSigned cannot be faithfully represented by a single global sign
            // without per-track signed groove mode; default them to 0% rather than flipping their swing direction.
            const bool sameSign = (delta == 0) || ((delta > 0) == (dominantSigned > 0));
            out.trackSwingPercent[static_cast<size_t> (i)] = sameSign
                ? std::clamp (static_cast<int> (std::lround (100.0 * std::abs (delta) / maxAbsDelta)), 0, 100)
                : 0;
        }
    }
    return out;
}

} // namespace upstreamcompat
