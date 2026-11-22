#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <functional>
#include <memory>
#include "SequencerEngine.h"

class SliderRhythmMidiProcessor final : public juce::AudioProcessor {
public:
    enum Control { slot, rate, velocity, groove, modAmount, mute, modMode, note, channel, length, grid, controlCount };
    struct Binding { int cc = -1; int channel = 0; };
    struct Snapshot {
        sliderseq::State sequence = sliderseq::makeDefaultState();
        std::array<std::array<Binding, controlCount>, sliderseq::maxTracks> bindings {};
        bool playRecordedMidi = false;
    };

    SliderRhythmMidiProcessor();
    ~SliderRhythmMidiProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
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

    std::shared_ptr<const Snapshot> snapshot() const { return std::atomic_load (&state); }
    void edit (const std::function<void (Snapshot&)>&);
    void armMidiLearn (int track, Control control);
    void cancelMidiLearn() { learnTarget.store (-1); }
    int getLearnTarget() const { return learnTarget.load(); }
    bool isHostPlaying() const { return hostPlaying.load(); }
    bool hasHostPosition() const { return hostPositionAvailable.load(); }
    bool isHostRecording() const { return hostRecording.load(); }
    unsigned long long generatedNoteCount() const { return generatedNotes.load(); }

private:
    static BusesProperties makeBusesProperties();
    template<typename Sample> void renderBlock (juce::AudioBuffer<Sample>&, juce::MidiBuffer&);
    std::shared_ptr<const Snapshot> state;
    std::atomic<int> learnTarget { -1 };
    std::atomic<bool> hostPlaying { false }, hostPositionAvailable { false }, hostRecording { false };
    std::atomic<unsigned long long> generatedNotes { 0 };
    double currentSampleRate = 44100.0;
    bool wasPlaying = false;
};
