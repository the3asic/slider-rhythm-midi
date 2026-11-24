#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "SequencerEngine.h"

namespace midiexporter {

inline bool isValidTimeSigDenominator (int den) noexcept {
    return den >= 1 && den <= 256 && (den & (den - 1)) == 0;
}

inline juce::MidiFile buildMidiFile (const sliderseq::State& state,
                                     int bars = 4,
                                     double bpm = 120.0,
                                     int timeSigNumerator = 4,
                                     int timeSigDenominator = 4) {
    const int clampedBars = std::clamp (bars, 1, 16);
    const int num = std::clamp (timeSigNumerator, 1, 32);
    const int den = isValidTimeSigDenominator (timeSigDenominator) ? timeSigDenominator : 4;
    // Clamp only to valid MIDI 24-bit microseconds-per-quarter range (~3.58..1000 BPM).
    const double safeBpm = std::isfinite (bpm) ? std::clamp (bpm, 4.0, 1000.0) : 120.0;
    const double quartersPerBar = static_cast<double> (num) * (4.0 / static_cast<double> (den));
    const double totalTicks = static_cast<double> (clampedBars) * quartersPerBar * static_cast<double> (sliderseq::ticksPerQuarter);

    juce::MidiFile file;
    file.setTicksPerQuarterNote (sliderseq::ticksPerQuarter);

    juce::MidiMessageSequence seq;
    const int microsecondsPerQuarter = std::clamp (static_cast<int> (std::lround (60000000.0 / safeBpm)), 1, 0x00ffffff);
    seq.addEvent (juce::MidiMessage::tempoMetaEvent (microsecondsPerQuarter), 0.0);
    seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (num, den), 0.0);

    // Unbounded offline rendering so dense 16-track 8-bar exports never silently truncate.
    const auto renderState = sliderseq::toRenderState (state, sliderseq::PlaybackMode::gen);
    const auto onsets = sliderseq::renderOnsetsVector (renderState, 0.0, totalTicks);

    std::map<std::pair<int, int>, double> activeOffTick;
    for (const auto& ev : onsets) {
        const double onTick = std::clamp (ev.absoluteTick, 0.0, std::max (0.0, totalTicks - 2.0));
        const auto key = std::make_pair (ev.channel, ev.note);
        if (auto it = activeOffTick.find (key); it != activeOffTick.end()) {
            const double prevOff = std::min (it->second, std::max (0.0, onTick));
            seq.addEvent (juce::MidiMessage::noteOff (ev.channel, ev.note), prevOff);
            activeOffTick.erase (it);
        }
        seq.addEvent (juce::MidiMessage::noteOn (ev.channel, ev.note, static_cast<juce::uint8> (ev.velocity)), onTick);
        activeOffTick[key] = std::min (onTick + std::max (1.0, ev.gateTicks), std::max (0.0, totalTicks - 1.0));
    }
    for (const auto& [key, offTick] : activeOffTick)
        seq.addEvent (juce::MidiMessage::noteOff (key.first, key.second), offTick);

    // Explicit End-of-Track at exact requested clip length so sparse or empty clips keep exact bar length.
    seq.addEvent (juce::MidiMessage::endOfTrack(), totalTicks);
    seq.sort();
    seq.updateMatchedPairs();
    file.addTrack (seq);
    return file;
}

inline bool writeMidiFile (const sliderseq::State& state,
                           const juce::File& destination,
                           int bars = 4,
                           double bpm = 120.0,
                           int timeSigNumerator = 4,
                           int timeSigDenominator = 4) {
    if (destination.getParentDirectory().createDirectory().failed())
        return false;

    const auto file = buildMidiFile (state, bars, bpm, timeSigNumerator, timeSigDenominator);
    const auto tmpFile = destination.getSiblingFile (destination.getFileName() + ".tmp." + juce::Uuid().toString());
    bool wroteOk = false;
    {
        juce::FileOutputStream stream (tmpFile);
        if (stream.openedOk()
            && stream.setPosition (0)
            && ! stream.truncate().failed()
            && file.writeTo (stream)) {
            stream.flush();
            wroteOk = ! stream.getStatus().failed();
        }
    }
    if (! wroteOk) {
        tmpFile.deleteFile();
        return false;
    }

    // Atomic same-volume replacement that preserves an existing destination if the rename fails.
    bool movedOk = false;
#if JUCE_WINDOWS
    movedOk = ::MoveFileExW (tmpFile.getFullPathName().toWideCharPointer(),
                             destination.getFullPathName().toWideCharPointer(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    movedOk = (std::rename (tmpFile.getFullPathName().toRawUTF8(),
                            destination.getFullPathName().toRawUTF8()) == 0);
#endif
    if (! movedOk) {
        tmpFile.deleteFile();
        return false;
    }
    return destination.existsAsFile() && destination.getSize() > 0;
}

} // namespace midiexporter
