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

struct Pattern {
    std::string name;
    int grid = 16;
    int length = 16;
    std::array<bool, 64> steps {};
    int subdivision = 16; // Steps per four quarter notes, independent of pattern length.

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
    int modMode = 0;
    int modAmount = 50;
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
    int modMode = 0;
    int modAmount = 50;
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
    out.modMode = std::clamp (track.modMode, 0, 2);
    out.modAmount = std::clamp (track.modAmount, 0, 100);
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
    for (int i = 0; i < 64; ++i)
        dst.steps[static_cast<size_t> (i)] = src.step (i);
}

inline int legacyStepCount (int grid, int length) {
    return std::clamp ((length * grid + 15) / 16, 1, grid);
}

inline Pattern makePattern (std::string name, int grid, std::initializer_list<int> steps) {
    Pattern result;
    result.name = std::move (name);
    result.grid = result.length = result.subdivision = grid;
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
        for (int slot = 0; slot < slotsPerTrack; ++slot)
            track.patterns[static_cast<size_t> (slot)] = makePattern (slot >= 9 ? "Fill" : "Empty", 16, {});
    }
    auto& kick = state.tracks[0].patterns;
    kick = { makePattern ("Four on floor",16,{0,4,8,12}), makePattern ("One and three",16,{0,8}),
             makePattern ("One",16,{0}), makePattern ("Four plus push",16,{0,4,8,12,14}),
             makePattern ("Offbeat",16,{2,6,10,14}), makePattern ("Break A",16,{0,7,10}),
             makePattern ("Break B",16,{0,6,11,14}), makePattern ("Two-step",16,{0,10}),
             makePattern ("Triplet",12,{0,6,7,9}), makePattern ("Fill double",16,{0,4,8,12,14,15}),
             makePattern ("Fill syncopated",16,{0,4,8,10,12,14,15}), makePattern ("Fill triplet",12,{0,3,6,9,10,11}) };
    auto& snare = state.tracks[1].patterns;
    snare = { makePattern ("Backbeat 2/4",16,{4,12}), makePattern ("Half time",16,{8}),
              makePattern ("Beat two",16,{4}), makePattern ("Beat four",16,{12}),
              makePattern ("Backbeat plus tail",16,{4,12,15}), makePattern ("Syncopated",16,{4,11}),
              makePattern ("Break response",16,{4,10,12}), makePattern ("Triplet accent",12,{3,8,9}),
              makePattern ("Break variation",16,{4,11,13}), makePattern ("Fill double",16,{4,12,14,15}),
              makePattern ("Fill roll",32,{8,24,25,26,27,28,29,30,31}), makePattern ("Fill triplet",12,{3,9,10,11}) };
    auto& hat = state.tracks[2].patterns;
    hat = { makePattern ("Eighths",16,{0,2,4,6,8,10,12,14}), makePattern ("Sixteenths",16,{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}),
            makePattern ("Offbeat eighths",16,{2,6,10,14}), makePattern ("Quarters",16,{0,4,8,12}),
            makePattern ("Broken sixteenths",16,{0,2,3,6,8,10,11,14}), makePattern ("Tail rush",16,{0,2,4,6,8,10,12,13,14,15}),
            makePattern ("Triplet pairs",12,{0,1,2,6,7,8}), makePattern ("Eighth triplets",12,{0,1,2,3,4,5,6,7,8,9,10,11}),
            makePattern ("Shifted eighths",16,{0,3,4,7,8,11,12,15}), makePattern ("Fill 32nds",32,{0,4,8,12,16,20,24,25,26,27,28,29,30,31}),
            makePattern ("Fill 64ths",64,{0,8,16,24,32,40,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63}),
            makePattern ("Fill triplets",48,{0,6,12,18,24,30,36,37,38,39,40,41,42,43,44,45,46,47}) };
    auto& open = state.tracks[3].patterns;
    open = { makePattern ("Offbeat open",16,{2,6,10,14}), makePattern ("Each beat",16,{0,4,8,12}),
             makePattern ("Alternate offbeat",16,{2,10}), makePattern ("End only",16,{14}),
             makePattern ("Second half",16,{10,14}), makePattern ("Syncopated 16ths",16,{3,7,11,15}),
             makePattern ("First three offbeats",16,{2,6,10}), makePattern ("Broken open",16,{2,9,14}),
             makePattern ("Triplet open",12,{1,4,7,10}), makePattern ("Fill double",16,{2,6,10,12,14}),
             makePattern ("Fill 16ths",16,{2,6,10,12,13,14,15}), makePattern ("Fill triplet",12,{1,4,7,9,10,11}) };
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

inline double accentValue (int modMode, int modAmount, int midiNote, int trackIndex, double phaseTick, double period) noexcept {
    const auto amount = std::clamp (modAmount / 100.0, 0.0, 1.0);
    double shape = 1.0;
    if (modMode == 2) {
        const auto seed = static_cast<uint32_t> (std::llround (phaseTick) * 1103515245u + midiNote * 12345u);
        shape = ((seed ^ (seed >> 13)) & 0xffffu) / 65535.0;
    } else if (trackIndex < 2) {
        shape = modMode == 0 ? phaseTick / period : 1.0 - phaseTick / period;
    } else {
        shape = (1.0 + std::cos (2.0 * 3.141592653589793 * phaseTick / (modMode == 0 ? 288.0 : 144.0))) * .5;
    }
    return 1.0 - amount + amount * (.35 + .65 * shape);
}

inline double accent (const Track& track, int trackIndex, double phaseTick, double period) {
    return accentValue (track.modMode, track.modAmount, track.midiNote, trackIndex, phaseTick, period);
}

inline bool isTrackAudible (const RenderState& state, int index, bool anySolo) noexcept {
    if (index < 0 || index >= std::clamp (state.trackCount, 0, maxTracks)) return false;
    const auto& track = state.tracks[static_cast<size_t> (index)];
    return ! track.muted && (! anySolo || track.solo);
}

inline size_t renderOnsetsInto (const RenderState& state, double startTick, double endTick,
                                OnsetEvent* out, size_t capacity) noexcept {
    if (out == nullptr || capacity == 0 || endTick <= startTick) return 0;
    size_t countOut = 0;
    const int count = std::clamp (state.trackCount, 0, maxTracks);
    bool anySolo = false;
    for (int i = 0; i < count; ++i)
        if (state.tracks[static_cast<size_t> (i)].solo) { anySolo = true; break; }

    const bool openIsAudible = count > 3 && isTrackAudible (state, 3, anySolo);
    const auto& openTrack = state.tracks[3];
    const auto& openPattern = openTrack.patterns[static_cast<size_t> (std::clamp (openTrack.slot, 0, slotsPerTrack - 1))];
    const bool openValid = openIsAudible && openTrack.rate > 0.0
        && openPattern.grid >= 1 && openPattern.grid <= 64
        && openPattern.length >= 1 && openPattern.length <= openPattern.grid
        && openPattern.subdivision >= 1 && openPattern.subdivision <= 64;
    const double openStepTicks = openValid ? openPattern.stepTicks() : 0.0;
    const double openPeriod = openValid ? openPattern.length * openStepTicks : 0.0;

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
                if (index == 2 && openValid) {
                    auto openPhase = std::fmod (nominal * openTrack.rate, openPeriod);
                    if (openPhase < 0.0) openPhase += openPeriod;
                    bool suppressed = false;
                    for (int os = 0; os < openPattern.length; ++os) {
                        if (openPattern.step (os) && std::abs (openPhase - os * openStepTicks) < 1e-6) {
                            suppressed = true;
                            break;
                        }
                    }
                    if (suppressed) continue;
                }
                if (countOut >= capacity) continue;
                const double gate = std::min ((index == 3 ? 144.0 : 24.0) / track.rate, 180.0);
                const int velocity = std::clamp (static_cast<int> (std::lround (
                    track.velocity * accentValue (track.modMode, track.modAmount, track.midiNote, index, phase, period)
                    * (track.slot >= 9 ? .72 : 1.0))), 1, 127);
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
        const auto& pattern = track.patterns[static_cast<size_t> (std::clamp (track.slot, 0, 11))];
        if (pattern.grid < 1 || pattern.grid > 64 || pattern.length < 1 || pattern.length > pattern.grid || pattern.subdivision < 1 || pattern.subdivision > 64) continue;
        const double stepTicks = pattern.stepTicks();
        const double period = pattern.length * stepTicks;
        for (int step = 0; step < pattern.length; ++step) {
            if (! pattern.steps[static_cast<size_t> (step)]) continue;
            const double phase = step * stepTicks;
            constexpr double maxSwingAndGate = 72.0 + 180.0;
            const auto firstCycle = static_cast<int64_t> (std::floor (((startTick - maxSwingAndGate) * track.rate - phase) / period)) - 1;
            const auto lastCycle = static_cast<int64_t> (std::ceil ((endTick * track.rate - phase) / period)) + 1;
            for (auto cycle = firstCycle; cycle <= lastCycle; ++cycle) {
                const double nominal = (cycle * period + phase) / track.rate;
                const double on = nominal + grooveOffset (track, nominal);
                const double off = on + std::min ((index == 3 ? 144.0 : 24.0) / track.rate, 180.0);
                const int velocity = std::clamp (static_cast<int> (std::lround (track.velocity * accent (track, index, phase, period) * (track.slot >= 9 ? .72 : 1.0))), 1, 127);
                if (on >= startTick && on < endTick)
                    events.push_back ({ on, nominal, index, track.midiChannel, track.midiNote, velocity, true });
                if (off >= startTick && off < endTick)
                    events.push_back ({ off, nominal, index, track.midiChannel, track.midiNote, 0, false });
            }
        }
    }
    // Open HH wins when the two built-in hat tracks hit the same nominal grid position.
    const auto openIsAudible = count > 3 && ! state.tracks[3].muted && (! anySolo || state.tracks[3].solo);
    const auto& openTrack = state.tracks[3];
    const auto& openPattern = openTrack.patterns[static_cast<size_t> (std::clamp (openTrack.slot, 0, 11))];
    events.erase (std::remove_if (events.begin(), events.end(), [&] (const Event& e) {
        if (e.trackIndex != 2 || ! openIsAudible || openTrack.rate <= 0 || openPattern.grid < 1 || openPattern.grid > 64
            || openPattern.length < 1 || openPattern.length > openPattern.grid
            || openPattern.subdivision < 1 || openPattern.subdivision > 64) return false;
        const auto stepTicks = openPattern.stepTicks();
        const auto period = openPattern.length * stepTicks;
        auto phase = std::fmod (e.nominalTick * openTrack.rate, period);
        if (phase < 0) phase += period;
        for (int step = 0; step < openPattern.length; ++step)
            if (openPattern.steps[static_cast<size_t> (step)] && std::abs (phase - step * stepTicks) < 1e-6) return true;
        return false;
    }), events.end());
    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) {
        if (a.absoluteTick != b.absoluteTick) return a.absoluteTick < b.absoluteTick;
        return ! a.noteOn && b.noteOn;
    });
    return events;
}

} // namespace sliderseq
