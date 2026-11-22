#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr std::array<double, 5> kRates { 1.0 / 3.0, 0.5, 1.0, 2.0, 3.0 };
constexpr std::array<int, 5> kGrids { 12, 16, 32, 48, 64 };
constexpr size_t kMidiBufferBytes = 32768;

inline void addNoteOnRaw (juce::MidiBuffer& buf, int channel, int note, int velocity, int sample) noexcept {
    const uint8_t bytes[3] {
        static_cast<uint8_t> (0x90u | static_cast<unsigned> (std::clamp (channel, 1, 16) - 1)),
        static_cast<uint8_t> (std::clamp (note, 0, 127)),
        static_cast<uint8_t> (std::clamp (velocity, 1, 127))
    };
    buf.addEvent (bytes, 3, sample);
}

inline void addNoteOffRaw (juce::MidiBuffer& buf, int channel, int note, int sample) noexcept {
    const uint8_t bytes[3] {
        static_cast<uint8_t> (0x80u | static_cast<unsigned> (std::clamp (channel, 1, 16) - 1)),
        static_cast<uint8_t> (std::clamp (note, 0, 127)),
        0u
    };
    buf.addEvent (bytes, 3, sample);
}

struct IncomingCC {
    int sample = 0;
    int cc = 0;
    int channel = 1;
    int value = 0;
};
} // namespace

juce::AudioProcessor::BusesProperties SliderRhythmMidiProcessor::makeBusesProperties() {
    if (juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_VST3)
        return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
    return {};
}

SliderRhythmMidiProcessor::SliderRhythmMidiProcessor()
    : AudioProcessor (makeBusesProperties()) {
    audioState = makeAudioState (messageState, 0);
    fullSyncSlots[0] = audioState;
    outputMidiA.ensureSize (kMidiBufferBytes);
    outputMidiB.ensureSize (kMidiBufferBytes);
}

bool SliderRhythmMidiProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const {
    if (! layouts.getMainInputChannelSet().isDisabled())
        return false;
    const auto output = layouts.getMainOutputChannelSet();
    if (wrapperType == wrapperType_VST3)
        return output == juce::AudioChannelSet::stereo() || output.isDisabled();
    return output.isDisabled();
}

void SliderRhythmMidiProcessor::prepareToPlay (double sampleRate, int) {
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    wasPlaying = false;
    hasExpectedTick = false;
    lastBpm = 0.0;
    pendingFlushOnNextBlock.store (true, std::memory_order_release);
    generatedNotes.store (0, std::memory_order_relaxed);
    outputMidiA.ensureSize (kMidiBufferBytes);
    outputMidiB.ensureSize (kMidiBufferBytes);
    swappedOutputOnce = false;
}

SliderRhythmMidiProcessor::FullAudioState SliderRhythmMidiProcessor::makeAudioState (const Snapshot& src, uint64_t seq) noexcept {
    FullAudioState out;
    out.seq = seq;
    out.render = sliderseq::toRenderState (src.sequence, src.playRecordedMidi);
    out.bindings = src.bindings;
    return out;
}

void SliderRhythmMidiProcessor::applyOpToAudio (const StateOp& op, FullAudioState& target) noexcept {
    auto& render = target.render;
    const int t = std::clamp (op.track, 0, sliderseq::maxTracks - 1);
    auto& track = render.tracks[static_cast<size_t> (t)];
    const int s = std::clamp (op.slotIndex, 0, sliderseq::slotsPerTrack - 1);

    switch (op.kind) {
        case OpKind::setTrackCount: render.trackCount = std::clamp (op.arg0, 1, sliderseq::maxTracks); break;
        case OpKind::setPlayRecordedMidi: render.playRecordedMidi = op.barg; break;
        case OpKind::setSlot: track.slot = std::clamp (op.arg0, 0, sliderseq::slotsPerTrack - 1); break;
        case OpKind::setRate: track.rate = op.darg; break;
        case OpKind::setVelocity: track.velocity = std::clamp (op.arg0, 1, 127); break;
        case OpKind::setGroove: track.groove = std::clamp (op.arg0, 0, 100); break;
        case OpKind::setModMode: track.modMode = std::clamp (op.arg0, 0, 2); break;
        case OpKind::setModAmount: track.modAmount = std::clamp (op.arg0, 0, 100); break;
        case OpKind::setMuted: track.muted = op.barg; break;
        case OpKind::setSolo: track.solo = op.barg; break;
        case OpKind::setExclusiveSolo:
            for (auto& other : render.tracks) other.solo = false;
            track.solo = true;
            track.muted = false;
            break;
        case OpKind::setMidiNote: track.midiNote = std::clamp (op.arg0, 0, 127); break;
        case OpKind::setMidiChannel: track.midiChannel = std::clamp (op.arg0, 1, 16); break;
        case OpKind::setStep:
            track.patterns[static_cast<size_t> (s)].setStep (op.arg0, op.barg);
            break;
        case OpKind::setPatternLength: {
            auto& pattern = track.patterns[static_cast<size_t> (s)];
            pattern.length = std::clamp (op.arg0, 1, std::clamp (pattern.grid, 1, 64));
            break;
        }
        case OpKind::resizePattern:
            track.patterns[static_cast<size_t> (s)].resize (op.arg0);
            break;
        case OpKind::setPattern:
            track.patterns[static_cast<size_t> (s)] = op.pattern;
            break;
        case OpKind::setBinding: {
            const int ctrl = std::clamp (op.slotIndex, 0, controlCount - 1);
            const int cc = std::clamp (op.arg0, -1, 127);
            const int ch = std::clamp (op.arg1, 0, 16);
            if (cc >= 0) {
                for (auto& tb : target.bindings)
                    for (auto& b : tb)
                        if (b.cc == cc && b.channel == ch) b = {};
            }
            target.bindings[static_cast<size_t> (t)][static_cast<size_t> (ctrl)] = { cc, ch };
            break;
        }
    }
}

void SliderRhythmMidiProcessor::applyOpToSnapshot (const StateOp& op, Snapshot& target) {
    const int t = std::clamp (op.track, 0, sliderseq::maxTracks - 1);
    auto& track = target.sequence.tracks[static_cast<size_t> (t)];
    const int s = std::clamp (op.slotIndex, 0, sliderseq::slotsPerTrack - 1);

    switch (op.kind) {
        case OpKind::setTrackCount: target.sequence.trackCount = std::clamp (op.arg0, 1, sliderseq::maxTracks); break;
        case OpKind::setPlayRecordedMidi: target.playRecordedMidi = op.barg; break;
        case OpKind::setSlot: track.slot = std::clamp (op.arg0, 0, sliderseq::slotsPerTrack - 1); break;
        case OpKind::setRate: track.rate = op.darg; break;
        case OpKind::setVelocity: track.velocity = std::clamp (op.arg0, 1, 127); break;
        case OpKind::setGroove: track.groove = std::clamp (op.arg0, 0, 100); break;
        case OpKind::setModMode: track.modMode = std::clamp (op.arg0, 0, 2); break;
        case OpKind::setModAmount: track.modAmount = std::clamp (op.arg0, 0, 100); break;
        case OpKind::setMuted: track.muted = op.barg; break;
        case OpKind::setSolo: track.solo = op.barg; break;
        case OpKind::setExclusiveSolo:
            for (auto& other : target.sequence.tracks) other.solo = false;
            track.solo = true;
            track.muted = false;
            break;
        case OpKind::setMidiNote: track.midiNote = std::clamp (op.arg0, 0, 127); break;
        case OpKind::setMidiChannel: track.midiChannel = std::clamp (op.arg0, 1, 16); break;
        case OpKind::setStep:
            if (op.arg0 >= 0 && op.arg0 < 64)
                track.patterns[static_cast<size_t> (s)].steps[static_cast<size_t> (op.arg0)] = op.barg;
            break;
        case OpKind::setPatternLength: {
            auto& pattern = track.patterns[static_cast<size_t> (s)];
            pattern.length = std::clamp (op.arg0, 1, std::clamp (pattern.grid, 1, 64));
            break;
        }
        case OpKind::resizePattern:
            track.patterns[static_cast<size_t> (s)].resize (std::clamp (op.arg0, 1, 64));
            break;
        case OpKind::setPattern:
            sliderseq::applyRenderPattern (op.pattern, track.patterns[static_cast<size_t> (s)]);
            break;
        case OpKind::setBinding: {
            const int ctrl = std::clamp (op.slotIndex, 0, controlCount - 1);
            const int cc = std::clamp (op.arg0, -1, 127);
            const int ch = std::clamp (op.arg1, 0, 16);
            if (cc >= 0) {
                for (auto& tb : target.bindings)
                    for (auto& b : tb)
                        if (b.cc == cc && b.channel == ch) b = {};
            }
            target.bindings[static_cast<size_t> (t)][static_cast<size_t> (ctrl)] = { cc, ch };
            break;
        }
    }
}

void SliderRhythmMidiProcessor::drainAudioToUiLocked() {
    const int ready = audioToUiFifo.getNumReady();
    if (ready <= 0) return;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    audioToUiFifo.prepareToRead (ready, start1, size1, start2, size2);
    for (int i = 0; i < size1; ++i)
        applyOpToSnapshot (audioToUiOps[static_cast<size_t> (start1 + i)], messageState);
    for (int i = 0; i < size2; ++i)
        applyOpToSnapshot (audioToUiOps[static_cast<size_t> (start2 + i)], messageState);
    audioToUiFifo.finishedRead (size1 + size2);
}

void SliderRhythmMidiProcessor::publishFullStateLocked() {
    ++messageSeq;
    fullSyncSlots[static_cast<size_t> (writerSyncSlot)] = makeAudioState (messageState, messageSeq);
    const uint32_t prev = sharedSyncSlot.exchange (static_cast<uint32_t> (writerSyncSlot) | kDirtyBit,
                                                   std::memory_order_acq_rel);
    writerSyncSlot = static_cast<int> (prev & 3u);
    version.fetch_add (1, std::memory_order_release);
}

void SliderRhythmMidiProcessor::pushAudioToUiOp (const StateOp& op) noexcept {
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    audioToUiFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0) {
        audioToUiOps[static_cast<size_t> (start1)] = op;
        audioToUiFifo.finishedWrite (1);
    } else if (size2 > 0) {
        audioToUiOps[static_cast<size_t> (start2)] = op;
        audioToUiFifo.finishedWrite (1);
    }
    version.fetch_add (1, std::memory_order_release);
}

std::shared_ptr<const SliderRhythmMidiProcessor::Snapshot> SliderRhythmMidiProcessor::snapshot() {
    std::lock_guard<std::mutex> lock (messageMutex);
    drainAudioToUiLocked();
    return std::make_shared<const Snapshot> (messageState);
}

void SliderRhythmMidiProcessor::edit (const std::function<void (Snapshot&)>& change) {
    std::lock_guard<std::mutex> lock (messageMutex);
    drainAudioToUiLocked();
    change (messageState);
    publishFullStateLocked();
}

void SliderRhythmMidiProcessor::applyUiOp (StateOp op) {
    std::lock_guard<std::mutex> lock (messageMutex);
    drainAudioToUiLocked();
    applyOpToSnapshot (op, messageState);
    op.seq = ++messageSeq;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    uiToAudioFifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 > 0) {
        uiToAudioOps[static_cast<size_t> (start1)] = op;
        uiToAudioFifo.finishedWrite (1);
        version.fetch_add (1, std::memory_order_release);
    } else if (size2 > 0) {
        uiToAudioOps[static_cast<size_t> (start2)] = op;
        uiToAudioFifo.finishedWrite (1);
        version.fetch_add (1, std::memory_order_release);
    } else {
        publishFullStateLocked();
    }
}

void SliderRhythmMidiProcessor::armMidiLearn (int track, Control control) {
    if (track < 0 || track >= sliderseq::maxTracks) return;
    learnTarget.store (track * controlCount + static_cast<int> (control), std::memory_order_release);
}

void SliderRhythmMidiProcessor::flushTrackNotes (int trackIndex, int samplePos, juce::MidiBuffer& outMidi) noexcept {
    for (auto& active : activeNotes) {
        if (! active.active || active.trackIndex != trackIndex) continue;
        addNoteOffRaw (outMidi, active.channel, active.note, samplePos);
        active.active = false;
    }
}

void SliderRhythmMidiProcessor::flushAllActiveNotes (int samplePos, juce::MidiBuffer& outMidi) noexcept {
    for (auto& active : activeNotes) {
        if (! active.active) continue;
        addNoteOffRaw (outMidi, active.channel, active.note, samplePos);
        active.active = false;
    }
}

void SliderRhythmMidiProcessor::flushRemovedOrChangedTracks (const sliderseq::RenderState& before,
                                                             const sliderseq::RenderState& after,
                                                             int samplePos,
                                                             juce::MidiBuffer& outMidi) noexcept {
    bool beforeAnySolo = false, afterAnySolo = false;
    for (int i = 0; i < before.trackCount; ++i)
        if (before.tracks[static_cast<size_t> (i)].solo) { beforeAnySolo = true; break; }
    for (int i = 0; i < after.trackCount; ++i)
        if (after.tracks[static_cast<size_t> (i)].solo) { afterAnySolo = true; break; }

    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const bool wasAudible = sliderseq::isTrackAudible (before, i, beforeAnySolo);
        const bool nowAudible = sliderseq::isTrackAudible (after, i, afterAnySolo);
        const auto& bt = before.tracks[static_cast<size_t> (i)];
        const auto& at = after.tracks[static_cast<size_t> (i)];
        if ((wasAudible && ! nowAudible) || i >= after.trackCount
            || bt.midiNote != at.midiNote || bt.midiChannel != at.midiChannel) {
            flushTrackNotes (i, samplePos, outMidi);
        }
    }
}

void SliderRhythmMidiProcessor::syncFromUi (juce::MidiBuffer& outMidi) noexcept {
    const sliderseq::RenderState before = audioState.render;
    bool changed = false;

    if ((sharedSyncSlot.load (std::memory_order_acquire) & kDirtyBit) != 0u) {
        const uint32_t prev = sharedSyncSlot.exchange (static_cast<uint32_t> (readerSyncSlot),
                                                       std::memory_order_acq_rel);
        readerSyncSlot = static_cast<int> (prev & 3u);
        audioState = fullSyncSlots[static_cast<size_t> (readerSyncSlot)];
        changed = true;
    }

    const int ready = uiToAudioFifo.getNumReady();
    if (ready > 0) {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        uiToAudioFifo.prepareToRead (ready, start1, size1, start2, size2);
        auto consume = [&] (int start, int count) {
            for (int i = 0; i < count; ++i) {
                const auto& op = uiToAudioOps[static_cast<size_t> (start + i)];
                if (op.seq > audioState.seq) {
                    applyOpToAudio (op, audioState);
                    audioState.seq = op.seq;
                    changed = true;
                }
            }
        };
        consume (start1, size1);
        consume (start2, size2);
        uiToAudioFifo.finishedRead (size1 + size2);
    }

    if (changed)
        flushRemovedOrChangedTracks (before, audioState.render, 0, outMidi);
}

void SliderRhythmMidiProcessor::applyIncomingCC (int cc, int ch, int value, int samplePos, juce::MidiBuffer& outMidi) noexcept {
    const int target = learnTarget.exchange (-1, std::memory_order_acq_rel);
    if (target >= 0) {
        StateOp op;
        op.kind = OpKind::setBinding;
        op.track = target / controlCount;
        op.slotIndex = target % controlCount;
        op.arg0 = cc;
        op.arg1 = ch;
        applyOpToAudio (op, audioState);
        pushAudioToUiOp (op);
        return;
    }

    const sliderseq::RenderState before = audioState.render;
    bool anyMatched = false;

    for (int t = 0; t < audioState.render.trackCount; ++t) {
        const auto& track = audioState.render.tracks[static_cast<size_t> (t)];
        for (int ctrl = 0; ctrl < controlCount; ++ctrl) {
            const auto& binding = audioState.bindings[static_cast<size_t> (t)][static_cast<size_t> (ctrl)];
            if (binding.cc != cc || binding.channel != ch) continue;

            StateOp op;
            op.track = t;
            op.slotIndex = std::clamp (track.slot, 0, sliderseq::slotsPerTrack - 1);
            switch (static_cast<Control> (ctrl)) {
                case slot:
                    op.kind = OpKind::setSlot;
                    op.arg0 = static_cast<int> (std::lround (value * 11.0 / 127.0));
                    break;
                case rate:
                    op.kind = OpKind::setRate;
                    op.darg = kRates[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (value * 4.0 / 127.0)), 0, 4))];
                    break;
                case velocity:
                    op.kind = OpKind::setVelocity;
                    op.arg0 = std::clamp (value, 1, 127);
                    break;
                case groove:
                    op.kind = OpKind::setGroove;
                    op.arg0 = static_cast<int> (std::lround (value * 100.0 / 127.0));
                    break;
                case modAmount:
                    op.kind = OpKind::setModAmount;
                    op.arg0 = static_cast<int> (std::lround (value * 100.0 / 127.0));
                    break;
                case mute:
                    op.kind = OpKind::setMuted;
                    op.barg = value >= 64;
                    break;
                case modMode:
                    op.kind = OpKind::setModMode;
                    op.arg0 = static_cast<int> (std::lround (value * 2.0 / 127.0));
                    break;
                case note:
                    op.kind = OpKind::setMidiNote;
                    op.arg0 = value;
                    break;
                case channel:
                    op.kind = OpKind::setMidiChannel;
                    op.arg0 = 1 + static_cast<int> (std::lround (value * 15.0 / 127.0));
                    break;
                case length: {
                    const auto& pat = track.patterns[static_cast<size_t> (op.slotIndex)];
                    op.kind = OpKind::setPatternLength;
                    op.arg0 = 1 + static_cast<int> (std::lround (value * (pat.grid - 1) / 127.0));
                    break;
                }
                case grid:
                    op.kind = OpKind::resizePattern;
                    op.arg0 = kGrids[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (value * 4.0 / 127.0)), 0, 4))];
                    break;
                case controlCount:
                    continue;
            }
            applyOpToAudio (op, audioState);
            pushAudioToUiOp (op);
            anyMatched = true;
        }
    }

    if (anyMatched)
        flushRemovedOrChangedTracks (before, audioState.render, samplePos, outMidi);
}

void SliderRhythmMidiProcessor::advanceActiveNotes (int segStart, int dtSamples, bool inclusiveEnd,
                                                    juce::MidiBuffer& outMidi) noexcept {
    if (dtSamples < 0) return;
    for (auto& active : activeNotes) {
        if (! active.active) continue;
        const bool due = inclusiveEnd ? (active.remainingSamples <= static_cast<double> (dtSamples))
                                      : (active.remainingSamples < static_cast<double> (dtSamples));
        if (due) {
            const int offset = std::clamp (static_cast<int> (std::lround (active.remainingSamples)),
                                           0, std::max (0, dtSamples - (inclusiveEnd ? 0 : 1)));
            addNoteOffRaw (outMidi, active.channel, active.note, segStart + offset);
            active.active = false;
        } else {
            active.remainingSamples -= static_cast<double> (dtSamples);
        }
    }
}

void SliderRhythmMidiProcessor::renderSegment (double segStartTick, int segStartSample, int segSamples,
                                               double ticksPerSample, juce::MidiBuffer& outMidi) noexcept {
    if (segSamples <= 0) return;
    const double segEndTick = segStartTick + ticksPerSample * static_cast<double> (segSamples);
    const size_t onsetCount = sliderseq::renderOnsetsInto (
        audioState.render, segStartTick, segEndTick, onsetScratch.data(), onsetScratch.size());

    int cursor = 0;
    for (size_t i = 0; i < onsetCount; ++i) {
        const auto& ev = onsetScratch[i];
        const int onsetRel = std::clamp (static_cast<int> (std::lround ((ev.absoluteTick - segStartTick) / ticksPerSample)),
                                         cursor, segSamples - 1);
        advanceActiveNotes (segStartSample + cursor, onsetRel - cursor, true, outMidi);
        cursor = onsetRel;

        // Retrigger on same (channel, note): emit Note Off before Note On.
        for (auto& active : activeNotes) {
            if (active.active && active.channel == ev.channel && active.note == ev.note) {
                addNoteOffRaw (outMidi, active.channel, active.note, segStartSample + onsetRel);
                active.active = false;
            }
        }

        // Reserve a scheduler slot before emitting Note On so Note Off is never dropped.
        ActiveNote* freeSlot = nullptr;
        for (auto& slotEntry : activeNotes) {
            if (! slotEntry.active) { freeSlot = &slotEntry; break; }
        }
        if (freeSlot == nullptr) continue;

        addNoteOnRaw (outMidi, ev.channel, ev.note, ev.velocity, segStartSample + onsetRel);
        generatedNotes.fetch_add (1, std::memory_order_relaxed);
        freeSlot->trackIndex = ev.trackIndex;
        freeSlot->channel = ev.channel;
        freeSlot->note = ev.note;
        freeSlot->remainingSamples = std::max (1.0, ev.gateTicks / ticksPerSample);
        freeSlot->active = true;
    }

    advanceActiveNotes (segStartSample + cursor, segSamples - cursor, false, outMidi);
}

juce::MidiBuffer& SliderRhythmMidiProcessor::acquireOutputBuffer() noexcept {
    auto& buf = swappedOutputOnce ? outputMidiB : outputMidiA;
    buf.clear();
    return buf;
}

void SliderRhythmMidiProcessor::finalizeOutputBuffer (juce::MidiBuffer& hostMidi, juce::MidiBuffer& built) noexcept {
    hostMidi.swapWith (built);
    swappedOutputOnce = true;
}

template<typename Sample>
void SliderRhythmMidiProcessor::renderBlock (juce::AudioBuffer<Sample>& audio, juce::MidiBuffer& midi) {
    audio.clear();
    const int numSamples = audio.getNumSamples();
    auto& outMidi = acquireOutputBuffer();

    if (pendingFlushOnNextBlock.exchange (false, std::memory_order_acq_rel))
        flushAllActiveNotes (0, outMidi);

    syncFromUi (outMidi);

    // Collect incoming CCs on the stack with zero allocations, keeping sample order.
    std::array<IncomingCC, 256> incomingCCs {};
    size_t ccCount = 0;
    for (const auto metadata : midi) {
        if (metadata.numBytes >= 3 && (metadata.data[0] & 0xf0u) == 0xb0u && ccCount < incomingCCs.size()) {
            incomingCCs[ccCount++] = {
                std::clamp (metadata.samplePosition, 0, std::max (0, numSamples - 1)),
                static_cast<int> (metadata.data[1] & 0x7fu),
                static_cast<int> ((metadata.data[0] & 0x0fu) + 1),
                static_cast<int> (metadata.data[2] & 0x7fu)
            };
        }
    }

    const auto* playhead = getPlayHead();
    const auto position = playhead != nullptr ? playhead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};
    const bool isLogicAu = wrapperType == wrapperType_AudioUnit && juce::PluginHostType().isLogic();
    // Logic may report recording without setting isPlaying on a MIDI FX playhead.
    const bool playing = position && (position->getIsPlaying() || (isLogicAu && position->getIsRecording()));
    hostPlaying.store (playing, std::memory_order_relaxed);
    hostRecording.store (position && position->getIsRecording(), std::memory_order_relaxed);
    hostPositionAvailable.store (position && position->getPpqPosition() && position->getBpm(), std::memory_order_relaxed);

    if (audioState.render.playRecordedMidi) {
        for (size_t i = 0; i < ccCount; ++i)
            applyIncomingCC (incomingCCs[i].cc, incomingCCs[i].channel, incomingCCs[i].value, incomingCCs[i].sample, outMidi);
        flushAllActiveNotes (0, outMidi);
        for (const auto metadata : midi)
            outMidi.addEvent (metadata.data, metadata.numBytes, metadata.samplePosition);
        wasPlaying = false;
        hasExpectedTick = false;
        finalizeOutputBuffer (midi, outMidi);
        return;
    }

    if (! playing || ! position || ! position->getPpqPosition() || ! position->getBpm() || numSamples <= 0) {
        for (size_t i = 0; i < ccCount; ++i)
            applyIncomingCC (incomingCCs[i].cc, incomingCCs[i].channel, incomingCCs[i].value, incomingCCs[i].sample, outMidi);
        if (wasPlaying)
            flushAllActiveNotes (0, outMidi);
        else
            flushAllActiveNotes (0, outMidi);
        wasPlaying = false;
        hasExpectedTick = false;
        finalizeOutputBuffer (midi, outMidi);
        return;
    }

    const double bpm = *position->getBpm();
    const double startTick = *position->getPpqPosition() * sliderseq::ticksPerQuarter;
    if (bpm <= 0.0 || currentSampleRate <= 0.0) {
        for (size_t i = 0; i < ccCount; ++i)
            applyIncomingCC (incomingCCs[i].cc, incomingCCs[i].channel, incomingCCs[i].value, incomingCCs[i].sample, outMidi);
        finalizeOutputBuffer (midi, outMidi);
        return;
    }

    if (lastBpm > 0.0 && std::abs (bpm - lastBpm) > 1e-6) {
        const double scale = lastBpm / bpm;
        for (auto& active : activeNotes)
            if (active.active) active.remainingSamples = std::max (1.0, active.remainingSamples * scale);
    }
    lastBpm = bpm;

    const double ticksPerSample = bpm * sliderseq::ticksPerQuarter / (60.0 * currentSampleRate);
    const bool looping = position->getIsLooping() && position->getLoopPoints().hasValue();
    const double loopStartTick = looping ? position->getLoopPoints()->ppqStart * sliderseq::ticksPerQuarter : 0.0;
    const double loopEndTick = looping ? position->getLoopPoints()->ppqEnd * sliderseq::ticksPerQuarter : 0.0;
    const bool validLoop = looping && loopEndTick > loopStartTick;

    if (hasExpectedTick) {
        const double tolerance = std::max (2.0, ticksPerSample * 4.0);
        const bool normalStep = std::abs (startTick - expectedNextTick) <= tolerance;
        const bool seamWrap = validLoop
            && std::abs (startTick - loopStartTick) <= tolerance
            && std::abs (expectedNextTick - loopEndTick) <= tolerance;
        if (! normalStep && ! seamWrap)
            flushAllActiveNotes (0, outMidi);
    }
    wasPlaying = true;

    // Render [0, numSamples) in sub-segments split by CC timestamps, mid-block loop wraps, and max 256-sample chunks.
    int curSample = 0;
    double curTick = startTick;
    size_t ccIdx = 0;
    constexpr int kMaxChunkSamples = 256;

    while (curSample < numSamples) {
        while (ccIdx < ccCount && incomingCCs[ccIdx].sample <= curSample) {
            applyIncomingCC (incomingCCs[ccIdx].cc, incomingCCs[ccIdx].channel, incomingCCs[ccIdx].value, curSample, outMidi);
            ++ccIdx;
        }
        if (audioState.render.playRecordedMidi) {
            flushAllActiveNotes (curSample, outMidi);
            break;
        }

        int nextSample = std::min (numSamples, curSample + kMaxChunkSamples);
        if (ccIdx < ccCount && incomingCCs[ccIdx].sample < nextSample)
            nextSample = incomingCCs[ccIdx].sample;

        const double segEndTick = curTick + ticksPerSample * static_cast<double> (nextSample - curSample);
        if (validLoop && curTick < loopEndTick && segEndTick > loopEndTick) {
            const int beforeWrap = std::clamp (static_cast<int> (std::floor ((loopEndTick - curTick) / ticksPerSample)),
                                               1, nextSample - curSample);
            renderSegment (curTick, curSample, beforeWrap, ticksPerSample, outMidi);
            curSample += beforeWrap;
            curTick = loopStartTick;
            continue;
        }

        renderSegment (curTick, curSample, nextSample - curSample, ticksPerSample, outMidi);
        curTick += ticksPerSample * static_cast<double> (nextSample - curSample);
        curSample = nextSample;
    }

    while (ccIdx < ccCount) {
        applyIncomingCC (incomingCCs[ccIdx].cc, incomingCCs[ccIdx].channel, incomingCCs[ccIdx].value,
                         std::max (0, numSamples - 1), outMidi);
        ++ccIdx;
    }

    expectedNextTick = curTick;
    hasExpectedTick = true;
    finalizeOutputBuffer (midi, outMidi);
}

template<typename Sample>
void SliderRhythmMidiProcessor::renderBypassed (juce::AudioBuffer<Sample>& audio, juce::MidiBuffer& midi) {
    audio.clear();
    auto& outMidi = acquireOutputBuffer();
    pendingFlushOnNextBlock.store (false, std::memory_order_relaxed);
    // Emit owned Note Offs BEFORE forwarding incoming passthrough messages at sample 0.
    flushAllActiveNotes (0, outMidi);
    for (const auto metadata : midi)
        outMidi.addEvent (metadata.data, metadata.numBytes, metadata.samplePosition);
    wasPlaying = false;
    hasExpectedTick = false;
    finalizeOutputBuffer (midi, outMidi);
}

void SliderRhythmMidiProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) { renderBlock (audio, midi); }
void SliderRhythmMidiProcessor::processBlock (juce::AudioBuffer<double>& audio, juce::MidiBuffer& midi) { renderBlock (audio, midi); }
void SliderRhythmMidiProcessor::processBlockBypassed (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) { renderBypassed (audio, midi); }
void SliderRhythmMidiProcessor::processBlockBypassed (juce::AudioBuffer<double>& audio, juce::MidiBuffer& midi) { renderBypassed (audio, midi); }

void SliderRhythmMidiProcessor::getStateInformation (juce::MemoryBlock& destination) {
    const auto current = snapshot();
    juce::XmlElement root ("SliderRhythmMidi");
    root.setAttribute ("version", 2);
    root.setAttribute ("trackCount", current->sequence.trackCount);
    root.setAttribute ("playRecordedMidi", current->playRecordedMidi);
    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& track = current->sequence.tracks[static_cast<size_t> (i)];
        auto* element = root.createNewChildElement ("Track");
        element->setAttribute ("index", i);
        element->setAttribute ("name", track.name);
        element->setAttribute ("slot", track.slot);
        element->setAttribute ("rate", track.rate);
        element->setAttribute ("note", track.midiNote);
        element->setAttribute ("channel", track.midiChannel);
        element->setAttribute ("velocity", track.velocity);
        element->setAttribute ("groove", track.groove);
        element->setAttribute ("modMode", track.modMode);
        element->setAttribute ("modAmount", track.modAmount);
        element->setAttribute ("muted", track.muted);
        element->setAttribute ("solo", track.solo);
        for (int slotIdx = 0; slotIdx < sliderseq::slotsPerTrack; ++slotIdx) {
            const auto& pattern = track.patterns[static_cast<size_t> (slotIdx)];
            auto* child = element->createNewChildElement ("Pattern");
            child->setAttribute ("slot", slotIdx);
            child->setAttribute ("name", pattern.name);
            child->setAttribute ("grid", pattern.grid);
            child->setAttribute ("subdivision", pattern.subdivision);
            child->setAttribute ("length", pattern.length);
            juce::String steps;
            for (int step = 0; step < 64; ++step) steps += pattern.steps[static_cast<size_t> (step)] ? "1" : "0";
            child->setAttribute ("steps", steps);
        }
        for (int ctrl = 0; ctrl < controlCount; ++ctrl) {
            const auto& binding = current->bindings[static_cast<size_t> (i)][static_cast<size_t> (ctrl)];
            if (binding.cc < 0) continue;
            auto* child = element->createNewChildElement ("Binding");
            child->setAttribute ("control", ctrl);
            child->setAttribute ("cc", binding.cc);
            child->setAttribute ("channel", binding.channel);
        }
    }
    copyXmlToBinary (root, destination);
}

void SliderRhythmMidiProcessor::setStateInformation (const void* data, int size) {
    const auto xml = getXmlFromBinary (data, size);
    if (! xml || ! xml->hasTagName ("SliderRhythmMidi")) return;
    Snapshot restored;
    restored.sequence.trackCount = std::clamp (xml->getIntAttribute ("trackCount", 4), 1, sliderseq::maxTracks);
    restored.playRecordedMidi = xml->getBoolAttribute ("playRecordedMidi", false);
    for (auto* element : xml->getChildIterator()) {
        if (! element->hasTagName ("Track")) continue;
        const int index = element->getIntAttribute ("index", -1);
        if (index < 0 || index >= sliderseq::maxTracks) continue;
        auto& track = restored.sequence.tracks[static_cast<size_t> (index)];
        track.name = element->getStringAttribute ("name", track.name).toStdString();
        track.slot = std::clamp (element->getIntAttribute ("slot", track.slot), 0, 11);
        const auto requestedRate = element->getDoubleAttribute ("rate", 1.0);
        track.rate = *std::min_element (kRates.begin(), kRates.end(), [=] (double a, double b) {
            return std::abs (a - requestedRate) < std::abs (b - requestedRate);
        });
        track.midiNote = std::clamp (element->getIntAttribute ("note", track.midiNote), 0, 127);
        track.midiChannel = std::clamp (element->getIntAttribute ("channel", track.midiChannel), 1, 16);
        track.velocity = std::clamp (element->getIntAttribute ("velocity", track.velocity), 1, 127);
        track.groove = std::clamp (element->getIntAttribute ("groove", track.groove), 0, 100);
        track.modMode = std::clamp (element->getIntAttribute ("modMode", track.modMode), 0, 2);
        track.modAmount = std::clamp (element->getIntAttribute ("modAmount", track.modAmount), 0, 100);
        track.muted = element->getBoolAttribute ("muted", false);
        track.solo = element->getBoolAttribute ("solo", false);
        for (auto* child : element->getChildIterator()) {
            if (child->hasTagName ("Pattern")) {
                const int slotIdx = child->getIntAttribute ("slot", -1);
                if (slotIdx < 0 || slotIdx >= sliderseq::slotsPerTrack) continue;
                auto& pattern = track.patterns[static_cast<size_t> (slotIdx)];
                pattern.name = child->getStringAttribute ("name", pattern.name).toStdString();
                pattern.grid = std::clamp (child->getIntAttribute ("grid", 16), 1, 64);
                pattern.subdivision = std::clamp (child->getIntAttribute ("subdivision", pattern.grid), 1, 64);
                const int savedLength = child->getIntAttribute ("length", 16);
                pattern.length = child->hasAttribute ("subdivision")
                    ? std::clamp (savedLength, 1, pattern.grid)
                    : sliderseq::legacyStepCount (pattern.grid, std::clamp (savedLength, 1, 16));
                const auto steps = child->getStringAttribute ("steps");
                for (int step = 0; step < 64; ++step)
                    pattern.steps[static_cast<size_t> (step)] = step < steps.length() && steps[step] == '1';
            } else if (child->hasTagName ("Binding")) {
                const int ctrl = child->getIntAttribute ("control", -1);
                if (ctrl < 0 || ctrl >= controlCount) continue;
                auto& binding = restored.bindings[static_cast<size_t> (index)][static_cast<size_t> (ctrl)];
                binding.cc = std::clamp (child->getIntAttribute ("cc", -1), -1, 127);
                binding.channel = std::clamp (child->getIntAttribute ("channel", 0), 0, 16);
            }
        }
    }
    std::lock_guard<std::mutex> lock (messageMutex);
    drainAudioToUiLocked();
    messageState = std::move (restored);
    publishFullStateLocked();
}

juce::AudioProcessorEditor* SliderRhythmMidiProcessor::createEditor() { return new SliderRhythmMidiEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SliderRhythmMidiProcessor(); }
