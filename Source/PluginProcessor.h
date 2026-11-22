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

class SliderRhythmMidiProcessor final : public juce::AudioProcessor {
public:
    // Keep existing indices 0..11 stable for saved XML backwards compatibility; append new controls after solo.
    enum Control { slot, rate, velocity, groove, modAmount, mute, modMode, note, channel, length, grid, solo, globalSwing, controlCount };
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
        setGlobalSwing,
        setSlot,
        setActiveZones,
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
        sliderseq::RenderPattern pattern {};
    };

    SliderRhythmMidiProcessor();
    ~SliderRhythmMidiProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
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
    unsigned long long generatedNoteCount() const { return generatedNotes.load (std::memory_order_relaxed); }
    uint64_t stateVersion() const noexcept { return version.load (std::memory_order_acquire); }
    int getActiveStep (int trackIndex) const noexcept {
        if (trackIndex < 0 || trackIndex >= sliderseq::maxTracks) return -1;
        return activeSteps[static_cast<size_t> (trackIndex)].load (std::memory_order_relaxed);
    }

    juce::AudioProcessorValueTreeState& parameters() noexcept { return apvts; }
    static juce::String paramId (int trackIndex, const char* suffix);
    static juce::String globalSwingParamId() { return "global_swing"; }
    void setHostParamValue (int trackIndex, const char* suffix, float plainValue);
    void setGlobalSwingValue (int plainValue);
    void setTrackActiveZones (int trackIndex, int zones);
    void loadMappingPreset (const mappingpresets::Preset& preset);
    void setActiveMappingPresetBaseline (const mappingpresets::Preset& preset);
    void unmuteAllTracks();
    void unsoloAllTracks();
    void soloTrackExclusive (int trackIndex);
    void toggleTrackSoloAdditive (int trackIndex);

private:
    struct TrackParamPtrs {
        std::atomic<float>* slot = nullptr;
        std::atomic<float>* mute = nullptr;
        std::atomic<float>* solo = nullptr;
        std::atomic<float>* velocity = nullptr;
        std::atomic<float>* rate = nullptr;
        std::atomic<float>* groove = nullptr;
        std::atomic<float>* modAmount = nullptr;
        std::atomic<float>* loopSteps = nullptr;
    };

    struct FullAudioState {
        uint64_t seq = 0;
        sliderseq::RenderState render = sliderseq::toRenderState (sliderseq::makeDefaultState(), sliderseq::PlaybackMode::gen);
        BindingArray bindings {};
    };

    struct ActiveNote {
        int trackIndex = 0;
        int channel = 10;
        int note = 36;
        double remainingSamples = 0.0;
        bool active = false;
    };

    struct IncomingMidi {
        int sample = 0;
        uint8_t data[3] { 0, 0, 0 };
        int numBytes = 0;
        bool consumedByLearnOrControl = false;
    };

    static BusesProperties makeBusesProperties();
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static void applyOpToAudio (const StateOp& op, FullAudioState& target) noexcept;
    static void applyOpToSnapshot (const StateOp& op, Snapshot& target);
    static FullAudioState makeAudioState (const Snapshot& src, uint64_t seq) noexcept;

    struct ApvtsSyncValues {
        float globalSwing = 0.0f;
        std::array<std::array<float, 8>, sliderseq::maxTracks> tracks {};
    };

    void drainAudioToUiLocked();
    ApvtsSyncValues captureApvtsSyncValuesLocked();
    void applyApvtsSyncValues (const ApvtsSyncValues& values);
    void syncApvtsToSnapshotLocked();
    void pullHostParamsIntoAudio (juce::MidiBuffer& outMidi) noexcept;
    void publishFullStateLocked();
    void pushAudioToUiOp (const StateOp& op) noexcept;
    void syncFromUi (juce::MidiBuffer& outMidi) noexcept;
    void flushTrackNotes (int trackIndex, int samplePos, juce::MidiBuffer& outMidi) noexcept;
    void flushAllActiveNotes (int samplePos, juce::MidiBuffer& outMidi) noexcept;
    void flushRemovedOrChangedTracks (const sliderseq::RenderState& before, const sliderseq::RenderState& after,
                                      int samplePos, juce::MidiBuffer& outMidi) noexcept;
    void advanceActiveNotes (int segStart, int dtSamples, bool inclusiveEnd, juce::MidiBuffer& outMidi) noexcept;
    void renderSegment (double segStartTick, int segStartSample, int segSamples, double ticksPerSample,
                        juce::MidiBuffer& outMidi) noexcept;
    bool applyIncomingControl (const IncomingMidi& msg, int samplePos, juce::MidiBuffer& outMidi) noexcept;
    void updateActiveSteps (double tick, bool playing) noexcept;
    juce::MidiBuffer& acquireOutputBuffer() noexcept;
    void finalizeOutputBuffer (juce::MidiBuffer& hostMidi, juce::MidiBuffer& built) noexcept;

    template<typename Sample> void renderBlock (juce::AudioBuffer<Sample>&, juce::MidiBuffer&);
    template<typename Sample> void renderBypassed (juce::AudioBuffer<Sample>&, juce::MidiBuffer&);

    // Message-thread authoritative state (never touched by audio thread).
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* globalSwingParam = nullptr;
    float lastHostGlobalSwing = 0.0f;
    float lastSnapshotGlobalSwing = 0.0f;
    std::array<TrackParamPtrs, sliderseq::maxTracks> paramPtrs {};
    std::array<std::array<float, 8>, sliderseq::maxTracks> lastHostParamValues {};
    std::array<std::array<float, 8>, sliderseq::maxTracks> lastSnapshotHostValues {};
    std::mutex messageMutex;
    Snapshot messageState {};
    uint64_t messageSeq = 0;

    // Wait-free triple buffer for bulk UI -> Audio syncs (setStateInformation, edit lambdas, queue fallback).
    static constexpr uint32_t kDirtyBit = 0x80000000u;
    std::array<FullAudioState, 3> fullSyncSlots {};
    int writerSyncSlot = 1;
    int readerSyncSlot = 2;
    std::atomic<uint32_t> sharedSyncSlot { 0 };

    // Lock-free SPSC operation FIFOs.
    static constexpr int kFifoCapacity = 512;
    juce::AbstractFifo uiToAudioFifo { kFifoCapacity };
    std::array<StateOp, kFifoCapacity> uiToAudioOps {};
    juce::AbstractFifo audioToUiFifo { kFifoCapacity };
    std::array<StateOp, kFifoCapacity> audioToUiOps {};

    // Audio-thread exclusive state (never touched by message thread).
    FullAudioState audioState {};
    std::array<ActiveNote, 256> activeNotes {};
    std::array<sliderseq::OnsetEvent, 512> onsetScratch {};
    std::array<IncomingMidi, 256> incomingScratch {};
    juce::MidiBuffer outputMidiA, outputMidiB;
    bool swappedOutputOnce = false;
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
    std::atomic<unsigned long long> generatedNotes { 0 };
    std::atomic<uint64_t> version { 1 };
    std::array<std::atomic<int>, sliderseq::maxTracks> activeSteps {};
};
