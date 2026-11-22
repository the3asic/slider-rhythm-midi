#include "../Source/SequencerEngine.h"
#include "../Source/DrumMaps.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace sliderseq;

static int countOns (const std::vector<Event>& events, int track) {
    return static_cast<int> (std::count_if (events.begin(), events.end(), [=] (const auto& e) { return e.noteOn && e.trackIndex == track; }));
}

int main() {
    auto state = makeDefaultState();
    assert (state.trackCount == 4);
    assert (state.globalSwing == 0);
    assert (state.tracks[0].slot == 1 && state.tracks[0].activeZones == 12 && state.tracks[0].groove == 100);
    assert (state.tracks[0].midiNote == 36 && state.tracks[1].midiNote == 38);
    assert (state.tracks[2].midiNote == 42 && state.tracks[3].midiNote == 46);

    // Slot 0 = EMPTY (live silent floor): emits 0 notes and does NOT choke Closed HH.
    {
        auto emptyCheck = makeDefaultState();
        emptyCheck.tracks[2].slot = 2; // Sixteenths (16 hits)
        emptyCheck.tracks[3].slot = 0; // OPEN HH pulled down to EMPTY
        const auto evs = render (emptyCheck, 0, ticksPerBar);
        assert (countOns (evs, 3) == 0);
        assert (countOns (evs, 2) == 16); // Not choked by an EMPTY Open HH track
    }

    // All twelve Open HH slots (1..12) must emit at 1x even when Closed HH is busy.
    state.tracks[2].slot = 2; // patterns[1] = Sixteenths
    const std::array<int, 12> expected { 4, 4, 2, 1, 2, 4, 3, 3, 4, 5, 7, 6 };
    for (int slot = 1; slot <= 12; ++slot) {
        state.tracks[3].slot = slot;
        const auto events = render (state, 0, ticksPerBar);
        assert (countOns (events, 3) == expected[static_cast<size_t> (slot - 1)]);
    }

    // Swing on Closed HH must not immediately choke a same-grid Open HH hit.
    state.globalSwing = 10;
    state.tracks[2].groove = 100;
    state.tracks[3].groove = 0;
    state.tracks[3].slot = 1;
    auto events = render (state, 0, ticksPerBar);
    assert (countOns (events, 3) == 4);
    assert (countOns (events, 2) == 12);
    const auto firstHalf = render (state, 0, ticksPerBar / 2.0);
    const auto secondHalf = render (state, ticksPerBar / 2.0, ticksPerBar);
    assert (countOns (firstHalf, 3) + countOns (secondHalf, 3) == 4);
    const auto afterSameGridHit = render (state, 440, 480);
    for (const auto& event : afterSameGridHit)
        assert (! (event.trackIndex == 2 && ! event.noteOn && std::abs (event.nominalTick - 432.0) < 1e-6));

    state.globalSwing = 0;
    state.tracks[0].rate = 2.0;
    events = render (state, 0, ticksPerBar);
    assert (countOns (events, 0) == 8);
    state.tracks[0].rate = 1.0 / 3.0;
    events = render (state, 0, ticksPerBar);
    assert (countOns (events, 0) == 2);

    state.tracks[0].rate = 1.0;
    state.tracks[0].solo = true;
    events = render (state, 0, ticksPerBar);
    assert (countOns (events, 0) == 4 && countOns (events, 1) == 0);
    state.tracks[0].solo = false;
    state.tracks[0].muted = true;
    events = render (state, 0, ticksPerBar);
    assert (countOns (events, 0) == 0);

    state.tracks[0].muted = false;
    state.tracks[0].patterns[0].length = 8;
    events = render (state, 0, ticksPerBar);
    assert (countOns (events, 0) == 4);

    state.trackCount = 16;
    state.tracks[15].patterns[0].steps[0] = true;
    events = render (state, 0, ticksPerBar);
    assert (countOns (events, 15) == 1);

    auto expanded = makeDefaultState();
    expanded.trackCount = 1;
    auto& longBeat = expanded.tracks[0].patterns[0];
    longBeat = makePattern ("Long beat", 16, { 0, 1 });
    longBeat.resize (64);
    longBeat.steps[63] = true;
    std::vector<double> onsets;
    for (const auto& event : render (expanded, 0, 4 * ticksPerBar + 100))
        if (event.noteOn)
            onsets.push_back (event.absoluteTick);
    assert ((onsets == std::vector<double> { 0, 72, 63 * 72, 64 * 72, 65 * 72 }));

    // Global Swing x per-track Swing % monotonic piecewise-linear warp across all grids.
    for (int globalSwingVal : { -100, -50, 50, 100 }) {
        for (int gridSize : { 12, 16, 32, 48, 64 }) {
            State swung;
            swung.trackCount = 1;
            swung.globalSwing = globalSwingVal;
            swung.tracks[0].slot = 1;
            swung.tracks[0].groove = 100;
            Pattern dense;
            dense.name = "All steps";
            dense.grid = dense.length = dense.subdivision = gridSize;
            for (int s = 0; s < gridSize; ++s) dense.steps[static_cast<size_t> (s)] = true;
            swung.tracks[0].patterns[0] = dense;
            std::vector<double> hits;
            for (const auto& event : render (swung, 0, ticksPerBar))
                if (event.noteOn) hits.push_back (event.absoluteTick);
            assert (static_cast<int> (hits.size()) == gridSize);
            for (size_t i = 1; i < hits.size(); ++i)
                assert (hits[i] > hits[i - 1] + 1e-6);
            if (globalSwingVal == 100 && gridSize == 32) {
                assert (std::abs (hits[4] - 216.0) < 1e-6);
                assert (std::abs (hits[5] - 234.0) < 1e-6);
            }
            if (globalSwingVal == 100 && gridSize == 16) {
                assert (std::abs (hits[2] - 216.0) < 1e-6);
                assert (std::abs (hits[3] - 252.0) < 1e-6);
            }
        }
    }

    // Per-track Swing % scales globalSwing cleanly (100% = full, 50% = half, 0% = straight).
    {
        State scaled;
        scaled.trackCount = 3;
        scaled.globalSwing = 100;
        for (int t = 0; t < 3; ++t) {
            scaled.tracks[static_cast<size_t> (t)].slot = 1;
            scaled.tracks[static_cast<size_t> (t)].patterns[0] = makePattern ("Off", 16, { 2 }); // nominal 144
        }
        scaled.tracks[0].groove = 100; // +72 -> 216
        scaled.tracks[1].groove = 50;  // +36 -> 180
        scaled.tracks[2].groove = 0;   // +0  -> 144
        const auto evs = render (scaled, 0, ticksPerBar);
        std::array<double, 3> times { -1.0, -1.0, -1.0 };
        for (const auto& e : evs)
            if (e.noteOn && times[static_cast<size_t> (e.trackIndex)] < 0.0)
                times[static_cast<size_t> (e.trackIndex)] = e.absoluteTick;
        assert (std::abs (times[0] - 216.0) < 1e-6);
        assert (std::abs (times[1] - 180.0) < 1e-6);
        assert (std::abs (times[2] - 144.0) < 1e-6);
    }

    // Host-timeline swing aligns hits across different track rates at shared nominal ticks.
    {
        State crossRate;
        crossRate.trackCount = 2;
        crossRate.globalSwing = 100;
        crossRate.tracks[0].slot = 1;
        crossRate.tracks[0].rate = 0.5;
        crossRate.tracks[0].groove = 100;
        crossRate.tracks[0].patterns[0] = makePattern ("Half", 16, { 1 }); // phase=72 -> nominal=144
        crossRate.tracks[1].slot = 1;
        crossRate.tracks[1].rate = 2.0;
        crossRate.tracks[1].groove = 100;
        crossRate.tracks[1].patterns[0] = makePattern ("Double", 16, { 4 }); // phase=288 -> nominal=144
        const auto aligned = render (crossRate, 0, ticksPerBar);
        double t0 = -1.0, t1 = -1.0;
        for (const auto& event : aligned) {
            if (event.noteOn && event.trackIndex == 0 && t0 < 0.0) t0 = event.absoluteTick;
            if (event.noteOn && event.trackIndex == 1 && t1 < 0.0) t1 = event.absoluteTick;
        }
        assert (std::abs (t0 - 216.0) < 1e-6 && std::abs (t1 - 216.0) < 1e-6);
    }

    // Unified track semantics: XO CLAP (track 3) must NOT suppress SNARE (track 2); OPEN HH (5) suppresses CLOSED HH (4).
    {
        auto xo = makeDefaultState();
        drummaps::apply (xo, drummaps::Preset::xo);
        assert (xo.trackCount == 8);
        xo.tracks[2].patterns[0] = makePattern ("Snare 2/4", 16, { 4, 12 });
        xo.tracks[3].patterns[0] = makePattern ("Clap 2/4", 16, { 4, 12 });
        xo.tracks[4].patterns[0] = makePattern ("CH 16ths", 16, { 0, 2, 4, 6, 8, 10, 12, 14 });
        xo.tracks[5].patterns[0] = makePattern ("OH offbeat", 16, { 2, 6, 10, 14 });
        const auto xoEvents = render (xo, 0, ticksPerBar);
        assert (countOns (xoEvents, 2) == 2);
        assert (countOns (xoEvents, 3) == 2);
        assert (countOns (xoEvents, 5) == 4);
        assert (countOns (xoEvents, 4) == 4);
    }

    // Arbitrary track chokeMask, gateTicks, modShape, velocityScale, and stepModulatedVelocity parity.
    {
        auto custom = makeDefaultState();
        drummaps::apply (custom, drummaps::Preset::drumRack16);
        assert (custom.trackCount == 16 && custom.tracks[15].midiNote == 51);
        custom.tracks[0].patterns[0] = makePattern ("Beat", 16, { 0, 4, 8, 12 });
        custom.tracks[7].patterns[0] = makePattern ("Choker", 16, { 4, 12 }, 0.5f);
        custom.tracks[7].chokeMask = static_cast<uint16_t> (1u << 0);
        custom.tracks[7].gateTicks = 144.0;
        custom.tracks[7].modShape = ModShape::rampDown;
        custom.tracks[7].modAmount = 0;
        custom.tracks[7].velocity = 100;
        for (int i = 1; i < 16; ++i)
            if (i != 7) custom.tracks[static_cast<size_t> (i)].muted = true;
        const auto evs = render (custom, 0, ticksPerBar);
        assert (countOns (evs, 0) == 2);
        assert (countOns (evs, 7) == 2);
        double on7 = -1.0, off7 = -1.0;
        int vel7 = 0;
        for (const auto& e : evs) {
            if (e.trackIndex == 7 && e.noteOn && on7 < 0.0) { on7 = e.absoluteTick; vel7 = e.velocity; }
            if (e.trackIndex == 7 && ! e.noteOn && off7 < 0.0) { off7 = e.absoluteTick; }
        }
        assert (vel7 == 50);
        assert (vel7 == stepModulatedVelocity (custom.tracks[7], custom.tracks[7].patterns[0], 4));
        assert (std::abs ((off7 - on7) - 144.0) < 1e-6);
    }

    std::cout << "Sequencer engine checks passed\n";
}
