#include "PluginProcessor.h"
#include "PatternPresets.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>

namespace {
thread_local bool gCountAllocations = false;
thread_local int gAllocationCount = 0;

struct TestPlayHead final : public juce::AudioPlayHead {
    double bpm = 120.0;
    double ppq = 0.0;
    bool playing = true;
    bool looping = false;
    double loopStartPpq = 0.0;
    double loopEndPpq = 4.0;

    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo info;
        info.setBpm (bpm);
        info.setPpqPosition (ppq);
        info.setIsPlaying (playing);
        info.setIsLooping (looping);
        if (looping)
            info.setLoopPoints (LoopPoints { loopStartPpq, loopEndPpq });
        return info;
    }
};
} // namespace

void* operator new (std::size_t size) {
    if (gCountAllocations) ++gAllocationCount;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

static void require (bool condition, const char* message) {
    if (! condition) { std::cerr << message << '\n'; std::exit (1); }
}

static void sendCC (SliderRhythmMidiProcessor& processor, int cc, int value, int channel = 1) {
    juce::AudioBuffer<float> audio (0, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::controllerEvent (channel, cc, value), 0);
    processor.processBlock (audio, midi);
}

static void trackMidiBalance (const juce::MidiBuffer& midi, std::map<std::pair<int, int>, int>& activeCount) {
    for (const auto metadata : midi) {
        const auto msg = metadata.getMessage();
        const auto key = std::make_pair (msg.getChannel(), msg.getNoteNumber());
        if (msg.isNoteOn()) {
            require (activeCount[key] == 0, "Duplicate overlapping Note On without prior Note Off");
            activeCount[key] += 1;
        } else if (msg.isNoteOff()) {
            require (activeCount[key] == 1, "Orphaned Note Off without matching active Note On");
            activeCount[key] -= 1;
        }
    }
}

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    SliderRhythmMidiProcessor processor;
    require (processor.getProgramName (0).isNotEmpty(), "Default program name must be non-empty for VST3 validation");
    require (processor.isMidiEffect() && processor.acceptsMidi() && processor.producesMidi(),
        "Processor must report MIDI effect input/output capabilities");
    {
        juce::AudioProcessor::BusesLayout emptyLayout;
        require (processor.isBusesLayoutSupported (emptyLayout), "Empty 0/0 bus layout should be supported for non-VST3");
        juce::AudioProcessor::BusesLayout stereoInLayout;
        stereoInLayout.inputBuses.add (juce::AudioChannelSet::stereo());
        require (! processor.isBusesLayoutSupported (stereoInLayout), "Audio input buses must be rejected");
    }
    processor.prepareToPlay (44100.0, 512);

    processor.armMidiLearn (1, SliderRhythmMidiProcessor::slot);
    sendCC (processor, 21, 64);
    require (processor.snapshot()->bindings[1][SliderRhythmMidiProcessor::slot].cc == 21, "Track 2 did not learn CC 21");
    require (processor.snapshot()->sequence.tracks[0].slot == 0, "Track 1 changed during Track 2 learn");
    sendCC (processor, 21, 127);
    require (processor.snapshot()->sequence.tracks[1].slot == 11, "Track 2 fader did not respond");
    require (processor.snapshot()->sequence.tracks[0].slot == 0, "Track 1 responded to Track 2 CC");

    processor.armMidiLearn (2, SliderRhythmMidiProcessor::velocity);
    sendCC (processor, 22, 0, 2);
    sendCC (processor, 22, 96, 2);
    require (processor.snapshot()->sequence.tracks[2].velocity == 96, "Track 3 velocity did not respond");
    require (processor.snapshot()->sequence.tracks[0].velocity == 100, "Track 1 responded to Track 3 CC");

    // Reusing the same physical CC moves ownership to the newly learned track.
    processor.armMidiLearn (2, SliderRhythmMidiProcessor::slot);
    sendCC (processor, 21, 0);
    require (processor.snapshot()->bindings[1][SliderRhythmMidiProcessor::slot].cc == -1, "Old Track 2 CC binding remains");
    sendCC (processor, 21, 0);
    require (processor.snapshot()->sequence.tracks[2].slot == 0, "CC did not move to Track 3");
    require (processor.snapshot()->sequence.tracks[1].slot == 11, "CC still changed Track 2");

    // Learning must also work while passing recorded MIDI through.
    processor.edit ([] (auto& state) { state.playRecordedMidi = true; });
    processor.armMidiLearn (3, SliderRhythmMidiProcessor::slot);
    sendCC (processor, 23, 64);
    require (processor.snapshot()->bindings[3][SliderRhythmMidiProcessor::slot].cc == 23, "Track 4 learn failed in recorded mode");
    const auto directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("slider-pattern-test-" + juce::Uuid().toString());
    auto beat = sliderseq::makePattern ("My fill", 16, { 0 });
    beat.resize (64);
    beat.steps[63] = true;
    beat.length = 55;
    require (patternpresets::save (beat, directory), "Could not save preset");
    const auto beats = patternpresets::load (directory);
    const auto kickFolder = patternpresets::trackFolder ("KICK", 0, directory);
    const auto snareFolder = patternpresets::trackFolder ("SNARE", 1, directory);
    require (patternpresets::save (beat, kickFolder) && kickFolder.getParentDirectory() == directory,
        "Could not save to the track directory");
    require (patternpresets::load (kickFolder).size() == 1
        && patternpresets::load (snareFolder).empty()
        && patternpresets::load (directory).size() == 1,
        "Track and legacy presets should load independently");
    directory.deleteRecursively();
    require (beats.size() == 1 && beats[0].name == beat.name
        && beats[0].grid == 64 && beats[0].length == 55 && beats[0].subdivision == 16 && beats[0].steps == beat.steps,
        "Preset round trip lost pattern data");

    processor.edit ([&] (auto& state) { state.sequence.tracks[1].patterns[4] = beats[0]; });
    juce::MemoryBlock projectState;
    processor.getStateInformation (projectState);
    SliderRhythmMidiProcessor reopened;
    reopened.setStateInformation (projectState.getData(), static_cast<int> (projectState.getSize()));
    const auto restored = reopened.snapshot()->sequence.tracks[1].patterns[4];
    require (restored.name == beat.name && restored.grid == beat.grid
        && restored.length == beat.length && restored.subdivision == 16 && restored.steps == beat.steps,
        "Project state lost the loaded slot");

    // Real-time zero-allocation test under heavy load (16 tracks, 64th notes, rate=3, CC flood, 4096-sample blocks).
    {
        SliderRhythmMidiProcessor rtProc;
        TestPlayHead playHead;
        rtProc.setPlayHead (&playHead);
        rtProc.prepareToPlay (48000.0, 4096);
        rtProc.edit ([] (auto& s) {
            s.sequence.trackCount = 16;
            for (int t = 0; t < 16; ++t) {
                s.sequence.tracks[static_cast<size_t> (t)].rate = 3.0;
                auto& p = s.sequence.tracks[static_cast<size_t> (t)].patterns[0];
                p.grid = p.length = p.subdivision = 64;
                p.steps.fill (true);
            }
            s.bindings[0][SliderRhythmMidiProcessor::slot] = { 10, 1 };
            s.bindings[1][SliderRhythmMidiProcessor::velocity] = { 11, 1 };
        });

        juce::AudioBuffer<float> audio (2, 4096);
        juce::MidiBuffer hostMidi;
        hostMidi.ensureSize (32768);
        const uint8_t ccA[3] { 0xb0, 10, 64 };
        const uint8_t ccB[3] { 0xb0, 11, 100 };

        // Warm-up block to swap buffers with host MidiBuffer.
        playHead.ppq = 0.0;
        rtProc.processBlock (audio, hostMidi);

        gAllocationCount = 0;
        gCountAllocations = true;
        for (int b = 1; b <= 8; ++b) {
            hostMidi.clear();
            hostMidi.addEvent (ccA, 3, 12);
            hostMidi.addEvent (ccB, 3, 250);
            playHead.ppq = b * (4096.0 * 120.0 / (60.0 * 48000.0));
            rtProc.processBlock (audio, hostMidi);
        }
        gCountAllocations = false;
        require (gAllocationCount == 0, "processBlock allocated memory on the audio thread");
    }

    // Note-Off exact pairing test across mute, note/channel changes, slot switch, 3-beat loop wrap, seek, and bypass.
    {
        SliderRhythmMidiProcessor noteProc;
        TestPlayHead playHead;
        playHead.bpm = 120.0;
        playHead.looping = true;
        playHead.loopStartPpq = 0.0;
        playHead.loopEndPpq = 3.0; // 3-beat loop
        noteProc.setPlayHead (&playHead);
        noteProc.prepareToPlay (48000.0, 512);
        noteProc.edit ([] (auto& s) {
            s.bindings[0][SliderRhythmMidiProcessor::mute] = { 30, 1 };
            s.bindings[0][SliderRhythmMidiProcessor::note] = { 31, 1 };
            s.bindings[0][SliderRhythmMidiProcessor::channel] = { 32, 1 };
            s.bindings[3][SliderRhythmMidiProcessor::slot] = { 33, 1 };
        });

        std::map<std::pair<int, int>, int> activeCount;
        juce::MidiBuffer midi;
        midi.ensureSize (32768);
        const double ppqPerSample = 120.0 / (60.0 * 48000.0);
        const std::array<int, 7> blockSizes { 37, 128, 256, 512, 64, 193, 333 };
        double ppq = 0.0;

        for (int iter = 0; iter < 400; ++iter) {
            const int n = blockSizes[static_cast<size_t> (iter % blockSizes.size())];
            juce::AudioBuffer<float> audio (0, n);
            midi.clear();
            if (iter == 10) midi.addEvent (juce::MidiMessage::controllerEvent (1, 31, 60), n / 2); // change kick note mid-gate
            if (iter == 25) midi.addEvent (juce::MidiMessage::controllerEvent (1, 32, 20), n / 2); // change kick channel
            if (iter == 40) midi.addEvent (juce::MidiMessage::controllerEvent (1, 30, 127), 5);    // mute kick mid-note
            if (iter == 55) midi.addEvent (juce::MidiMessage::controllerEvent (1, 30, 0), 5);      // unmute kick
            if (iter == 70) midi.addEvent (juce::MidiMessage::controllerEvent (1, 33, 100), 10);   // switch Open HH slot
            if (iter == 120) playHead.bpm = 90.0;                                                   // tempo drop mid-gate
            if (iter == 160) ppq = 1.75;                                                            // sudden seek jump

            playHead.ppq = ppq;
            noteProc.processBlock (audio, midi);
            trackMidiBalance (midi, activeCount);

            const double curPpqPerSample = playHead.bpm / (60.0 * 48000.0);
            ppq += n * curPpqPerSample;
            while (ppq >= playHead.loopEndPpq) ppq -= (playHead.loopEndPpq - playHead.loopStartPpq);
        }

        // Bypass must emit owned Note Offs BEFORE any passthrough Note On at sample 0.
        {
            juce::AudioBuffer<float> audio (0, 256);
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOn (10, 60, static_cast<juce::uint8> (100)), 0);
            noteProc.processBlockBypassed (audio, midi);
            bool seenPassthrough = false;
            for (const auto metadata : midi) {
                const auto msg = metadata.getMessage();
                if (msg.isNoteOn() && msg.getNoteNumber() == 60 && msg.getChannel() == 10) {
                    seenPassthrough = true;
                    continue;
                }
                if (msg.isNoteOff()) {
                    require (! seenPassthrough, "Bypass Note Off must precede passthrough Note On at sample 0");
                    const auto key = std::make_pair (msg.getChannel(), msg.getNoteNumber());
                    require (activeCount[key] == 1, "Bypass emitted unexpected Note Off");
                    activeCount[key] -= 1;
                }
            }
            require (seenPassthrough, "Bypass must preserve incoming passthrough MIDI");
            for (const auto& [key, count] : activeCount)
                require (count == 0, "Stuck note remained after bypass flush");
        }
    }

    // Editor creation/destruction smoke test (SharedResourcePointer LookAndFeel teardown).
    {
        SliderRhythmMidiProcessor uiProc;
        std::unique_ptr<juce::AudioProcessorEditor> ed (uiProc.createEditor());
        require (ed != nullptr, "Failed to create editor");
    }

    std::cout << "MIDI Learn, real-time, and Note-Off checks passed\n";
}
