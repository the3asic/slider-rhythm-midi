#include "../Source/SequencerEngine.h"
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
    assert (state.tracks[0].midiNote == 36 && state.tracks[1].midiNote == 38);
    assert (state.tracks[2].midiNote == 42 && state.tracks[3].midiNote == 46);

    // All twelve Open HH slots must emit at 1x even when Closed HH is busy.
    state.tracks[2].slot = 1;
    const std::array<int, 12> expected { 4, 4, 2, 1, 2, 4, 3, 3, 4, 5, 7, 6 };
    for (int slot = 0; slot < 12; ++slot) {
        state.tracks[3].slot = slot;
        const auto events = render (state, 0, ticksPerBar);
        assert (countOns (events, 3) == expected[static_cast<size_t> (slot)]);
    }

    // Swing on Closed HH must not immediately choke a same-grid Open HH hit.
    state.tracks[2].groove = 55;
    state.tracks[3].slot = 0;
    auto events = render (state, 0, ticksPerBar);
    assert (countOns (events, 3) == 4);
    assert (countOns (events, 2) == 12);
    const auto firstHalf = render (state, 0, ticksPerBar / 2.0);
    const auto secondHalf = render (state, ticksPerBar / 2.0, ticksPerBar);
    assert (countOns (firstHalf, 3) + countOns (secondHalf, 3) == 4);
    const auto afterSameGridHit = render (state, 440, 480);
    for (const auto& event : afterSameGridHit)
        assert (! (event.trackIndex == 2 && ! event.noteOn && std::abs (event.nominalTick - 432.0) < 1e-6));

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

    std::cout << "Sequencer engine checks passed\n";
}
