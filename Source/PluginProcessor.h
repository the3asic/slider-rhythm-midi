#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include "SequencerEngine.h"

class SliderRhythmMidiProcessor final : public juce::AudioProcessor {
public:
    enum Control { slot, rate, velocity, groove, modAmount, mute, modMode, note, channel, length, grid, controlCount };
    struct Binding { int cc = -1; int channel = 0; };
    using BindingArray = std::array<std::array<Binding, controlCount>, sliderseq::maxTracks>;

    struct Snapshot {
        sliderseq::State sequence = sliderseq::makeDefaultState();
        BindingArray bindings {};
        bool playRecordedMidi = false;
    };

    enum class OpKind : uint8_t {
        setTrackCount,
        setPlayRecordedMidi,
        setSlot,
        setRate,
        setVelocity,
        setGroove,
        setModMode,
        setModAmount,
        setMuted,
        setSolo,
        setExclusiveSolo,
        setMidiNote,
        setMidiChannel,
        setStep,
        setPatternLength,
        resizePattern,
        setPattern,
        setBinding
    };

    struct StateOp {
        uint64_t seq = 0;
        OpKind kind = OpKind::setSlot;
        int track = 0;
        int slotIndex = 0;
        int arg0 = 0;
        int arg1 = 0;
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
    bool isHostPlaying() const { return hostPlaying.load (std::memory_order_relaxed); }
    bool hasHostPosition() const { return hostPositionAvailable.load (std::memory_order_relaxed); }
    bool isHostRecording() const { return hostRecording.load (std::memory_order_relaxed); }
    unsigned long long generatedNoteCount() const { return generatedNotes.load (std::memory_order_relaxed); }
    uint64_t stateVersion() const noexcept { return version.load (std::memory_order_acquire); }

private:
    struct FullAudioState {
        uint64_t seq = 0;
        sliderseq::RenderState render = sliderseq::toRenderState (sliderseq::makeDefaultState(), false);
        BindingArray bindings {};
    };

    struct ActiveNote {
        int trackIndex = 0;
        int channel = 10;
        int note = 36;
        double remainingSamples = 0.0;
        bool active = false;
    };

    static BusesProperties makeBusesProperties();
    static void applyOpToAudio (const StateOp& op, FullAudioState& target) noexcept;
    static void applyOpToSnapshot (const StateOp& op, Snapshot& target);
    static FullAudioState makeAudioState (const Snapshot& src, uint64_t seq) noexcept;

    void drainAudioToUiLocked();
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
    void applyIncomingCC (int cc, int channel, int value, int samplePos, juce::MidiBuffer& outMidi) noexcept;
    juce::MidiBuffer& acquireOutputBuffer() noexcept;
    void finalizeOutputBuffer (juce::MidiBuffer& hostMidi, juce::MidiBuffer& built) noexcept;

    template<typename Sample> void renderBlock (juce::AudioBuffer<Sample>&, juce::MidiBuffer&);
    template<typename Sample> void renderBypassed (juce::AudioBuffer<Sample>&, juce::MidiBuffer&);

    // Message-thread authoritative state (never touched by audio thread).
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
    juce::MidiBuffer outputMidiA, outputMidiB;
    bool swappedOutputOnce = false;
    double currentSampleRate = 44100.0;
    double lastBpm = 0.0;
    double expectedNextTick = 0.0;
    bool hasExpectedTick = false;
    bool wasPlaying = false;
    std::atomic<bool> pendingFlushOnNextBlock { false };

    std::atomic<int> learnTarget { -1 };
    std::atomic<bool> hostPlaying { false }, hostPositionAvailable { false }, hostRecording { false };
    std::atomic<unsigned long long> generatedNotes { 0 };
    std::atomic<uint64_t> version { 1 };
};
