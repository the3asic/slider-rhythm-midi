#pragma once
#include "SequencerEngine.h"

namespace drummaps {
inline constexpr std::array<const char*, 12> roles {
    "KICK", "SNARE", "CLOSED HH", "OPEN HH", "HI TOM", "MID TOM",
    "LOW TOM", "CLAP", "SIDESTICK", "PERC 1", "PERC 2", "PERC 3" };
// MIDI note numbers; octave labels vary among hosts.
inline constexpr std::array<int, 12> gm { 36, 38, 42, 46, 50, 47, 43, 39, 37, 53, 54, 56 };
inline constexpr std::array<int, 12> chromaticPads { 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47 };
// AD2 Standard: Flexi slots depend on the loaded ADpak/kit piece.
inline constexpr std::array<int, 12> ad2Standard { 36, 38, 49, 56, 71, 69, 67, 47, 44, 74, 76, 96 };
inline constexpr std::array<const char*, 8> xoRoles {
    "KICK 1", "KICK 2", "SNARE", "CLAP", "CLOSED HH", "OPEN HH", "FLEX 1", "FLEX 2" };

enum class Preset { gm, chromatic, ad2Standard, xo };
inline void apply (sliderseq::State& state, Preset preset) {
    state.trackCount = preset == Preset::xo ? 8 : 12;
    for (size_t i = 0; i < static_cast<size_t> (state.trackCount); ++i) {
        auto& track = state.tracks[i];
        track.name = preset == Preset::xo ? xoRoles[i] : roles[i];
        track.midiNote = preset == Preset::xo ? 36 + static_cast<int> (i)
                       : preset == Preset::ad2Standard ? ad2Standard[i]
                       : preset == Preset::chromatic ? chromaticPads[i] : gm[i];
        track.midiChannel = 10;
    }
}
}
