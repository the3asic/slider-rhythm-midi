#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <map>
#include <vector>
#include "SequencerEngine.h"

namespace midiexporter {

inline juce::MidiFile buildMidiFile (const sliderseq::State& state, int bars = 4, double bpm = 120.0) {
    const int clampedBars = std::clamp (bars, 1, 16);
    const double clampedBpm = std::clamp (bpm, 20.0, 300.0);
    const double totalTicks = static_cast<double> (clampedBars * sliderseq::ticksPerBar);

    juce::MidiFile file;
    file.setTicksPerQuarterNote (sliderseq::ticksPerQuarter);

    juce::MidiMessageSequence seq;
    const int microsecondsPerQuarter = static_cast<int> (std::lround (60000000.0 / clampedBpm));
    seq.addEvent (juce::MidiMessage::tempoMetaEvent (microsecondsPerQuarter), 0.0);

    // Collect only Note Ons starting inside [0, totalTicks) so prior-bar Note Offs never leak into tick 0.
    const auto renderState = sliderseq::toRenderState (state, sliderseq::PlaybackMode::gen);
    std::vector<sliderseq::OnsetEvent> onsets (16384);
    const size_t count = sliderseq::renderOnsetsInto (renderState, 0.0, totalTicks, onsets.data(), onsets.size());

    std::map<std::pair<int, int>, double> activeOffTick;
    for (size_t i = 0; i < count; ++i) {
        const auto& ev = onsets[i];
        const double onTick = std::clamp (ev.absoluteTick, 0.0, std::max (0.0, totalTicks - 2.0));
        const auto key = std::make_pair (ev.channel, ev.note);
        if (auto it = activeOffTick.find (key); it != activeOffTick.end()) {
            const double prevOff = std::min (it->second, std::max (0.0, onTick));
            seq.addEvent (juce::MidiMessage::noteOff (ev.channel, ev.note), prevOff);
            activeOffTick.erase (it);
        }
        seq.addEvent (juce::MidiMessage::noteOn (ev.channel, ev.note, static_cast<juce::uint8> (ev.velocity)), onTick);
        activeOffTick[key] = std::min (onTick + std::max (1.0, ev.gateTicks), totalTicks - 1.0);
    }
    for (const auto& [key, offTick] : activeOffTick)
        seq.addEvent (juce::MidiMessage::noteOff (key.first, key.second), offTick);

    seq.sort();
    seq.updateMatchedPairs();
    file.addTrack (seq);
    return file;
}

inline bool writeMidiFile (const sliderseq::State& state, const juce::File& destination,
                           int bars = 4, double bpm = 120.0) {
    const auto file = buildMidiFile (state, bars, bpm);
    juce::FileOutputStream stream (destination);
    if (! stream.openedOk())
        return false;
    stream.setPosition (0);
    stream.truncate();
    return file.writeTo (stream);
}

} // namespace midiexporter
