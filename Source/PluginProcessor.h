#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include "MappingPresets.h"
#include "SequencerEngine.h"
#include "UiPreferences.h"
#include "UpstreamCompatibility.h"

class SliderRhythmMidiProcessor final : public juce::AudioProcessor {
public:
    // Keep existing indices 0..12 stable for saved XML backwards compatibility; append new controls after globalSwing.
    enum Control {
        slot, rate, velocity, groove, modAmount, mute, modMode, note, channel, length, grid, solo, globalSwing, macro,
        controlCount
    };
    enum class BindingKind : uint8_t { cc = 0, note = 1 };
    enum class TriggerMode : uint8_t { toggle = 0, momentary = 1 };

    struct Binding {
        int cc = -1; // CC number (0..127) when kind==cc, or MIDI note number (0..127) when kind==note.
        int channel = 0;
        BindingKind kind = BindingKind::cc;
        TriggerMode triggerMode = TriggerMode::toggle;
        bool lastHigh = false;
    };
    using BindingArray = std::array<std::array<Binding, controlCount>, sliderseq::maxTracks>;

    struct Snapshot {
        sliderseq::State sequence = sliderseq::makeDefaultState();
        BindingArray bindings {};
        sliderseq::PlaybackMode playbackMode = sliderseq::PlaybackMode::gen;
        bool playRecordedMidi = false; // Mirror of playbackMode == PlaybackMode::thru for compatibility.
        mappingpresets::Preset activeMappingPreset = mappingpresets::makeDefaultKitPreset();
    };

    enum class OpKind : uint8_t {
        setTrackCount,
        setPlaybackMode,
        setUnmappedMode,
        setInputChannelMatch,
        setGlobalSwing,
        setSlot,
        setActiveZones,
        setTopMode,
        setMidMode,
        setBottomMode,
        setMacroMode,
        setMacroValue,
        setRate,
        setVelocity,
        setGroove,
        setModShape,
        setModAmount,
        setLoopStepsOverride,
        setGateTicks,
        setChokeMask,
        setMuted,
        setSolo,
        setExclusiveSolo,
        setMidiNote,
        setMidiChannel,
        setStep,
        setPatternLength,
        setPatternVelocityScale,
        resizePattern,
        setPattern,
        setBinding
    };

    struct StateOp {
        uint64_t seq = 0;
        OpKind kind = OpKind::setSlot;
        int track = 0;
        int slotIndex = 0; // Pattern index 0..11 for pattern ops, or Control index for setBinding.
        int arg0 = 0;
        int arg1 = 0;
        int arg2 = 0;
        int arg3 = 0;
        double darg = 0.0;
        bool barg = false;
        bool syncHostBaseline = false; // True only when message thread also synced APVTS for this op.
        float hostBaselineValue = 0.0f;
        sliderseq::RenderPattern pattern {};
    };

    static void compactVst3OutputBuffer (const juce::MidiBuffer& src, juce::MidiBuffer& dst,
                                         int numSamples, size_t maxEvents = 2048) noexcept;

    static bool supportsNoteLearnControl (Control control) noexcept {
        return control == mute || control == solo || control == slot || control == rate || control == modMode;
    }

    SliderRhythmMidiProcessor();
    ~SliderRhythmMidiProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int index) override { return index == 0 ? juce::String ("Default") : juce::String {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    std::shared_ptr<const Snapshot> snapshot();
    void edit (const std::function<void (Snapshot&)>&);
    void applyUiOp (StateOp op);
    void armMidiLearn (int track, Control control);
    void cancelMidiLearn() { learnTarget.store (-1, std::memory_order_release); }
    int getLearnTarget() const { return learnTarget.load (std::memory_order_acquire); }
    void setNoteLearnEnabled (bool enabled) noexcept { noteLearnEnabled.store (enabled, std::memory_order_release); }
    bool isNoteLearnEnabled() const noexcept { return noteLearnEnabled.load (std::memory_order_acquire); }
    void setLearnTriggerMode (TriggerMode mode) noexcept { learnTriggerMode.store (static_cast<int> (mode), std::memory_order_release); }
    TriggerMode getLearnTriggerMode() const noexcept { return static_cast<TriggerMode> (learnTriggerMode.load (std::memory_order_acquire)); }

    bool isHostPlaying() const { return hostPlaying.load (std::memory_order_relaxed); }
    bool hasHostPosition() const { return hostPositionAvailable.load (std::memory_order_relaxed); }
    bool isHostRecording() const { return hostRecording.load (std::memory_order_relaxed); }
    double getLastKnownBpm() const noexcept { return lastKnownBpm.load (std::memory_order_relaxed); }
    int getLastTimeSigNumerator() const noexcept { return lastTimeSigNum.load (std::memory_order_relaxed); }
    int getLastTimeSigDenominator() const noexcept { return lastTimeSigDen.load (std::memory_order_relaxed); }
    unsigned long long generatedNoteCount() const { return generatedNotes.load (std::memory_order_relaxed); }
    uint64_t stateVersion() const noexcept { return version.load (std::memory_order_acquire); }
    bool hasPendingHostOrAudioChanges() const noexcept;
    void syncAudioCcToApvts();
    int getActiveStep (int trackIndex) const noexcept {
        if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return -1;
        return activeSteps[static_cast<size_t> (trackIndex)].load (std::memory_order_relaxed);
    }

    juce::AudioProcessorValueTreeState& parameters() noexcept { return apvts; }
    static juce::String paramId (int trackIndex, const char* suffix);
    static juce::String globalSwingParamId() { return "global_swing"; }
    void setHostParamValue (int trackIndex, const char* suffix, float plainValue, bool wrapGesture = true);
    void setGlobalSwingValue (int plainValue, bool wrapGesture = true);
    void setTrackActiveZones (int trackIndex, int zones);
    void setTrackTopMode (int trackIndex, sliderseq::EndZoneMode mode);
    void setTrackMidMode (int trackIndex, sliderseq::MidRouteMode mode);
    void setTrackBottomMode (int trackIndex, sliderseq::EndZoneMode mode);
    void setTrackMacroMode (int trackIndex, sliderseq::MacroMode mode, bool resetToNeutral = true);
    void setInputChannelMatch (sliderseq::InputChannelMatch matchMode);
    void loadMappingPreset (const mappingpresets::Preset& preset);
    void setActiveMappingPresetBaseline (const mappingpresets::Preset& preset);
    void unmuteAllTracks();
    void unsoloAllTracks();
    void soloTrackExclusive (int trackIndex);
    void toggleTrackSoloAdditive (int trackIndex);

private:
    static constexpr int kTrackHostParamCount = 9;
    static constexpr size_t kVst3SafeEventBudget = 2048;

    struct TrackParamPtrs {
        std::atomic<float>* slot = nullptr;
        std::atomic<float>* mute = nullptr;
        std::atomic<float>* solo = nullptr;
        std::atomic<float>* velocity = nullptr;
        std::atomic<float>* rate = nullptr;
        std::atomic<float>* groove = nullptr;
        std::atomic<float>* modAmount = nullptr;
        std::atomic<float>* loopSteps = nullptr;
        std::atomic<float>* macro = nullptr;
    };

    struct FullAudioState {
        uint64_t seq = 0;
        uint64_t audioOpSeq = 0;
        sliderseq::RenderState render = sliderseq::toRenderState (sliderseq::makeDefaultState(), sliderseq::PlaybackMode::gen);
        BindingArray bindings {};
        float hostBaselineGlobalSwing = 0.0f;
        std::array<std::array<float, kTrackHostParamCount>, sliderseq::maxTracks> hostBaselineParams {};
        bool resetHeldControls = false;
    };

    struct ActiveNote {
        int trackIndex = 0;
        int channel = 10;
        int note = 36;
        double remainingSamples = 0.0;
        bool active = false;
    };

    enum class VoiceOwner : uint8_t {
        none = 0,
        generated = 1,
        thru = 2
    };

    struct CommittedOnsetKey {
        uint32_t transportGen = 0;
        int trackIndex = -1;
        int64_t cycle = 0;
        int stepIndex = -1;
        int subIndex = -1;
    };

    struct ApvtsSyncDelta {
        bool syncGlobalSwing = false;
        float globalSwing = 0.0f;
        std::array<std::array<bool, kTrackHostParamCount>, sliderseq::maxTracks> dirty {};
        std::array<std::array<float, kTrackHostParamCount>, sliderseq::maxTracks> values {};
    };

    static BusesProperties makeBusesProperties();
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static void applyOpToAudio (const StateOp& op, FullAudioState& target) noexcept;
    static void applyOpToSnapshot (const StateOp& op, Snapshot& target);
    static FullAudioState makeAudioState (const Snapshot& src, uint64_t seq,
                                          float baselineGlobalSwing,
                                          const std::array<std::array<float, kTrackHostParamCount>, sliderseq::maxTracks>& baselineParams,
                                          bool resetHeld = false) noexcept;
    static bool shouldPassNoteThrough (const sliderseq::RenderState& render, int channel, int note) noexcept;
    static bool isProtectedControlCc (int ccNumber) noexcept;
    static int midiMessagePriorityTier (const uint8_t* data, int numBytes) noexcept;

    void drainAudioToUiLocked();
    ApvtsSyncDelta captureDirtyApvtsDeltaLocked (const Snapshot& before, const Snapshot& after);
    ApvtsSyncDelta captureFullApvtsDeltaLocked (const Snapshot& state);
    void mergePendingAudioCcIntoDeltaLocked (ApvtsSyncDelta& delta);
    void stampExpectedSnapshotHostBaselineLocked (const ApvtsSyncDelta& delta);
    void applyApvtsSyncDelta (const ApvtsSyncDelta& delta);
    bool syncApvtsToSnapshotLocked();
    void syncSingleTrackSlotApvts (int trackIndex);
    void pullHostParamsIntoAudio (juce::MidiBuffer& outMidi) noexcept;
    void publishFullStateLocked (bool resetHeld = false);
    void pushAudioToUiOp (StateOp op) noexcept;
    void publishAudioRecoverySlot() noexcept;
    void rebuildControlLookupMasks() noexcept;
    bool isPotentialControlMessage (const uint8_t* data, int numBytes) const noexcept;
    void syncFromUi (juce::MidiBuffer& outMidi) noexcept;
    bool canEmitNoteOnInBlock (size_t criticalAndNoteCount) const noexcept;
    bool wasOnsetCommitted (const sliderseq::OnsetEvent& ev) const noexcept;
    void recordCommittedOnset (const sliderseq::OnsetEvent& ev) noexcept;
    void flushTrackNotes (int trackIndex, int samplePos, juce::MidiBuffer& outMidi) noexcept;
    void flushAllActiveNotes (int samplePos, juce::MidiBuffer& outMidi) noexcept;
    void flushAllPassedThruNotes (int samplePos, juce::MidiBuffer& outMidi) noexcept;
    void flushPedals (int samplePos, juce::MidiBuffer& outMidi) noexcept;
    void flushRemovedOrChangedTracks (const sliderseq::RenderState& before, const sliderseq::RenderState& after,
                                      int samplePos, juce::MidiBuffer& outMidi) noexcept;
    void routeIncomingMidi (const uint8_t* data, int numBytes, int samplePos, juce::MidiBuffer& outMidi,
                            size_t& criticalAndNoteCount, size_t& tier3Count) noexcept;
    void routeAllThruVerbatim (const uint8_t* data, int numBytes, int samplePos, juce::MidiBuffer& outMidi,
                               size_t& criticalAndNoteCount, size_t& tier3Count) noexcept;
    void advanceActiveNotes (int segStart, int dtSamples, bool inclusiveEnd, juce::MidiBuffer& outMidi, size_t& criticalAndNoteCount) noexcept;
    void drainPassthroughUntil (int limitSampleExclusive, int spanStartSample, int& cursor,
                                juce::MidiBufferIterator& midiIt, const juce::MidiBufferIterator& midiEnd, int numSamples,
                                juce::MidiBuffer& outMidi, size_t& criticalAndNoteCount, size_t& tier3Count) noexcept;
    void renderMusicalPiece (double pieceStartTick, double pieceEndTick, double pieceElapsedTicks,
                             int spanStartSample, int spanSamples, int& cursor, double ticksPerSample,
                             juce::MidiBufferIterator& midiIt, const juce::MidiBufferIterator& midiEnd, int numSamples,
                             juce::MidiBuffer& outMidi, size_t& criticalAndNoteCount, size_t& tier3Count, int splitDepth = 0) noexcept;
    bool applyIncomingControl (const uint8_t* data, int numBytes, int samplePos, juce::MidiBuffer& outMidi, bool allowStateMutation) noexcept;
    void updateActiveSteps (double tick, bool playing) noexcept;
    void commitMidiOutput (juce::MidiBuffer& dest, int numSamples) noexcept;

    template<typename Sample> void renderBlock (juce::AudioBuffer<Sample>&, juce::MidiBuffer&);
    template<typename Sample> void renderBypassed (juce::AudioBuffer<Sample>&, juce::MidiBuffer&);

    // Cached host identity flags (computed once on message thread in constructor, read-only on audio thread).
    bool isLogicAuHost = false;

    // Message-thread authoritative state (never touched by audio thread).
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* globalSwingParam = nullptr;
    float lastHostGlobalSwing = 0.0f;
    float lastSnapshotGlobalSwing = 0.0f;
    std::array<TrackParamPtrs, sliderseq::maxTracks> paramPtrs {};
    std::array<std::array<float, kTrackHostParamCount>, sliderseq::maxTracks> lastHostParamValues {};
    std::array<std::array<float, kTrackHostParamCount>, sliderseq::maxTracks> lastSnapshotHostValues {};
    mutable std::mutex messageMutex;
    Snapshot messageState {};
    uint64_t messageSeq = 0;
    uint64_t lastDrainedAudioOpSeq = 0;
    ApvtsSyncDelta pendingAudioCcApvtsDelta {};
    bool hasPendingAudioCcApvtsDelta = false;
    std::atomic<bool> pendingAudioCcSyncFlag { false };
    std::atomic<uint32_t> bulkApvtsWriteInProgress { 0 };

    // Wait-free triple buffer for bulk UI -> Audio syncs.
    static constexpr uint32_t kDirtyBit = 0x80000000u;
    std::array<FullAudioState, 3> fullSyncSlots {};
    int writerSyncSlot = 1;
    int readerSyncSlot = 2;
    std::atomic<uint32_t> sharedSyncSlot { 0 };

    // Wait-free triple buffer for Audio -> UI recovery if audioToUiFifo ever overflows while editor is closed.
    std::array<FullAudioState, 3> audioRecoverySlots {};
    int audioRecWriterSlot = 1;
    int audioRecReaderSlot = 2;
    std::atomic<uint32_t> audioRecSharedSlot { 0 };

    // Lock-free SPSC operation FIFOs.
    static constexpr int kFifoCapacity = 512;
    juce::AbstractFifo uiToAudioFifo { kFifoCapacity };
    std::array<StateOp, kFifoCapacity> uiToAudioOps {};
    juce::AbstractFifo audioToUiFifo { kFifoCapacity };
    std::array<StateOp, kFifoCapacity> audioToUiOps {};

    // Audio-thread exclusive state (never touched by message thread).
    FullAudioState audioState {};
    uint64_t audioOpSeq = 0;
    std::array<std::array<bool, controlCount>, sliderseq::maxTracks> audioToggleLastHigh {};
    std::array<uint8_t, 128> boundCcMask {};
    std::array<uint8_t, 128> boundNoteMask {};
    std::array<ActiveNote, 256> activeNotes {};
    std::array<uint8_t, 16 * 128> passedThruCount {};
    std::array<VoiceOwner, 16 * 128> voiceOwner {};
    std::array<uint8_t, 16> sustainPedalDown {};
    std::array<uint8_t, 16> sostenutoPedalDown {};
    std::array<uint8_t, 16 * 128> heldNoteControlTrack {}; // 0 = none, 1..16 = trackIndex+1 for HLD release safety
    std::array<uint8_t, 16 * 128> heldNoteControlKind {};  // 1 = mute, 2 = solo
    std::array<CommittedOnsetKey, 256> committedOnsets {};
    size_t committedWriteCursor = 0;
    uint32_t transportGeneration = 1;
    std::array<sliderseq::OnsetEvent, 4096> onsetScratch {};
    juce::MidiBuffer outputMidi;
    double currentSampleRate = 44100.0;
    double lastBpm = 0.0;
    double expectedNextTick = 0.0;
    bool hasExpectedTick = false;
    bool wasPlaying = false;
    std::atomic<bool> pendingFlushOnNextBlock { false };

    std::atomic<int> learnTarget { -1 };
    std::atomic<bool> noteLearnEnabled { false };
    std::atomic<int> learnTriggerMode { static_cast<int> (TriggerMode::toggle) };
    std::atomic<bool> hostPlaying { false }, hostPositionAvailable { false }, hostRecording { false };
    std::atomic<double> lastKnownBpm { 120.0 };
    std::atomic<int> lastTimeSigNum { 4 }, lastTimeSigDen { 4 };
    std::atomic<unsigned long long> generatedNotes { 0 };
    std::atomic<uint64_t> version { 1 };
    std::array<std::atomic<int>, sliderseq::maxTracks> activeSteps {};
};
