#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr std::array<double, 5> kRates { 1.0 / 3.0, 0.5, 1.0, 2.0, 3.0 };
constexpr std::array<int, 5> kGrids { 12, 16, 32, 48, 64 };
constexpr size_t kMidiBufferBytes = 65536;

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

inline void addCCRaw (juce::MidiBuffer& buf, int channel, int cc, int val, int sample) noexcept {
    const uint8_t bytes[3] {
        static_cast<uint8_t> (0xb0u | static_cast<unsigned> (std::clamp (channel, 1, 16) - 1)),
        static_cast<uint8_t> (std::clamp (cc, 0, 127)),
        static_cast<uint8_t> (std::clamp (val, 0, 127))
    };
    buf.addEvent (bytes, 3, sample);
}

inline sliderseq::PlaybackMode clampPlaybackMode (int raw) noexcept {
    return static_cast<sliderseq::PlaybackMode> (std::clamp (raw, 0, 2));
}

inline size_t voiceKey (int channel, int note) noexcept {
    return static_cast<size_t> ((std::clamp (channel, 1, 16) - 1) * 128 + std::clamp (note, 0, 127));
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

    // Gen 1 core per-track parameters (AU versionHint = 1).
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "slot"), upstreamcompat::kAuHintGen1CoreTrackParams },
            trackPrefix (i) + "Slot", 0, 13, 1,
            juce::AudioParameterIntAttributes()
                .withStringFromValueFunction ([] (int v, int) {
                    if (v <= 0) return juce::String ("0 BOT");
                    if (v >= 13) return juce::String ("TOP");
                    return juce::String (v);
                })
                .withValueFromStringFunction ([] (const juce::String& text) {
                    const auto t = text.trim().toUpperCase();
                    if (t == "TOP" || t == "T" || t == "THRU" || t == "T THRU" || t == "T BLK") return 13;
                    if (t == "0 BOT" || t == "BOT" || t == "EMPTY" || t == "0 BLOCK" || t == "0 BLK" || t == "0 THRU") return 0;
                    return std::clamp (t.getIntValue(), 0, 13);
                })));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { paramId (i, "mute"), upstreamcompat::kAuHintGen1CoreTrackParams },
            trackPrefix (i) + "Mute", false));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { paramId (i, "solo"), upstreamcompat::kAuHintGen1CoreTrackParams },
            trackPrefix (i) + "Solo", false));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "velocity"), upstreamcompat::kAuHintGen1CoreTrackParams },
            trackPrefix (i) + "Velocity", 1, 127, 100));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { paramId (i, "rate"), upstreamcompat::kAuHintGen1CoreTrackParams },
            trackPrefix (i) + "Rate", rateNames, 2));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "groove"), upstreamcompat::kAuHintGen1CoreTrackParams },
            trackPrefix (i) + "Swing %", 0, 100, 100));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "mod_amt"), upstreamcompat::kAuHintGen1CoreTrackParams },
            trackPrefix (i) + "Mod Amt", 0, 100, 50));
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "loop_steps"), upstreamcompat::kAuHintGen1CoreTrackParams },
            trackPrefix (i) + "Loop Steps", 0, 64, 0));

    // Gen 2 Global Swing parameter (AU versionHint = 2 so Gen 1 indices remain stable).
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { globalSwingParamId(), upstreamcompat::kAuHintGen2GlobalSwing },
        "Global Swing", -100, 100, 0,
        juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) {
            if (v > 0) return "+" + juce::String (v) + "%";
            return juce::String (v) + "%";
        })));

    // Gen 3 Per-track Macro parameters (AU versionHint = 3).
    for (int i = 0; i < sliderseq::maxTracks; ++i)
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (i, "macro"), upstreamcompat::kAuHintGen3TrackMacro },
            trackPrefix (i) + "Macro", 0, 100, 0));
    return layout;
}

SliderRhythmMidiProcessor::SliderRhythmMidiProcessor()
    : AudioProcessor (makeBusesProperties()),
      isLogicAuHost (wrapperType == wrapperType_AudioUnit && juce::PluginHostType().isLogic()),
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
        ptrs.macro = apvts.getRawParameterValue (paramId (i, "macro"));

        const std::array<float, kTrackHostParamCount> initVals {
            ptrs.slot->load(), ptrs.mute->load(), ptrs.solo->load(), ptrs.velocity->load(),
            ptrs.rate->load(), ptrs.groove->load(), ptrs.modAmount->load(), ptrs.loopSteps->load(),
            ptrs.macro->load()
        };
        lastHostParamValues[static_cast<size_t> (i)] = initVals;
        lastSnapshotHostValues[static_cast<size_t> (i)] = initVals;
    }
    audioState = makeAudioState (messageState, 0);
    fullSyncSlots[0] = audioState;
    audioRecoverySlots[0] = audioState;
    rebuildControlLookupMasks();
    outputMidi.ensureSize (kMidiBufferBytes);
}

void SliderRhythmMidiProcessor::setGlobalSwingValue (int plainValue, bool wrapGesture) {
    const int clamped = std::clamp (plainValue, -100, 100);
    if (auto* param = apvts.getParameter (globalSwingParamId())) {
        if (wrapGesture) param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (clamped)));
        if (wrapGesture) param->endChangeGesture();
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

void SliderRhythmMidiProcessor::syncSingleTrackSlotApvts (int trackIndex) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
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
        pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (trackIndex)][0] = false;
    }
}

void SliderRhythmMidiProcessor::setTrackActiveZones (int trackIndex, int zones) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
    StateOp op;
    op.kind = OpKind::setActiveZones;
    op.track = trackIndex;
    op.arg0 = std::clamp (zones, 1, sliderseq::slotsPerTrack);
    applyUiOp (op);
    syncSingleTrackSlotApvts (trackIndex);
}

void SliderRhythmMidiProcessor::setTrackTopMode (int trackIndex, sliderseq::EndZoneMode mode) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
    StateOp op;
    op.kind = OpKind::setTopMode;
    op.track = trackIndex;
    op.arg0 = static_cast<int> (mode);
    applyUiOp (op);
    syncSingleTrackSlotApvts (trackIndex);
}

void SliderRhythmMidiProcessor::setTrackMidMode (int trackIndex, sliderseq::MidRouteMode mode) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
    StateOp op;
    op.kind = OpKind::setMidMode;
    op.track = trackIndex;
    op.arg0 = static_cast<int> (mode);
    applyUiOp (op);
}

void SliderRhythmMidiProcessor::setTrackBottomMode (int trackIndex, sliderseq::EndZoneMode mode) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
    StateOp op;
    op.kind = OpKind::setBottomMode;
    op.track = trackIndex;
    op.arg0 = static_cast<int> (mode);
    applyUiOp (op);
    syncSingleTrackSlotApvts (trackIndex);
}

void SliderRhythmMidiProcessor::setTrackMacroMode (int trackIndex, sliderseq::MacroMode mode, bool resetToNeutral) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
    // Publish macroMode and neutral macroValue together in one atomic transaction.
    edit ([trackIndex, mode, resetToNeutral] (Snapshot& s) {
        auto& t = s.sequence.tracks[static_cast<size_t> (trackIndex)];
        t.macroMode = mode;
        if (resetToNeutral)
            t.macroValue = sliderseq::defaultMacroValueForMode (mode);
    });
}

void SliderRhythmMidiProcessor::setInputChannelMatch (sliderseq::InputChannelMatch matchMode) {
    StateOp op;
    op.kind = OpKind::setInputChannelMatch;
    op.arg0 = static_cast<int> (matchMode);
    applyUiOp (op);
}

void SliderRhythmMidiProcessor::loadMappingPreset (const mappingpresets::Preset& preset) {
    edit ([&preset] (Snapshot& s) {
        mappingpresets::applyToState (preset, s.sequence);
        s.activeMappingPreset = preset;
    });
    const int armed = learnTarget.load (std::memory_order_acquire);
    if (armed >= 0 && (armed / controlCount) >= preset.trackCount)
        cancelMidiLearn();
}

void SliderRhythmMidiProcessor::setActiveMappingPresetBaseline (const mappingpresets::Preset& preset) {
    edit ([&preset] (Snapshot& s) {
        s.activeMappingPreset = preset;
    });
}

void SliderRhythmMidiProcessor::setHostParamValue (int trackIndex, const char* suffix, float plainValue, bool wrapGesture) {
    if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return;
    const juce::String key (suffix);
    float effectiveValue = plainValue;
    if (key == "slot") {
        const auto cur = snapshot();
        const auto& t = cur->sequence.tracks[static_cast<size_t> (trackIndex)];
        effectiveValue = static_cast<float> (sliderseq::clampTrackSlot (
            static_cast<int> (std::lround (plainValue)), t.activeZones, t.bottomMode, t.topMode));
    }

    if (auto* param = apvts.getParameter (paramId (trackIndex, suffix))) {
        if (wrapGesture) param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (effectiveValue));
        if (wrapGesture) param->endChangeGesture();
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
    else if (key == "macro") { op.kind = OpKind::setMacroValue; op.arg0 = static_cast<int> (std::lround (effectiveValue)); paramSlotIdx = 8; }
    else { mapped = false; }

    if (paramSlotIdx >= 0) {
        std::lock_guard<std::mutex> lock (messageMutex);
        const auto& ptrs = paramPtrs[static_cast<size_t> (trackIndex)];
        const std::array<std::atomic<float>*, kTrackHostParamCount> raw {
            ptrs.slot, ptrs.mute, ptrs.solo, ptrs.velocity, ptrs.rate,
            ptrs.groove, ptrs.modAmount, ptrs.loopSteps, ptrs.macro
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
    currentSampleRate = (std::isfinite (sampleRate) && sampleRate > 0.0) ? sampleRate : 44100.0;
    wasPlaying = false;
    hasExpectedTick = false;
    lastBpm = 0.0;
    ++transportGeneration;
    // Preserve activeNotes / passedThruCount / sustainPedalDown so the first non-zero block flushes Note Offs cleanly.
    pendingFlushOnNextBlock.store (true, std::memory_order_release);
    generatedNotes.store (0, std::memory_order_relaxed);
    for (auto& step : activeSteps) step.store (-1, std::memory_order_relaxed);
    outputMidi.ensureSize (kMidiBufferBytes);
}

SliderRhythmMidiProcessor::FullAudioState SliderRhythmMidiProcessor::makeAudioState (const Snapshot& src, uint64_t seq) noexcept {
    FullAudioState out;
    out.seq = seq;
    const auto mode = src.playRecordedMidi ? sliderseq::PlaybackMode::thru : src.playbackMode;
    out.render = sliderseq::toRenderState (src.sequence, mode);
    out.bindings = src.bindings;
    return out;
}

bool SliderRhythmMidiProcessor::shouldPassNoteThrough (const sliderseq::RenderState& render, int channel, int note) noexcept {
    if (render.playbackMode == sliderseq::PlaybackMode::thru)
        return true;

    const int count = std::clamp (render.trackCount, 0, sliderseq::maxTracks);
    bool anySolo = false;
    int exactOwners = 0;
    int noteOwners = 0;
    for (int i = 0; i < count; ++i) {
        const auto& t = render.tracks[static_cast<size_t> (i)];
        if (t.solo) anySolo = true;
        if (t.midiNote == note) {
            ++noteOwners;
            if (t.midiChannel == channel) ++exactOwners;
        }
    }

    const bool allowNoteFallback = (render.inputChannelMatch == sliderseq::InputChannelMatch::omni)
                                || (render.inputChannelMatch == sliderseq::InputChannelMatch::autoLiveCh1 && channel == 1);

    if (exactOwners > 0) {
        for (int i = 0; i < count; ++i) {
            const auto& t = render.tracks[static_cast<size_t> (i)];
            if (t.midiNote != note || t.midiChannel != channel) continue;
            const bool preMuteThru = sliderseq::trackPassesInputPreMute (
                t.slot, t.activeZones, t.bottomMode, t.midMode, t.topMode);
            if (preMuteThru && ! t.muted && (! anySolo || t.solo))
                return true;
        }
        return false;
    }

    if (noteOwners > 0 && allowNoteFallback) {
        for (int i = 0; i < count; ++i) {
            const auto& t = render.tracks[static_cast<size_t> (i)];
            if (t.midiNote != note) continue;
            const bool preMuteThru = sliderseq::trackPassesInputPreMute (
                t.slot, t.activeZones, t.bottomMode, t.midMode, t.topMode);
            if (preMuteThru && ! t.muted && (! anySolo || t.solo))
                return true;
        }
        return false;
    }

    return render.unmappedMode == sliderseq::UnmappedRouteMode::thru;
}

void SliderRhythmMidiProcessor::applyOpToAudio (const StateOp& op, FullAudioState& target) noexcept {
    auto& render = target.render;
    const int t = std::clamp (op.track, 0, sliderseq::maxTracks - 1);
    auto& track = render.tracks[static_cast<size_t> (t)];
    const int s = std::clamp (op.slotIndex, 0, sliderseq::slotsPerTrack - 1);

    switch (op.kind) {
        case OpKind::setTrackCount: render.trackCount = std::clamp (op.arg0, 1, sliderseq::maxTracks); break;
        case OpKind::setPlaybackMode: render.playbackMode = clampPlaybackMode (op.arg0); break;
        case OpKind::setUnmappedMode: render.unmappedMode = sliderseq::clampUnmappedRouteMode (op.arg0); break;
        case OpKind::setInputChannelMatch: render.inputChannelMatch = sliderseq::clampInputChannelMatch (op.arg0); break;
        case OpKind::setGlobalSwing: render.globalSwing = std::clamp (op.arg0, -100, 100); break;
        case OpKind::setSlot:
            track.slot = sliderseq::clampTrackSlot (op.arg0, track.activeZones, track.bottomMode, track.topMode);
            if (sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode))
                track.lastPatternSlot = track.slot;
            break;
        case OpKind::setActiveZones: {
            const int oldZones = track.activeZones;
            const int newZones = std::clamp (op.arg0, 1, sliderseq::slotsPerTrack);
            track.activeZones = newZones;
            track.slot = sliderseq::remapSlotForConfigChange (
                track.slot, oldZones, track.bottomMode, track.topMode,
                newZones, track.bottomMode, track.topMode);
            if (sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode))
                track.lastPatternSlot = track.slot;
            else
                track.lastPatternSlot = std::clamp (track.lastPatternSlot, 1, track.activeZones);
            break;
        }
        case OpKind::setTopMode: {
            const auto oldTop = track.topMode;
            const auto newTop = sliderseq::clampEndZoneMode (op.arg0);
            track.topMode = newTop;
            track.slot = sliderseq::remapSlotForConfigChange (
                track.slot, track.activeZones, track.bottomMode, oldTop,
                track.activeZones, track.bottomMode, newTop);
            if (sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode))
                track.lastPatternSlot = track.slot;
            break;
        }
        case OpKind::setMidMode:
            track.midMode = sliderseq::clampMidRouteMode (op.arg0);
            break;
        case OpKind::setBottomMode: {
            const auto oldBot = track.bottomMode;
            const auto newBot = sliderseq::clampEndZoneMode (op.arg0);
            track.bottomMode = newBot;
            track.slot = sliderseq::remapSlotForConfigChange (
                track.slot, track.activeZones, oldBot, track.topMode,
                track.activeZones, newBot, track.topMode);
            if (sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode))
                track.lastPatternSlot = track.slot;
            break;
        }
        case OpKind::setMacroMode:
            track.macroMode = sliderseq::clampMacroMode (op.arg0);
            break;
        case OpKind::setMacroValue:
            track.macroValue = std::clamp (op.arg0, 0, 100);
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
        case OpKind::setUnmappedMode:
            target.sequence.unmappedMode = sliderseq::clampUnmappedRouteMode (op.arg0);
            break;
        case OpKind::setInputChannelMatch:
            target.sequence.inputChannelMatch = sliderseq::clampInputChannelMatch (op.arg0);
            break;
        case OpKind::setGlobalSwing:
            target.sequence.globalSwing = std::clamp (op.arg0, -100, 100);
            break;
        case OpKind::setSlot:
            track.slot = sliderseq::clampTrackSlot (op.arg0, track.activeZones, track.bottomMode, track.topMode);
            if (sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode))
                track.lastPatternSlot = track.slot;
            break;
        case OpKind::setActiveZones: {
            const int oldZones = track.activeZones;
            const int newZones = std::clamp (op.arg0, 1, sliderseq::slotsPerTrack);
            track.activeZones = newZones;
            track.slot = sliderseq::remapSlotForConfigChange (
                track.slot, oldZones, track.bottomMode, track.topMode,
                newZones, track.bottomMode, track.topMode);
            if (sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode))
                track.lastPatternSlot = track.slot;
            else
                track.lastPatternSlot = std::clamp (track.lastPatternSlot, 1, track.activeZones);
            break;
        }
        case OpKind::setTopMode: {
            const auto oldTop = track.topMode;
            const auto newTop = sliderseq::clampEndZoneMode (op.arg0);
            track.topMode = newTop;
            track.slot = sliderseq::remapSlotForConfigChange (
                track.slot, track.activeZones, track.bottomMode, oldTop,
                track.activeZones, track.bottomMode, newTop);
            if (sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode))
                track.lastPatternSlot = track.slot;
            break;
        }
        case OpKind::setMidMode:
            track.midMode = sliderseq::clampMidRouteMode (op.arg0);
            break;
        case OpKind::setBottomMode: {
            const auto oldBot = track.bottomMode;
            const auto newBot = sliderseq::clampEndZoneMode (op.arg0);
            track.bottomMode = newBot;
            track.slot = sliderseq::remapSlotForConfigChange (
                track.slot, track.activeZones, oldBot, track.topMode,
                track.activeZones, newBot, track.topMode);
            if (sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode))
                track.lastPatternSlot = track.slot;
            break;
        }
        case OpKind::setMacroMode:
            track.macroMode = sliderseq::clampMacroMode (op.arg0);
            break;
        case OpKind::setMacroValue:
            track.macroValue = std::clamp (op.arg0, 0, 100);
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
    auto recordAudioHostDelta = [this] (const StateOp& op) {
        const int t = std::clamp (op.track, 0, sliderseq::maxTracks - 1);
        const auto& tr = messageState.sequence.tracks[static_cast<size_t> (t)];
        auto mark = [&] (int pIdx, float v) {
            pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (t)][static_cast<size_t> (pIdx)] = true;
            pendingAudioCcApvtsDelta.values[static_cast<size_t> (t)][static_cast<size_t> (pIdx)] = v;
            hasPendingAudioCcApvtsDelta = true;
            pendingAudioCcSyncFlag.store (true, std::memory_order_release);
        };
        switch (op.kind) {
            case OpKind::setGlobalSwing:
                pendingAudioCcApvtsDelta.syncGlobalSwing = true;
                pendingAudioCcApvtsDelta.globalSwing = static_cast<float> (messageState.sequence.globalSwing);
                hasPendingAudioCcApvtsDelta = true;
                pendingAudioCcSyncFlag.store (true, std::memory_order_release);
                break;
            case OpKind::setSlot: mark (0, static_cast<float> (tr.slot)); break;
            case OpKind::setMuted: mark (1, tr.muted ? 1.0f : 0.0f); break;
            case OpKind::setSolo: mark (2, tr.solo ? 1.0f : 0.0f); break;
            case OpKind::setVelocity: mark (3, static_cast<float> (tr.velocity)); break;
            case OpKind::setRate: mark (4, static_cast<float> (rateToIndex (tr.rate))); break;
            case OpKind::setGroove: mark (5, static_cast<float> (tr.groove)); break;
            case OpKind::setModAmount: mark (6, static_cast<float> (tr.modAmount)); break;
            case OpKind::setLoopStepsOverride: mark (7, static_cast<float> (tr.loopStepsOverride)); break;
            case OpKind::setMacroValue: mark (8, static_cast<float> (tr.macroValue)); break;
            default: break;
        }
    };

    // If audioToUiFifo overflowed while editor was closed, recover complete audio-authored state first.
    if ((audioRecSharedSlot.load (std::memory_order_acquire) & kDirtyBit) != 0u) {
        const uint32_t prev = audioRecSharedSlot.exchange (static_cast<uint32_t> (audioRecReaderSlot),
                                                           std::memory_order_acq_rel);
        audioRecReaderSlot = static_cast<int> (prev & 3u);
        const auto& rec = audioRecoverySlots[static_cast<size_t> (audioRecReaderSlot)];
        messageState.sequence.globalSwing = rec.render.globalSwing;
        messageState.bindings = rec.bindings;
        pendingAudioCcApvtsDelta.syncGlobalSwing = true;
        pendingAudioCcApvtsDelta.globalSwing = static_cast<float> (rec.render.globalSwing);
        hasPendingAudioCcApvtsDelta = true;
        pendingAudioCcSyncFlag.store (true, std::memory_order_release);
        for (int i = 0; i < sliderseq::maxTracks; ++i) {
            const auto& rt = rec.render.tracks[static_cast<size_t> (i)];
            auto& mt = messageState.sequence.tracks[static_cast<size_t> (i)];
            mt.slot = rt.slot;
            mt.lastPatternSlot = rt.lastPatternSlot;
            mt.rate = rt.rate;
            mt.velocity = rt.velocity;
            mt.groove = rt.groove;
            mt.modShape = rt.modShape;
            mt.modAmount = rt.modAmount;
            mt.midiNote = rt.midiNote;
            mt.midiChannel = rt.midiChannel;
            mt.muted = rt.muted;
            mt.solo = rt.solo;
            mt.macroValue = rt.macroValue;
            for (int p = 0; p < sliderseq::slotsPerTrack; ++p)
                sliderseq::applyRenderPattern (rt.patterns[static_cast<size_t> (p)], mt.patterns[static_cast<size_t> (p)]);
            const std::array<float, kTrackHostParamCount> row {
                static_cast<float> (mt.slot), mt.muted ? 1.0f : 0.0f, mt.solo ? 1.0f : 0.0f,
                static_cast<float> (mt.velocity), static_cast<float> (rateToIndex (mt.rate)),
                static_cast<float> (mt.groove), static_cast<float> (mt.modAmount),
                static_cast<float> (mt.loopStepsOverride), static_cast<float> (mt.macroValue)
            };
            pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (i)].fill (true);
            pendingAudioCcApvtsDelta.values[static_cast<size_t> (i)] = row;
        }
    }

    const int ready = audioToUiFifo.getNumReady();
    if (ready <= 0) return;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    audioToUiFifo.prepareToRead (ready, start1, size1, start2, size2);
    for (int i = 0; i < size1; ++i) {
        const auto& op = audioToUiOps[static_cast<size_t> (start1 + i)];
        applyOpToSnapshot (op, messageState);
        recordAudioHostDelta (op);
    }
    for (int i = 0; i < size2; ++i) {
        const auto& op = audioToUiOps[static_cast<size_t> (start2 + i)];
        applyOpToSnapshot (op, messageState);
        recordAudioHostDelta (op);
    }
    audioToUiFifo.finishedRead (size1 + size2);
}

SliderRhythmMidiProcessor::ApvtsSyncDelta SliderRhythmMidiProcessor::captureDirtyApvtsDeltaLocked (
    const Snapshot& before, const Snapshot& after) {
    ApvtsSyncDelta delta;
    if (after.sequence.globalSwing != before.sequence.globalSwing) {
        delta.syncGlobalSwing = true;
        delta.globalSwing = static_cast<float> (after.sequence.globalSwing);
        pendingAudioCcApvtsDelta.syncGlobalSwing = false;
    }

    auto extractRow = [] (const sliderseq::Track& t) -> std::array<float, kTrackHostParamCount> {
        return {
            static_cast<float> (t.slot), t.muted ? 1.0f : 0.0f, t.solo ? 1.0f : 0.0f,
            static_cast<float> (t.velocity), static_cast<float> (rateToIndex (t.rate)),
            static_cast<float> (t.groove), static_cast<float> (t.modAmount),
            static_cast<float> (t.loopStepsOverride), static_cast<float> (t.macroValue)
        };
    };

    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto bRow = extractRow (before.sequence.tracks[static_cast<size_t> (i)]);
        const auto aRow = extractRow (after.sequence.tracks[static_cast<size_t> (i)]);
        for (int p = 0; p < kTrackHostParamCount; ++p) {
            if (aRow[static_cast<size_t> (p)] != bRow[static_cast<size_t> (p)]) {
                delta.dirty[static_cast<size_t> (i)][static_cast<size_t> (p)] = true;
                delta.values[static_cast<size_t> (i)][static_cast<size_t> (p)] = aRow[static_cast<size_t> (p)];
                pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (i)][static_cast<size_t> (p)] = false;
            }
        }
    }
    return delta;
}

SliderRhythmMidiProcessor::ApvtsSyncDelta SliderRhythmMidiProcessor::captureFullApvtsDeltaLocked (const Snapshot& state) {
    ApvtsSyncDelta delta;
    delta.syncGlobalSwing = true;
    delta.globalSwing = static_cast<float> (state.sequence.globalSwing);
    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& t = state.sequence.tracks[static_cast<size_t> (i)];
        const std::array<float, kTrackHostParamCount> row {
            static_cast<float> (t.slot), t.muted ? 1.0f : 0.0f, t.solo ? 1.0f : 0.0f,
            static_cast<float> (t.velocity), static_cast<float> (rateToIndex (t.rate)),
            static_cast<float> (t.groove), static_cast<float> (t.modAmount),
            static_cast<float> (t.loopStepsOverride), static_cast<float> (t.macroValue)
        };
        delta.dirty[static_cast<size_t> (i)].fill (true);
        delta.values[static_cast<size_t> (i)] = row;
    }
    return delta;
}

void SliderRhythmMidiProcessor::applyApvtsSyncDelta (const ApvtsSyncDelta& delta) {
    // Must be called WITHOUT holding messageMutex: setValueNotifyingHost synchronously triggers
    // JUCE ParameterAttachment callbacks on the message thread that may read processor.snapshot().
    bool anyWritten = false;
    if (delta.syncGlobalSwing) {
        if (auto* gParam = apvts.getParameter (globalSwingParamId())) {
            const float norm = gParam->convertTo0to1 (delta.globalSwing);
            if (std::abs (gParam->getValue() - norm) > 1e-6f)
                gParam->setValueNotifyingHost (norm);
            anyWritten = true;
        }
    }

    static constexpr std::array<const char*, kTrackHostParamCount> kSuffixes {
        "slot", "mute", "solo", "velocity", "rate", "groove", "mod_amt", "loop_steps", "macro"
    };
    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& dRow = delta.dirty[static_cast<size_t> (i)];
        const auto& vRow = delta.values[static_cast<size_t> (i)];
        for (int p = 0; p < kTrackHostParamCount; ++p) {
            if (! dRow[static_cast<size_t> (p)]) continue;
            if (auto* param = apvts.getParameter (paramId (i, kSuffixes[static_cast<size_t> (p)]))) {
                const float norm = param->convertTo0to1 (vRow[static_cast<size_t> (p)]);
                if (std::abs (param->getValue() - norm) > 1e-6f)
                    param->setValueNotifyingHost (norm);
                anyWritten = true;
            }
        }
    }

    if (! anyWritten) return;

    // Record the exact APVTS values we just wrote so syncApvtsToSnapshotLocked does not treat them as external host moves.
    std::lock_guard<std::mutex> lock (messageMutex);
    if (delta.syncGlobalSwing && globalSwingParam != nullptr)
        lastSnapshotGlobalSwing = globalSwingParam->load (std::memory_order_relaxed);
    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& dRow = delta.dirty[static_cast<size_t> (i)];
        const auto& ptrs = paramPtrs[static_cast<size_t> (i)];
        const std::array<std::atomic<float>*, kTrackHostParamCount> raw {
            ptrs.slot, ptrs.mute, ptrs.solo, ptrs.velocity, ptrs.rate,
            ptrs.groove, ptrs.modAmount, ptrs.loopSteps, ptrs.macro
        };
        for (int p = 0; p < kTrackHostParamCount; ++p) {
            if (dRow[static_cast<size_t> (p)])
                lastSnapshotHostValues[static_cast<size_t> (i)][static_cast<size_t> (p)] =
                    raw[static_cast<size_t> (p)]->load (std::memory_order_relaxed);
        }
    }
}

void SliderRhythmMidiProcessor::syncAudioCcToApvts() {
    ApvtsSyncDelta delta;
    bool needSync = false;
    {
        std::lock_guard<std::mutex> lock (messageMutex);
        drainAudioToUiLocked();
        if (hasPendingAudioCcApvtsDelta) {
            delta = pendingAudioCcApvtsDelta;
            pendingAudioCcApvtsDelta = {};
            hasPendingAudioCcApvtsDelta = false;
            needSync = true;
        }
        pendingAudioCcSyncFlag.store (false, std::memory_order_release);
    }
    if (needSync)
        applyApvtsSyncDelta (delta);
}

bool SliderRhythmMidiProcessor::hasPendingHostOrAudioChanges() const noexcept {
    if (pendingAudioCcSyncFlag.load (std::memory_order_acquire)
        || audioToUiFifo.getNumReady() > 0
        || (audioRecSharedSlot.load (std::memory_order_acquire) & kDirtyBit) != 0u)
        return true;
    if (globalSwingParam != nullptr && globalSwingParam->load (std::memory_order_relaxed) != lastSnapshotGlobalSwing)
        return true;
    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& ptrs = paramPtrs[static_cast<size_t> (i)];
        const auto& prev = lastSnapshotHostValues[static_cast<size_t> (i)];
        if (ptrs.slot->load (std::memory_order_relaxed) != prev[0]
            || ptrs.mute->load (std::memory_order_relaxed) != prev[1]
            || ptrs.solo->load (std::memory_order_relaxed) != prev[2]
            || ptrs.velocity->load (std::memory_order_relaxed) != prev[3]
            || ptrs.rate->load (std::memory_order_relaxed) != prev[4]
            || ptrs.groove->load (std::memory_order_relaxed) != prev[5]
            || ptrs.modAmount->load (std::memory_order_relaxed) != prev[6]
            || ptrs.loopSteps->load (std::memory_order_relaxed) != prev[7]
            || ptrs.macro->load (std::memory_order_relaxed) != prev[8])
            return true;
    }
    return false;
}

bool SliderRhythmMidiProcessor::syncApvtsToSnapshotLocked() {
    bool anyChanged = false;
    if (globalSwingParam != nullptr) {
        const float vGlobal = globalSwingParam->load (std::memory_order_relaxed);
        if (vGlobal != lastSnapshotGlobalSwing) {
            messageState.sequence.globalSwing = std::clamp (static_cast<int> (std::lround (vGlobal)), -100, 100);
            lastSnapshotGlobalSwing = vGlobal;
            anyChanged = true;
        }
    }

    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& ptrs = paramPtrs[static_cast<size_t> (i)];
        auto& prev = lastSnapshotHostValues[static_cast<size_t> (i)];
        auto& t = messageState.sequence.tracks[static_cast<size_t> (i)];

        const float vSlot = ptrs.slot->load (std::memory_order_relaxed);
        if (vSlot != prev[0]) {
            t.slot = sliderseq::clampTrackSlot (static_cast<int> (std::lround (vSlot)),
                                                t.activeZones, t.bottomMode, t.topMode);
            if (sliderseq::isTrackGeneratingZone (t.slot, t.activeZones, t.bottomMode, t.topMode))
                t.lastPatternSlot = t.slot;
            prev[0] = vSlot;
            anyChanged = true;
        }
        const float vMute = ptrs.mute->load (std::memory_order_relaxed);
        if (vMute != prev[1]) { t.muted = vMute >= 0.5f; prev[1] = vMute; anyChanged = true; }
        const float vSolo = ptrs.solo->load (std::memory_order_relaxed);
        if (vSolo != prev[2]) { t.solo = vSolo >= 0.5f; prev[2] = vSolo; anyChanged = true; }
        const float vVel = ptrs.velocity->load (std::memory_order_relaxed);
        if (vVel != prev[3]) { t.velocity = std::clamp (static_cast<int> (std::lround (vVel)), 1, 127); prev[3] = vVel; anyChanged = true; }
        const float vRate = ptrs.rate->load (std::memory_order_relaxed);
        if (vRate != prev[4]) { t.rate = kRates[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (vRate)), 0, 4))]; prev[4] = vRate; anyChanged = true; }
        const float vGroove = ptrs.groove->load (std::memory_order_relaxed);
        if (vGroove != prev[5]) { t.groove = std::clamp (static_cast<int> (std::lround (vGroove)), 0, 100); prev[5] = vGroove; anyChanged = true; }
        const float vMod = ptrs.modAmount->load (std::memory_order_relaxed);
        if (vMod != prev[6]) { t.modAmount = std::clamp (static_cast<int> (std::lround (vMod)), 0, 100); prev[6] = vMod; anyChanged = true; }
        const float vLoop = ptrs.loopSteps->load (std::memory_order_relaxed);
        if (vLoop != prev[7]) { t.loopStepsOverride = std::clamp (static_cast<int> (std::lround (vLoop)), 0, 64); prev[7] = vLoop; anyChanged = true; }
        const float vMacro = ptrs.macro->load (std::memory_order_relaxed);
        if (vMacro != prev[8]) { t.macroValue = std::clamp (static_cast<int> (std::lround (vMacro)), 0, 100); prev[8] = vMacro; anyChanged = true; }
    }
    if (anyChanged)
        version.fetch_add (1, std::memory_order_release);
    return anyChanged;
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

        const float vals[kTrackHostParamCount] {
            ptrs.slot->load (std::memory_order_relaxed),
            ptrs.mute->load (std::memory_order_relaxed),
            ptrs.solo->load (std::memory_order_relaxed),
            ptrs.velocity->load (std::memory_order_relaxed),
            ptrs.rate->load (std::memory_order_relaxed),
            ptrs.groove->load (std::memory_order_relaxed),
            ptrs.modAmount->load (std::memory_order_relaxed),
            ptrs.loopSteps->load (std::memory_order_relaxed),
            ptrs.macro->load (std::memory_order_relaxed)
        };

        for (int p = 0; p < kTrackHostParamCount; ++p) {
            if (vals[p] == prev[static_cast<size_t> (p)]) continue;
            if (! capturedBefore) { before = audioState.render; capturedBefore = true; }
            prev[static_cast<size_t> (p)] = vals[p];
            changed = true;
            switch (p) {
                case 0:
                    t.slot = sliderseq::clampTrackSlot (static_cast<int> (std::lround (vals[0])),
                                                        t.activeZones, t.bottomMode, t.topMode);
                    if (sliderseq::isTrackGeneratingZone (t.slot, t.activeZones, t.bottomMode, t.topMode))
                        t.lastPatternSlot = t.slot;
                    break;
                case 1: t.muted = vals[1] >= 0.5f; break;
                case 2: t.solo = vals[2] >= 0.5f; break;
                case 3: t.velocity = std::clamp (static_cast<int> (std::lround (vals[3])), 1, 127); break;
                case 4: t.rate = kRates[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (vals[4])), 0, 4))]; break;
                case 5: t.groove = std::clamp (static_cast<int> (std::lround (vals[5])), 0, 100); break;
                case 6: t.modAmount = std::clamp (static_cast<int> (std::lround (vals[6])), 0, 100); break;
                case 7: t.loopStepsOverride = std::clamp (static_cast<int> (std::lround (vals[7])), 0, 64); break;
                case 8: t.macroValue = std::clamp (static_cast<int> (std::lround (vals[8])), 0, 100); break;
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

void SliderRhythmMidiProcessor::publishAudioRecoverySlot() noexcept {
    audioRecoverySlots[static_cast<size_t> (audioRecWriterSlot)] = audioState;
    const uint32_t prev = audioRecSharedSlot.exchange (static_cast<uint32_t> (audioRecWriterSlot) | kDirtyBit,
                                                       std::memory_order_acq_rel);
    audioRecWriterSlot = static_cast<int> (prev & 3u);
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
    } else {
        // FIFO full while editor is closed: publish full audio recovery slot so zero CC edits are ever lost.
        publishAudioRecoverySlot();
    }
    version.fetch_add (1, std::memory_order_release);
}

void SliderRhythmMidiProcessor::rebuildControlLookupMasks() noexcept {
    boundCcMask.fill (0u);
    boundNoteMask.fill (0u);
    const int count = std::clamp (audioState.render.trackCount, 0, sliderseq::maxTracks);
    for (int t = 0; t < count; ++t) {
        for (int ctrl = 0; ctrl < controlCount; ++ctrl) {
            const auto& b = audioState.bindings[static_cast<size_t> (t)][static_cast<size_t> (ctrl)];
            if (b.cc < 0 || b.cc >= 128) continue;
            if (b.kind == BindingKind::cc) boundCcMask[static_cast<size_t> (b.cc)] = 1u;
            else boundNoteMask[static_cast<size_t> (b.cc)] = 1u;
        }
    }
}

bool SliderRhythmMidiProcessor::isPotentialControlMessage (const uint8_t* data, int numBytes) const noexcept {
    if (numBytes != 3 || data == nullptr) return false;
    const uint8_t status = data[0] & 0xf0u;
    const uint8_t d1 = data[1] & 0x7fu;
    if (status == 0xb0u)
        return learnTarget.load (std::memory_order_relaxed) >= 0 || boundCcMask[d1] != 0u;
    if (status == 0x90u || status == 0x80u) {
        const size_t key = voiceKey (static_cast<int> ((data[0] & 0x0fu) + 1), d1);
        if (heldNoteControlTrack[key] != 0u) return true;
        if (! noteLearnEnabled.load (std::memory_order_relaxed)) return false;
        return learnTarget.load (std::memory_order_relaxed) >= 0 || boundNoteMask[d1] != 0u;
    }
    return false;
}

std::shared_ptr<const SliderRhythmMidiProcessor::Snapshot> SliderRhythmMidiProcessor::snapshot() {
    std::lock_guard<std::mutex> lock (messageMutex);
    // Sync host params BEFORE draining newer audio-thread CC ops so same-block CC overrides stay authoritative.
    syncApvtsToSnapshotLocked();
    drainAudioToUiLocked();
    return std::make_shared<const Snapshot> (messageState);
}

void SliderRhythmMidiProcessor::edit (const std::function<void (Snapshot&)>& change) {
    ApvtsSyncDelta apvtsDelta;
    {
        std::lock_guard<std::mutex> lock (messageMutex);
        syncApvtsToSnapshotLocked();
        drainAudioToUiLocked();
        const Snapshot before = messageState;
        const bool prevRecorded = messageState.playRecordedMidi;
        const auto prevMode = messageState.playbackMode;
        change (messageState);
        if (messageState.playRecordedMidi != prevRecorded && messageState.playbackMode == prevMode)
            messageState.playbackMode = messageState.playRecordedMidi ? sliderseq::PlaybackMode::thru : sliderseq::PlaybackMode::gen;
        else
            messageState.playRecordedMidi = (messageState.playbackMode == sliderseq::PlaybackMode::thru);
        apvtsDelta = captureDirtyApvtsDeltaLocked (before, messageState);
        publishFullStateLocked();
    }
    applyApvtsSyncDelta (apvtsDelta);
}

void SliderRhythmMidiProcessor::applyUiOp (StateOp op) {
    int newTrackCount = -1;
    {
        std::lock_guard<std::mutex> lock (messageMutex);
        syncApvtsToSnapshotLocked();
        drainAudioToUiLocked();
        applyOpToSnapshot (op, messageState);
        // Clear any older pending audio-CC APVTS delta superseded by this newer UI op.
        const int tIdx = std::clamp (op.track, 0, sliderseq::maxTracks - 1);
        switch (op.kind) {
            case OpKind::setGlobalSwing: pendingAudioCcApvtsDelta.syncGlobalSwing = false; break;
            case OpKind::setSlot:
            case OpKind::setActiveZones:
            case OpKind::setTopMode:
            case OpKind::setBottomMode: pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (tIdx)][0] = false; break;
            case OpKind::setMuted: pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (tIdx)][1] = false; break;
            case OpKind::setSolo: pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (tIdx)][2] = false; break;
            case OpKind::setExclusiveSolo:
                for (int i = 0; i < sliderseq::maxTracks; ++i) pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (i)][2] = false;
                pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (tIdx)][1] = false;
                break;
            case OpKind::setVelocity: pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (tIdx)][3] = false; break;
            case OpKind::setRate: pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (tIdx)][4] = false; break;
            case OpKind::setGroove: pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (tIdx)][5] = false; break;
            case OpKind::setModAmount: pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (tIdx)][6] = false; break;
            case OpKind::setLoopStepsOverride: pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (tIdx)][7] = false; break;
            case OpKind::setMacroValue: pendingAudioCcApvtsDelta.dirty[static_cast<size_t> (tIdx)][8] = false; break;
            default: break;
        }
        if (op.kind == OpKind::setTrackCount)
            newTrackCount = messageState.sequence.trackCount;

        // If an earlier bulk snapshot is still pending in the triple buffer, fold this op into
        // a newer bulk snapshot so the audio thread can NEVER apply triple-buffer and FIFO out of order.
        if ((sharedSyncSlot.load (std::memory_order_acquire) & kDirtyBit) != 0u) {
            publishFullStateLocked();
        } else {
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
    }
    if (newTrackCount > 0) {
        const int armed = learnTarget.load (std::memory_order_acquire);
        if (armed >= 0 && (armed / controlCount) >= newTrackCount)
            cancelMidiLearn();
    }
}

void SliderRhythmMidiProcessor::armMidiLearn (int track, Control control) {
    if (track < 0 || track >= sliderseq::maxTracks) return;
    learnTarget.store (track * controlCount + static_cast<int> (control), std::memory_order_release);
}

bool SliderRhythmMidiProcessor::canEmitNoteOnInBlock (size_t emittedCount) const noexcept {
    if (wrapperType != wrapperType_VST3) return true;
    size_t reserve = 48;
    for (const auto& a : activeNotes)
        if (a.active) ++reserve;
    for (uint8_t c : passedThruCount)
        if (c > 0) ++reserve;
    return (emittedCount + reserve) < kVst3SafeEventBudget;
}

bool SliderRhythmMidiProcessor::wasOnsetCommitted (const sliderseq::OnsetEvent& ev) const noexcept {
    for (const auto& k : committedOnsets) {
        if (k.transportGen == transportGeneration && k.trackIndex == ev.trackIndex
            && k.cycle == ev.cycle && k.stepIndex == ev.stepIndex && k.subIndex == ev.subIndex)
            return true;
    }
    return false;
}

void SliderRhythmMidiProcessor::recordCommittedOnset (const sliderseq::OnsetEvent& ev) noexcept {
    committedOnsets[committedWriteCursor] = {
        transportGeneration, ev.trackIndex, ev.cycle, ev.stepIndex, ev.subIndex
    };
    committedWriteCursor = (committedWriteCursor + 1) % committedOnsets.size();
}

void SliderRhythmMidiProcessor::flushTrackNotes (int trackIndex, int samplePos, juce::MidiBuffer& outMidi) noexcept {
    for (auto& active : activeNotes) {
        if (! active.active || active.trackIndex != trackIndex) continue;
        const size_t idx = voiceKey (active.channel, active.note);
        addNoteOffRaw (outMidi, active.channel, active.note, samplePos);
        if (voiceOwner[idx] == VoiceOwner::generated)
            voiceOwner[idx] = VoiceOwner::none;
        active.active = false;
    }
}

void SliderRhythmMidiProcessor::flushAllActiveNotes (int samplePos, juce::MidiBuffer& outMidi) noexcept {
    for (auto& active : activeNotes) {
        if (! active.active) continue;
        const size_t idx = voiceKey (active.channel, active.note);
        addNoteOffRaw (outMidi, active.channel, active.note, samplePos);
        if (voiceOwner[idx] == VoiceOwner::generated)
            voiceOwner[idx] = VoiceOwner::none;
        active.active = false;
    }
}

void SliderRhythmMidiProcessor::flushAllPassedThruNotes (int samplePos, juce::MidiBuffer& outMidi) noexcept {
    for (int chIdx = 0; chIdx < 16; ++chIdx) {
        for (int nt = 0; nt < 128; ++nt) {
            const size_t idx = static_cast<size_t> (chIdx * 128 + nt);
            if (passedThruCount[idx] > 0u) {
                if (voiceOwner[idx] == VoiceOwner::thru) {
                    addNoteOffRaw (outMidi, chIdx + 1, nt, samplePos);
                    voiceOwner[idx] = VoiceOwner::none;
                }
                passedThruCount[idx] = 0u;
            }
        }
    }
}

void SliderRhythmMidiProcessor::flushPedals (int samplePos, juce::MidiBuffer& outMidi) noexcept {
    for (int chIdx = 0; chIdx < 16; ++chIdx) {
        if (sustainPedalDown[static_cast<size_t> (chIdx)] != 0u) {
            addCCRaw (outMidi, chIdx + 1, 64, 0, samplePos);
            sustainPedalDown[static_cast<size_t> (chIdx)] = 0u;
        }
        if (sostenutoPedalDown[static_cast<size_t> (chIdx)] != 0u) {
            addCCRaw (outMidi, chIdx + 1, 66, 0, samplePos);
            sostenutoPedalDown[static_cast<size_t> (chIdx)] = 0u;
        }
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

    // Flush any live passed-through note whose owning track just switched from THRU to BLOCK/GEN or got Muted/Unsoloed.
    for (int chIdx = 0; chIdx < 16; ++chIdx) {
        for (int nt = 0; nt < 128; ++nt) {
            const size_t idx = static_cast<size_t> (chIdx * 128 + nt);
            if (passedThruCount[idx] == 0u) continue;
            if (! shouldPassNoteThrough (after, chIdx + 1, nt)) {
                if (voiceOwner[idx] == VoiceOwner::thru) {
                    addNoteOffRaw (outMidi, chIdx + 1, nt, samplePos);
                    voiceOwner[idx] = VoiceOwner::none;
                }
                passedThruCount[idx] = 0u;
            }
        }
    }
}

void SliderRhythmMidiProcessor::routeIncomingMidi (const uint8_t* data, int numBytes, int samplePos,
                                                   juce::MidiBuffer& outMidi, size_t& emittedCount) noexcept {
    if (data == nullptr || numBytes <= 0)
        return;

    const bool allThru = (audioState.render.playbackMode == sliderseq::PlaybackMode::thru);
    const bool allowOther = allThru || (audioState.render.unmappedMode == sliderseq::UnmappedRouteMode::thru);

    // SysEx or non-3-byte messages: preserve intact whenever passthrough is allowed.
    if (numBytes != 3) {
        if (allowOther) {
            outMidi.addEvent (data, numBytes, samplePos);
            ++emittedCount;
        }
        return;
    }

    const uint8_t status = static_cast<uint8_t> (data[0] & 0xf0u);
    const int ch = static_cast<int> ((data[0] & 0x0fu) + 1);
    const int d1 = static_cast<int> (data[1] & 0x7fu);
    const int d2 = static_cast<int> (data[2] & 0x7fu);

    const bool isNoteOn = (status == 0x90u && d2 > 0);
    const bool isNoteOff = (status == 0x80u || (status == 0x90u && d2 == 0));

    if (status == 0xb0u) {
        // Emergency panic / controller reset CCs are NEVER blocked by OTHER:BLK.
        if (d1 == 120 || d1 == 121 || d1 == 123) {
            if (d1 == 120 || d1 == 123) {
                for (int nt = 0; nt < 128; ++nt) {
                    const size_t idx = voiceKey (ch, nt);
                    passedThruCount[idx] = 0u;
                    if (voiceOwner[idx] == VoiceOwner::thru) voiceOwner[idx] = VoiceOwner::none;
                }
            }
            sustainPedalDown[static_cast<size_t> (ch - 1)] = 0u;
            sostenutoPedalDown[static_cast<size_t> (ch - 1)] = 0u;
            outMidi.addEvent (data, numBytes, samplePos);
            ++emittedCount;
            return;
        }
        // Sustain (CC64) and Sostenuto (CC66): always allow release if press was previously forwarded.
        if (d1 == 64) {
            auto& held = sustainPedalDown[static_cast<size_t> (ch - 1)];
            if (d2 >= 64) {
                if (allowOther) {
                    held = 1u;
                    outMidi.addEvent (data, numBytes, samplePos);
                    ++emittedCount;
                }
            } else if (held != 0u || allowOther) {
                held = 0u;
                outMidi.addEvent (data, numBytes, samplePos);
                ++emittedCount;
            }
            return;
        }
        if (d1 == 66) {
            auto& held = sostenutoPedalDown[static_cast<size_t> (ch - 1)];
            if (d2 >= 64) {
                if (allowOther) {
                    held = 1u;
                    outMidi.addEvent (data, numBytes, samplePos);
                    ++emittedCount;
                }
            } else if (held != 0u || allowOther) {
                held = 0u;
                outMidi.addEvent (data, numBytes, samplePos);
                ++emittedCount;
            }
            return;
        }
        if (allowOther) {
            outMidi.addEvent (data, numBytes, samplePos);
            ++emittedCount;
        }
        return;
    }

    if (! isNoteOn && ! isNoteOff) {
        if (allowOther) {
            outMidi.addEvent (data, numBytes, samplePos);
            ++emittedCount;
        }
        return;
    }

    const size_t idx = voiceKey (ch, d1);
    if (isNoteOff) {
        if (passedThruCount[idx] > 0u) {
            --passedThruCount[idx];
            if (passedThruCount[idx] == 0u && voiceOwner[idx] == VoiceOwner::thru) {
                outMidi.addEvent (data, numBytes, samplePos);
                ++emittedCount;
                voiceOwner[idx] = VoiceOwner::none;
            }
        }
        return;
    }

    // Note On: route through per-track 3-zone policy -> M/S gate (or unmappedMode / ALL THRU).
    if (! shouldPassNoteThrough (audioState.render, ch, d1))
        return;
    if (! canEmitNoteOnInBlock (emittedCount))
        return;

    // Close any active generated voice on the same (ch, note) cleanly before retriggering via passthrough.
    for (auto& active : activeNotes) {
        if (active.active && active.channel == ch && active.note == d1) {
            addNoteOffRaw (outMidi, ch, d1, samplePos);
            ++emittedCount;
            active.active = false;
        }
    }
    if (passedThruCount[idx] > 0u && voiceOwner[idx] == VoiceOwner::thru) {
        addNoteOffRaw (outMidi, ch, d1, samplePos);
        ++emittedCount;
    }
    if (passedThruCount[idx] < 255u)
        ++passedThruCount[idx];
    voiceOwner[idx] = VoiceOwner::thru;
    outMidi.addEvent (data, numBytes, samplePos);
    ++emittedCount;
}

void SliderRhythmMidiProcessor::syncFromUi (juce::MidiBuffer& outMidi) noexcept {
    const sliderseq::RenderState before = audioState.render;
    bool changed = false;
    bool bindingsChanged = false;

    if ((sharedSyncSlot.load (std::memory_order_acquire) & kDirtyBit) != 0u) {
        const uint32_t prev = sharedSyncSlot.exchange (static_cast<uint32_t> (readerSyncSlot),
                                                       std::memory_order_acq_rel);
        readerSyncSlot = static_cast<int> (prev & 3u);
        const auto& incoming = fullSyncSlots[static_cast<size_t> (readerSyncSlot)];
        if (incoming.seq >= audioState.seq) {
            audioState = incoming;
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
                    static_cast<float> (t.loopStepsOverride),
                    static_cast<float> (t.macroValue)
                };
            }
            changed = true;
            bindingsChanged = true;
        }
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
                    if (op.kind == OpKind::setBinding || op.kind == OpKind::setTrackCount)
                        bindingsChanged = true;
                }
            }
        };
        consume (start1, size1);
        consume (start2, size2);
        uiToAudioFifo.finishedRead (size1 + size2);
    }

    if (bindingsChanged)
        rebuildControlLookupMasks();
    if (changed)
        flushRemovedOrChangedTracks (before, audioState.render, 0, outMidi);
    pullHostParamsIntoAudio (outMidi);
}

bool SliderRhythmMidiProcessor::applyIncomingControl (const uint8_t* data, int numBytes, int samplePos,
                                                      juce::MidiBuffer& outMidi, bool allowStateMutation) noexcept {
    if (data == nullptr || numBytes != 3) return false;
    const uint8_t status = data[0] & 0xf0u;
    const int ch = static_cast<int> ((data[0] & 0x0fu) + 1);
    const int d1 = static_cast<int> (data[1] & 0x7fu);
    const int d2 = static_cast<int> (data[2] & 0x7fu);

    const bool isCC = (status == 0xb0u);
    const bool isNoteOn = (status == 0x90u && d2 > 0);
    const bool isNoteOff = (status == 0x80u || (status == 0x90u && d2 == 0));
    if (! isCC && ! isNoteOn && ! isNoteOff) return false;

    const bool noteEnabled = noteLearnEnabled.load (std::memory_order_acquire);

    // Handle active MIDI Learn arming (even in ALL THRU mode; note learn only accepts controls with defined Note actions).
    int armed = learnTarget.load (std::memory_order_acquire);
    if (armed >= 0 && (isCC || (noteEnabled && isNoteOn))) {
        const auto ctrl = static_cast<Control> (armed % controlCount);
        if (isCC || supportsNoteLearnControl (ctrl)) {
            if (learnTarget.compare_exchange_strong (armed, -1, std::memory_order_acq_rel)) {
                StateOp op;
                op.kind = OpKind::setBinding;
                op.track = armed / controlCount;
                op.slotIndex = static_cast<int> (ctrl);
                op.arg0 = d1;
                op.arg1 = ch;
                op.arg2 = static_cast<int> (isCC ? BindingKind::cc : BindingKind::note);
                op.arg3 = learnTriggerMode.load (std::memory_order_acquire);
                applyOpToAudio (op, audioState);
                audioToggleLastHigh[static_cast<size_t> (op.track)][static_cast<size_t> (op.slotIndex)] = false;
                rebuildControlLookupMasks();
                pushAudioToUiOp (op);
                return allowStateMutation; // In ALL THRU (allowStateMutation=false), don't consume passthrough MIDI.
            }
        }
    }

    if (! allowStateMutation)
        return false;

    // Guarantee HLD Note-Off release even if Note Learn was turned OFF while holding a pad.
    if (isNoteOff) {
        const size_t nKey = voiceKey (ch, d1);
        if (heldNoteControlTrack[nKey] != 0u) {
            const int heldTrack = static_cast<int> (heldNoteControlTrack[nKey]) - 1;
            const uint8_t heldKind = heldNoteControlKind[nKey];
            heldNoteControlTrack[nKey] = 0u;
            heldNoteControlKind[nKey] = 0u;
            const sliderseq::RenderState before = audioState.render;
            StateOp op;
            op.track = heldTrack;
            op.kind = heldKind == 1u ? OpKind::setMuted : OpKind::setSolo;
            op.barg = false;
            applyOpToAudio (op, audioState);
            pushAudioToUiOp (op);
            flushRemovedOrChangedTracks (before, audioState.render, samplePos, outMidi);
            return true;
        }
    }

    const BindingKind msgKind = isCC ? BindingKind::cc : BindingKind::note;
    if (msgKind == BindingKind::note && ! noteEnabled)
        return false;
    // Never eat a Note Off for a note that was previously passed through (e.g., user toggled NOTE on mid-note).
    if (isNoteOff && passedThruCount[voiceKey (ch, d1)] > 0u)
        return false;

    const sliderseq::RenderState before = audioState.render;
    bool consumed = false;

    for (int t = 0; t < audioState.render.trackCount; ++t) {
        const auto& track = audioState.render.tracks[static_cast<size_t> (t)];
        const int editPatIdx = sliderseq::resolveEditingPatternIndex (track);
        for (int ctrl = 0; ctrl < controlCount; ++ctrl) {
            const auto& binding = audioState.bindings[static_cast<size_t> (t)][static_cast<size_t> (ctrl)];
            if (binding.cc != d1 || binding.channel != ch || binding.kind != msgKind) continue;
            if (msgKind == BindingKind::note && ! supportsNoteLearnControl (static_cast<Control> (ctrl)))
                continue;
            consumed = true;

            StateOp op;
            op.track = t;
            op.slotIndex = editPatIdx;
            bool emitOp = false;

            if (msgKind == BindingKind::cc) {
                auto& lastHigh = audioToggleLastHigh[static_cast<size_t> (t)][static_cast<size_t> (ctrl)];
                switch (static_cast<Control> (ctrl)) {
                    case slot: {
                        const int minS = sliderseq::trackMinSlot (track.bottomMode);
                        const int maxS = sliderseq::trackMaxSlot (track.activeZones, track.topMode);
                        op.kind = OpKind::setSlot;
                        op.arg0 = minS + static_cast<int> (std::lround (d2 * static_cast<double> (maxS - minS) / 127.0));
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
                        } else if (high && ! lastHigh) {
                            op.kind = OpKind::setMuted;
                            op.barg = ! track.muted;
                            emitOp = true;
                        }
                        lastHigh = high;
                        break;
                    }
                    case solo: {
                        const bool high = d2 >= 64;
                        if (binding.triggerMode == TriggerMode::momentary) {
                            op.kind = OpKind::setSolo;
                            op.barg = high;
                            emitOp = true;
                        } else if (high && ! lastHigh) {
                            op.kind = OpKind::setSolo;
                            op.barg = ! track.solo;
                            emitOp = true;
                        }
                        lastHigh = high;
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
                        const auto& pat = track.patterns[static_cast<size_t> (editPatIdx)];
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
                    case macro:
                        op.kind = OpKind::setMacroValue;
                        op.arg0 = (d2 == 64 && sliderseq::isMacroBipolar (track.macroMode))
                            ? 50
                            : std::clamp (static_cast<int> (std::lround (d2 * 100.0 / 127.0)), 0, 100);
                        emitOp = true;
                        break;
                    case controlCount:
                        break;
                }
            } else {
                const size_t nKey = voiceKey (ch, d1);
                switch (static_cast<Control> (ctrl)) {
                    case mute:
                        if (binding.triggerMode == TriggerMode::momentary) {
                            op.kind = OpKind::setMuted;
                            op.barg = isNoteOn;
                            heldNoteControlTrack[nKey] = isNoteOn ? static_cast<uint8_t> (t + 1) : 0u;
                            heldNoteControlKind[nKey] = isNoteOn ? 1u : 0u;
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
                            heldNoteControlTrack[nKey] = isNoteOn ? static_cast<uint8_t> (t + 1) : 0u;
                            heldNoteControlKind[nKey] = isNoteOn ? 2u : 0u;
                            emitOp = true;
                        } else if (isNoteOn) {
                            op.kind = OpKind::setSolo;
                            op.barg = ! track.solo;
                            emitOp = true;
                        }
                        break;
                    case slot:
                        if (isNoteOn) {
                            const int minS = sliderseq::trackMinSlot (track.bottomMode);
                            const int maxS = sliderseq::trackMaxSlot (track.activeZones, track.topMode);
                            const int span = std::max (1, maxS - minS + 1);
                            op.kind = OpKind::setSlot;
                            op.arg0 = minS + ((track.slot - minS + 1) % span);
                            emitOp = true;
                        }
                        break;
                    case rate:
                        if (isNoteOn) {
                            op.kind = OpKind::setRate;
                            op.darg = kRates[static_cast<size_t> ((rateToIndex (track.rate) + 1) % 5)];
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
        if (! sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode)) {
            activeSteps[static_cast<size_t> (i)].store (-1, std::memory_order_relaxed);
            continue;
        }
        const int patIdx = std::clamp (track.slot - 1, 0, sliderseq::slotsPerTrack - 1);
        const auto& pat = track.patterns[static_cast<size_t> (patIdx)];
        const auto eff = sliderseq::computeEffectiveTrackForPattern (track, pat);
        if (eff.rate <= 0.0) {
            activeSteps[static_cast<size_t> (i)].store (-1, std::memory_order_relaxed);
            continue;
        }
        const int activeLen = sliderseq::effectivePatternLength (pat.length, pat.grid, eff.loopStepsOverride);
        const double stepTicks = pat.stepTicks();
        const double period = activeLen * stepTicks;
        if (period <= 0.0) {
            activeSteps[static_cast<size_t> (i)].store (-1, std::memory_order_relaxed);
            continue;
        }
        double phase = std::fmod (tick * eff.rate, period);
        if (phase < 0.0) phase += period;
        const int step = std::clamp (static_cast<int> (std::floor (phase / stepTicks)), 0, activeLen - 1);
        activeSteps[static_cast<size_t> (i)].store (step, std::memory_order_relaxed);
    }
}

void SliderRhythmMidiProcessor::advanceActiveNotes (int segStart, int dtSamples, bool inclusiveEnd,
                                                    juce::MidiBuffer& outMidi, size_t& emittedCount) noexcept {
    if (dtSamples < 0) return;
    for (auto& active : activeNotes) {
        if (! active.active) continue;
        const bool due = inclusiveEnd ? (active.remainingSamples <= static_cast<double> (dtSamples))
                                      : (active.remainingSamples < static_cast<double> (dtSamples));
        if (due) {
            const int offset = std::clamp (static_cast<int> (std::lround (active.remainingSamples)),
                                           0, std::max (0, dtSamples - (inclusiveEnd ? 0 : 1)));
            const size_t idx = voiceKey (active.channel, active.note);
            addNoteOffRaw (outMidi, active.channel, active.note, segStart + offset);
            ++emittedCount;
            if (voiceOwner[idx] == VoiceOwner::generated)
                voiceOwner[idx] = VoiceOwner::none;
            active.active = false;
        } else {
            active.remainingSamples -= static_cast<double> (dtSamples);
        }
    }
}

void SliderRhythmMidiProcessor::renderSegment (double segStartTick, double segEndTick, int segStartSample, int segSamples,
                                               double ticksPerSample, juce::MidiBuffer& outMidi, size_t& emittedCount) noexcept {
    if (segSamples <= 0 || segEndTick <= segStartTick) return;
    const size_t onsetCount = sliderseq::renderOnsetsInto (
        audioState.render, segStartTick, segEndTick, onsetScratch.data(), onsetScratch.size());

    int cursor = 0;
    for (size_t i = 0; i < onsetCount; ++i) {
        const auto& ev = onsetScratch[i];
        if (wasOnsetCommitted (ev)) continue;

        const int onsetRel = std::clamp (static_cast<int> (std::floor ((ev.absoluteTick - segStartTick) / ticksPerSample + 1e-9)),
                                         cursor, segSamples - 1);
        advanceActiveNotes (segStartSample + cursor, onsetRel - cursor, true, outMidi, emittedCount);
        cursor = onsetRel;

        if (! canEmitNoteOnInBlock (emittedCount))
            continue;

        const size_t idx = voiceKey (ev.channel, ev.note);
        for (auto& active : activeNotes) {
            if (active.active && active.channel == ev.channel && active.note == ev.note) {
                addNoteOffRaw (outMidi, active.channel, active.note, segStartSample + onsetRel);
                ++emittedCount;
                active.active = false;
            }
        }
        // If a passed-through voice currently owns this (ch, note), send clean Note Off before retriggering.
        if (voiceOwner[idx] == VoiceOwner::thru) {
            addNoteOffRaw (outMidi, ev.channel, ev.note, segStartSample + onsetRel);
            ++emittedCount;
        }

        ActiveNote* freeSlot = nullptr;
        for (auto& slotEntry : activeNotes) {
            if (! slotEntry.active) { freeSlot = &slotEntry; break; }
        }
        if (freeSlot == nullptr) continue;

        addNoteOnRaw (outMidi, ev.channel, ev.note, ev.velocity, segStartSample + onsetRel);
        ++emittedCount;
        recordCommittedOnset (ev);
        generatedNotes.fetch_add (1, std::memory_order_relaxed);
        voiceOwner[idx] = VoiceOwner::generated;
        freeSlot->trackIndex = ev.trackIndex;
        freeSlot->channel = ev.channel;
        freeSlot->note = ev.note;
        freeSlot->remainingSamples = std::max (1.0, ev.gateTicks / ticksPerSample);
        freeSlot->active = true;
    }

    advanceActiveNotes (segStartSample + cursor, segSamples - cursor, false, outMidi, emittedCount);
}

template<typename Sample>
void SliderRhythmMidiProcessor::renderBlock (juce::AudioBuffer<Sample>& audio, juce::MidiBuffer& midi) {
    audio.clear();
    const int numSamples = audio.getNumSamples();
    outputMidi.clear();
    size_t emittedCount = 0;

    // Zero-sample block: never emit events to nonexistent sample 0 and never consume pendingFlushOnNextBlock.
    if (numSamples <= 0) {
        midi.clear();
        return;
    }

    if (pendingFlushOnNextBlock.exchange (false, std::memory_order_acq_rel)) {
        flushAllActiveNotes (0, outputMidi);
        flushAllPassedThruNotes (0, outputMidi);
        flushPedals (0, outputMidi);
    }

    syncFromUi (outputMidi);

    const auto* playhead = getPlayHead();
    const auto position = playhead != nullptr ? playhead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};
    const bool playing = position && (position->getIsPlaying() || (isLogicAuHost && position->getIsRecording()));
    const bool validTempo = position && position->getBpm() && std::isfinite (*position->getBpm()) && *position->getBpm() > 0.0;
    const bool validPpq = position && position->getPpqPosition() && std::isfinite (*position->getPpqPosition());
    hostPlaying.store (playing, std::memory_order_relaxed);
    hostRecording.store (position && position->getIsRecording(), std::memory_order_relaxed);
    hostPositionAvailable.store (validTempo && validPpq, std::memory_order_relaxed);
    if (validTempo)
        lastKnownBpm.store (*position->getBpm(), std::memory_order_relaxed);
    if (position && position->getTimeSignature()) {
        const auto ts = *position->getTimeSignature();
        if (ts.numerator > 0 && ts.denominator > 0) {
            lastTimeSigNum.store (ts.numerator, std::memory_order_relaxed);
            lastTimeSigDen.store (ts.denominator, std::memory_order_relaxed);
        }
    }

    auto commitOutput = [&] () noexcept {
        midi.clear();
        midi.addEvents (outputMidi, 0, numSamples, 0);
    };

    if (audioState.render.playbackMode == sliderseq::PlaybackMode::thru) {
        flushAllActiveNotes (0, outputMidi);
        for (const auto metadata : midi) {
            const int pos = std::clamp (metadata.samplePosition, 0, numSamples - 1);
            // Inspect MIDI Learn non-destructively while passing all MIDI through untouched.
            applyIncomingControl (metadata.data, metadata.numBytes, pos, outputMidi, false);
            routeIncomingMidi (metadata.data, metadata.numBytes, pos, outputMidi, emittedCount);
        }
        wasPlaying = false;
        hasExpectedTick = false;
        updateActiveSteps (0.0, false);
        commitOutput();
        return;
    }

    if (! playing || ! validTempo || ! validPpq || ! std::isfinite (currentSampleRate) || currentSampleRate <= 0.0) {
        flushAllActiveNotes (0, outputMidi);
        if (wasPlaying) {
            // Transport transitioned Playing -> Stopped/Invalid: flush timeline notes & pedals once.
            flushAllPassedThruNotes (0, outputMidi);
            flushPedals (0, outputMidi);
            ++transportGeneration;
        }
        for (const auto metadata : midi) {
            const int pos = std::clamp (metadata.samplePosition, 0, numSamples - 1);
            const bool consumed = applyIncomingControl (metadata.data, metadata.numBytes, pos, outputMidi, true);
            if (! consumed)
                routeIncomingMidi (metadata.data, metadata.numBytes, pos, outputMidi, emittedCount);
        }
        wasPlaying = false;
        hasExpectedTick = false;
        updateActiveSteps (0.0, false);
        commitOutput();
        return;
    }

    const double bpm = *position->getBpm();
    const double rawStartTick = *position->getPpqPosition() * sliderseq::ticksPerQuarter;

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
    const bool validLoop = looping && std::isfinite (loopStartTick) && std::isfinite (loopEndTick) && loopEndTick > loopStartTick;

    double effectiveStartTick = rawStartTick;
    if (hasExpectedTick) {
        const double tolerance = std::max (2.0, ticksPerSample * 4.0);
        const bool normalStep = std::abs (rawStartTick - expectedNextTick) <= tolerance;
        const bool seamWrap = validLoop
            && std::abs (rawStartTick - loopStartTick) <= tolerance
            && std::abs (expectedNextTick - loopEndTick) <= tolerance;
        if (normalStep && std::abs (rawStartTick - expectedNextTick) <= 1e-3) {
            // Reuse exact contiguous edge to eliminate float drift double-triggers (e.g. 125 BPM / 48 kHz).
            effectiveStartTick = expectedNextTick;
        } else if (seamWrap) {
            effectiveStartTick = loopStartTick;
        } else if (! normalStep) {
            flushAllActiveNotes (0, outputMidi);
            flushAllPassedThruNotes (0, outputMidi);
            flushPedals (0, outputMidi);
            ++transportGeneration;
        }
    }
    wasPlaying = true;

    // Direct iteration over host MidiBuffer (no 256-message cap, SysEx preserved).
    // Split sequencer segments only at loop boundaries or genuine control-changing MIDI events.
    auto it = midi.begin();
    const auto endIt = midi.end();

    int curSample = 0;
    double curTick = effectiveStartTick;

    while (curSample < numSamples) {
        // Process all incoming MIDI at or before curSample.
        while (it != endIt && std::clamp ((*it).samplePosition, 0, numSamples - 1) <= curSample) {
            const auto meta = *it;
            const int pos = std::clamp (meta.samplePosition, 0, numSamples - 1);
            const bool consumed = applyIncomingControl (meta.data, meta.numBytes, pos, outputMidi, true);
            if (! consumed)
                routeIncomingMidi (meta.data, meta.numBytes, pos, outputMidi, emittedCount);
            ++it;
        }

        if (audioState.render.playbackMode == sliderseq::PlaybackMode::thru) {
            flushAllActiveNotes (curSample, outputMidi);
            while (it != endIt) {
                const auto meta = *it;
                const int pos = std::clamp (meta.samplePosition, 0, numSamples - 1);
                applyIncomingControl (meta.data, meta.numBytes, pos, outputMidi, false);
                routeIncomingMidi (meta.data, meta.numBytes, pos, outputMidi, emittedCount);
                ++it;
            }
            break;
        }

        // Advance to the next potential state-changing control message, routing non-control messages inline.
        int nextSample = numSamples;
        auto scan = it;
        while (scan != endIt) {
            const auto meta = *scan;
            const int pos = std::clamp (meta.samplePosition, 0, numSamples - 1);
            if (isPotentialControlMessage (meta.data, meta.numBytes)) {
                nextSample = std::min (nextSample, pos);
                break;
            }
            ++scan;
        }

        // If curTick already sits at or past loopEndTick in a valid loop, wrap immediately with fractional carry.
        if (validLoop && curTick >= loopEndTick - 1e-9) {
            const double loopLen = loopEndTick - loopStartTick;
            curTick = loopStartTick + std::fmod (std::max (0.0, curTick - loopEndTick), loopLen);
        }

        const int spanSamples = nextSample - curSample;
        const double segEndTick = curTick + ticksPerSample * static_cast<double> (spanSamples);

        if (validLoop && curTick < loopEndTick && segEndTick >= loopEndTick - 1e-9) {
            const double ticksToEnd = std::max (0.0, loopEndTick - curTick);
            const int beforeWrap = std::clamp (static_cast<int> (std::ceil (ticksToEnd / ticksPerSample - 1e-9)),
                                               1, spanSamples);
            // Flush non-control passthrough messages strictly inside [curSample, curSample + beforeWrap).
            while (it != endIt && std::clamp ((*it).samplePosition, 0, numSamples - 1) < curSample + beforeWrap) {
                const auto meta = *it;
                const int pos = std::clamp (meta.samplePosition, 0, numSamples - 1);
                routeIncomingMidi (meta.data, meta.numBytes, pos, outputMidi, emittedCount);
                ++it;
            }
            renderSegment (curTick, loopEndTick, curSample, beforeWrap, ticksPerSample, outputMidi, emittedCount);
            const double overshoot = std::max (0.0, beforeWrap * ticksPerSample - ticksToEnd);
            const double loopLen = loopEndTick - loopStartTick;
            curSample += beforeWrap;
            curTick = loopStartTick + (loopLen > 1e-9 ? std::fmod (overshoot, loopLen) : 0.0);
            continue;
        }

        // Route all non-control incoming messages in [curSample, nextSample) at their exact sample offsets.
        while (it != endIt && std::clamp ((*it).samplePosition, 0, numSamples - 1) < nextSample) {
            const auto meta = *it;
            const int pos = std::clamp (meta.samplePosition, 0, numSamples - 1);
            routeIncomingMidi (meta.data, meta.numBytes, pos, outputMidi, emittedCount);
            ++it;
        }

        renderSegment (curTick, segEndTick, curSample, spanSamples, ticksPerSample, outputMidi, emittedCount);
        curTick = segEndTick;
        curSample = nextSample;
    }

    while (it != endIt) {
        const auto meta = *it;
        const int pos = std::clamp (meta.samplePosition, 0, numSamples - 1);
        const bool consumed = applyIncomingControl (meta.data, meta.numBytes, pos, outputMidi, true);
        if (! consumed)
            routeIncomingMidi (meta.data, meta.numBytes, pos, outputMidi, emittedCount);
        ++it;
    }

    expectedNextTick = curTick;
    hasExpectedTick = true;
    updateActiveSteps (effectiveStartTick, true);
    commitOutput();
}

template<typename Sample>
void SliderRhythmMidiProcessor::renderBypassed (juce::AudioBuffer<Sample>& audio, juce::MidiBuffer& midi) {
    audio.clear();
    const int numSamples = audio.getNumSamples();
    if (numSamples <= 0) {
        midi.clear();
        return;
    }
    outputMidi.clear();
    if (pendingFlushOnNextBlock.exchange (false, std::memory_order_acq_rel)) {
        flushAllPassedThruNotes (0, outputMidi);
        flushPedals (0, outputMidi);
    }

    // Flush ONLY generated notes on bypass entry; keep live passed-through notes sounding and
    // continue tracking Note On/Off and sustain pedals during bypass so returning to ACTIVE never sticks notes.
    flushAllActiveNotes (0, outputMidi);
    for (const auto metadata : midi) {
        const int pos = std::clamp (metadata.samplePosition, 0, numSamples - 1);
        if (metadata.numBytes == 3) {
            const uint8_t status = static_cast<uint8_t> (metadata.data[0] & 0xf0u);
            const int ch = static_cast<int> ((metadata.data[0] & 0x0fu) + 1);
            const int d1 = static_cast<int> (metadata.data[1] & 0x7fu);
            const int d2 = static_cast<int> (metadata.data[2] & 0x7fu);
            if (status == 0x90u && d2 > 0) {
                const size_t idx = voiceKey (ch, d1);
                if (passedThruCount[idx] < 255u) ++passedThruCount[idx];
                voiceOwner[idx] = VoiceOwner::thru;
            } else if (status == 0x80u || (status == 0x90u && d2 == 0)) {
                const size_t idx = voiceKey (ch, d1);
                if (passedThruCount[idx] > 0u) --passedThruCount[idx];
                if (passedThruCount[idx] == 0u && voiceOwner[idx] == VoiceOwner::thru)
                    voiceOwner[idx] = VoiceOwner::none;
            } else if (status == 0xb0u) {
                if (d1 == 64) sustainPedalDown[static_cast<size_t> (ch - 1)] = (d2 >= 64) ? 1u : 0u;
                else if (d1 == 66) sostenutoPedalDown[static_cast<size_t> (ch - 1)] = (d2 >= 64) ? 1u : 0u;
                else if (d1 == 120 || d1 == 123) {
                    for (int nt = 0; nt < 128; ++nt) {
                        const size_t idx = voiceKey (ch, nt);
                        passedThruCount[idx] = 0u;
                        if (voiceOwner[idx] == VoiceOwner::thru) voiceOwner[idx] = VoiceOwner::none;
                    }
                    sustainPedalDown[static_cast<size_t> (ch - 1)] = 0u;
                    sostenutoPedalDown[static_cast<size_t> (ch - 1)] = 0u;
                }
            }
        }
        outputMidi.addEvent (metadata.data, metadata.numBytes, pos);
    }
    wasPlaying = false;
    hasExpectedTick = false;
    updateActiveSteps (0.0, false);
    midi.clear();
    midi.addEvents (outputMidi, 0, numSamples, 0);
}

void SliderRhythmMidiProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) { renderBlock (audio, midi); }
void SliderRhythmMidiProcessor::processBlock (juce::AudioBuffer<double>& audio, juce::MidiBuffer& midi) { renderBlock (audio, midi); }
void SliderRhythmMidiProcessor::processBlockBypassed (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) { renderBypassed (audio, midi); }
void SliderRhythmMidiProcessor::processBlockBypassed (juce::AudioBuffer<double>& audio, juce::MidiBuffer& midi) { renderBypassed (audio, midi); }

void SliderRhythmMidiProcessor::getStateInformation (juce::MemoryBlock& destination) {
    // Merge all pending audio CC edits and host params into ONE authoritative snapshot before serializing both XML & APVTS.
    syncAudioCcToApvts();
    const auto current = snapshot();

    juce::XmlElement root ("SliderRhythmMidi");
    root.setAttribute ("version", 5);
    root.setAttribute ("trackCount", current->sequence.trackCount);
    root.setAttribute ("globalSwing", current->sequence.globalSwing);
    root.setAttribute ("unmappedMode", static_cast<int> (current->sequence.unmappedMode));
    root.setAttribute ("inputChannelMatch", static_cast<int> (current->sequence.inputChannelMatch));
    root.setAttribute ("playbackMode", static_cast<int> (current->playbackMode));
    root.setAttribute ("playRecordedMidi", current->playbackMode == sliderseq::PlaybackMode::thru);
    root.setAttribute ("noteLearnEnabled", isNoteLearnEnabled());
    root.setAttribute ("learnTriggerMode", static_cast<int> (getLearnTriggerMode()));

    {
        const auto& mp = current->activeMappingPreset;
        auto* mpEl = root.createNewChildElement ("MappingPreset");
        mpEl->setAttribute ("version", 2);
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
            tEl->setAttribute ("topMode", static_cast<int> (m.topMode));
            tEl->setAttribute ("midMode", static_cast<int> (m.midMode));
            tEl->setAttribute ("bottomMode", static_cast<int> (m.bottomMode));
            tEl->setAttribute ("macroMode", static_cast<int> (m.macroMode));
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
        element->setAttribute ("lastPatternSlot", track.lastPatternSlot);
        element->setAttribute ("activeZones", track.activeZones);
        element->setAttribute ("topMode", static_cast<int> (track.topMode));
        element->setAttribute ("midMode", static_cast<int> (track.midMode));
        element->setAttribute ("bottomMode", static_cast<int> (track.bottomMode));
        element->setAttribute ("macroMode", static_cast<int> (track.macroMode));
        element->setAttribute ("macroValue", track.macroValue);
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
    cancelMidiLearn();

    const int savedVersion = xml->getIntAttribute ("version", 1);
    const int rawPlaybackMode = xml->hasAttribute ("playbackMode")
        ? xml->getIntAttribute ("playbackMode", 0)
        : (xml->getBoolAttribute ("playRecordedMidi", false) ? 2 : 0);
    const auto routeMig = upstreamcompat::migrateLegacyRouting (
        savedVersion, rawPlaybackMode, xml->getBoolAttribute ("playRecordedMidi", false));

    Snapshot restored;
    restored.sequence.trackCount = std::clamp (xml->getIntAttribute ("trackCount", 4), 1, sliderseq::maxTracks);
    restored.sequence.globalSwing = std::clamp (xml->getIntAttribute ("globalSwing", 0), -100, 100);
    restored.sequence.unmappedMode = xml->hasAttribute ("unmappedMode")
        ? sliderseq::clampUnmappedRouteMode (xml->getIntAttribute ("unmappedMode", 0))
        : routeMig.unmappedMode;
    restored.sequence.inputChannelMatch = sliderseq::clampInputChannelMatch (
        xml->getIntAttribute ("inputChannelMatch", static_cast<int> (sliderseq::InputChannelMatch::autoLiveCh1)));
    restored.playbackMode = routeMig.playbackMode;
    restored.playRecordedMidi = (restored.playbackMode == sliderseq::PlaybackMode::thru);
    setNoteLearnEnabled (xml->getBoolAttribute ("noteLearnEnabled", false));
    setLearnTriggerMode (static_cast<TriggerMode> (std::clamp (xml->getIntAttribute ("learnTriggerMode", 0), 0, 1)));

    std::array<int, sliderseq::maxTracks> legacyGrooves {};
    legacyGrooves.fill (50);

    for (auto* element : xml->getChildIterator()) {
        if (element->hasTagName ("MappingPreset")) {
            auto mp = mappingpresets::makeDefaultKitPreset();
            const int mpVer = element->getIntAttribute ("version", savedVersion < 5 ? 1 : 2);
            mp.id = element->getStringAttribute ("id", "factory:default4").toStdString();
            mp.name = element->getStringAttribute ("name", "Default 4-Piece Kit").toStdString();
            mp.isFactory = element->getBoolAttribute ("isFactory", true);
            mp.trackCount = std::clamp (element->getIntAttribute ("trackCount", 4), 1, sliderseq::maxTracks);
            for (auto* tEl : element->getChildIterator()) {
                if (! tEl->hasTagName ("Track")) continue;
                const int idx = tEl->getIntAttribute ("index", -1);
                if (idx < 0 || idx >= sliderseq::maxTracks) continue;
                mappingpresets::parseTrackMappingElement (*tEl, idx, mpVer, mp.tracks[static_cast<size_t> (idx)]);
            }
            // Re-associate restored user preset with its on-disk XML file by stable ID so SAVE & Delete work immediately.
            if (! mp.isFactory)
                mp.file = mappingpresets::findUserPresetFileById (mp.id);
            restored.activeMappingPreset = std::move (mp);
            continue;
        }
        if (! element->hasTagName ("Track")) continue;
        const int index = element->getIntAttribute ("index", -1);
        if (index < 0 || index >= sliderseq::maxTracks) continue;
        auto& track = restored.sequence.tracks[static_cast<size_t> (index)];
        track.name = element->getStringAttribute ("name", track.name).toStdString();
        track.activeZones = std::clamp (element->getIntAttribute ("activeZones", 12), 1, sliderseq::slotsPerTrack);

        if (savedVersion < 5) {
            track.topMode = routeMig.topMode;
            track.midMode = routeMig.midMode;
            track.bottomMode = routeMig.bottomMode;
            track.macroMode = sliderseq::MacroMode::build;
            track.macroValue = 0;
        } else {
            track.topMode = sliderseq::clampEndZoneMode (element->getIntAttribute ("topMode", static_cast<int> (sliderseq::EndZoneMode::thru)));
            track.midMode = sliderseq::clampMidRouteMode (element->getIntAttribute ("midMode", static_cast<int> (sliderseq::MidRouteMode::gen)));
            track.bottomMode = sliderseq::clampEndZoneMode (element->getIntAttribute ("bottomMode", static_cast<int> (sliderseq::EndZoneMode::block)));
            track.macroMode = sliderseq::clampMacroMode (element->getIntAttribute ("macroMode", 0));
            track.macroValue = std::clamp (element->getIntAttribute ("macroValue", sliderseq::defaultMacroValueForMode (track.macroMode)), 0, 100);
        }

        if (savedVersion < 4) {
            const int legacySlot = std::clamp (element->getIntAttribute ("slot", 0), 0, 11);
            track.slot = legacySlot + 1;
        } else {
            track.slot = sliderseq::clampTrackSlot (element->getIntAttribute ("slot", 1),
                                                    track.activeZones, track.bottomMode, track.topMode);
        }
        track.lastPatternSlot = sliderseq::isTrackGeneratingZone (track.slot, track.activeZones, track.bottomMode, track.topMode)
            ? track.slot
            : std::clamp (element->getIntAttribute ("lastPatternSlot", 1), 1, track.activeZones);

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
        const auto gm = upstreamcompat::migrateLegacyGrooves (legacyGrooves, restored.sequence.trackCount);
        restored.sequence.globalSwing = gm.globalSwing;
        for (int i = 0; i < sliderseq::maxTracks; ++i)
            restored.sequence.tracks[static_cast<size_t> (i)].groove = gm.trackSwingPercent[static_cast<size_t> (i)];
    }

    // Authoritative restore: <Track> and root attributes always win (never clobbered by stale <Parameters> from CC edits),
    // then APVTS is synchronized cleanly outside messageMutex.
    ApvtsSyncDelta fullDelta;
    {
        std::lock_guard<std::mutex> lock (messageMutex);
        drainAudioToUiLocked();
        pendingAudioCcApvtsDelta = {};
        hasPendingAudioCcApvtsDelta = false;
        pendingAudioCcSyncFlag.store (false, std::memory_order_release);
        messageState = std::move (restored);
        fullDelta = captureFullApvtsDeltaLocked (messageState);
        publishFullStateLocked();
    }
    applyApvtsSyncDelta (fullDelta);
}

juce::AudioProcessorEditor* SliderRhythmMidiProcessor::createEditor() { return new SliderRhythmMidiEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SliderRhythmMidiProcessor(); }
