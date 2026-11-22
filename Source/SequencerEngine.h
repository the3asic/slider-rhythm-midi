#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace sliderseq {

constexpr int maxTracks = 16;
constexpr int slotsPerTrack = 12;
constexpr int ticksPerQuarter = 288;
constexpr int ticksPerBar = ticksPerQuarter * 4;

enum class ModShape : int {
    rampUp = 0,
    rampDown = 1,
    cos8 = 2,
    cos16 = 3,
    random = 4,
    count = 5
};

enum class EndZoneMode : int {
    off = 0,    // No dedicated end detent; fader stops at 1 (bottom) or activeZones (top).
    block = 1,  // Dedicated end detent: generator OFF, matching incoming MIDI BLOCKED.
    thru = 2    // Dedicated end detent: generator OFF, matching incoming MIDI PASSED THROUGH (pre-M/S).
};

enum class MidRouteMode : int {
    gen = 0,        // Middle 1..activeZones: generator ON, matching incoming MIDI BLOCKED (replace).
    genPlusThru = 1 // Middle 1..activeZones: generator ON, matching incoming MIDI PASSED THROUGH (layer).
};

enum class UnmappedRouteMode : int {
    thru = 0,   // Notes not owned by any active track pass through.
    block = 1   // Notes not owned by any active track are blocked.
};

enum class MacroMode : int {
    build = 0,     // 0..100%: Velocity up + flatten mod + loop cap tightening + 2x roll climax.
    density = 1,   // Bipolar 0..100 (50=0%): Thinning (<50) <-> Original (50) <-> Ghost + Ratchet (>50).
    roll = 2,      // 0..100%: Phase-locked polyrhythmic stutter / roll (OFF -> 8 -> 6 -> 4 -> 3 -> 2 -> 1 -> 2x 1).
    human = 3,     // Bipolar 0..100 (50=0%): Tight/Machine (<50) <-> Original (50) <-> Loose/Humanized (>50).
    velocity = 4,  // Direct single-parameter override: Velocity (1..127).
    swing = 5,     // Direct single-parameter override: Track Swing % (0..100).
    loopSteps = 6, // Direct single-parameter override: Loop Steps (0=OFF, 16..1).
    rate = 7,      // Direct single-parameter override: Rate (1/3..3x).
    modAmount = 8, // Direct single-parameter override: Mod Amount (0..100).
    count = 9
};

inline const char* modShapeLabel (ModShape shape) noexcept {
    switch (shape) {
        case ModShape::rampUp: return "RAMP UP";
        case ModShape::rampDown: return "RAMP DN";
        case ModShape::cos8: return "COS 8";
        case ModShape::cos16: return "COS 16";
        case ModShape::random: return "RANDOM";
        case ModShape::count: break;
    }
    return "RAMP UP";
}

inline ModShape clampModShape (int raw) noexcept {
    return static_cast<ModShape> (std::clamp (raw, 0, static_cast<int> (ModShape::count) - 1));
}

inline EndZoneMode clampEndZoneMode (int raw) noexcept {
    return static_cast<EndZoneMode> (std::clamp (raw, 0, 2));
}

inline MidRouteMode clampMidRouteMode (int raw) noexcept {
    return static_cast<MidRouteMode> (std::clamp (raw, 0, 1));
}

inline UnmappedRouteMode clampUnmappedRouteMode (int raw) noexcept {
    return static_cast<UnmappedRouteMode> (std::clamp (raw, 0, 1));
}

inline MacroMode clampMacroMode (int raw) noexcept {
    return static_cast<MacroMode> (std::clamp (raw, 0, static_cast<int> (MacroMode::count) - 1));
}

inline bool isMacroBipolar (MacroMode mode) noexcept {
    return mode == MacroMode::density || mode == MacroMode::human;
}

inline int defaultMacroValueForMode (MacroMode mode) noexcept {
    return isMacroBipolar (mode) ? 50 : 0;
}

inline const char* macroModeLabel (MacroMode mode) noexcept {
    switch (mode) {
        case MacroMode::build: return "BUILD";
        case MacroMode::density: return "DENSITY";
        case MacroMode::roll: return "ROLL";
        case MacroMode::human: return "HUMAN";
        case MacroMode::velocity: return "-> VEL";
        case MacroMode::swing: return "-> SWING";
        case MacroMode::loopSteps: return "-> LOOP";
        case MacroMode::rate: return "-> RATE";
        case MacroMode::modAmount: return "-> MOD";
        case MacroMode::count: break;
    }
    return "BUILD";
}

inline const char* endZoneShortLabel (EndZoneMode mode) noexcept {
    switch (mode) {
        case EndZoneMode::off: return "OFF";
        case EndZoneMode::block: return "BLK";
        case EndZoneMode::thru: return "THRU";
    }
    return "OFF";
}

inline int trackMinSlot (EndZoneMode bottomMode) noexcept {
    return bottomMode == EndZoneMode::off ? 1 : 0;
}

inline int trackMaxSlot (int activeZones, EndZoneMode topMode) noexcept {
    const int z = std::clamp (activeZones, 1, slotsPerTrack);
    return topMode == EndZoneMode::off ? z : (z + 1);
}

inline int clampTrackSlot (int slot, int activeZones, EndZoneMode bottomMode, EndZoneMode topMode) noexcept {
    return std::clamp (slot, trackMinSlot (bottomMode), trackMaxSlot (activeZones, topMode));
}

inline bool isTrackInBottomZone (int slot, EndZoneMode bottomMode) noexcept {
    return bottomMode != EndZoneMode::off && slot <= 0;
}

inline bool isTrackInTopZone (int slot, int activeZones, EndZoneMode topMode) noexcept {
    return topMode != EndZoneMode::off && slot > std::clamp (activeZones, 1, slotsPerTrack);
}

inline bool isTrackGeneratingZone (int slot, int activeZones, EndZoneMode bottomMode, EndZoneMode topMode) noexcept {
    const int z = std::clamp (activeZones, 1, slotsPerTrack);
    return ! isTrackInBottomZone (slot, bottomMode)
        && ! isTrackInTopZone (slot, z, topMode)
        && slot >= 1 && slot <= z;
}

// Pre-M/S track input routing: returns true if incoming notes owned by this track pass through.
inline bool trackPassesInputPreMute (int slot, int activeZones,
                                     EndZoneMode bottomMode, MidRouteMode midMode, EndZoneMode topMode) noexcept {
    if (isTrackInBottomZone (slot, bottomMode)) return bottomMode == EndZoneMode::thru;
    if (isTrackInTopZone (slot, activeZones, topMode)) return topMode == EndZoneMode::thru;
    return midMode == MidRouteMode::genPlusThru;
}

inline ModShape legacyModShape (int trackIndex, int legacyModMode) noexcept {
    const int mode = std::clamp (legacyModMode, 0, 2);
    if (mode == 2) return ModShape::random;
    if (trackIndex < 2) return mode == 0 ? ModShape::rampUp : ModShape::rampDown;
    return mode == 0 ? ModShape::cos8 : ModShape::cos16;
}

struct Pattern {
    std::string name;
    int grid = 16;
    int length = 16;
    std::array<bool, 64> steps {};
    int subdivision = 16; // Steps per four quarter notes, independent of pattern length.
    float velocityScale = 1.0f;

    double stepTicks() const { return static_cast<double> (ticksPerBar) / subdivision; }

    void resize (int stepsInGrid) { grid = length = stepsInGrid; }
};

struct Track {
    std::string name = "TRACK";
    std::array<Pattern, slotsPerTrack> patterns {};
    int slot = 1;           // 0 = bottom end-zone (if bottomMode != off), 1..activeZones = pattern, activeZones+1 = top end-zone (if topMode != off).
    int activeZones = 12;   // 1..12 configurable fader partition count.
    EndZoneMode topMode = EndZoneMode::thru;
    MidRouteMode midMode = MidRouteMode::gen;
    EndZoneMode bottomMode = EndZoneMode::block;
    MacroMode macroMode = MacroMode::build;
    int macroValue = 0;     // 0..100 (bipolar modes treat 50 as 0% neutral).
    double rate = 1.0;
    int midiNote = 36;
    int midiChannel = 10;
    int velocity = 100;
    int groove = 100;       // Per-track Swing % (0..100), scales globalSwing.
    ModShape modShape = ModShape::rampUp;
    int modAmount = 50;
    int loopStepsOverride = 0; // 0 = use active pattern length, 1..64 = non-destructive loop step cap.
    double gateTicks = 24.0;
    uint16_t chokeMask = 0; // Bit j set => this track suppresses track j when hitting the same nominal tick.
    bool muted = false;
    bool solo = false;
};

struct State {
    std::array<Track, maxTracks> tracks {};
    int trackCount = 4;
    int globalSwing = 0;    // -100..+100: negative = 16th Shuffle, 0 = Straight, positive = 8th Swing.
    UnmappedRouteMode unmappedMode = UnmappedRouteMode::thru;
};

struct Event {
    double absoluteTick = 0.0;
    double nominalTick = 0.0;
    int trackIndex = 0;
    int channel = 10;
    int note = 36;
    int velocity = 0;
    bool noteOn = false;
};

struct RenderPattern {
    int grid = 16;
    int length = 16;
    int subdivision = 16;
    uint64_t stepsMask = 0;
    float velocityScale = 1.0f;

    double stepTicks() const noexcept { return static_cast<double> (ticksPerBar) / subdivision; }

    bool step (int index) const noexcept {
        return index >= 0 && index < 64 && ((stepsMask >> static_cast<unsigned> (index)) & 1ull) != 0ull;
    }

    void setStep (int index, bool enabled) noexcept {
        if (index < 0 || index >= 64) return;
        const uint64_t bit = 1ull << static_cast<unsigned> (index);
        if (enabled) stepsMask |= bit;
        else stepsMask &= ~bit;
    }

    void resize (int stepsInGrid) noexcept {
        grid = length = std::clamp (stepsInGrid, 1, 64);
    }
};

struct RenderTrack {
    std::array<RenderPattern, slotsPerTrack> patterns {};
    int slot = 1;
    int activeZones = 12;
    EndZoneMode topMode = EndZoneMode::thru;
    MidRouteMode midMode = MidRouteMode::gen;
    EndZoneMode bottomMode = EndZoneMode::block;
    MacroMode macroMode = MacroMode::build;
    int macroValue = 0;
    double rate = 1.0;
    int midiNote = 36;
    int midiChannel = 10;
    int velocity = 100;
    int groove = 100;       // Per-track Swing % (0..100).
    ModShape modShape = ModShape::rampUp;
    int modAmount = 50;
    int loopStepsOverride = 0;
    double gateTicks = 24.0;
    uint16_t chokeMask = 0;
    bool muted = false;
    bool solo = false;
};

enum class PlaybackMode : int {
    gen = 0,          // Active (per-track 3-zone routing + unmappedMode).
    genPlusThru = 1,  // Legacy v1-v3 compatibility value (migrated on load).
    thru = 2          // Master bypass (ALL THRU: generator off, all incoming MIDI passed through).
};

struct RenderState {
    std::array<RenderTrack, maxTracks> tracks {};
    int trackCount = 4;
    int globalSwing = 0;    // -100..+100.
    PlaybackMode playbackMode = PlaybackMode::gen;
    UnmappedRouteMode unmappedMode = UnmappedRouteMode::thru;
};

struct OnsetEvent {
    double absoluteTick = 0.0;
    double nominalTick = 0.0;
    double gateTicks = 24.0;
    int trackIndex = 0;
    int channel = 10;
    int note = 36;
    int velocity = 100;
};

inline RenderPattern toRenderPattern (const Pattern& pattern) noexcept {
    RenderPattern out;
    out.grid = std::clamp (pattern.grid, 1, 64);
    out.subdivision = std::clamp (pattern.subdivision, 1, 64);
    out.length = std::clamp (pattern.length, 1, out.grid);
    out.velocityScale = std::clamp (pattern.velocityScale, 0.1f, 2.0f);
    uint64_t mask = 0;
    for (int i = 0; i < 64; ++i)
        if (pattern.steps[static_cast<size_t> (i)]) mask |= (1ull << static_cast<unsigned> (i));
    out.stepsMask = mask;
    return out;
}

inline RenderTrack toRenderTrack (const Track& track) noexcept {
    RenderTrack out;
    out.activeZones = std::clamp (track.activeZones, 1, slotsPerTrack);
    out.topMode = track.topMode;
    out.midMode = track.midMode;
    out.bottomMode = track.bottomMode;
    out.macroMode = track.macroMode;
    out.macroValue = std::clamp (track.macroValue, 0, 100);
    out.slot = clampTrackSlot (track.slot, out.activeZones, out.bottomMode, out.topMode);
    out.rate = track.rate;
    out.midiNote = std::clamp (track.midiNote, 0, 127);
    out.midiChannel = std::clamp (track.midiChannel, 1, 16);
    out.velocity = std::clamp (track.velocity, 1, 127);
    out.groove = std::clamp (track.groove, 0, 100);
    out.modShape = track.modShape;
    out.modAmount = std::clamp (track.modAmount, 0, 100);
    out.loopStepsOverride = std::clamp (track.loopStepsOverride, 0, 64);
    out.gateTicks = std::clamp (track.gateTicks, 6.0, 288.0);
    out.chokeMask = track.chokeMask;
    out.muted = track.muted;
    out.solo = track.solo;
    for (size_t i = 0; i < slotsPerTrack; ++i)
        out.patterns[i] = toRenderPattern (track.patterns[i]);
    return out;
}

inline RenderState toRenderState (const State& state, PlaybackMode playbackMode) noexcept {
    RenderState out;
    out.trackCount = std::clamp (state.trackCount, 1, maxTracks);
    out.globalSwing = std::clamp (state.globalSwing, -100, 100);
    out.playbackMode = playbackMode;
    out.unmappedMode = state.unmappedMode;
    for (size_t i = 0; i < maxTracks; ++i)
        out.tracks[i] = toRenderTrack (state.tracks[i]);
    return out;
}

inline void applyRenderPattern (const RenderPattern& src, Pattern& dst) noexcept {
    dst.grid = std::clamp (src.grid, 1, 64);
    dst.subdivision = std::clamp (src.subdivision, 1, 64);
    dst.length = std::clamp (src.length, 1, dst.grid);
    dst.velocityScale = std::clamp (src.velocityScale, 0.1f, 2.0f);
    for (int i = 0; i < 64; ++i)
        dst.steps[static_cast<size_t> (i)] = src.step (i);
}

inline int legacyStepCount (int grid, int length) {
    return std::clamp ((length * grid + 15) / 16, 1, grid);
}

inline Pattern makePattern (std::string name, int grid, std::initializer_list<int> steps, float velocityScale = 1.0f) {
    Pattern result;
    result.name = std::move (name);
    result.grid = result.length = result.subdivision = grid;
    result.velocityScale = velocityScale;
    for (auto step : steps)
        if (step >= 0 && step < grid && step < 64) result.steps[static_cast<size_t> (step)] = true;
    return result;
}

inline State makeDefaultState() {
    State state;
    state.globalSwing = 0;
    state.unmappedMode = UnmappedRouteMode::thru;
    const std::array<const char*, 4> names { "KICK", "SNARE", "CLOSED HH", "OPEN HH" };
    const std::array<int, 4> notes { 36, 38, 42, 46 }; // Ultrabeat's C1-B2 GM-compatible range.
    for (int i = 0; i < maxTracks; ++i) {
        auto& track = state.tracks[static_cast<size_t> (i)];
        track.name = i < 4 ? names[static_cast<size_t> (i)] : "TRACK " + std::to_string (i + 1);
        track.slot = 1;
        track.activeZones = 12;
        track.topMode = EndZoneMode::thru;
        track.midMode = MidRouteMode::gen;
        track.bottomMode = EndZoneMode::block;
        track.macroMode = MacroMode::build;
        track.macroValue = 0;
        track.groove = 100;
        track.midiNote = i < 4 ? notes[static_cast<size_t> (i)] : 48 + i - 4;
        track.modShape = i < 2 ? ModShape::rampUp : ModShape::cos8;
        track.gateTicks = i == 3 ? 144.0 : 24.0;
        track.chokeMask = i == 3 ? static_cast<uint16_t> (1u << 2) : 0u;
        for (int slot = 0; slot < slotsPerTrack; ++slot)
            track.patterns[static_cast<size_t> (slot)] = makePattern (slot >= 9 ? "Fill" : "Empty", 16, {}, slot >= 9 ? 0.72f : 1.0f);
    }
    auto& kick = state.tracks[0].patterns;
    kick = { makePattern ("Four on floor",16,{0,4,8,12}), makePattern ("One and three",16,{0,8}),
             makePattern ("One",16,{0}), makePattern ("Four plus push",16,{0,4,8,12,14}),
             makePattern ("Offbeat",16,{2,6,10,14}), makePattern ("Break A",16,{0,7,10}),
             makePattern ("Break B",16,{0,6,11,14}), makePattern ("Two-step",16,{0,10}),
             makePattern ("Triplet",12,{0,6,7,9}), makePattern ("Fill double",16,{0,4,8,12,14,15},0.72f),
             makePattern ("Fill syncopated",16,{0,4,8,10,12,14,15},0.72f), makePattern ("Fill triplet",12,{0,3,6,9,10,11},0.72f) };
    auto& snare = state.tracks[1].patterns;
    snare = { makePattern ("Backbeat 2/4",16,{4,12}), makePattern ("Half time",16,{8}),
              makePattern ("Beat two",16,{4}), makePattern ("Beat four",16,{12}),
              makePattern ("Backbeat plus tail",16,{4,12,15}), makePattern ("Syncopated",16,{4,11}),
              makePattern ("Break response",16,{4,10,12}), makePattern ("Triplet accent",12,{3,8,9}),
              makePattern ("Break variation",16,{4,11,13}), makePattern ("Fill double",16,{4,12,14,15},0.72f),
              makePattern ("Fill roll",32,{8,24,25,26,27,28,29,30,31},0.72f), makePattern ("Fill triplet",12,{3,9,10,11},0.72f) };
    auto& hat = state.tracks[2].patterns;
    hat = { makePattern ("Eighths",16,{0,2,4,6,8,10,12,14}), makePattern ("Sixteenths",16,{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}),
            makePattern ("Offbeat eighths",16,{2,6,10,14}), makePattern ("Quarters",16,{0,4,8,12}),
            makePattern ("Broken sixteenths",16,{0,2,3,6,8,10,11,14}), makePattern ("Tail rush",16,{0,2,4,6,8,10,12,13,14,15}),
            makePattern ("Triplet pairs",12,{0,1,2,6,7,8}), makePattern ("Eighth triplets",12,{0,1,2,3,4,5,6,7,8,9,10,11}),
            makePattern ("Shifted eighths",16,{0,3,4,7,8,11,12,15}), makePattern ("Fill 32nds",32,{0,4,8,12,16,20,24,25,26,27,28,29,30,31},0.72f),
            makePattern ("Fill 64ths",64,{0,8,16,24,32,40,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63},0.72f),
            makePattern ("Fill triplets",48,{0,6,12,18,24,30,36,37,38,39,40,41,42,43,44,45,46,47},0.72f) };
    auto& open = state.tracks[3].patterns;
    open = { makePattern ("Offbeat open",16,{2,6,10,14}), makePattern ("Each beat",16,{0,4,8,12}),
             makePattern ("Alternate offbeat",16,{2,10}), makePattern ("End only",16,{14}),
             makePattern ("Second half",16,{10,14}), makePattern ("Syncopated 16ths",16,{3,7,11,15}),
             makePattern ("First three offbeats",16,{2,6,10}), makePattern ("Broken open",16,{2,9,14}),
             makePattern ("Triplet open",12,{1,4,7,10}), makePattern ("Fill double",16,{2,6,10,12,14},0.72f),
             makePattern ("Fill 16ths",16,{2,6,10,12,13,14,15},0.72f), makePattern ("Fill triplet",12,{1,4,7,9,10,11},0.72f) };
    return state;
}

// Monotonic piecewise-linear host-timeline groove warp driven by globalSwing (-100..+100) scaled by trackSwingPercent (0..100).
inline double grooveOffsetValue (int globalSwing, int trackSwingPercent, double hostTick) noexcept {
    const double effective = (std::clamp (globalSwing, -100, 100) / 100.0)
                           * (std::clamp (trackSwingPercent, 0, 100) / 100.0);
    if (std::abs (effective) < 1e-9) return 0.0;
    const double pairTicks = effective < 0.0 ? 144.0 : 288.0;
    const double half = pairTicks * 0.5;
    const double amount = std::abs (effective);
    const double midRatio = 0.5 + 0.25 * amount;
    double x = std::fmod (hostTick, pairTicks);
    if (x < 0.0) x += pairTicks;
    const double warped = x < half
        ? x * (2.0 * midRatio)
        : midRatio * pairTicks + (x - half) * (2.0 * (1.0 - midRatio));
    return warped - x;
}

inline double grooveOffset (const State& state, const Track& track, double hostTick) noexcept {
    return grooveOffsetValue (state.globalSwing, track.groove, hostTick);
}

inline double accentValue (ModShape modShape, int modAmount, int midiNote, double phaseTick, double period) noexcept {
    const auto amount = std::clamp (modAmount / 100.0, 0.0, 1.0);
    double shape = 1.0;
    switch (modShape) {
        case ModShape::rampUp:
            shape = period > 0.0 ? phaseTick / period : 1.0;
            break;
        case ModShape::rampDown:
            shape = period > 0.0 ? 1.0 - phaseTick / period : 1.0;
            break;
        case ModShape::cos8:
            shape = (1.0 + std::cos (2.0 * 3.141592653589793 * phaseTick / 288.0)) * 0.5;
            break;
        case ModShape::cos16:
            shape = (1.0 + std::cos (2.0 * 3.141592653589793 * phaseTick / 144.0)) * 0.5;
            break;
        case ModShape::random: {
            const auto seed = static_cast<uint32_t> (std::llround (phaseTick) * 1103515245u + midiNote * 12345u);
            shape = ((seed ^ (seed >> 13)) & 0xffffu) / 65535.0;
            break;
        }
        case ModShape::count:
            break;
    }
    return 1.0 - amount + amount * (0.35 + 0.65 * shape);
}

inline double accent (const Track& track, double phaseTick, double period) {
    return accentValue (track.modShape, track.modAmount, track.midiNote, phaseTick, period);
}

inline int effectivePatternLength (int patternLength, int patternGrid, int loopStepsOverride) noexcept {
    const int clampedGrid = std::clamp (patternGrid, 1, 64);
    if (loopStepsOverride > 0)
        return std::clamp (loopStepsOverride, 1, clampedGrid);
    return std::clamp (patternLength, 1, clampedGrid);
}

inline double deterministicUnitHash (uint32_t a, uint32_t b, uint32_t c) noexcept {
    uint32_t x = a * 747796405u + b * 2891336453u + c * 277803737u + 1234567u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<double> (x & 0x00ffffffu) / static_cast<double> (0x01000000u);
}

struct EffectiveTrackParams {
    int velocity = 100;
    int groove = 100;
    int modAmount = 50;
    int loopStepsOverride = 0;
    double rate = 1.0;
    double gateTicks = 24.0;
    double densityDelta = 0.0; // -1..+1 (non-zero in MacroMode::density).
    double humanLoose = 0.0;   // 0..1 (positive half of MacroMode::human).
};

template <typename TrackType>
inline EffectiveTrackParams computeEffectiveTrack (const TrackType& track, int patternLength, int patternGrid) noexcept {
    EffectiveTrackParams eff;
    eff.velocity = std::clamp (track.velocity, 1, 127);
    eff.groove = std::clamp (track.groove, 0, 100);
    eff.modAmount = std::clamp (track.modAmount, 0, 100);
    eff.loopStepsOverride = std::clamp (track.loopStepsOverride, 0, 64);
    eff.rate = track.rate;
    eff.gateTicks = std::clamp (track.gateTicks, 6.0, 288.0);

    const int mv = std::clamp (track.macroValue, 0, 100);
    const int baseLen = effectivePatternLength (patternLength, patternGrid, track.loopStepsOverride);
    static constexpr std::array<double, 5> kMacroRates { 1.0 / 3.0, 0.5, 1.0, 2.0, 3.0 };

    switch (track.macroMode) {
        case MacroMode::build: {
            if (mv <= 0) break;
            const double m = static_cast<double> (mv) / 100.0;
            const double vBlend = std::clamp (m / 0.60, 0.0, 1.0);
            const double targetVel = static_cast<double> (std::max (eff.velocity, 124));
            eff.velocity = std::clamp (static_cast<int> (std::lround (eff.velocity + (targetVel - eff.velocity) * vBlend)), 1, 127);
            const double modFlatten = std::clamp (m / 0.50, 0.0, 1.0);
            eff.modAmount = std::clamp (static_cast<int> (std::lround (eff.modAmount * (1.0 - modFlatten))), 0, 100);
            if (m >= 0.88) eff.loopStepsOverride = 1;
            else if (m >= 0.74) eff.loopStepsOverride = std::min (baseLen, 2);
            else if (m >= 0.58) eff.loopStepsOverride = std::min (baseLen, 4);
            else if (m >= 0.40) eff.loopStepsOverride = std::min (baseLen, 8);
            if (m >= 0.82) eff.rate = std::min (4.0, eff.rate * 2.0);
            eff.gateTicks = std::clamp (eff.gateTicks * (1.0 - 0.35 * m), 6.0, 288.0);
            break;
        }
        case MacroMode::density: {
            const double d = (static_cast<double> (mv) - 50.0) / 50.0;
            if (std::abs (d) >= 0.02)
                eff.densityDelta = std::clamp (d, -1.0, 1.0);
            break;
        }
        case MacroMode::roll: {
            if (mv < 5) break;
            if (mv < 18) eff.loopStepsOverride = std::min (baseLen, 8);
            else if (mv < 32) eff.loopStepsOverride = std::min (baseLen, 6);
            else if (mv < 46) eff.loopStepsOverride = std::min (baseLen, 4);
            else if (mv < 60) eff.loopStepsOverride = std::min (baseLen, 3);
            else if (mv < 74) eff.loopStepsOverride = std::min (baseLen, 2);
            else if (mv < 87) eff.loopStepsOverride = 1;
            else {
                eff.loopStepsOverride = 1;
                eff.rate = std::min (4.0, eff.rate * 2.0);
            }
            break;
        }
        case MacroMode::human: {
            const double h = (static_cast<double> (mv) - 50.0) / 50.0;
            if (h <= -0.02) {
                const double tight = -h;
                eff.groove = std::clamp (static_cast<int> (std::lround (eff.groove * (1.0 - tight))), 0, 100);
                eff.modAmount = std::clamp (static_cast<int> (std::lround (eff.modAmount * (1.0 - 0.85 * tight))), 0, 100);
                eff.gateTicks = std::clamp (eff.gateTicks * (1.0 - 0.50 * tight), 6.0, 288.0);
            } else if (h >= 0.02) {
                eff.groove = std::clamp (static_cast<int> (std::lround (eff.groove + (100.0 - eff.groove) * (0.70 * h))), 0, 100);
                eff.modAmount = std::clamp (static_cast<int> (std::lround (eff.modAmount + (100.0 - eff.modAmount) * (0.50 * h))), 0, 100);
                eff.humanLoose = h;
            }
            break;
        }
        case MacroMode::velocity:
            eff.velocity = std::clamp (1 + static_cast<int> (std::lround (mv * 126.0 / 100.0)), 1, 127);
            break;
        case MacroMode::swing:
            eff.groove = mv;
            break;
        case MacroMode::loopSteps:
            eff.loopStepsOverride = mv <= 0 ? 0 : std::clamp (17 - static_cast<int> (std::lround (1.0 + (mv - 1) * 15.0 / 99.0)), 1, 16);
            break;
        case MacroMode::rate:
            eff.rate = kMacroRates[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (mv * 4.0 / 100.0)), 0, 4))];
            break;
        case MacroMode::modAmount:
            eff.modAmount = mv;
            break;
        case MacroMode::count:
            break;
    }
    return eff;
}

struct MacroStepEval {
    bool fires = false;
    bool isGhost = false;
    bool isThinned = false;
    int ratchetCount = 1; // 1 = normal, 2 or 3 = subdivided ratchet burst.
    float velocityFactor = 1.0f;
};

inline MacroStepEval evaluateMacroStep (bool baseStepActive, int stepIndex, int activeLen,
                                        int midiNote, double densityDelta) noexcept {
    MacroStepEval out;
    if (std::abs (densityDelta) < 0.02) {
        out.fires = baseStepActive;
        return out;
    }
    const double h = deterministicUnitHash (static_cast<uint32_t> (stepIndex + 1),
                                            static_cast<uint32_t> (midiNote + 17),
                                            static_cast<uint32_t> (activeLen + 31));
    if (densityDelta < 0.0) {
        if (! baseStepActive) return out;
        // Musical thinning priority: keep downbeat 0 longest, then quarters, then 8ths, drop weak 16ths first.
        double priority = 0.0;
        if (stepIndex == 0) priority = 0.99;
        else if (stepIndex % 4 == 0) priority = 0.78 + 0.16 * h;
        else if (stepIndex % 2 == 0) priority = 0.45 + 0.28 * h;
        else priority = 0.05 + 0.36 * h;
        if ((-densityDelta) > priority) {
            out.fires = false;
            out.isThinned = true;
            return out;
        }
        out.fires = true;
        return out;
    }

    // Positive densityDelta: add deterministic ghost notes on empty steps & ratchets on active steps.
    if (baseStepActive) {
        out.fires = true;
        if (densityDelta > 0.55) {
            const double rHash = deterministicUnitHash (static_cast<uint32_t> (stepIndex + 97),
                                                        static_cast<uint32_t> (midiNote + 5), 73u);
            const double ratchetProb = (densityDelta - 0.55) / 0.45;
            if (rHash < ratchetProb * 0.65)
                out.ratchetCount = (densityDelta > 0.82 && rHash < ratchetProb * 0.30) ? 3 : 2;
        }
        return out;
    }

    const double baseThresh = (stepIndex % 2 == 0) ? (0.06 + 0.48 * h) : (0.16 + 0.72 * h);
    if (densityDelta >= baseThresh) {
        out.fires = true;
        out.isGhost = true;
        const double depth = std::clamp ((densityDelta - baseThresh) / std::max (0.15, 1.0 - baseThresh), 0.0, 1.0);
        out.velocityFactor = static_cast<float> (0.38 + 0.42 * depth);
    }
    return out;
}

inline int stepModulatedVelocity (const Track& track, const Pattern& pattern, int stepIndex) noexcept {
    const auto eff = computeEffectiveTrack (track, pattern.length, pattern.grid);
    const int activeLen = effectivePatternLength (pattern.length, pattern.grid, eff.loopStepsOverride);
    const double stepTicks = pattern.stepTicks();
    const double period = std::max (1.0, activeLen * stepTicks);
    const int clampedStep = std::clamp (stepIndex, 0, std::max (0, activeLen - 1));
    const double phase = clampedStep * stepTicks;
    const auto stepEval = evaluateMacroStep (pattern.steps[static_cast<size_t> (clampedStep)],
                                             clampedStep, activeLen, track.midiNote, eff.densityDelta);
    return std::clamp (static_cast<int> (std::lround (
        eff.velocity
        * accentValue (track.modShape, eff.modAmount, track.midiNote, phase, period)
        * std::clamp (pattern.velocityScale, 0.1f, 2.0f)
        * stepEval.velocityFactor)), 1, 127);
}

inline bool isTrackAudible (const RenderState& state, int index, bool anySolo) noexcept {
    if (index < 0 || index >= std::clamp (state.trackCount, 0, maxTracks)) return false;
    const auto& track = state.tracks[static_cast<size_t> (index)];
    return isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode)
        && ! track.muted && (! anySolo || track.solo);
}

inline bool isTrackUnmutedForRouting (const RenderState& state, int index, bool anySolo) noexcept {
    if (index < 0 || index >= std::clamp (state.trackCount, 0, maxTracks)) return false;
    const auto& track = state.tracks[static_cast<size_t> (index)];
    return ! track.muted && (! anySolo || track.solo);
}

template <typename TrackType>
inline bool trackHitsNominalTick (const TrackType& track, double nominalTick) noexcept {
    if (! isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode)) return false;
    const int patternIdx = std::clamp (track.slot - 1, 0, slotsPerTrack - 1);
    const auto& pat = track.patterns[static_cast<size_t> (patternIdx)];
    if (pat.grid < 1 || pat.grid > 64 || pat.length < 1 || pat.length > pat.grid
        || pat.subdivision < 1 || pat.subdivision > 64) return false;
    const auto eff = computeEffectiveTrack (track, pat.length, pat.grid);
    if (eff.rate <= 0.0) return false;
    const int activeLen = effectivePatternLength (pat.length, pat.grid, eff.loopStepsOverride);
    const double stepTicks = pat.stepTicks();
    const double period = activeLen * stepTicks;
    auto phase = std::fmod (nominalTick * eff.rate, period);
    if (phase < 0.0) phase += period;
    for (int s = 0; s < activeLen; ++s) {
        bool baseActive = false;
        if constexpr (std::is_same_v<TrackType, RenderTrack>)
            baseActive = pat.step (s);
        else
            baseActive = pat.steps[static_cast<size_t> (s)];
        const auto eval = evaluateMacroStep (baseActive, s, activeLen, track.midiNote, eff.densityDelta);
        if (eval.fires && std::abs (phase - s * stepTicks) < 1e-6) return true;
    }
    return false;
}

template <typename StateType>
inline bool isSuppressedByChoke (const StateType& state, int trackCount, bool anySolo,
                                 int targetIndex, double nominalTick) noexcept {
    const uint16_t targetBit = static_cast<uint16_t> (1u << static_cast<unsigned> (targetIndex));
    for (int other = 0; other < trackCount; ++other) {
        if (other == targetIndex) continue;
        const auto& ot = state.tracks[static_cast<size_t> (other)];
        if ((ot.chokeMask & targetBit) == 0u) continue;
        if (! isTrackGeneratingZone (ot.slot, ot.activeZones, ot.bottomMode, ot.topMode)
            || ot.muted || (anySolo && ! ot.solo)) continue;
        if (trackHitsNominalTick (ot, nominalTick)) return true;
    }
    return false;
}

inline size_t renderOnsetsInto (const RenderState& state, double startTick, double endTick,
                                OnsetEvent* out, size_t capacity) noexcept {
    if (out == nullptr || capacity == 0 || endTick <= startTick) return 0;
    size_t countOut = 0;
    const int count = std::clamp (state.trackCount, 0, maxTracks);
    bool anySolo = false;
    for (int i = 0; i < count; ++i)
        if (state.tracks[static_cast<size_t> (i)].solo) { anySolo = true; break; }

    for (int index = 0; index < count; ++index) {
        const auto& track = state.tracks[static_cast<size_t> (index)];
        if (! isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode)
            || track.muted || (anySolo && ! track.solo)) continue;
        const int maxZone = std::clamp (track.activeZones, 1, slotsPerTrack);
        const int patternIdx = std::clamp (track.slot - 1, 0, maxZone - 1);
        const auto& pattern = track.patterns[static_cast<size_t> (patternIdx)];
        if (pattern.grid < 1 || pattern.grid > 64 || pattern.length < 1 || pattern.length > pattern.grid
            || pattern.subdivision < 1 || pattern.subdivision > 64) continue;

        const auto eff = computeEffectiveTrack (track, pattern.length, pattern.grid);
        if (eff.rate <= 0.0) continue;
        const int activeLen = effectivePatternLength (pattern.length, pattern.grid, eff.loopStepsOverride);
        const double stepTicks = pattern.stepTicks();
        const double period = activeLen * stepTicks;
        constexpr double maxSwingAndHuman = 90.0;

        for (int step = 0; step < activeLen; ++step) {
            const auto eval = evaluateMacroStep (pattern.step (step), step, activeLen, track.midiNote, eff.densityDelta);
            if (! eval.fires) continue;
            const double phase = step * stepTicks;
            const auto firstCycle = static_cast<int64_t> (std::floor (((startTick - maxSwingAndHuman) * eff.rate - phase) / period)) - 1;
            const auto lastCycle = static_cast<int64_t> (std::ceil (((endTick + maxSwingAndHuman) * eff.rate - phase) / period)) + 1;
            for (auto cycle = firstCycle; cycle <= lastCycle; ++cycle) {
                const double baseNominal = (cycle * period + phase) / eff.rate;
                if (isSuppressedByChoke (state, count, anySolo, index, baseNominal)) continue;

                double humanShift = 0.0;
                double humanVelMul = 1.0;
                if (eff.humanLoose > 0.0) {
                    const double hTime = deterministicUnitHash (static_cast<uint32_t> (cycle + 4096),
                                                                static_cast<uint32_t> (step + 1),
                                                                static_cast<uint32_t> (track.midiNote + 101));
                    const double hVel = deterministicUnitHash (static_cast<uint32_t> (cycle + 8192),
                                                               static_cast<uint32_t> (step + 13),
                                                               static_cast<uint32_t> (track.midiNote + 211));
                    humanShift = (hTime * 2.0 - 1.0) * 14.0 * eff.humanLoose;
                    humanVelMul = 1.0 + (hVel * 2.0 - 1.0) * 0.18 * eff.humanLoose;
                }

                const int subCount = std::clamp (eval.ratchetCount, 1, 3);
                const double subStepTicks = (stepTicks / eff.rate) / static_cast<double> (subCount);
                for (int r = 0; r < subCount; ++r) {
                    const double nominal = baseNominal + r * subStepTicks;
                    const double on = nominal + grooveOffsetValue (state.globalSwing, eff.groove, nominal)
                                    + (r == 0 ? humanShift : humanShift * 0.5);
                    if (on < startTick || on >= endTick) continue;
                    if (countOut >= capacity) continue;
                    const double rawGate = std::min (eff.gateTicks / eff.rate, 288.0);
                    const double gate = subCount > 1 ? std::max (4.0, std::min (rawGate, subStepTicks * 0.72)) : rawGate;
                    const float ratchetFade = subCount > 1 ? (1.0f - 0.14f * static_cast<float> (r)) : 1.0f;
                    const int velocity = std::clamp (static_cast<int> (std::lround (
                        eff.velocity
                        * accentValue (track.modShape, eff.modAmount, track.midiNote, phase, period)
                        * std::clamp (pattern.velocityScale, 0.1f, 2.0f)
                        * eval.velocityFactor * ratchetFade * humanVelMul)), 1, 127);
                    out[countOut++] = { on, nominal, gate, index, track.midiChannel, track.midiNote, velocity };
                }
            }
        }
    }
    std::sort (out, out + countOut, [] (const OnsetEvent& a, const OnsetEvent& b) noexcept {
        if (a.absoluteTick != b.absoluteTick) return a.absoluteTick < b.absoluteTick;
        if (a.trackIndex != b.trackIndex) return a.trackIndex < b.trackIndex;
        return a.note < b.note;
    });
    return countOut;
}

inline std::vector<Event> render (const State& state, double startTick, double endTick) {
    std::vector<Event> events;
    const int count = std::clamp (state.trackCount, 0, maxTracks);
    const bool anySolo = std::any_of (state.tracks.begin(), state.tracks.begin() + count, [] (const auto& t) { return t.solo; });
    for (int index = 0; index < count; ++index) {
        const auto& track = state.tracks[static_cast<size_t> (index)];
        if (! isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode)
            || track.muted || (anySolo && ! track.solo)) continue;
        const int maxZone = std::clamp (track.activeZones, 1, slotsPerTrack);
        const int patternIdx = std::clamp (track.slot - 1, 0, maxZone - 1);
        const auto& pattern = track.patterns[static_cast<size_t> (patternIdx)];
        if (pattern.grid < 1 || pattern.grid > 64 || pattern.length < 1 || pattern.length > pattern.grid
            || pattern.subdivision < 1 || pattern.subdivision > 64) continue;

        const auto eff = computeEffectiveTrack (track, pattern.length, pattern.grid);
        if (eff.rate <= 0.0) continue;
        const int activeLen = effectivePatternLength (pattern.length, pattern.grid, eff.loopStepsOverride);
        const double stepTicks = pattern.stepTicks();
        const double period = activeLen * stepTicks;
        constexpr double maxSwingAndGate = 90.0 + 288.0;

        for (int step = 0; step < activeLen; ++step) {
            const auto eval = evaluateMacroStep (pattern.steps[static_cast<size_t> (step)], step, activeLen, track.midiNote, eff.densityDelta);
            if (! eval.fires) continue;
            const double phase = step * stepTicks;
            const auto firstCycle = static_cast<int64_t> (std::floor (((startTick - maxSwingAndGate) * eff.rate - phase) / period)) - 1;
            const auto lastCycle = static_cast<int64_t> (std::ceil (((endTick + 90.0) * eff.rate - phase) / period)) + 1;
            for (auto cycle = firstCycle; cycle <= lastCycle; ++cycle) {
                const double baseNominal = (cycle * period + phase) / eff.rate;
                if (isSuppressedByChoke (state, count, anySolo, index, baseNominal)) continue;

                double humanShift = 0.0;
                double humanVelMul = 1.0;
                if (eff.humanLoose > 0.0) {
                    const double hTime = deterministicUnitHash (static_cast<uint32_t> (cycle + 4096),
                                                                static_cast<uint32_t> (step + 1),
                                                                static_cast<uint32_t> (track.midiNote + 101));
                    const double hVel = deterministicUnitHash (static_cast<uint32_t> (cycle + 8192),
                                                               static_cast<uint32_t> (step + 13),
                                                               static_cast<uint32_t> (track.midiNote + 211));
                    humanShift = (hTime * 2.0 - 1.0) * 14.0 * eff.humanLoose;
                    humanVelMul = 1.0 + (hVel * 2.0 - 1.0) * 0.18 * eff.humanLoose;
                }

                const int subCount = std::clamp (eval.ratchetCount, 1, 3);
                const double subStepTicks = (stepTicks / eff.rate) / static_cast<double> (subCount);
                for (int r = 0; r < subCount; ++r) {
                    const double nominal = baseNominal + r * subStepTicks;
                    const double on = nominal + grooveOffsetValue (state.globalSwing, eff.groove, nominal)
                                    + (r == 0 ? humanShift : humanShift * 0.5);
                    const double rawGate = std::min (eff.gateTicks / eff.rate, 288.0);
                    const double gate = subCount > 1 ? std::max (4.0, std::min (rawGate, subStepTicks * 0.72)) : rawGate;
                    const double off = on + gate;
                    const float ratchetFade = subCount > 1 ? (1.0f - 0.14f * static_cast<float> (r)) : 1.0f;
                    const int velocity = std::clamp (static_cast<int> (std::lround (
                        eff.velocity
                        * accentValue (track.modShape, eff.modAmount, track.midiNote, phase, period)
                        * std::clamp (pattern.velocityScale, 0.1f, 2.0f)
                        * eval.velocityFactor * ratchetFade * humanVelMul)), 1, 127);
                    if (on >= startTick && on < endTick)
                        events.push_back ({ on, nominal, index, track.midiChannel, track.midiNote, velocity, true });
                    if (off >= startTick && off < endTick)
                        events.push_back ({ off, nominal, index, track.midiChannel, track.midiNote, 0, false });
                }
            }
        }
    }
    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) {
        if (a.absoluteTick != b.absoluteTick) return a.absoluteTick < b.absoluteTick;
        return ! a.noteOn && b.noteOn;
    });
    return events;
}

} // namespace sliderseq
