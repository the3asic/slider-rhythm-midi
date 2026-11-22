#include "PluginProcessor.h"
#include "MappingPresets.h"
#include "MidiExporter.h"
#include "PatternPresets.h"
#include "UiPreferences.h"
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

    // Slot CC maps 0..127 -> 0 (EMPTY) .. activeZones (12 by default).
    processor.armMidiLearn (1, SliderRhythmMidiProcessor::slot);
    sendCC (processor, 21, 64);
    require (processor.snapshot()->bindings[1][SliderRhythmMidiProcessor::slot].cc == 21, "Track 2 did not learn CC 21");
    require (processor.snapshot()->sequence.tracks[0].slot == 1, "Track 1 changed during Track 2 learn");
    sendCC (processor, 21, 127);
    require (processor.snapshot()->sequence.tracks[1].slot == 12, "Track 2 fader did not reach top zone 12");
    sendCC (processor, 21, 0);
    require (processor.snapshot()->sequence.tracks[1].slot == 0, "Track 2 CC 0 did not reach EMPTY floor (0)");
    require (processor.snapshot()->sequence.tracks[0].slot == 1, "Track 1 responded to Track 2 CC");

    // Configurable per-track activeZones scales CC throw across 0..activeZones.
    processor.setTrackActiveZones (1, 4);
    sendCC (processor, 21, 127);
    require (processor.snapshot()->sequence.tracks[1].slot == 4, "Track 2 with 4 zones must map CC 127 to slot 4");
    sendCC (processor, 21, 0);
    require (processor.snapshot()->sequence.tracks[1].slot == 0, "Track 2 with 4 zones must map CC 0 to EMPTY (0)");
    processor.setTrackActiveZones (1, 12);
    sendCC (processor, 21, 127);

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
    require (processor.snapshot()->sequence.tracks[2].slot == 0, "CC did not move to Track 3 EMPTY");
    require (processor.snapshot()->sequence.tracks[1].slot == 12, "CC still changed Track 2");

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
            s.sequence.globalSwing = 35;
            for (int t = 0; t < 16; ++t) {
                s.sequence.tracks[static_cast<size_t> (t)].slot = 1;
                s.sequence.tracks[static_cast<size_t> (t)].rate = 3.0;
                auto& p = s.sequence.tracks[static_cast<size_t> (t)].patterns[0];
                p.grid = p.length = p.subdivision = 64;
                p.steps.fill (true);
            }
            s.bindings[0][SliderRhythmMidiProcessor::slot] = { 10, 1 };
            s.bindings[1][SliderRhythmMidiProcessor::velocity] = { 11, 1 };
            s.bindings[0][SliderRhythmMidiProcessor::globalSwing] = { 12, 1 };
        });

        juce::AudioBuffer<float> audio (2, 4096);
        juce::MidiBuffer hostMidi;
        hostMidi.ensureSize (32768);
        const uint8_t ccA[3] { 0xb0, 10, 64 };
        const uint8_t ccB[3] { 0xb0, 11, 100 };
        const uint8_t ccG[3] { 0xb0, 12, 90 };

        playHead.ppq = 0.0;
        rtProc.processBlock (audio, hostMidi);

        gAllocationCount = 0;
        gCountAllocations = true;
        for (int b = 1; b <= 8; ++b) {
            hostMidi.clear();
            hostMidi.addEvent (ccA, 3, 12);
            hostMidi.addEvent (ccB, 3, 250);
            hostMidi.addEvent (ccG, 3, 500);
            playHead.ppq = b * (4096.0 * 120.0 / (60.0 * 48000.0));
            rtProc.processBlock (audio, hostMidi);
        }
        gCountAllocations = false;
        require (gAllocationCount == 0, "processBlock allocated memory on the audio thread");
    }

    // Note-Off exact pairing test across EMPTY floor slam, mute, note/channel changes, slot switch, 3-beat loop wrap, seek, and bypass.
    {
        SliderRhythmMidiProcessor noteProc;
        TestPlayHead playHead;
        playHead.bpm = 120.0;
        playHead.looping = true;
        playHead.loopStartPpq = 0.0;
        playHead.loopEndPpq = 3.0;
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
        const std::array<int, 7> blockSizes { 37, 128, 256, 512, 64, 193, 333 };
        double ppq = 0.0;

        for (int iter = 0; iter < 400; ++iter) {
            const int n = blockSizes[static_cast<size_t> (iter % blockSizes.size())];
            juce::AudioBuffer<float> audio (0, n);
            midi.clear();
            if (iter == 10) midi.addEvent (juce::MidiMessage::controllerEvent (1, 31, 60), n / 2);
            if (iter == 25) midi.addEvent (juce::MidiMessage::controllerEvent (1, 32, 20), n / 2);
            if (iter == 40) midi.addEvent (juce::MidiMessage::controllerEvent (1, 30, 127), 5);
            if (iter == 55) midi.addEvent (juce::MidiMessage::controllerEvent (1, 30, 0), 5);
            if (iter == 70) midi.addEvent (juce::MidiMessage::controllerEvent (1, 33, 0), 10);   // slam Open HH fader to EMPTY mid-gate
            if (iter == 85) midi.addEvent (juce::MidiMessage::controllerEvent (1, 33, 100), 10); // bring Open HH back up
            if (iter == 120) playHead.bpm = 90.0;
            if (iter == 160) ppq = 1.75;

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

    // v2 -> v4 project state migration check: legacy slot 0->1, groove 75 -> globalSwing +50, modMode, Open HH gate/choke, Fill velocityScale.
    {
        juce::XmlElement legacyRoot ("SliderRhythmMidi");
        legacyRoot.setAttribute ("version", 2);
        legacyRoot.setAttribute ("trackCount", 4);
        for (int i = 0; i < 4; ++i) {
            auto* t = legacyRoot.createNewChildElement ("Track");
            t->setAttribute ("index", i);
            t->setAttribute ("slot", i); // legacy 0..3 -> v4 1..4
            t->setAttribute ("groove", i == 0 ? 75 : 50); // legacy groove 75 -> globalSwing +50, track 0 = 100%, others 0%
            t->setAttribute ("modMode", i == 0 ? 1 : (i == 2 ? 1 : 0));
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
        require (snap->sequence.tracks[0].slot == 1 && snap->sequence.tracks[1].slot == 2, "v2 -> v4 slot shift failed");
        require (snap->sequence.tracks[0].activeZones == 12, "v2 -> v4 activeZones default failed");
        require (snap->sequence.globalSwing == 50, "v2 -> v4 globalSwing migration failed");
        require (snap->sequence.tracks[0].groove == 100 && snap->sequence.tracks[1].groove == 0, "v2 -> v4 per-track swing % migration failed");
        require (snap->sequence.tracks[0].modShape == sliderseq::ModShape::rampDown, "v2 track 0 modMode migration failed");
        require (snap->sequence.tracks[2].modShape == sliderseq::ModShape::cos16, "v2 track 2 modMode migration failed");
        require (std::abs (snap->sequence.tracks[3].gateTicks - 144.0) < 1e-6, "v2 Open HH gate migration failed");
        require (snap->sequence.tracks[3].chokeMask == (1u << 2), "v2 Open HH chokeMask migration failed");
        require (std::abs (snap->sequence.tracks[0].patterns[10].velocityScale - 0.72f) < 1e-4f, "v2 Fill velocityScale migration failed");
    }

    // APVTS 129 host parameters check (1 global_swing + 16x8 per-track): exact count, EMPTY slot, non-destructive Loop Steps, zero-alloc modulation.
    {
        SliderRhythmMidiProcessor apvtsProc;
        require (apvtsProc.getParameters().size() == 129, "Expected 129 exposed host parameters (1 global + 16 tracks x 8)");
        TestPlayHead playHead;
        apvtsProc.setPlayHead (&playHead);
        apvtsProc.prepareToPlay (48000.0, 512);

        auto* globalSwingParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::globalSwingParamId());
        auto* loopParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "loop_steps"));
        auto* slotParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "slot"));
        auto* muteParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "mute"));
        auto* velParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "velocity"));
        require (globalSwingParam && loopParam && slotParam && muteParam && velParam, "Missing expected host parameter IDs");

        globalSwingParam->setValueNotifyingHost (globalSwingParam->convertTo0to1 (34.0f));
        loopParam->setValueNotifyingHost (loopParam->convertTo0to1 (4.0f));
        slotParam->setValueNotifyingHost (slotParam->convertTo0to1 (2.0f));
        velParam->setValueNotifyingHost (velParam->convertTo0to1 (77.0f));

        juce::AudioBuffer<float> audio (2, 512);
        juce::MidiBuffer hostMidi;
        hostMidi.ensureSize (32768);
        apvtsProc.processBlock (audio, hostMidi);

        gAllocationCount = 0;
        gCountAllocations = true;
        for (int b = 1; b <= 6; ++b) {
            const float vNorm = velParam->convertTo0to1 (static_cast<float> (60 + b * 5));
            velParam->setValue (vNorm);
            velParam->sendValueChangedMessageToListeners (vNorm);
            const float gNorm = globalSwingParam->convertTo0to1 (static_cast<float> (-20 + b * 10));
            globalSwingParam->setValue (gNorm);
            globalSwingParam->sendValueChangedMessageToListeners (gNorm);
            const float mNorm = b == 3 ? 1.0f : 0.0f;
            muteParam->setValue (mNorm);
            muteParam->sendValueChangedMessageToListeners (mNorm);
            hostMidi.clear();
            playHead.ppq = b * (512.0 * 120.0 / (60.0 * 48000.0));
            apvtsProc.processBlock (audio, hostMidi);
        }
        gCountAllocations = false;
        require (gAllocationCount == 0, "Host parameter modulation caused audio-thread heap allocation");

        const auto snap = apvtsProc.snapshot();
        require (snap->sequence.globalSwing == 40, "Global Swing host param did not sync to snapshot");
        require (snap->sequence.tracks[0].loopStepsOverride == 4, "Loop Steps override did not sync to snapshot");
        require (snap->sequence.tracks[0].slot == 2, "Host Slot parameter did not sync to snapshot");
        require (snap->sequence.tracks[0].patterns[0].length == 16 && snap->sequence.tracks[0].patterns[1].length == 16,
                 "Non-destructive Loop Steps override must not overwrite stored pattern lengths");

        // Save/restore round trip preserves globalSwing, activeZones, slot, and Loop Steps.
        apvtsProc.setTrackActiveZones (0, 6);
        juce::MemoryBlock saved;
        apvtsProc.getStateInformation (saved);
        SliderRhythmMidiProcessor reloaded;
        reloaded.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        require (reloaded.snapshot()->sequence.globalSwing == 40, "Reloaded state lost Global Swing");
        require (reloaded.snapshot()->sequence.tracks[0].activeZones == 6, "Reloaded state lost activeZones");
        require (reloaded.snapshot()->sequence.tracks[0].loopStepsOverride == 4, "Reloaded state lost Loop Steps");
        require (reloaded.snapshot()->sequence.tracks[0].slot == 2, "Reloaded state lost host Slot");
    }

    // Mapping Preset dirty (*) detection, load/revert, and user XML round trip + UiPreferences round trip.
    {
        SliderRhythmMidiProcessor mpProc;
        const auto factories = mappingpresets::factoryPresets();
        require (factories.size() == 6, "Expected 6 factory mapping presets");
        const auto& drumRack = factories.back();
        mpProc.loadMappingPreset (drumRack);
        require (mappingpresets::matchesState (mpProc.snapshot()->activeMappingPreset, mpProc.snapshot()->sequence),
                 "Freshly loaded Mapping Preset must match state cleanly (not dirty)");

        // Mutate PAD 3 MIDI note -> must become dirty (*).
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setMidiNote;
        op.track = 2;
        op.arg0 = 0;
        mpProc.applyUiOp (op);
        require (! mappingpresets::matchesState (mpProc.snapshot()->activeMappingPreset, mpProc.snapshot()->sequence),
                 "Modified MIDI note must mark Mapping Preset dirty (*)");

        // Save custom preset XML to temp dir & reload.
        const auto mpDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("slider-mapping-test-" + juce::Uuid().toString());
        auto customMp = mappingpresets::captureFromState (mpProc.snapshot()->sequence, {}, "My Live 16", false);
        require (mappingpresets::saveUserPreset (customMp, mpDir), "Failed to save user mapping preset");
        const auto loadedUsers = mappingpresets::loadUserPresets (mpDir);
        require (loadedUsers.size() == 1 && loadedUsers[0].name == "My Live 16" && loadedUsers[0].tracks[2].midiNote == 0,
                 "Reloaded user mapping preset mismatch");
        mpDir.deleteRecursively();

        // UiPreferences XML round trip (vertical/horizontal wheel toggles, darkMode, scalePercent).
        const auto prefFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("slider-uiprefs-test-" + juce::Uuid().toString() + ".xml");
        uiprefs::Settings prefs { true, 85, false, true };
        require (uiprefs::save (prefs, prefFile), "Failed to save UI preferences");
        const auto readPrefs = uiprefs::load (prefFile);
        prefFile.deleteFile();
        require (readPrefs.darkMode == true && readPrefs.scalePercent == 85
                 && readPrefs.verticalWheelControls == false && readPrefs.horizontalWheelControls == true,
                 "UI preferences round trip mismatch");
    }

    // Workflow tests: Toggle vs Momentary CC, optional Note On Learn switch, 3-state GEN/GEN+THRU/THRU,
    // playhead active step, Ableton-style exclusive/additive solo + UNMUTE/UNSOLO ALL, and .mid export round trip.
    {
        SliderRhythmMidiProcessor wfProc;
        TestPlayHead playHead;
        wfProc.setPlayHead (&playHead);
        wfProc.prepareToPlay (48000.0, 512);

        wfProc.setLearnTriggerMode (SliderRhythmMidiProcessor::TriggerMode::toggle);
        wfProc.armMidiLearn (0, SliderRhythmMidiProcessor::mute);
        sendCC (wfProc, 40, 127);
        sendCC (wfProc, 40, 127);
        require (wfProc.snapshot()->sequence.tracks[0].muted, "Toggle CC 127 should mute track");
        sendCC (wfProc, 40, 0);
        require (wfProc.snapshot()->sequence.tracks[0].muted, "Toggle CC 0 release must NOT unmute track");
        sendCC (wfProc, 40, 127);
        require (! wfProc.snapshot()->sequence.tracks[0].muted, "Second Toggle CC 127 should unmute track");

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

        wfProc.edit ([] (auto& s) { s.playbackMode = sliderseq::PlaybackMode::genPlusThru; });
        {
            juce::AudioBuffer<float> audio (0, 256);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 24, static_cast<juce::uint8> (100)), 0);
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (90)), 10);
            playHead.ppq = 0.0;
            wfProc.processBlock (audio, midi);
            bool sawControlNote = false, sawLiveNote = false;
            for (const auto m : midi) {
                const auto msg = m.getMessage();
                if (msg.isNoteOn() && msg.getNoteNumber() == 24 && msg.getChannel() == 1) sawControlNote = true;
                if (msg.isNoteOn() && msg.getNoteNumber() == 60 && msg.getChannel() == 1) sawLiveNote = true;
            }
            require (! sawControlNote, "Learned Note control must be consumed and not leak to drum kit");
            require (sawLiveNote, "GEN + THRU must pass through unlearned live notes");
            require (wfProc.snapshot()->sequence.tracks[1].solo, "Learned Note On should toggle Solo");
            require (wfProc.getActiveStep (0) == 0, "Playhead active step should report step 0 at PPQ 0");
        }

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

    // Editor creation/destruction smoke test (Braun RootCanvas, scopes, fader, and SharedResourcePointer teardown).
    {
        SliderRhythmMidiProcessor uiProc;
        std::unique_ptr<juce::AudioProcessorEditor> ed (uiProc.createEditor());
        require (ed != nullptr, "Failed to create editor");
        ed->setSize (1140, 628);

        if (const char* dumpDir = std::getenv ("SLIDER_DUMP_UI_PNG_DIR"); dumpDir != nullptr && dumpDir[0] != '\0') {
            const auto outDir = juce::File (dumpDir);
            outDir.createDirectory();
            const auto factories = mappingpresets::factoryPresets();
            uiProc.loadMappingPreset (factories.back()); // Ableton Drum Rack 16
            uiProc.setGlobalSwingValue (26);
            uiProc.edit ([] (auto& s) {
                s.sequence.trackCount = 6;
                const std::array<const char*, 6> names { "KICK 1", "SNARE", "CLAP", "CLOSED HH", "OPEN HH", "PERC 1" };
                const std::array<int, 6> zones { 6, 8, 4, 12, 6, 8 };
                const std::array<int, 6> slots { 5, 6, 3, 9, 0, 6 };
                const std::array<int, 6> swings { 100, 75, 0, 100, 50, 85 };
                const std::array<sliderseq::ModShape, 6> shapes {
                    sliderseq::ModShape::rampUp, sliderseq::ModShape::cos8, sliderseq::ModShape::rampDown,
                    sliderseq::ModShape::random, sliderseq::ModShape::cos16, sliderseq::ModShape::rampUp
                };
                for (int i = 0; i < 6; ++i) {
                    auto& t = s.sequence.tracks[static_cast<size_t> (i)];
                    t.name = names[static_cast<size_t> (i)];
                    t.activeZones = zones[static_cast<size_t> (i)];
                    t.slot = slots[static_cast<size_t> (i)];
                    t.groove = swings[static_cast<size_t> (i)];
                    t.modShape = shapes[static_cast<size_t> (i)];
                    t.modAmount = 70;
                    t.velocity = 110;
                    if (t.slot > 0 && i >= 4)
                        t.patterns[static_cast<size_t> (t.slot - 1)] = sliderseq::makePattern ("Perc 1/8", 16, { 0, 3, 6, 10, 14 });
                }
            });
            const auto savedPrefs = uiprefs::load();
            auto renderMode = [&] (bool dark, const char* fileName) {
                uiprefs::Settings tmp = savedPrefs;
                tmp.darkMode = dark;
                tmp.scalePercent = 100;
                uiprefs::save (tmp);
                std::unique_ptr<juce::AudioProcessorEditor> snapEd (uiProc.createEditor());
                snapEd->setSize (1132, 628);
                auto img = snapEd->createComponentSnapshot (snapEd->getLocalBounds(), true, 2.0f);
                juce::FileOutputStream fos (outDir.getChildFile (fileName));
                if (fos.openedOk()) {
                    fos.setPosition (0);
                    fos.truncate();
                    juce::PNGImageFormat png;
                    png.writeImageToStream (img, fos);
                }
            };
            renderMode (false, "actual-braun-light.png");
            renderMode (true, "actual-braun-dark.png");
            uiprefs::save (savedPrefs);
        }
    }

    std::cout << "MIDI Learn, real-time, Mapping Presets, Braun UI, and Note-Off checks passed\n";
}
