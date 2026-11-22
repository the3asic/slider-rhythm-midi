#include "PluginProcessor.h"
#include "PatternPresets.h"
#include "MidiExporter.h"
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
            s.bindings[0][SliderRhythmMidiProcessor::mute] = { 30, 1, SliderRhythmMidiProcessor::BindingKind::cc, SliderRhythmMidiProcessor::TriggerMode::momentary, false };
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

    // v2 -> v3 project state migration check: legacy modMode, Open HH gate/choke, and Fill velocityScale.
    {
        juce::XmlElement legacyRoot ("SliderRhythmMidi");
        legacyRoot.setAttribute ("version", 2);
        legacyRoot.setAttribute ("trackCount", 4);
        for (int i = 0; i < 4; ++i) {
            auto* t = legacyRoot.createNewChildElement ("Track");
            t->setAttribute ("index", i);
            t->setAttribute ("modMode", i == 0 ? 1 : (i == 2 ? 1 : 0)); // track 0 -> rampDown, track 2 -> cos16
            auto* p = t->createNewChildElement ("Pattern");
            p->setAttribute ("slot", 10);
            p->setAttribute ("name", "Legacy Fill");
            p->setAttribute ("grid", 16);
            p->setAttribute ("subdivision", 16);
            p->setAttribute ("length", 16);
            p->setAttribute ("steps", juce::String::repeatedString ("0", 64));
        }
        juce::MemoryBlock legacyBin;
        SliderRhythmMidiProcessor::copyXmlToBinary (legacyRoot, legacyBin);
        SliderRhythmMidiProcessor migrated;
        migrated.setStateInformation (legacyBin.getData(), static_cast<int> (legacyBin.getSize()));
        const auto snap = migrated.snapshot();
        require (snap->sequence.tracks[0].modShape == sliderseq::ModShape::rampDown, "v2 track 0 modMode migration failed");
        require (snap->sequence.tracks[2].modShape == sliderseq::ModShape::cos16, "v2 track 2 modMode migration failed");
        require (std::abs (snap->sequence.tracks[3].gateTicks - 144.0) < 1e-6, "v2 Open HH gate migration failed");
        require (snap->sequence.tracks[3].chokeMask == (1u << 2), "v2 Open HH chokeMask migration failed");
        require (std::abs (snap->sequence.tracks[0].patterns[10].velocityScale - 0.72f) < 1e-4f, "v2 Fill velocityScale migration failed");
    }

    // APVTS 128 host parameters check: exact count, automation playback, non-destructive Loop Steps, and zero-alloc modulation.
    {
        SliderRhythmMidiProcessor apvtsProc;
        require (apvtsProc.getParameters().size() == 128, "Expected exactly 128 exposed host parameters (16 tracks x 8)");
        TestPlayHead playHead;
        apvtsProc.setPlayHead (&playHead);
        apvtsProc.prepareToPlay (48000.0, 512);

        // Set non-destructive Loop Steps override on Track 0 via host parameter and verify pattern.length is untouched across slot switch.
        auto* loopParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "loop_steps"));
        auto* slotParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "slot"));
        auto* muteParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "mute"));
        auto* velParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "velocity"));
        require (loopParam && slotParam && muteParam && velParam, "Missing expected host parameter IDs");

        loopParam->setValueNotifyingHost (loopParam->convertTo0to1 (4.0f));
        slotParam->setValueNotifyingHost (slotParam->convertTo0to1 (2.0f));
        velParam->setValueNotifyingHost (velParam->convertTo0to1 (77.0f));

        juce::AudioBuffer<float> audio (2, 512);
        juce::MidiBuffer hostMidi;
        hostMidi.ensureSize (32768);
        apvtsProc.processBlock (audio, hostMidi); // warm-up swap

        // Modulate host parameters every block while asserting zero allocations on audio thread.
        gAllocationCount = 0;
        gCountAllocations = true;
        for (int b = 1; b <= 6; ++b) {
            velParam->setValue (velParam->convertTo0to1 (static_cast<float> (60 + b * 5)));
            muteParam->setValue (b == 3 ? 1.0f : 0.0f);
            hostMidi.clear();
            playHead.ppq = b * (512.0 * 120.0 / (60.0 * 48000.0));
            apvtsProc.processBlock (audio, hostMidi);
        }
        gCountAllocations = false;
        require (gAllocationCount == 0, "Host parameter modulation caused audio-thread heap allocation");

        const auto snap = apvtsProc.snapshot();
        require (snap->sequence.tracks[0].loopStepsOverride == 4, "Loop Steps override did not sync to snapshot");
        require (snap->sequence.tracks[0].slot == 1, "Host Slot parameter did not sync to snapshot");
        require (snap->sequence.tracks[0].patterns[0].length == 16 && snap->sequence.tracks[0].patterns[1].length == 16,
                 "Non-destructive Loop Steps override must not overwrite stored pattern lengths");

        // Save/restore round trip preserves host parameter values.
        juce::MemoryBlock saved;
        apvtsProc.getStateInformation (saved);
        SliderRhythmMidiProcessor reloaded;
        reloaded.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        require (reloaded.snapshot()->sequence.tracks[0].loopStepsOverride == 4, "Reloaded state lost Loop Steps");
        require (reloaded.snapshot()->sequence.tracks[0].slot == 1, "Reloaded state lost host Slot");
    }

    // PR 6 workflow tests: Toggle vs Momentary CC, optional Note On Learn switch, 3-state GEN/GEN+THRU/THRU,
    // playhead active step, Ableton-style exclusive/additive solo + UNMUTE/UNSOLO ALL, and .mid export round trip.
    {
        SliderRhythmMidiProcessor wfProc;
        TestPlayHead playHead;
        wfProc.setPlayHead (&playHead);
        wfProc.prepareToPlay (48000.0, 512);

        // 1) Toggle mode on CC mute: press 127 flips mute ON, release 0 keeps mute ON, second 127 flips OFF.
        wfProc.setLearnTriggerMode (SliderRhythmMidiProcessor::TriggerMode::toggle);
        wfProc.armMidiLearn (0, SliderRhythmMidiProcessor::mute);
        sendCC (wfProc, 40, 127);
        sendCC (wfProc, 40, 127);
        require (wfProc.snapshot()->sequence.tracks[0].muted, "Toggle CC 127 should mute track");
        sendCC (wfProc, 40, 0);
        require (wfProc.snapshot()->sequence.tracks[0].muted, "Toggle CC 0 release must NOT unmute track");
        sendCC (wfProc, 40, 127);
        require (! wfProc.snapshot()->sequence.tracks[0].muted, "Second Toggle CC 127 should unmute track");

        // 2) Note Learn OFF ignores Note On during learn; ON learns Note On for Solo and consumes it in GEN+THRU.
        wfProc.setNoteLearnEnabled (false);
        wfProc.armMidiLearn (1, SliderRhythmMidiProcessor::solo);
        {
            juce::AudioBuffer<float> audio (0, 256);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 24, static_cast<juce::uint8> (100)), 0);
            wfProc.processBlock (audio, midi);
        }
        require (wfProc.getLearnTarget() >= 0, "Note On must be ignored during Learn while Note Learn toggle is OFF");

        wfProc.setNoteLearnEnabled (true);
        {
            juce::AudioBuffer<float> audio (0, 256);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 24, static_cast<juce::uint8> (100)), 0);
            wfProc.processBlock (audio, midi);
        }
        require (wfProc.snapshot()->bindings[1][SliderRhythmMidiProcessor::solo].cc == 24
                 && wfProc.snapshot()->bindings[1][SliderRhythmMidiProcessor::solo].kind == SliderRhythmMidiProcessor::BindingKind::note,
                 "Note On Learn failed when Note Learn toggle is ON");

        // Switch to GEN + THRU: learned note 24 toggles Track 1 solo AND is consumed; unlearned note 60 passes through!
        wfProc.edit ([] (auto& s) { s.playbackMode = sliderseq::PlaybackMode::genPlusThru; });
        {
            juce::AudioBuffer<float> audio (0, 256);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 24, static_cast<juce::uint8> (100)), 0);
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (90)), 10);
            playHead.ppq = 0.0;
            wfProc.processBlock (audio, midi);
            bool sawControlNote = false, sawLiveNote = false, sawGenerated = false;
            for (const auto m : midi) {
                const auto msg = m.getMessage();
                if (msg.isNoteOn() && msg.getNoteNumber() == 24 && msg.getChannel() == 1) sawControlNote = true;
                if (msg.isNoteOn() && msg.getNoteNumber() == 60 && msg.getChannel() == 1) sawLiveNote = true;
                if (msg.isNoteOn() && msg.getChannel() == 10) sawGenerated = true;
            }
            require (! sawControlNote, "Learned Note control must be consumed and not leak to drum kit");
            require (sawLiveNote, "GEN + THRU must pass through unlearned live notes");
            require (wfProc.snapshot()->sequence.tracks[1].solo, "Learned Note On should toggle Solo");
            require (wfProc.getActiveStep (0) == 0, "Playhead active step should report step 0 at PPQ 0");
            (void) sawGenerated;
        }

        // 3) Ableton-style exclusive solo, additive solo, UNSOLO ALL, UNMUTE ALL.
        wfProc.unsoloAllTracks();
        wfProc.soloTrackExclusive (0);
        require (wfProc.snapshot()->sequence.tracks[0].solo && ! wfProc.snapshot()->sequence.tracks[1].solo, "Exclusive solo failed");
        wfProc.toggleTrackSoloAdditive (2);
        require (wfProc.snapshot()->sequence.tracks[0].solo && wfProc.snapshot()->sequence.tracks[2].solo, "Additive solo failed");
        wfProc.unsoloAllTracks();
        require (! wfProc.snapshot()->sequence.tracks[0].solo && ! wfProc.snapshot()->sequence.tracks[2].solo, "UNSOLO ALL failed");
        wfProc.setHostParamValue (0, "mute", 1.0f);
        wfProc.unmuteAllTracks();
        require (! wfProc.snapshot()->sequence.tracks[0].muted, "UNMUTE ALL failed");

        // 4) Drag .mid exporter creates a valid Standard MIDI File readable back via juce::MidiFile.
        const auto midFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("slider-export-test-" + juce::Uuid().toString() + ".mid");
        require (midiexporter::writeMidiFile (sliderseq::makeDefaultState(), midFile, 2, 128.0), "Failed to write exported MIDI file");
        juce::FileInputStream inStream (midFile);
        juce::MidiFile parsed;
        require (inStream.openedOk() && parsed.readFrom (inStream), "Failed to parse exported MIDI file");
        require (parsed.getNumTracks() == 1 && parsed.getTimeFormat() == sliderseq::ticksPerQuarter, "Exported MIDI header mismatch");
        int noteOns = 0, noteOffs = 0;
        const auto* trackSeq = parsed.getTrack (0);
        for (int i = 0; i < trackSeq->getNumEvents(); ++i) {
            const auto& msg = trackSeq->getEventPointer (i)->message;
            if (msg.isNoteOn()) ++noteOns;
            if (msg.isNoteOff()) ++noteOffs;
        }
        require (noteOns == 28 && noteOffs == 28, "2-bar default export must contain 28 matched Note On/Off pairs");
        midFile.deleteFile();
    }

    // Editor creation/destruction smoke test (SharedResourcePointer LookAndFeel teardown).
    {
        SliderRhythmMidiProcessor uiProc;
        std::unique_ptr<juce::AudioProcessorEditor> ed (uiProc.createEditor());
        require (ed != nullptr, "Failed to create editor");
    }

    std::cout << "MIDI Learn, real-time, and Note-Off checks passed\n";
}
