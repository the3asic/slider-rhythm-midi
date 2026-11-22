#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
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

inline const char* modShapeLabel (ModShape shape) noexcept {
    switch (shape) {
        case ModShape::rampUp: return "RAMP UP";
        case ModShape::rampDown: return "RAMP DOWN";
        case ModShape::cos8: return "8TH";
        case ModShape::cos16: return "16TH";
        case ModShape::random: return "RANDOM";
        case ModShape::count: break;
    }
    return "RAMP UP";
}

inline ModShape clampModShape (int raw) noexcept {
    return static_cast<ModShape> (std::clamp (raw, 0, static_cast<int> (ModShape::count) - 1));
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
    int slot = 0;
    double rate = 1.0;
    int midiNote = 36;
    int midiChannel = 10;
    int velocity = 100;
    int groove = 50;
    ModShape modShape = ModShape::rampUp;
    int modAmount = 50;
    double gateTicks = 24.0;
    uint16_t chokeMask = 0; // Bit j set => this track suppresses track j when hitting the same nominal tick.
    bool muted = false;
    bool solo = false;
};

struct State {
    std::array<Track, maxTracks> tracks {};
    int trackCount = 4;
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
    int slot = 0;
    double rate = 1.0;
    int midiNote = 36;
    int midiChannel = 10;
    int velocity = 100;
    int groove = 50;
    ModShape modShape = ModShape::rampUp;
    int modAmount = 50;
    double gateTicks = 24.0;
    uint16_t chokeMask = 0;
    bool muted = false;
    bool solo = false;
};

struct RenderState {
    std::array<RenderTrack, maxTracks> tracks {};
    int trackCount = 4;
    bool playRecordedMidi = false;
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
    for (size_t s = 0; s < slotsPerTrack; ++s)
        out.patterns[s] = toRenderPattern (track.patterns[s]);
    out.slot = std::clamp (track.slot, 0, slotsPerTrack - 1);
    out.rate = track.rate;
    out.midiNote = std::clamp (track.midiNote, 0, 127);
    out.midiChannel = std::clamp (track.midiChannel, 1, 16);
    out.velocity = std::clamp (track.velocity, 1, 127);
    out.groove = std::clamp (track.groove, 0, 100);
    out.modShape = clampModShape (static_cast<int> (track.modShape));
    out.modAmount = std::clamp (track.modAmount, 0, 100);
    out.gateTicks = std::clamp (track.gateTicks, 6.0, 288.0);
    out.chokeMask = track.chokeMask;
    out.muted = track.muted;
    out.solo = track.solo;
    return out;
}

inline RenderState toRenderState (const State& state, bool playRecordedMidi = false) noexcept {
    RenderState out;
    out.trackCount = std::clamp (state.trackCount, 1, maxTracks);
    out.playRecordedMidi = playRecordedMidi;
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
    const std::array<const char*, 4> names { "KICK", "SNARE", "CLOSED HH", "OPEN HH" };
    const std::array<int, 4> notes { 36, 38, 42, 46 }; // Ultrabeat's C1-B2 GM-compatible range.
    for (int i = 0; i < maxTracks; ++i) {
        auto& track = state.tracks[static_cast<size_t> (i)];
        track.name = i < 4 ? names[static_cast<size_t> (i)] : "TRACK " + std::to_string (i + 1);
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

inline double grooveOffsetValue (int groove, double hostTick) noexcept {
    if (groove == 50) return 0.0;
    const double pairTicks = groove < 50 ? 144.0 : 288.0;
    const double half = pairTicks * 0.5;
    const double amount = std::abs (groove - 50) / 50.0;
    const double midRatio = 0.5 + 0.25 * amount;
    double x = std::fmod (hostTick, pairTicks);
    if (x < 0.0) x += pairTicks;
    const double warped = x < half
        ? x * (2.0 * midRatio)
        : midRatio * pairTicks + (x - half) * (2.0 * (1.0 - midRatio));
    return warped - x;
}

inline double grooveOffset (const Track& track, double hostTick) {
    return grooveOffsetValue (track.groove, hostTick);
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

inline bool isTrackAudible (const RenderState& state, int index, bool anySolo) noexcept {
    if (index < 0 || index >= std::clamp (state.trackCount, 0, maxTracks)) return false;
    const auto& track = state.tracks[static_cast<size_t> (index)];
    return ! track.muted && (! anySolo || track.solo);
}

template <typename TrackType>
inline bool trackHitsNominalTick (const TrackType& track, double nominalTick) noexcept {
    if (track.rate <= 0.0) return false;
    const auto& pat = track.patterns[static_cast<size_t> (std::clamp (track.slot, 0, slotsPerTrack - 1))];
    if (pat.grid < 1 || pat.grid > 64 || pat.length < 1 || pat.length > pat.grid
        || pat.subdivision < 1 || pat.subdivision > 64) return false;
    const double stepTicks = pat.stepTicks();
    const double period = pat.length * stepTicks;
    auto phase = std::fmod (nominalTick * track.rate, period);
    if (phase < 0.0) phase += period;
    for (int s = 0; s < pat.length; ++s) {
        bool active = false;
        if constexpr (std::is_same_v<TrackType, RenderTrack>)
            active = pat.step (s);
        else
            active = pat.steps[static_cast<size_t> (s)];
        if (active && std::abs (phase - s * stepTicks) < 1e-6) return true;
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
        if (ot.muted || (anySolo && ! ot.solo)) continue;
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
        if (track.muted || (anySolo && ! track.solo) || track.rate <= 0.0) continue;
        const auto& pattern = track.patterns[static_cast<size_t> (std::clamp (track.slot, 0, slotsPerTrack - 1))];
        if (pattern.grid < 1 || pattern.grid > 64 || pattern.length < 1 || pattern.length > pattern.grid
            || pattern.subdivision < 1 || pattern.subdivision > 64) continue;
        const double stepTicks = pattern.stepTicks();
        const double period = pattern.length * stepTicks;
        constexpr double maxSwing = 72.0;
        for (int step = 0; step < pattern.length; ++step) {
            if (! pattern.step (step)) continue;
            const double phase = step * stepTicks;
            const auto firstCycle = static_cast<int64_t> (std::floor (((startTick - maxSwing) * track.rate - phase) / period)) - 1;
            const auto lastCycle = static_cast<int64_t> (std::ceil ((endTick * track.rate - phase) / period)) + 1;
            for (auto cycle = firstCycle; cycle <= lastCycle; ++cycle) {
                const double nominal = (cycle * period + phase) / track.rate;
                const double on = nominal + grooveOffsetValue (track.groove, nominal);
                if (on < startTick || on >= endTick) continue;
                if (isSuppressedByChoke (state, count, anySolo, index, nominal)) continue;
                if (countOut >= capacity) continue;
                const double gate = std::min (std::clamp (track.gateTicks, 6.0, 288.0) / track.rate, 288.0);
                const int velocity = std::clamp (static_cast<int> (std::lround (
                    track.velocity * accentValue (track.modShape, track.modAmount, track.midiNote, phase, period)
                    * std::clamp (pattern.velocityScale, 0.1f, 2.0f))), 1, 127);
                out[countOut++] = { on, nominal, gate, index, track.midiChannel, track.midiNote, velocity };
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
        if (track.muted || (anySolo && ! track.solo) || track.rate <= 0.0) continue;
        const auto& pattern = track.patterns[static_cast<size_t> (std::clamp (track.slot, 0, slotsPerTrack - 1))];
        if (pattern.grid < 1 || pattern.grid > 64 || pattern.length < 1 || pattern.length > pattern.grid
            || pattern.subdivision < 1 || pattern.subdivision > 64) continue;
        const double stepTicks = pattern.stepTicks();
        const double period = pattern.length * stepTicks;
        constexpr double maxSwingAndGate = 72.0 + 288.0;
        for (int step = 0; step < pattern.length; ++step) {
            if (! pattern.steps[static_cast<size_t> (step)]) continue;
            const double phase = step * stepTicks;
            const auto firstCycle = static_cast<int64_t> (std::floor (((startTick - maxSwingAndGate) * track.rate - phase) / period)) - 1;
            const auto lastCycle = static_cast<int64_t> (std::ceil ((endTick * track.rate - phase) / period)) + 1;
            for (auto cycle = firstCycle; cycle <= lastCycle; ++cycle) {
                const double nominal = (cycle * period + phase) / track.rate;
                if (isSuppressedByChoke (state, count, anySolo, index, nominal)) continue;
                const double on = nominal + grooveOffset (track, nominal);
                const double off = on + std::min (std::clamp (track.gateTicks, 6.0, 288.0) / track.rate, 288.0);
                const int velocity = std::clamp (static_cast<int> (std::lround (
                    track.velocity * accent (track, phase, period) * std::clamp (pattern.velocityScale, 0.1f, 2.0f))), 1, 127);
                if (on >= startTick && on < endTick)
                    events.push_back ({ on, nominal, index, track.midiChannel, track.midiNote, velocity, true });
                if (off >= startTick && off < endTick)
                    events.push_back ({ off, nominal, index, track.midiChannel, track.midiNote, 0, false });
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
