#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr std::array<double, 5> kRates { 1.0 / 3.0, 0.5, 1.0, 2.0, 3.0 };
constexpr std::array<int, 5> kGrids { 12, 16, 32, 48, 64 };
constexpr size_t kMidiBufferBytes = 32768;

int rateToIndex (double rate) noexcept {
    int best = 2;
    double bestDiff = std::abs (kRates[2] - rate);
    for (int i = 0; i < 5; ++i) {
        const double diff = std::abs (kRates[static_cast<size_t> (i)] - rate);
        if (diff < bestDiff) { bestDiff = diff; best = i; }
    }
    return best;
}

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

inline sliderseq::PlaybackMode clampPlaybackMode (int raw) noexcept {
    return static_cast<sliderseq::PlaybackMode> (std::clamp (raw, 0, 2));
}
} // namespace

juce::AudioProcessor::BusesProperties SliderRhythmMidiProcessor::makeBusesProperties() {
    if (juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_VST3)
        return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
    return {};
}

juce::String SliderRhythmMidiProcessor::paramId (int trackIndex, const char* suffix) {
    const int n = std::clamp (trackIndex, 0, sliderseq::maxTracks - 1) + 1;
    return "t" + juce::String (n).paddedLeft ('0', 2) + "_" + suffix;
}

juce::AudioProcessorValueTreeState::ParameterLayout SliderRhythmMidiProcessor::createParameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const juce::StringArray rateNames { "1/3", "1/2", "1x", "2x", "3x" };
    auto trackPrefix = [] (int i) {
        return "T" + juce::String (i + 1).paddedLeft ('0', 2) + " ";
    };

    // Global master Shuffle (-100) <-> Straight (0) <-> Swing (+100).
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { globalSwingParamId(), 1 }, "Global Swing", -100, 100, 0,
        juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) {
            if (v > 0) return "+" + juce::String (v) + "%";
            return juce::String (v) + "%";
        })));

    // Register 128 per-track automatable performance parameters grouped by control (8 pages of 16 tracks).
    // Slot 0 = EMPTY (silent live floor), 1..12 = Pattern 1..12.
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "slot"), 1 }, trackPrefix (i) + "Slot", 0, 12, 1,
            juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) {
                return v <= 0 ? juce::String ("EMPTY") : juce::String (v);
            })));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { paramId (i, "mute"), 1 }, trackPrefix (i) + "Mute", false));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { paramId (i, "solo"), 1 }, trackPrefix (i) + "Solo", false));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "velocity"), 1 }, trackPrefix (i) + "Velocity", 1, 127, 100));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { paramId (i, "rate"), 1 }, trackPrefix (i) + "Rate", rateNames, 2));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "groove"), 1 }, trackPrefix (i) + "Swing %", 0, 100, 100));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "mod_amt"), 1 }, trackPrefix (i) + "Mod Amt", 0, 100, 50));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "loop_steps"), 1 }, trackPrefix (i) + "Loop Steps", 0, 64, 0));
    return layout;
}

SliderRhythmMidiProcessor::SliderRhythmMidiProcessor()
    : AudioProcessor (makeBusesProperties()),
      apvts (*this, nullptr, "Parameters", createParameterLayout()) {
    globalSwingParam = apvts.getRawParameterValue (globalSwingParamId());
    lastHostGlobalSwing = globalSwingParam != nullptr ? globalSwingParam->load() : 0.0f;
    lastSnapshotGlobalSwing = lastHostGlobalSwing;

    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        activeSteps[static_cast<size_t> (i)].store (-1, std::memory_order_relaxed);
        auto& ptrs = paramPtrs[static_cast<size_t> (i)];
        ptrs.slot = apvts.getRawParameterValue (paramId (i, "slot"));
        ptrs.mute = apvts.getRawParameterValue (paramId (i, "mute"));
        ptrs.solo = apvts.getRawParameterValue (paramId (i, "solo"));
        ptrs.velocity = apvts.getRawParameterValue (paramId (i, "velocity"));
        ptrs.rate = apvts.getRawParameterValue (paramId (i, "rate"));
        ptrs.groove = apvts.getRawParameterValue (paramId (i, "groove"));
        ptrs.modAmount = apvts.getRawParameterValue (paramId (i, "mod_amt"));
        ptrs.loopSteps = apvts.getRawParameterValue (paramId (i, "loop_steps"));

        const std::array<float, 8> initVals {
            ptrs.slot->load(), ptrs.mute->load(), ptrs.solo->load(), ptrs.velocity->load(),
            ptrs.rate->load(), ptrs.groove->load(), ptrs.modAmount->load(), ptrs.loopSteps->load()
        };
        lastHostParamValues[static_cast<size_t> (i)] = initVals;
        lastSnapshotHostValues[static_cast<size_t> (i)] = initVals;
    }
    audioState = makeAudioState (messageState, 0);
    fullSyncSlots[0] = audioState;
    outputMidiA.ensureSize (kMidiBufferBytes);
    outputMidiB.ensureSize (kMidiBufferBytes);
}

void SliderRhythmMidiProcessor::setGlobalSwingValue (int plainValue) {
    const int clamped = std::clamp (plainValue, -100, 100);
    if (auto* param = apvts.getParameter (globalSwingParamId())) {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (clamped)));
        param->endChangeGesture();
    }
    {
        std::lock_guard<std::mutex> lock (messageMutex);
        if (globalSwingParam != nullptr)
            lastSnapshotGlobalSwing = globalSwingParam->load (std::memory_order_relaxed);
    }
    StateOp op;
    op.kind = OpKind::setGlobalSwing;
    op.arg0 = clamped;
    applyUiOp (op);
}

void SliderRhythmMidiProcessor::setTrackActiveZones (int trackIndex, int zones) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
    const int clampedZones = std::clamp (zones, 1, sliderseq::slotsPerTrack);
    StateOp op;
    op.kind = OpKind::setActiveZones;
    op.track = trackIndex;
    op.arg0 = clampedZones;
    applyUiOp (op);

    // Keep APVTS slot consistent if the current slot exceeded the newly reduced zone count.
    const auto cur = snapshot();
    const int curSlot = cur->sequence.tracks[static_cast<size_t> (trackIndex)].slot;
    if (auto* slotParam = apvts.getParameter (paramId (trackIndex, "slot"))) {
        const float norm = slotParam->convertTo0to1 (static_cast<float> (curSlot));
        if (std::abs (slotParam->getValue() - norm) > 1e-6f) {
            slotParam->beginChangeGesture();
            slotParam->setValueNotifyingHost (norm);
            slotParam->endChangeGesture();
        }
    }
    {
        std::lock_guard<std::mutex> lock (messageMutex);
        lastSnapshotHostValues[static_cast<size_t> (trackIndex)][0] =
            paramPtrs[static_cast<size_t> (trackIndex)].slot->load (std::memory_order_relaxed);
    }
}

void SliderRhythmMidiProcessor::loadMappingPreset (const mappingpresets::Preset& preset) {
    edit ([&preset] (Snapshot& s) {
        mappingpresets::applyToState (preset, s.sequence);
        s.activeMappingPreset = preset;
    });
}

void SliderRhythmMidiProcessor::setActiveMappingPresetBaseline (const mappingpresets::Preset& preset) {
    edit ([&preset] (Snapshot& s) {
        s.activeMappingPreset = preset;
    });
}

void SliderRhythmMidiProcessor::setHostParamValue (int trackIndex, const char* suffix, float plainValue) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
    const juce::String key (suffix);
    float effectiveValue = plainValue;
    if (key == "slot") {
        const int maxZone = snapshot()->sequence.tracks[static_cast<size_t> (trackIndex)].activeZones;
        effectiveValue = static_cast<float> (std::clamp (static_cast<int> (std::lround (plainValue)), 0, maxZone));
    }

    if (auto* param = apvts.getParameter (paramId (trackIndex, suffix))) {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (effectiveValue));
        param->endChangeGesture();
    }

    StateOp op;
    op.track = trackIndex;
    int paramSlotIdx = -1;
    bool mapped = true;
    if (key == "slot") { op.kind = OpKind::setSlot; op.arg0 = static_cast<int> (std::lround (effectiveValue)); paramSlotIdx = 0; }
    else if (key == "mute") { op.kind = OpKind::setMuted; op.barg = effectiveValue >= 0.5f; paramSlotIdx = 1; }
    else if (key == "solo") { op.kind = OpKind::setSolo; op.barg = effectiveValue >= 0.5f; paramSlotIdx = 2; }
    else if (key == "velocity") { op.kind = OpKind::setVelocity; op.arg0 = static_cast<int> (std::lround (effectiveValue)); paramSlotIdx = 3; }
    else if (key == "rate") {
        op.kind = OpKind::setRate;
        op.darg = kRates[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (effectiveValue)), 0, 4))];
        paramSlotIdx = 4;
    }
    else if (key == "groove") { op.kind = OpKind::setGroove; op.arg0 = static_cast<int> (std::lround (effectiveValue)); paramSlotIdx = 5; }
    else if (key == "mod_amt") { op.kind = OpKind::setModAmount; op.arg0 = static_cast<int> (std::lround (effectiveValue)); paramSlotIdx = 6; }
    else if (key == "loop_steps") { op.kind = OpKind::setLoopStepsOverride; op.arg0 = static_cast<int> (std::lround (effectiveValue)); paramSlotIdx = 7; }
    else { mapped = false; }

    if (paramSlotIdx >= 0) {
        std::lock_guard<std::mutex> lock (messageMutex);
        const auto& ptrs = paramPtrs[static_cast<size_t> (trackIndex)];
        const std::array<std::atomic<float>*, 8> raw {
            ptrs.slot, ptrs.mute, ptrs.solo, ptrs.velocity, ptrs.rate, ptrs.groove, ptrs.modAmount, ptrs.loopSteps
        };
        lastSnapshotHostValues[static_cast<size_t> (trackIndex)][static_cast<size_t> (paramSlotIdx)] =
            raw[static_cast<size_t> (paramSlotIdx)]->load (std::memory_order_relaxed);
    }

    if (mapped) applyUiOp (op);
    else version.fetch_add (1, std::memory_order_release);
}

void SliderRhythmMidiProcessor::unmuteAllTracks() {
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        setHostParamValue (i, "mute", 0.0f);
}

void SliderRhythmMidiProcessor::unsoloAllTracks() {
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        setHostParamValue (i, "solo", 0.0f);
}

void SliderRhythmMidiProcessor::soloTrackExclusive (int trackIndex) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
    const auto current = snapshot();
    int soloCount = 0;
    for (int i = 0; i < current->sequence.trackCount; ++i)
        if (current->sequence.tracks[static_cast<size_t> (i)].solo) ++soloCount;
    const bool alreadyOnlySolo = current->sequence.tracks[static_cast<size_t> (trackIndex)].solo && soloCount == 1;
    if (alreadyOnlySolo) {
        setHostParamValue (trackIndex, "solo", 0.0f);
        return;
    }
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        if (i != trackIndex) setHostParamValue (i, "solo", 0.0f);
    setHostParamValue (trackIndex, "mute", 0.0f);
    setHostParamValue (trackIndex, "solo", 1.0f);
}

void SliderRhythmMidiProcessor::toggleTrackSoloAdditive (int trackIndex) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
    const auto current = snapshot();
    const bool nextSolo = ! current->sequence.tracks[static_cast<size_t> (trackIndex)].solo;
    if (nextSolo) setHostParamValue (trackIndex, "mute", 0.0f);
    setHostParamValue (trackIndex, "solo", nextSolo ? 1.0f : 0.0f);
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
    for (auto& step : activeSteps) step.store (-1, std::memory_order_relaxed);
    outputMidiA.ensureSize (kMidiBufferBytes);
    outputMidiB.ensureSize (kMidiBufferBytes);
    swappedOutputOnce = false;
}

SliderRhythmMidiProcessor::FullAudioState SliderRhythmMidiProcessor::makeAudioState (const Snapshot& src, uint64_t seq) noexcept {
    FullAudioState out;
    out.seq = seq;
    const auto mode = src.playRecordedMidi ? sliderseq::PlaybackMode::thru : src.playbackMode;
    out.render = sliderseq::toRenderState (src.sequence, mode);
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
        case OpKind::setPlaybackMode: render.playbackMode = clampPlaybackMode (op.arg0); break;
        case OpKind::setGlobalSwing: render.globalSwing = std::clamp (op.arg0, -100, 100); break;
        case OpKind::setSlot:
            track.slot = std::clamp (op.arg0, 0, std::clamp (track.activeZones, 1, sliderseq::slotsPerTrack));
            break;
        case OpKind::setActiveZones:
            track.activeZones = std::clamp (op.arg0, 1, sliderseq::slotsPerTrack);
            track.slot = std::min (track.slot, track.activeZones);
            break;
        case OpKind::setRate: track.rate = op.darg; break;
        case OpKind::setVelocity: track.velocity = std::clamp (op.arg0, 1, 127); break;
        case OpKind::setGroove: track.groove = std::clamp (op.arg0, 0, 100); break;
        case OpKind::setModShape: track.modShape = sliderseq::clampModShape (op.arg0); break;
        case OpKind::setModAmount: track.modAmount = std::clamp (op.arg0, 0, 100); break;
        case OpKind::setLoopStepsOverride: track.loopStepsOverride = std::clamp (op.arg0, 0, 64); break;
        case OpKind::setGateTicks: track.gateTicks = std::clamp (op.darg, 6.0, 288.0); break;
        case OpKind::setChokeMask:
            track.chokeMask = static_cast<uint16_t> (op.arg0 & 0xffff) & static_cast<uint16_t> (~(1u << static_cast<unsigned> (t)));
            break;
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
        case OpKind::setPatternVelocityScale:
            track.patterns[static_cast<size_t> (s)].velocityScale = static_cast<float> (std::clamp (op.darg, 0.1, 2.0));
            break;
        case OpKind::resizePattern:
            track.patterns[static_cast<size_t> (s)].resize (op.arg0);
            break;
        case OpKind::setPattern:
            track.patterns[static_cast<size_t> (s)] = op.pattern;
            break;
        case OpKind::setBinding: {
            const int ctrl = std::clamp (op.slotIndex, 0, controlCount - 1);
            const int code = std::clamp (op.arg0, -1, 127);
            const int ch = std::clamp (op.arg1, 0, 16);
            const auto kind = static_cast<BindingKind> (std::clamp (op.arg2, 0, 1));
            const auto trig = static_cast<TriggerMode> (std::clamp (op.arg3, 0, 1));
            if (code >= 0) {
                for (auto& tb : target.bindings)
                    for (auto& b : tb)
                        if (b.cc == code && b.channel == ch && b.kind == kind) b = {};
            }
            target.bindings[static_cast<size_t> (t)][static_cast<size_t> (ctrl)] = { code, ch, kind, trig, false };
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
        case OpKind::setPlaybackMode:
            target.playbackMode = clampPlaybackMode (op.arg0);
            target.playRecordedMidi = (target.playbackMode == sliderseq::PlaybackMode::thru);
            break;
        case OpKind::setGlobalSwing:
            target.sequence.globalSwing = std::clamp (op.arg0, -100, 100);
            break;
        case OpKind::setSlot:
            track.slot = std::clamp (op.arg0, 0, std::clamp (track.activeZones, 1, sliderseq::slotsPerTrack));
            break;
        case OpKind::setActiveZones:
            track.activeZones = std::clamp (op.arg0, 1, sliderseq::slotsPerTrack);
            track.slot = std::min (track.slot, track.activeZones);
            break;
        case OpKind::setRate: track.rate = op.darg; break;
        case OpKind::setVelocity: track.velocity = std::clamp (op.arg0, 1, 127); break;
        case OpKind::setGroove: track.groove = std::clamp (op.arg0, 0, 100); break;
        case OpKind::setModShape: track.modShape = sliderseq::clampModShape (op.arg0); break;
        case OpKind::setModAmount: track.modAmount = std::clamp (op.arg0, 0, 100); break;
        case OpKind::setLoopStepsOverride: track.loopStepsOverride = std::clamp (op.arg0, 0, 64); break;
        case OpKind::setGateTicks: track.gateTicks = std::clamp (op.darg, 6.0, 288.0); break;
        case OpKind::setChokeMask:
            track.chokeMask = static_cast<uint16_t> (op.arg0 & 0xffff) & static_cast<uint16_t> (~(1u << static_cast<unsigned> (t)));
            break;
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
        case OpKind::setPatternVelocityScale:
            track.patterns[static_cast<size_t> (s)].velocityScale = static_cast<float> (std::clamp (op.darg, 0.1, 2.0));
            break;
        case OpKind::resizePattern:
            track.patterns[static_cast<size_t> (s)].resize (std::clamp (op.arg0, 1, 64));
            break;
        case OpKind::setPattern:
            sliderseq::applyRenderPattern (op.pattern, track.patterns[static_cast<size_t> (s)]);
            break;
        case OpKind::setBinding: {
            const int ctrl = std::clamp (op.slotIndex, 0, controlCount - 1);
            const int code = std::clamp (op.arg0, -1, 127);
            const int ch = std::clamp (op.arg1, 0, 16);
            const auto kind = static_cast<BindingKind> (std::clamp (op.arg2, 0, 1));
            const auto trig = static_cast<TriggerMode> (std::clamp (op.arg3, 0, 1));
            if (code >= 0) {
                for (auto& tb : target.bindings)
                    for (auto& b : tb)
                        if (b.cc == code && b.channel == ch && b.kind == kind) b = {};
            }
            target.bindings[static_cast<size_t> (t)][static_cast<size_t> (ctrl)] = { code, ch, kind, trig, false };
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

SliderRhythmMidiProcessor::ApvtsSyncValues SliderRhythmMidiProcessor::captureApvtsSyncValuesLocked() {
    ApvtsSyncValues values;
    values.globalSwing = static_cast<float> (messageState.sequence.globalSwing);
    lastSnapshotGlobalSwing = values.globalSwing;

    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& t = messageState.sequence.tracks[static_cast<size_t> (i)];
        const std::array<float, 8> row {
            static_cast<float> (t.slot),
            t.muted ? 1.0f : 0.0f,
            t.solo ? 1.0f : 0.0f,
            static_cast<float> (t.velocity),
            static_cast<float> (rateToIndex (t.rate)),
            static_cast<float> (t.groove),
            static_cast<float> (t.modAmount),
            static_cast<float> (t.loopStepsOverride)
        };
        values.tracks[static_cast<size_t> (i)] = row;
        lastSnapshotHostValues[static_cast<size_t> (i)] = row;
    }
    return values;
}

void SliderRhythmMidiProcessor::applyApvtsSyncValues (const ApvtsSyncValues& values) {
    // Must be called WITHOUT holding messageMutex: setValueNotifyingHost synchronously triggers
    // JUCE ParameterAttachment callbacks on the message thread that may read processor.snapshot().
    if (auto* gParam = apvts.getParameter (globalSwingParamId())) {
        const float norm = gParam->convertTo0to1 (values.globalSwing);
        if (std::abs (gParam->getValue() - norm) > 1e-6f)
            gParam->setValueNotifyingHost (norm);
    }

    static constexpr std::array<const char*, 8> kSuffixes {
        "slot", "mute", "solo", "velocity", "rate", "groove", "mod_amt", "loop_steps"
    };
    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& row = values.tracks[static_cast<size_t> (i)];
        for (int p = 0; p < 8; ++p) {
            if (auto* param = apvts.getParameter (paramId (i, kSuffixes[static_cast<size_t> (p)]))) {
                const float norm = param->convertTo0to1 (row[static_cast<size_t> (p)]);
                if (std::abs (param->getValue() - norm) > 1e-6f)
                    param->setValueNotifyingHost (norm);
            }
        }
    }
}

void SliderRhythmMidiProcessor::syncApvtsToSnapshotLocked() {
    if (globalSwingParam != nullptr) {
        const float vGlobal = globalSwingParam->load (std::memory_order_relaxed);
        if (vGlobal != lastSnapshotGlobalSwing) {
            messageState.sequence.globalSwing = std::clamp (static_cast<int> (std::lround (vGlobal)), -100, 100);
            lastSnapshotGlobalSwing = vGlobal;
        }
    }

    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& ptrs = paramPtrs[static_cast<size_t> (i)];
        auto& prev = lastSnapshotHostValues[static_cast<size_t> (i)];
        auto& t = messageState.sequence.tracks[static_cast<size_t> (i)];

        const float vSlot = ptrs.slot->load (std::memory_order_relaxed);
        if (vSlot != prev[0]) {
            t.slot = std::clamp (static_cast<int> (std::lround (vSlot)), 0, std::clamp (t.activeZones, 1, sliderseq::slotsPerTrack));
            prev[0] = vSlot;
        }
        const float vMute = ptrs.mute->load (std::memory_order_relaxed);
        if (vMute != prev[1]) { t.muted = vMute >= 0.5f; prev[1] = vMute; }
        const float vSolo = ptrs.solo->load (std::memory_order_relaxed);
        if (vSolo != prev[2]) { t.solo = vSolo >= 0.5f; prev[2] = vSolo; }
        const float vVel = ptrs.velocity->load (std::memory_order_relaxed);
        if (vVel != prev[3]) { t.velocity = std::clamp (static_cast<int> (std::lround (vVel)), 1, 127); prev[3] = vVel; }
        const float vRate = ptrs.rate->load (std::memory_order_relaxed);
        if (vRate != prev[4]) { t.rate = kRates[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (vRate)), 0, 4))]; prev[4] = vRate; }
        const float vGroove = ptrs.groove->load (std::memory_order_relaxed);
        if (vGroove != prev[5]) { t.groove = std::clamp (static_cast<int> (std::lround (vGroove)), 0, 100); prev[5] = vGroove; }
        const float vMod = ptrs.modAmount->load (std::memory_order_relaxed);
        if (vMod != prev[6]) { t.modAmount = std::clamp (static_cast<int> (std::lround (vMod)), 0, 100); prev[6] = vMod; }
        const float vLoop = ptrs.loopSteps->load (std::memory_order_relaxed);
        if (vLoop != prev[7]) { t.loopStepsOverride = std::clamp (static_cast<int> (std::lround (vLoop)), 0, 64); prev[7] = vLoop; }
    }
}

void SliderRhythmMidiProcessor::pullHostParamsIntoAudio (juce::MidiBuffer& outMidi) noexcept {
    sliderseq::RenderState before;
    bool capturedBefore = false;
    bool changed = false;

    if (globalSwingParam != nullptr) {
        const float vGlobal = globalSwingParam->load (std::memory_order_relaxed);
        if (vGlobal != lastHostGlobalSwing) {
            if (! capturedBefore) { before = audioState.render; capturedBefore = true; }
            lastHostGlobalSwing = vGlobal;
            audioState.render.globalSwing = std::clamp (static_cast<int> (std::lround (vGlobal)), -100, 100);
            changed = true;
        }
    }

    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& ptrs = paramPtrs[static_cast<size_t> (i)];
        auto& prev = lastHostParamValues[static_cast<size_t> (i)];
        auto& t = audioState.render.tracks[static_cast<size_t> (i)];

        const float vals[8] {
            ptrs.slot->load (std::memory_order_relaxed),
            ptrs.mute->load (std::memory_order_relaxed),
            ptrs.solo->load (std::memory_order_relaxed),
            ptrs.velocity->load (std::memory_order_relaxed),
            ptrs.rate->load (std::memory_order_relaxed),
            ptrs.groove->load (std::memory_order_relaxed),
            ptrs.modAmount->load (std::memory_order_relaxed),
            ptrs.loopSteps->load (std::memory_order_relaxed)
        };

        for (int p = 0; p < 8; ++p) {
            if (vals[p] == prev[static_cast<size_t> (p)]) continue;
            if (! capturedBefore) { before = audioState.render; capturedBefore = true; }
            prev[static_cast<size_t> (p)] = vals[p];
            changed = true;
            switch (p) {
                case 0:
                    t.slot = std::clamp (static_cast<int> (std::lround (vals[0])), 0,
                                         std::clamp (t.activeZones, 1, sliderseq::slotsPerTrack));
                    break;
                case 1: t.muted = vals[1] >= 0.5f; break;
                case 2: t.solo = vals[2] >= 0.5f; break;
                case 3: t.velocity = std::clamp (static_cast<int> (std::lround (vals[3])), 1, 127); break;
                case 4: t.rate = kRates[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (vals[4])), 0, 4))]; break;
                case 5: t.groove = std::clamp (static_cast<int> (std::lround (vals[5])), 0, 100); break;
                case 6: t.modAmount = std::clamp (static_cast<int> (std::lround (vals[6])), 0, 100); break;
                case 7: t.loopStepsOverride = std::clamp (static_cast<int> (std::lround (vals[7])), 0, 64); break;
            }
        }
    }

    if (changed) {
        flushRemovedOrChangedTracks (before, audioState.render, 0, outMidi);
        version.fetch_add (1, std::memory_order_release);
    }
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
    syncApvtsToSnapshotLocked();
    return std::make_shared<const Snapshot> (messageState);
}

void SliderRhythmMidiProcessor::edit (const std::function<void (Snapshot&)>& change) {
    ApvtsSyncValues apvtsValues;
    {
        std::lock_guard<std::mutex> lock (messageMutex);
        drainAudioToUiLocked();
        syncApvtsToSnapshotLocked();
        const bool prevRecorded = messageState.playRecordedMidi;
        const auto prevMode = messageState.playbackMode;
        change (messageState);
        if (messageState.playRecordedMidi != prevRecorded && messageState.playbackMode == prevMode)
            messageState.playbackMode = messageState.playRecordedMidi ? sliderseq::PlaybackMode::thru : sliderseq::PlaybackMode::gen;
        else
            messageState.playRecordedMidi = (messageState.playbackMode == sliderseq::PlaybackMode::thru);
        apvtsValues = captureApvtsSyncValuesLocked();
        publishFullStateLocked();
    }
    applyApvtsSyncValues (apvtsValues);
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
        lastHostGlobalSwing = static_cast<float> (audioState.render.globalSwing);
        for (int i = 0; i < sliderseq::maxTracks; ++i) {
            const auto& t = audioState.render.tracks[static_cast<size_t> (i)];
            lastHostParamValues[static_cast<size_t> (i)] = {
                static_cast<float> (t.slot),
                t.muted ? 1.0f : 0.0f,
                t.solo ? 1.0f : 0.0f,
                static_cast<float> (t.velocity),
                static_cast<float> (rateToIndex (t.rate)),
                static_cast<float> (t.groove),
                static_cast<float> (t.modAmount),
                static_cast<float> (t.loopStepsOverride)
            };
        }
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
    pullHostParamsIntoAudio (outMidi);
}

bool SliderRhythmMidiProcessor::applyIncomingControl (const IncomingMidi& msg, int samplePos, juce::MidiBuffer& outMidi) noexcept {
    if (msg.numBytes < 3) return false;
    const uint8_t status = msg.data[0] & 0xf0u;
    const int ch = static_cast<int> ((msg.data[0] & 0x0fu) + 1);
    const int d1 = static_cast<int> (msg.data[1] & 0x7fu);
    const int d2 = static_cast<int> (msg.data[2] & 0x7fu);

    const bool isCC = (status == 0xb0u);
    const bool isNoteOn = (status == 0x90u && d2 > 0);
    const bool isNoteOff = (status == 0x80u || (status == 0x90u && d2 == 0));
    if (! isCC && ! isNoteOn && ! isNoteOff) return false;

    const bool noteEnabled = noteLearnEnabled.load (std::memory_order_acquire);

    // Check active MIDI Learn arming (CC always eligible; Note On only when noteLearnEnabled is ON).
    int armed = learnTarget.load (std::memory_order_acquire);
    if (armed >= 0 && (isCC || (noteEnabled && isNoteOn))) {
        if (learnTarget.compare_exchange_strong (armed, -1, std::memory_order_acq_rel)) {
            StateOp op;
            op.kind = OpKind::setBinding;
            op.track = armed / controlCount;
            op.slotIndex = armed % controlCount;
            op.arg0 = d1;
            op.arg1 = ch;
            op.arg2 = static_cast<int> (isCC ? BindingKind::cc : BindingKind::note);
            op.arg3 = learnTriggerMode.load (std::memory_order_acquire);
            applyOpToAudio (op, audioState);
            pushAudioToUiOp (op);
            return true;
        }
    }

    const BindingKind msgKind = isCC ? BindingKind::cc : BindingKind::note;
    if (msgKind == BindingKind::note && ! noteEnabled)
        return false;

    const sliderseq::RenderState before = audioState.render;
    bool consumed = false;

    for (int t = 0; t < audioState.render.trackCount; ++t) {
        const auto& track = audioState.render.tracks[static_cast<size_t> (t)];
        const int activePatIdx = std::clamp (track.slot > 0 ? track.slot - 1 : 0, 0, sliderseq::slotsPerTrack - 1);
        for (int ctrl = 0; ctrl < controlCount; ++ctrl) {
            auto& binding = audioState.bindings[static_cast<size_t> (t)][static_cast<size_t> (ctrl)];
            if (binding.cc != d1 || binding.channel != ch || binding.kind != msgKind) continue;
            consumed = true;

            StateOp op;
            op.track = t;
            op.slotIndex = activePatIdx;
            bool emitOp = false;

            if (msgKind == BindingKind::cc) {
                switch (static_cast<Control> (ctrl)) {
                    case slot: {
                        const int zones = std::clamp (track.activeZones, 1, sliderseq::slotsPerTrack);
                        op.kind = OpKind::setSlot;
                        op.arg0 = static_cast<int> (std::lround (d2 * static_cast<double> (zones) / 127.0));
                        emitOp = true;
                        break;
                    }
                    case rate:
                        op.kind = OpKind::setRate;
                        op.darg = kRates[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (d2 * 4.0 / 127.0)), 0, 4))];
                        emitOp = true;
                        break;
                    case velocity:
                        op.kind = OpKind::setVelocity;
                        op.arg0 = std::clamp (d2, 1, 127);
                        emitOp = true;
                        break;
                    case groove:
                        op.kind = OpKind::setGroove;
                        op.arg0 = static_cast<int> (std::lround (d2 * 100.0 / 127.0));
                        emitOp = true;
                        break;
                    case modAmount:
                        op.kind = OpKind::setModAmount;
                        op.arg0 = static_cast<int> (std::lround (d2 * 100.0 / 127.0));
                        emitOp = true;
                        break;
                    case mute: {
                        const bool high = d2 >= 64;
                        if (binding.triggerMode == TriggerMode::momentary) {
                            op.kind = OpKind::setMuted;
                            op.barg = high;
                            emitOp = true;
                        } else if (high && ! binding.lastHigh) {
                            op.kind = OpKind::setMuted;
                            op.barg = ! track.muted;
                            emitOp = true;
                        }
                        binding.lastHigh = high;
                        break;
                    }
                    case solo: {
                        const bool high = d2 >= 64;
                        if (binding.triggerMode == TriggerMode::momentary) {
                            op.kind = OpKind::setSolo;
                            op.barg = high;
                            emitOp = true;
                        } else if (high && ! binding.lastHigh) {
                            op.kind = OpKind::setSolo;
                            op.barg = ! track.solo;
                            emitOp = true;
                        }
                        binding.lastHigh = high;
                        break;
                    }
                    case modMode:
                        op.kind = OpKind::setModShape;
                        op.arg0 = static_cast<int> (std::lround (d2 * static_cast<double> (static_cast<int> (sliderseq::ModShape::count) - 1) / 127.0));
                        emitOp = true;
                        break;
                    case note:
                        op.kind = OpKind::setMidiNote;
                        op.arg0 = d2;
                        emitOp = true;
                        break;
                    case channel:
                        op.kind = OpKind::setMidiChannel;
                        op.arg0 = 1 + static_cast<int> (std::lround (d2 * 15.0 / 127.0));
                        emitOp = true;
                        break;
                    case length: {
                        const auto& pat = track.patterns[static_cast<size_t> (activePatIdx)];
                        op.kind = OpKind::setPatternLength;
                        op.arg0 = 1 + static_cast<int> (std::lround (d2 * (pat.grid - 1) / 127.0));
                        emitOp = true;
                        break;
                    }
                    case grid:
                        op.kind = OpKind::resizePattern;
                        op.arg0 = kGrids[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (d2 * 4.0 / 127.0)), 0, 4))];
                        emitOp = true;
                        break;
                    case globalSwing:
                        op.kind = OpKind::setGlobalSwing;
                        op.arg0 = d2 == 64 ? 0 : std::clamp (static_cast<int> (std::lround (d2 * 200.0 / 127.0)) - 100, -100, 100);
                        emitOp = true;
                        break;
                    case controlCount:
                        break;
                }
            } else {
                // Note binding (pad trigger).
                switch (static_cast<Control> (ctrl)) {
                    case mute:
                        if (binding.triggerMode == TriggerMode::momentary) {
                            op.kind = OpKind::setMuted;
                            op.barg = isNoteOn;
                            emitOp = true;
                        } else if (isNoteOn) {
                            op.kind = OpKind::setMuted;
                            op.barg = ! track.muted;
                            emitOp = true;
                        }
                        break;
                    case solo:
                        if (binding.triggerMode == TriggerMode::momentary) {
                            op.kind = OpKind::setSolo;
                            op.barg = isNoteOn;
                            emitOp = true;
                        } else if (isNoteOn) {
                            op.kind = OpKind::setSolo;
                            op.barg = ! track.solo;
                            emitOp = true;
                        }
                        break;
                    case slot:
                        if (isNoteOn) {
                            const int zones = std::clamp (track.activeZones, 1, sliderseq::slotsPerTrack);
                            op.kind = OpKind::setSlot;
                            op.arg0 = (track.slot + 1) % (zones + 1);
                            emitOp = true;
                        }
                        break;
                    case modMode:
                        if (isNoteOn) {
                            op.kind = OpKind::setModShape;
                            op.arg0 = (static_cast<int> (track.modShape) + 1) % static_cast<int> (sliderseq::ModShape::count);
                            emitOp = true;
                        }
                        break;
                    default:
                        break;
                }
            }

            if (emitOp) {
                applyOpToAudio (op, audioState);
                pushAudioToUiOp (op);
            }
        }
    }

    if (consumed)
        flushRemovedOrChangedTracks (before, audioState.render, samplePos, outMidi);
    return consumed;
}

void SliderRhythmMidiProcessor::updateActiveSteps (double tick, bool playing) noexcept {
    if (! playing || audioState.render.playbackMode == sliderseq::PlaybackMode::thru) {
        for (auto& step : activeSteps) step.store (-1, std::memory_order_relaxed);
        return;
    }
    const int count = std::clamp (audioState.render.trackCount, 0, sliderseq::maxTracks);

    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        if (i >= count) {
            activeSteps[static_cast<size_t> (i)].store (-1, std::memory_order_relaxed);
            continue;
        }
        const auto& track = audioState.render.tracks[static_cast<size_t> (i)];
        if (track.rate <= 0.0 || track.slot <= 0) {
            activeSteps[static_cast<size_t> (i)].store (-1, std::memory_order_relaxed);
            continue;
        }
        const int patIdx = std::clamp (track.slot - 1, 0, sliderseq::slotsPerTrack - 1);
        const auto& pat = track.patterns[static_cast<size_t> (patIdx)];
        const int activeLen = sliderseq::effectivePatternLength (pat.length, pat.grid, track.loopStepsOverride);
        const double stepTicks = pat.stepTicks();
        const double period = activeLen * stepTicks;
        if (period <= 0.0) {
            activeSteps[static_cast<size_t> (i)].store (-1, std::memory_order_relaxed);
            continue;
        }
        double phase = std::fmod (tick * track.rate, period);
        if (phase < 0.0) phase += period;
        const int step = std::clamp (static_cast<int> (std::floor (phase / stepTicks)), 0, activeLen - 1);
        activeSteps[static_cast<size_t> (i)].store (step, std::memory_order_relaxed);
    }
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

        for (auto& active : activeNotes) {
            if (active.active && active.channel == ev.channel && active.note == ev.note) {
                addNoteOffRaw (outMidi, active.channel, active.note, segStartSample + onsetRel);
                active.active = false;
            }
        }

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

    size_t inCount = 0;
    for (const auto metadata : midi) {
        if (metadata.numBytes <= 3 && metadata.numBytes > 0 && inCount < incomingScratch.size()) {
            auto& dst = incomingScratch[inCount++];
            dst.sample = std::clamp (metadata.samplePosition, 0, std::max (0, numSamples - 1));
            dst.numBytes = metadata.numBytes;
            dst.consumedByLearnOrControl = false;
            for (int b = 0; b < metadata.numBytes; ++b) dst.data[b] = metadata.data[b];
        }
    }

    const auto* playhead = getPlayHead();
    const auto position = playhead != nullptr ? playhead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};
    const bool isLogicAu = wrapperType == wrapperType_AudioUnit && juce::PluginHostType().isLogic();
    const bool playing = position && (position->getIsPlaying() || (isLogicAu && position->getIsRecording()));
    hostPlaying.store (playing, std::memory_order_relaxed);
    hostRecording.store (position && position->getIsRecording(), std::memory_order_relaxed);
    hostPositionAvailable.store (position && position->getPpqPosition() && position->getBpm(), std::memory_order_relaxed);
    if (position && position->getBpm() && *position->getBpm() > 0.0)
        lastKnownBpm.store (*position->getBpm(), std::memory_order_relaxed);

    auto forwardUnconsumed = [&] (size_t idx) {
        const auto& m = incomingScratch[idx];
        if (m.consumedByLearnOrControl || m.numBytes <= 0) return;
        if (m.numBytes >= 3 && (m.data[0] & 0xf0u) == 0x90u && (m.data[2] & 0x7fu) > 0) {
            const int ch = static_cast<int> ((m.data[0] & 0x0fu) + 1);
            const int nt = static_cast<int> (m.data[1] & 0x7fu);
            for (auto& active : activeNotes) {
                if (active.active && active.channel == ch && active.note == nt) {
                    addNoteOffRaw (outMidi, ch, nt, m.sample);
                    active.active = false;
                }
            }
        }
        outMidi.addEvent (m.data, m.numBytes, m.sample);
    };

    if (audioState.render.playbackMode == sliderseq::PlaybackMode::thru) {
        flushAllActiveNotes (0, outMidi);
        for (size_t i = 0; i < inCount; ++i) {
            incomingScratch[i].consumedByLearnOrControl = applyIncomingControl (incomingScratch[i], incomingScratch[i].sample, outMidi);
            forwardUnconsumed (i);
        }
        wasPlaying = false;
        hasExpectedTick = false;
        updateActiveSteps (0.0, false);
        finalizeOutputBuffer (midi, outMidi);
        return;
    }

    if (! playing || ! position || ! position->getPpqPosition() || ! position->getBpm() || numSamples <= 0) {
        flushAllActiveNotes (0, outMidi);
        for (size_t i = 0; i < inCount; ++i) {
            incomingScratch[i].consumedByLearnOrControl = applyIncomingControl (incomingScratch[i], incomingScratch[i].sample, outMidi);
            if (audioState.render.playbackMode == sliderseq::PlaybackMode::genPlusThru)
                forwardUnconsumed (i);
        }
        wasPlaying = false;
        hasExpectedTick = false;
        updateActiveSteps (0.0, false);
        finalizeOutputBuffer (midi, outMidi);
        return;
    }

    const double bpm = *position->getBpm();
    const double startTick = *position->getPpqPosition() * sliderseq::ticksPerQuarter;
    if (bpm <= 0.0 || currentSampleRate <= 0.0) {
        for (size_t i = 0; i < inCount; ++i)
            applyIncomingControl (incomingScratch[i], incomingScratch[i].sample, outMidi);
        updateActiveSteps (0.0, false);
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

    int curSample = 0;
    double curTick = startTick;
    size_t inIdx = 0;
    constexpr int kMaxChunkSamples = 256;

    while (curSample < numSamples) {
        while (inIdx < inCount && incomingScratch[inIdx].sample <= curSample) {
            incomingScratch[inIdx].consumedByLearnOrControl = applyIncomingControl (incomingScratch[inIdx], curSample, outMidi);
            if (audioState.render.playbackMode == sliderseq::PlaybackMode::genPlusThru)
                forwardUnconsumed (inIdx);
            ++inIdx;
        }
        if (audioState.render.playbackMode == sliderseq::PlaybackMode::thru) {
            flushAllActiveNotes (curSample, outMidi);
            while (inIdx < inCount) {
                incomingScratch[inIdx].consumedByLearnOrControl = applyIncomingControl (incomingScratch[inIdx], incomingScratch[inIdx].sample, outMidi);
                forwardUnconsumed (inIdx);
                ++inIdx;
            }
            break;
        }

        int nextSample = std::min (numSamples, curSample + kMaxChunkSamples);
        if (inIdx < inCount && incomingScratch[inIdx].sample < nextSample)
            nextSample = incomingScratch[inIdx].sample;

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

    while (inIdx < inCount) {
        incomingScratch[inIdx].consumedByLearnOrControl = applyIncomingControl (incomingScratch[inIdx], std::max (0, numSamples - 1), outMidi);
        if (audioState.render.playbackMode == sliderseq::PlaybackMode::genPlusThru)
            forwardUnconsumed (inIdx);
        ++inIdx;
    }

    expectedNextTick = curTick;
    hasExpectedTick = true;
    updateActiveSteps (startTick, true);
    finalizeOutputBuffer (midi, outMidi);
}

template<typename Sample>
void SliderRhythmMidiProcessor::renderBypassed (juce::AudioBuffer<Sample>& audio, juce::MidiBuffer& midi) {
    audio.clear();
    auto& outMidi = acquireOutputBuffer();
    pendingFlushOnNextBlock.store (false, std::memory_order_relaxed);
    flushAllActiveNotes (0, outMidi);
    for (const auto metadata : midi)
        outMidi.addEvent (metadata.data, metadata.numBytes, metadata.samplePosition);
    wasPlaying = false;
    hasExpectedTick = false;
    updateActiveSteps (0.0, false);
    finalizeOutputBuffer (midi, outMidi);
}

void SliderRhythmMidiProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) { renderBlock (audio, midi); }
void SliderRhythmMidiProcessor::processBlock (juce::AudioBuffer<double>& audio, juce::MidiBuffer& midi) { renderBlock (audio, midi); }
void SliderRhythmMidiProcessor::processBlockBypassed (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) { renderBypassed (audio, midi); }
void SliderRhythmMidiProcessor::processBlockBypassed (juce::AudioBuffer<double>& audio, juce::MidiBuffer& midi) { renderBypassed (audio, midi); }

void SliderRhythmMidiProcessor::getStateInformation (juce::MemoryBlock& destination) {
    const auto current = snapshot();
    juce::XmlElement root ("SliderRhythmMidi");
    root.setAttribute ("version", 4);
    root.setAttribute ("trackCount", current->sequence.trackCount);
    root.setAttribute ("globalSwing", current->sequence.globalSwing);
    root.setAttribute ("playbackMode", static_cast<int> (current->playbackMode));
    root.setAttribute ("playRecordedMidi", current->playbackMode == sliderseq::PlaybackMode::thru);
    root.setAttribute ("noteLearnEnabled", isNoteLearnEnabled());
    root.setAttribute ("learnTriggerMode", static_cast<int> (getLearnTriggerMode()));

    // Persist active Mapping Preset baseline so '*' dirty detection survives project reload.
    {
        const auto& mp = current->activeMappingPreset;
        auto* mpEl = root.createNewChildElement ("MappingPreset");
        mpEl->setAttribute ("id", juce::String (mp.id));
        mpEl->setAttribute ("name", juce::String (mp.name));
        mpEl->setAttribute ("isFactory", mp.isFactory);
        mpEl->setAttribute ("trackCount", mp.trackCount);
        for (int i = 0; i < sliderseq::maxTracks; ++i) {
            const auto& m = mp.tracks[static_cast<size_t> (i)];
            auto* tEl = mpEl->createNewChildElement ("Track");
            tEl->setAttribute ("index", i);
            tEl->setAttribute ("name", juce::String (m.name));
            tEl->setAttribute ("note", m.midiNote);
            tEl->setAttribute ("channel", m.midiChannel);
            tEl->setAttribute ("modShape", static_cast<int> (m.modShape));
            tEl->setAttribute ("gateTicks", m.gateTicks);
            tEl->setAttribute ("chokeMask", static_cast<int> (m.chokeMask));
            tEl->setAttribute ("activeZones", m.activeZones);
            tEl->setAttribute ("groove", m.groove);
        }
    }

    if (auto paramsXml = apvts.copyState().createXml())
        root.addChildElement (paramsXml.release());

    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& track = current->sequence.tracks[static_cast<size_t> (i)];
        auto* element = root.createNewChildElement ("Track");
        element->setAttribute ("index", i);
        element->setAttribute ("name", track.name);
        element->setAttribute ("slot", track.slot);
        element->setAttribute ("activeZones", track.activeZones);
        element->setAttribute ("rate", track.rate);
        element->setAttribute ("note", track.midiNote);
        element->setAttribute ("channel", track.midiChannel);
        element->setAttribute ("velocity", track.velocity);
        element->setAttribute ("groove", track.groove);
        element->setAttribute ("modShape", static_cast<int> (track.modShape));
        element->setAttribute ("modAmount", track.modAmount);
        element->setAttribute ("loopStepsOverride", track.loopStepsOverride);
        element->setAttribute ("gateTicks", track.gateTicks);
        element->setAttribute ("chokeMask", static_cast<int> (track.chokeMask));
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
            child->setAttribute ("velocityScale", static_cast<double> (pattern.velocityScale));
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
            child->setAttribute ("kind", static_cast<int> (binding.kind));
            child->setAttribute ("triggerMode", static_cast<int> (binding.triggerMode));
        }
    }
    copyXmlToBinary (root, destination);
}

void SliderRhythmMidiProcessor::setStateInformation (const void* data, int size) {
    const auto xml = getXmlFromBinary (data, size);
    if (! xml || ! xml->hasTagName ("SliderRhythmMidi")) return;
    const int savedVersion = xml->getIntAttribute ("version", 1);
    Snapshot restored;
    restored.sequence.trackCount = std::clamp (xml->getIntAttribute ("trackCount", 4), 1, sliderseq::maxTracks);
    restored.sequence.globalSwing = std::clamp (xml->getIntAttribute ("globalSwing", 0), -100, 100);
    if (xml->hasAttribute ("playbackMode"))
        restored.playbackMode = clampPlaybackMode (xml->getIntAttribute ("playbackMode", 0));
    else
        restored.playbackMode = xml->getBoolAttribute ("playRecordedMidi", false)
            ? sliderseq::PlaybackMode::thru : sliderseq::PlaybackMode::gen;
    restored.playRecordedMidi = (restored.playbackMode == sliderseq::PlaybackMode::thru);
    setNoteLearnEnabled (xml->getBoolAttribute ("noteLearnEnabled", false));
    setLearnTriggerMode (static_cast<TriggerMode> (std::clamp (xml->getIntAttribute ("learnTriggerMode", 0), 0, 1)));

    std::array<int, sliderseq::maxTracks> legacyGrooves {};
    legacyGrooves.fill (50);

    for (auto* element : xml->getChildIterator()) {
        if (element->hasTagName ("MappingPreset")) {
            auto mp = mappingpresets::makeDefaultKitPreset();
            mp.id = element->getStringAttribute ("id", "factory:default4").toStdString();
            mp.name = element->getStringAttribute ("name", "Default 4-Piece Kit").toStdString();
            mp.isFactory = element->getBoolAttribute ("isFactory", true);
            mp.trackCount = std::clamp (element->getIntAttribute ("trackCount", 4), 1, sliderseq::maxTracks);
            for (auto* tEl : element->getChildIterator()) {
                if (! tEl->hasTagName ("Track")) continue;
                const int idx = tEl->getIntAttribute ("index", -1);
                if (idx < 0 || idx >= sliderseq::maxTracks) continue;
                auto& m = mp.tracks[static_cast<size_t> (idx)];
                m.name = tEl->getStringAttribute ("name", "TRACK").toStdString();
                m.midiNote = std::clamp (tEl->getIntAttribute ("note", 36), 0, 127);
                m.midiChannel = std::clamp (tEl->getIntAttribute ("channel", 10), 1, 16);
                m.modShape = sliderseq::clampModShape (tEl->getIntAttribute ("modShape", 0));
                m.gateTicks = std::clamp (tEl->getDoubleAttribute ("gateTicks", 24.0), 6.0, 288.0);
                m.chokeMask = static_cast<uint16_t> (tEl->getIntAttribute ("chokeMask", 0) & 0xffff)
                            & static_cast<uint16_t> (~(1u << static_cast<unsigned> (idx)));
                m.activeZones = std::clamp (tEl->getIntAttribute ("activeZones", 12), 1, sliderseq::slotsPerTrack);
                m.groove = std::clamp (tEl->getIntAttribute ("groove", 100), 0, 100);
            }
            restored.activeMappingPreset = std::move (mp);
            continue;
        }
        if (! element->hasTagName ("Track")) continue;
        const int index = element->getIntAttribute ("index", -1);
        if (index < 0 || index >= sliderseq::maxTracks) continue;
        auto& track = restored.sequence.tracks[static_cast<size_t> (index)];
        track.name = element->getStringAttribute ("name", track.name).toStdString();
        track.activeZones = std::clamp (element->getIntAttribute ("activeZones", 12), 1, sliderseq::slotsPerTrack);
        if (savedVersion < 4) {
            const int legacySlot = std::clamp (element->getIntAttribute ("slot", 0), 0, 11);
            track.slot = legacySlot + 1;
        } else {
            track.slot = std::clamp (element->getIntAttribute ("slot", 1), 0, track.activeZones);
        }
        const auto requestedRate = element->getDoubleAttribute ("rate", 1.0);
        track.rate = *std::min_element (kRates.begin(), kRates.end(), [=] (double a, double b) {
            return std::abs (a - requestedRate) < std::abs (b - requestedRate);
        });
        track.midiNote = std::clamp (element->getIntAttribute ("note", track.midiNote), 0, 127);
        track.midiChannel = std::clamp (element->getIntAttribute ("channel", track.midiChannel), 1, 16);
        track.velocity = std::clamp (element->getIntAttribute ("velocity", track.velocity), 1, 127);
        if (savedVersion < 4) {
            legacyGrooves[static_cast<size_t> (index)] = std::clamp (element->getIntAttribute ("groove", 50), 0, 100);
        } else {
            track.groove = std::clamp (element->getIntAttribute ("groove", 100), 0, 100);
        }
        if (element->hasAttribute ("modShape"))
            track.modShape = sliderseq::clampModShape (element->getIntAttribute ("modShape", 0));
        else
            track.modShape = sliderseq::legacyModShape (index, element->getIntAttribute ("modMode", 0));
        track.modAmount = std::clamp (element->getIntAttribute ("modAmount", track.modAmount), 0, 100);
        track.loopStepsOverride = std::clamp (element->getIntAttribute ("loopStepsOverride", 0), 0, 64);
        if (element->hasAttribute ("gateTicks"))
            track.gateTicks = std::clamp (element->getDoubleAttribute ("gateTicks", 24.0), 6.0, 288.0);
        else
            track.gateTicks = (savedVersion < 3 && index == 3) ? 144.0 : 24.0;
        if (element->hasAttribute ("chokeMask"))
            track.chokeMask = static_cast<uint16_t> (element->getIntAttribute ("chokeMask", 0) & 0xffff)
                            & static_cast<uint16_t> (~(1u << static_cast<unsigned> (index)));
        else
            track.chokeMask = (savedVersion < 3 && index == 3) ? static_cast<uint16_t> (1u << 2) : 0u;
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
                if (child->hasAttribute ("velocityScale"))
                    pattern.velocityScale = static_cast<float> (std::clamp (child->getDoubleAttribute ("velocityScale", 1.0), 0.1, 2.0));
                else
                    pattern.velocityScale = (savedVersion < 3 && slotIdx >= 9) ? 0.72f : 1.0f;
                const auto steps = child->getStringAttribute ("steps");
                for (int step = 0; step < 64; ++step)
                    pattern.steps[static_cast<size_t> (step)] = step < steps.length() && steps[step] == '1';
            } else if (child->hasTagName ("Binding")) {
                const int ctrl = child->getIntAttribute ("control", -1);
                if (ctrl < 0 || ctrl >= controlCount) continue;
                auto& binding = restored.bindings[static_cast<size_t> (index)][static_cast<size_t> (ctrl)];
                binding.cc = std::clamp (child->getIntAttribute ("cc", -1), -1, 127);
                binding.channel = std::clamp (child->getIntAttribute ("channel", 0), 0, 16);
                binding.kind = static_cast<BindingKind> (std::clamp (child->getIntAttribute ("kind", 0), 0, 1));
                const int defaultTrigger = child->hasAttribute ("triggerMode") ? 0 : static_cast<int> (TriggerMode::momentary);
                binding.triggerMode = static_cast<TriggerMode> (std::clamp (child->getIntAttribute ("triggerMode", defaultTrigger), 0, 1));
                binding.lastHigh = false;
            }
        }
    }

    if (savedVersion < 4) {
        int maxAbsDelta = 0;
        int dominantSigned = 0;
        for (int i = 0; i < restored.sequence.trackCount; ++i) {
            const int delta = legacyGrooves[static_cast<size_t> (i)] - 50;
            if (std::abs (delta) > maxAbsDelta) {
                maxAbsDelta = std::abs (delta);
                dominantSigned = delta;
            }
        }
        if (maxAbsDelta == 0) {
            restored.sequence.globalSwing = 0;
            for (auto& t : restored.sequence.tracks) t.groove = 100;
        } else {
            restored.sequence.globalSwing = std::clamp (dominantSigned * 2, -100, 100);
            for (int i = 0; i < sliderseq::maxTracks; ++i) {
                const int d = std::abs (legacyGrooves[static_cast<size_t> (i)] - 50);
                restored.sequence.tracks[static_cast<size_t> (i)].groove =
                    std::clamp (static_cast<int> (std::lround (100.0 * d / maxAbsDelta)), 0, 100);
            }
        }
    }

    auto* savedParams = xml->getChildByName (apvts.state.getType());
    if (savedVersion >= 4 && savedParams != nullptr) {
        apvts.replaceState (juce::ValueTree::fromXml (*savedParams));
        std::lock_guard<std::mutex> lock (messageMutex);
        drainAudioToUiLocked();
        messageState = std::move (restored);
        lastSnapshotGlobalSwing = -9999.0f;
        for (int i = 0; i < sliderseq::maxTracks; ++i)
            lastSnapshotHostValues[static_cast<size_t> (i)].fill (-9999.0f);
        syncApvtsToSnapshotLocked();
        publishFullStateLocked();
    } else {
        ApvtsSyncValues apvtsValues;
        {
            std::lock_guard<std::mutex> lock (messageMutex);
            drainAudioToUiLocked();
            messageState = std::move (restored);
            apvtsValues = captureApvtsSyncValuesLocked();
            publishFullStateLocked();
        }
        applyApvtsSyncValues (apvtsValues);
    }
}

juce::AudioProcessorEditor* SliderRhythmMidiProcessor::createEditor() { return new SliderRhythmMidiEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SliderRhythmMidiProcessor(); }
