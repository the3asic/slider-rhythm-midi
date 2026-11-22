#include "PluginProcessor.h"
#include "MappingPresets.h"
#include "MidiExporter.h"
#include "PatternPresets.h"
#include "UiPreferences.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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

    // Slot CC maps 0..127 -> minSlot..maxSlot (default TOP=THRU, BOT=BLOCK -> 0..13 for 12 zones).
    processor.armMidiLearn (1, SliderRhythmMidiProcessor::slot);
    sendCC (processor, 21, 64);
    require (processor.snapshot()->bindings[1][SliderRhythmMidiProcessor::slot].cc == 21, "Track 2 did not learn CC 21");
    require (processor.snapshot()->sequence.tracks[0].slot == 1, "Track 1 changed during Track 2 learn");
    sendCC (processor, 21, 127);
    require (processor.snapshot()->sequence.tracks[1].slot == 13, "Track 2 fader did not reach TOP THRU detent (13)");
    sendCC (processor, 21, 0);
    require (processor.snapshot()->sequence.tracks[1].slot == 0, "Track 2 CC 0 did not reach 0 BLOCK floor");
    require (processor.snapshot()->sequence.tracks[0].slot == 1, "Track 1 responded to Track 2 CC");

    // Configurable per-track activeZones + TOP/BOT end-zone modes scale CC throw dynamically.
    processor.setTrackActiveZones (1, 4);
    sendCC (processor, 21, 127);
    require (processor.snapshot()->sequence.tracks[1].slot == 5, "Track 2 with 4 zones + TOP:THRU must map CC 127 to slot 5 (TOP)");
    processor.setTrackTopMode (1, sliderseq::EndZoneMode::off);
    require (processor.snapshot()->sequence.tracks[1].slot == 4, "Disabling TOP detent must clamp slot to activeZones (4)");
    sendCC (processor, 21, 127);
    require (processor.snapshot()->sequence.tracks[1].slot == 4, "Track 2 with 4 zones + TOP:OFF must map CC 127 to slot 4");
    sendCC (processor, 21, 0);
    require (processor.snapshot()->sequence.tracks[1].slot == 0, "Track 2 with 4 zones + BOT:BLOCK must map CC 0 to 0");
    processor.setTrackBottomMode (1, sliderseq::EndZoneMode::off);
    require (processor.snapshot()->sequence.tracks[1].slot == 1, "Disabling BOT detent must clamp slot to 1");
    processor.setTrackTopMode (1, sliderseq::EndZoneMode::thru);
    processor.setTrackBottomMode (1, sliderseq::EndZoneMode::block);
    processor.setTrackActiveZones (1, 12);
    sendCC (processor, 21, 127);
    require (processor.snapshot()->sequence.tracks[1].slot == 13, "Track 2 restored to 12 zones + TOP:THRU must reach 13");

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
    require (processor.snapshot()->sequence.tracks[2].slot == 0, "CC did not move to Track 3 bottom zone");
    require (processor.snapshot()->sequence.tracks[1].slot == 13, "CC still changed Track 2");

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
        require (snap->sequence.unmappedMode == sliderseq::UnmappedRouteMode::block
                 && snap->sequence.tracks[0].topMode == sliderseq::EndZoneMode::off
                 && snap->sequence.tracks[0].bottomMode == sliderseq::EndZoneMode::off,
                 "v2 legacy GEN routing migration failed");
    }

    // APVTS 145 host parameters check (1 global_swing + 16x9 per-track including Macro): exact count, zero-alloc modulation.
    {
        SliderRhythmMidiProcessor apvtsProc;
        require (apvtsProc.getParameters().size() == 145, "Expected 145 exposed host parameters (1 global + 16 tracks x 9)");
        TestPlayHead playHead;
        apvtsProc.setPlayHead (&playHead);
        apvtsProc.prepareToPlay (48000.0, 512);

        auto* globalSwingParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::globalSwingParamId());
        auto* loopParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "loop_steps"));
        auto* slotParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "slot"));
        auto* muteParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "mute"));
        auto* velParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "velocity"));
        auto* macroParam = apvtsProc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "macro"));
        require (globalSwingParam && loopParam && slotParam && muteParam && velParam && macroParam, "Missing expected host parameter IDs");

        globalSwingParam->setValueNotifyingHost (globalSwingParam->convertTo0to1 (34.0f));
        loopParam->setValueNotifyingHost (loopParam->convertTo0to1 (4.0f));
        slotParam->setValueNotifyingHost (slotParam->convertTo0to1 (2.0f));
        velParam->setValueNotifyingHost (velParam->convertTo0to1 (77.0f));
        macroParam->setValueNotifyingHost (macroParam->convertTo0to1 (55.0f));

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
            const float macNorm = macroParam->convertTo0to1 (static_cast<float> (10 * b));
            macroParam->setValue (macNorm);
            macroParam->sendValueChangedMessageToListeners (macNorm);
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
        require (snap->sequence.tracks[0].macroValue == 60, "Host Macro parameter did not sync to snapshot");
        require (snap->sequence.tracks[0].patterns[0].length == 16 && snap->sequence.tracks[0].patterns[1].length == 16,
                 "Non-destructive Loop Steps override must not overwrite stored pattern lengths");

        // Save/restore round trip preserves globalSwing, activeZones, 3-zone routing, macroMode/macroValue, slot, and Loop Steps.
        apvtsProc.setTrackActiveZones (0, 6);
        apvtsProc.setTrackTopMode (0, sliderseq::EndZoneMode::block);
        apvtsProc.setTrackMidMode (0, sliderseq::MidRouteMode::genPlusThru);
        apvtsProc.setTrackBottomMode (0, sliderseq::EndZoneMode::thru);
        apvtsProc.setTrackMacroMode (0, sliderseq::MacroMode::density, false);
        juce::MemoryBlock saved;
        apvtsProc.getStateInformation (saved);
        SliderRhythmMidiProcessor reloaded;
        reloaded.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        const auto rs = reloaded.snapshot();
        require (rs->sequence.globalSwing == 40, "Reloaded state lost Global Swing");
        require (rs->sequence.tracks[0].activeZones == 6, "Reloaded state lost activeZones");
        require (rs->sequence.tracks[0].topMode == sliderseq::EndZoneMode::block, "Reloaded state lost topMode");
        require (rs->sequence.tracks[0].midMode == sliderseq::MidRouteMode::genPlusThru, "Reloaded state lost midMode");
        require (rs->sequence.tracks[0].bottomMode == sliderseq::EndZoneMode::thru, "Reloaded state lost bottomMode");
        require (rs->sequence.tracks[0].macroMode == sliderseq::MacroMode::density, "Reloaded state lost macroMode");
        require (rs->sequence.tracks[0].macroValue == 60, "Reloaded state lost macroValue");
        require (rs->sequence.tracks[0].loopStepsOverride == 4, "Reloaded state lost Loop Steps");
        require (rs->sequence.tracks[0].slot == 2, "Reloaded state lost host Slot");
    }

    // Per-track 3-zone MIDI routing (TOP/MID/0), Ableton Ch 1 smart matching, unmapped note policy, and THRU pre-M/S + zero stuck notes.
    {
        SliderRhythmMidiProcessor routeProc;
        TestPlayHead playHead;
        routeProc.setPlayHead (&playHead);
        routeProc.prepareToPlay (48000.0, 512);

        auto sendNoteAndCollect = [&] (int ch, int nt, bool isOn, bool& sawNoteOn, bool& sawNoteOff, int targetNote = -1) {
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            if (isOn) midi.addEvent (juce::MidiMessage::noteOn (ch, nt, static_cast<juce::uint8> (100)), 10);
            else midi.addEvent (juce::MidiMessage::noteOff (ch, nt), 10);
            playHead.playing = false; // Auditioning / routing works even while stopped
            routeProc.processBlock (audio, midi);
            sawNoteOn = sawNoteOff = false;
            const int checkNt = targetNote >= 0 ? targetNote : nt;
            for (const auto m : midi) {
                const auto msg = m.getMessage();
                if (msg.getNoteNumber() == checkNt) {
                    if (msg.isNoteOn()) sawNoteOn = true;
                    if (msg.isNoteOff()) sawNoteOff = true;
                }
            }
        };

        bool on = false, off = false;
        // Track 0 owns Kick (Note 36, Ch 10). Default: slot=1, TOP=THRU, MID=GEN, BOT=BLOCK, unmapped=THRU.
        // 1) In MID=GEN, incoming Ableton Ch 1 Note 36 must be BLOCKED (replaced by generator).
        sendNoteAndCollect (1, 36, true, on, off);
        require (! on, "MID:GEN must block incoming Kick Note 36 (even arriving on Ableton Ch 1)");
        // 2) Unmapped Note 55 (Crash) passes through when unmappedMode == THRU, blocked when unmappedMode == BLOCK.
        sendNoteAndCollect (1, 55, true, on, off);
        require (on, "Unmapped Note 55 must pass through when unmappedMode == THRU");
        sendNoteAndCollect (1, 55, false, on, off);
        require (off, "Unmapped Note 55 Note Off must pass through");
        {
            SliderRhythmMidiProcessor::StateOp op;
            op.kind = SliderRhythmMidiProcessor::OpKind::setUnmappedMode;
            op.arg0 = static_cast<int> (sliderseq::UnmappedRouteMode::block);
            routeProc.applyUiOp (op);
        }
        sendNoteAndCollect (1, 55, true, on, off);
        require (! on, "Unmapped Note 55 must be blocked when unmappedMode == BLOCK");

        // 3) Push Track 0 to TOP (slot = 13, TOP:THRU): Ableton Ch 1 Note 36 passes through!
        routeProc.setHostParamValue (0, "slot", 13.0f);
        sendNoteAndCollect (1, 36, true, on, off);
        require (on, "TOP:THRU must pass incoming Ch 1 Kick Note 36 through");

        // 4) THRU is BEFORE M/S: muting Track 0 mid-held-note must flush Note Off immediately AND block subsequent Note On!
        routeProc.setHostParamValue (0, "mute", 1.0f);
        {
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            routeProc.processBlock (audio, midi);
            bool flushedOff = false;
            for (const auto m : midi)
                if (m.getMessage().isNoteOff() && m.getMessage().getNoteNumber() == 36 && m.getMessage().getChannel() == 1)
                    flushedOff = true;
            require (flushedOff, "Muting a THRU track mid-note must emit immediate Note Off (zero stuck notes)");
        }
        sendNoteAndCollect (1, 36, true, on, off);
        require (! on, "Muted track must block THRU input (THRU is pre-M/S)");

        // Unmute Track 0, Solo Track 1: Track 0 THRU must be blocked by Track 1 Solo (pre-M/S).
        routeProc.setHostParamValue (0, "mute", 0.0f);
        routeProc.soloTrackExclusive (1);
        sendNoteAndCollect (1, 36, true, on, off);
        require (! on, "Non-soloed track must block THRU input when another track is soloed (THRU is pre-M/S)");
        routeProc.unsoloAllTracks();

        // 5) Pull Track 0 to 0 (BOT:BLOCK) -> blocked; switch BOT to THRU -> passes through!
        routeProc.setHostParamValue (0, "slot", 0.0f);
        sendNoteAndCollect (1, 36, true, on, off);
        require (! on, "0:BLOCK must block incoming Kick Note 36");
        routeProc.setTrackBottomMode (0, sliderseq::EndZoneMode::thru);
        sendNoteAndCollect (1, 36, true, on, off);
        require (on, "0:THRU must pass incoming Kick Note 36");
        sendNoteAndCollect (1, 36, false, on, off);
        require (off, "0:THRU Note Off must pass through");

        // 6) MID:GEN+THRU passes incoming Kick Note 36 in middle zone!
        routeProc.setHostParamValue (0, "slot", 1.0f);
        routeProc.setTrackMidMode (0, sliderseq::MidRouteMode::genPlusThru);
        sendNoteAndCollect (1, 36, true, on, off);
        require (on, "MID:GEN+THRU must pass incoming Kick Note 36");
        sendNoteAndCollect (1, 36, false, on, off);
        require (off, "MID:GEN+THRU Note Off must pass");
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

        // Per-track routing + Note Learn: learned Note 24 consumed while unmapped live Note 60 passes through.
        wfProc.edit ([] (auto& s) {
            s.playbackMode = sliderseq::PlaybackMode::gen;
            s.sequence.unmappedMode = sliderseq::UnmappedRouteMode::thru;
        });
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
            require (sawLiveNote, "Unmapped live note must pass through when unmappedMode == THRU");
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

    // === Pi Astra 6.2 Max Audit Regression Tests ===

    // CRITICAL #1: CC-only edits (without editor open) must survive getStateInformation -> setStateInformation!
    {
        SliderRhythmMidiProcessor ccSaveProc;
        ccSaveProc.prepareToPlay (48000.0, 512);
        ccSaveProc.edit ([] (auto& s) {
            s.bindings[0][SliderRhythmMidiProcessor::velocity] = { 50, 1 };
            s.bindings[0][SliderRhythmMidiProcessor::slot] = { 51, 1 };
            s.bindings[0][SliderRhythmMidiProcessor::macro] = { 52, 1 };
        });
        sendCC (ccSaveProc, 50, 42);  // Velocity -> 42
        sendCC (ccSaveProc, 51, 127); // Slot -> 13 (TOP THRU)
        sendCC (ccSaveProc, 52, 90);  // Macro -> ~71

        juce::MemoryBlock saved;
        ccSaveProc.getStateInformation (saved);

        SliderRhythmMidiProcessor ccReloaded;
        ccReloaded.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        const auto rs = ccReloaded.snapshot();
        require (rs->sequence.tracks[0].velocity == 42, "CRITICAL #1: CC-edited velocity lost on save/reload");
        require (rs->sequence.tracks[0].slot == 13, "CRITICAL #1: CC-edited slot lost on save/reload");
        require (rs->sequence.tracks[0].macroValue == static_cast<int> (std::lround (90.0 * 100.0 / 127.0)),
                 "CRITICAL #1: CC-edited macroValue lost on save/reload");
    }

    // CRITICAL #2: >256 MIDI messages in a single block AND SysEx > 3 bytes must NEVER be dropped!
    {
        SliderRhythmMidiProcessor floodProc;
        floodProc.prepareToPlay (48000.0, 512);
        floodProc.edit ([] (auto& s) { s.playbackMode = sliderseq::PlaybackMode::thru; });

        juce::AudioBuffer<float> audio (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);
        for (int i = 1; i <= 260; ++i)
            midi.addEvent (juce::MidiMessage::controllerEvent (1, 74, i % 128), std::min (500, i));
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 505);
        const uint8_t sysexPayload[8] { 0x7e, 0x7f, 0x06, 0x01, 0x11, 0x22, 0x33, 0x44 };
        midi.addEvent (juce::MidiMessage::createSysExMessage (sysexPayload, 8), 510);

        floodProc.processBlock (audio, midi);
        bool sawNoteOff = false, sawFullSysex = false;
        int ccCount = 0;
        for (const auto m : midi) {
            const auto msg = m.getMessage();
            if (msg.isNoteOff() && msg.getNoteNumber() == 60) sawNoteOff = true;
            if (msg.isController() && msg.getControllerNumber() == 74) ++ccCount;
            if (msg.isSysEx() && msg.getRawDataSize() == 10 && msg.getSysExDataSize() == 8) sawFullSysex = true;
        }
        require (ccCount == 260, "CRITICAL #2: Events past 256 in a single block were truncated");
        require (sawNoteOff, "CRITICAL #2: Note Off #262 was dropped by 256-entry scratch cap");
        require (sawFullSysex, "CRITICAL #2: SysEx message (>3 bytes) was dropped");
    }

    // CRITICAL #3: Bypassed Note On -> un-bypass into MID:GEN -> Note Off must STILL pass through (zero stuck notes),
    // and entering bypass must NOT flush an externally passed-through live note!
    {
        SliderRhythmMidiProcessor bpProc;
        TestPlayHead playHead;
        playHead.playing = false;
        bpProc.setPlayHead (&playHead);
        bpProc.prepareToPlay (48000.0, 512);

        juce::AudioBuffer<float> audio (2, 512);
        juce::MidiBuffer midi;
        // 1) Bypassed: incoming Kick Note On (Ch 1, Note 36) passes through host bypass.
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (100)), 10);
        bpProc.processBlockBypassed (audio, midi);

        // 2) Un-bypass while Track 0 is in MID:GEN (normally blocks Note 36 Note On): matching Note Off MUST pass!
        midi.clear();
        midi.addEvent (juce::MidiMessage::noteOff (1, 36), 20);
        bpProc.processBlock (audio, midi);
        bool sawOff = false;
        for (const auto m : midi)
            if (m.getMessage().isNoteOff() && m.getMessage().getNoteNumber() == 36 && m.getMessage().getChannel() == 1)
                sawOff = true;
        require (sawOff, "CRITICAL #3: Note Off for a note started during bypass was swallowed after un-bypassing");

        // 3) Active TOP:THRU passes Note On -> enter bypass -> live thru note must NOT be killed by bypass flush!
        bpProc.setHostParamValue (0, "slot", 13.0f);
        midi.clear();
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (100)), 5);
        bpProc.processBlock (audio, midi);
        midi.clear();
        bpProc.processBlockBypassed (audio, midi);
        bool killedThru = false;
        for (const auto m : midi)
            if (m.getMessage().isNoteOff() && m.getMessage().getNoteNumber() == 36 && m.getMessage().getChannel() == 1)
                killedThru = true;
        require (! killedThru, "CRITICAL #3: Entering bypass must not kill externally passed-through live notes");
    }

    // CRITICAL #4: Sustain pedal (CC64) release & All Notes Off (CC123) must pass even under OTHER:BLK!
    {
        SliderRhythmMidiProcessor pedProc;
        pedProc.prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> audio (2, 512);
        juce::MidiBuffer midi;

        // Press Sustain pedal while OTHER:THRU
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
        pedProc.processBlock (audio, midi);

        // Switch to OTHER:BLK
        SliderRhythmMidiProcessor::StateOp op;
        op.kind = SliderRhythmMidiProcessor::OpKind::setUnmappedMode;
        op.arg0 = static_cast<int> (sliderseq::UnmappedRouteMode::block);
        pedProc.applyUiOp (op);

        // Release Sustain (CC64=0) + send CC123 (All Notes Off) -> BOTH must pass through!
        midi.clear();
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 10);
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 123, 0), 20);
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 74, 64), 30); // Non-pedal CC still blocked under OTHER:BLK
        pedProc.processBlock (audio, midi);
        bool sawPedalUp = false, sawPanic = false, sawBlockedCc = false;
        for (const auto m : midi) {
            const auto msg = m.getMessage();
            if (msg.isController() && msg.getControllerNumber() == 64 && msg.getControllerValue() == 0) sawPedalUp = true;
            if (msg.isController() && msg.getControllerNumber() == 123) sawPanic = true;
            if (msg.isController() && msg.getControllerNumber() == 74) sawBlockedCc = true;
        }
        require (sawPedalUp, "CRITICAL #4: Sustain pedal release (CC64=0) was swallowed under OTHER:BLK");
        require (sawPanic, "CRITICAL #4: All Notes Off (CC123) was swallowed under OTHER:BLK");
        require (! sawBlockedCc, "Non-pedal unmapped CC74 should remain blocked under OTHER:BLK");
    }

    // HIGH #1, #2, #6, #7, #15 & 64-bit Double Precision Processing:
    // - 125 BPM / 48 kHz / 512-sample contiguous blocks must NOT double-trigger Kick at tick 576
    // - Loop boundary wrap at exact block end must not skip beat 1
    // - Invalid BPM (0 / NaN) must not crash or leak notes
    // - InputChannelMatch: autoLiveCh1 vs strict vs omni
    {
        SliderRhythmMidiProcessor edgeProc;
        require (edgeProc.supportsDoublePrecisionProcessing(), "Processor must support 64-bit double precision");
        TestPlayHead playHead;
        playHead.bpm = 125.0;
        playHead.playing = true;
        edgeProc.setPlayHead (&playHead);
        edgeProc.prepareToPlay (48000.0, 512);
        edgeProc.edit ([] (auto& s) {
            s.sequence.trackCount = 1;
            s.sequence.tracks[0].slot = 1; // Four on floor: 0, 288, 576, 864
        });

        int kickOns = 0;
        const double ppqPerBlock = 512.0 * 125.0 / (60.0 * 48000.0);
        juce::AudioBuffer<double> audioD (2, 512);
        juce::MidiBuffer midi;
        for (int b = 0; b < 180; ++b) { // > 4 beats at 125 BPM
            playHead.ppq = b * ppqPerBlock;
            midi.clear();
            edgeProc.processBlock (audioD, midi);
            for (const auto m : midi)
                if (m.getMessage().isNoteOn() && m.getMessage().getNoteNumber() == 36)
                    ++kickOns;
            if (playHead.ppq + ppqPerBlock >= 4.0) break;
        }
        require (kickOns == 4, "HIGH #6: 125 BPM / 48 kHz / 512-sample blocks must emit exactly 4 kicks per bar (no double trigger)");

        // Invalid BPM = NaN must not crash and must flush any active note cleanly.
        playHead.bpm = std::numeric_limits<double>::quiet_NaN();
        midi.clear();
        edgeProc.processBlock (audioD, midi);

        // InputChannelMatch strict vs autoLiveCh1:
        SliderRhythmMidiProcessor chProc;
        TestPlayHead chPh;
        chPh.playing = false;
        chProc.setPlayHead (&chPh);
        chProc.prepareToPlay (48000.0, 512);
        // Track 0 is Ch 10 Note 36, MID:GEN. Under default autoLiveCh1: Ch 1 Note 36 is blocked, Ch 2 Note 36 passes (unmapped=THRU).
        juce::AudioBuffer<float> audioF (2, 512);
        midi.clear();
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (100)), 0);
        midi.addEvent (juce::MidiMessage::noteOn (2, 36, static_cast<juce::uint8> (100)), 10);
        chProc.processBlock (audioF, midi);
        bool sawCh1 = false, sawCh2 = false;
        for (const auto m : midi) {
            if (m.getMessage().isNoteOn() && m.getMessage().getNoteNumber() == 36) {
                if (m.getMessage().getChannel() == 1) sawCh1 = true;
                if (m.getMessage().getChannel() == 2) sawCh2 = true;
            }
        }
        require (! sawCh1 && sawCh2, "HIGH #15: autoLiveCh1 must match Ch 1 to Track 0 (Ch 10) while leaving Ch 2 unmapped");

        // Under strict: Ch 1 Note 36 must NOT match Track 0 (Ch 10) and passes through as unmapped!
        chProc.setInputChannelMatch (sliderseq::InputChannelMatch::strict);
        midi.clear();
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (100)), 0);
        chProc.processBlock (audioF, midi);
        sawCh1 = false;
        for (const auto m : midi)
            if (m.getMessage().isNoteOn() && m.getMessage().getChannel() == 1 && m.getMessage().getNoteNumber() == 36)
                sawCh1 = true;
        require (sawCh1, "HIGH #15: strict channel match must not route Ch 1 Note 36 to Ch 10 Track 0");
    }

    // HIGH #14 & MEDIUM #10: Dense 16-track 64th 3x 8-bar MIDI export must NOT truncate at 16,384 events,
    // and sparse export must emit explicit End-of-Track at exact requested bar length.
    {
        auto dense = sliderseq::makeDefaultState();
        dense.trackCount = 16;
        for (int t = 0; t < 16; ++t) {
            auto& tr = dense.tracks[static_cast<size_t> (t)];
            tr.slot = 1;
            tr.rate = 3.0;
            tr.chokeMask = 0;
            tr.midiNote = 36 + t;
            tr.patterns[0].grid = tr.patterns[0].length = tr.patterns[0].subdivision = 64;
            tr.patterns[0].steps.fill (true);
        }
        const auto denseFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("slider-dense-export-" + juce::Uuid().toString() + ".mid");
        require (midiexporter::writeMidiFile (dense, denseFile, 8, 120.0, 4, 4), "Dense 8-bar export failed");
        juce::FileInputStream inDense (denseFile);
        juce::MidiFile parsedDense;
        require (inDense.openedOk() && parsedDense.readFrom (inDense), "Failed to parse dense MIDI file");
        int denseOns = 0;
        for (int i = 0; i < parsedDense.getTrack (0)->getNumEvents(); ++i)
            if (parsedDense.getTrack (0)->getEventPointer (i)->message.isNoteOn()) ++denseOns;
        denseFile.deleteFile();
        require (denseOns == 24576, "HIGH #14: Dense 16-track 64th 3x 8-bar export must emit all 24,576 onsets (no truncation)");

        // Sparse 4-bar 3/4 export: explicit End-of-Track at 4 * 3 * 288 = 3456 ticks.
        auto sparse = sliderseq::makeDefaultState();
        sparse.trackCount = 1;
        sparse.tracks[0].patterns[0] = sliderseq::makePattern ("One", 16, { 0 });
        const auto sparseFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("slider-sparse-34-" + juce::Uuid().toString() + ".mid");
        require (midiexporter::writeMidiFile (sparse, sparseFile, 4, 120.0, 3, 4), "Sparse 3/4 export failed");
        juce::FileInputStream inSparse (sparseFile);
        juce::MidiFile parsedSparse;
        require (inSparse.openedOk() && parsedSparse.readFrom (inSparse), "Failed to parse sparse MIDI file");
        const double lastTs = parsedSparse.getTrack (0)->getEndTime();
        sparseFile.deleteFile();
        require (std::abs (lastTs - 3456.0) < 1.0, "MEDIUM #10: Exported 4-bar 3/4 MIDI clip must preserve exact 3456-tick End-of-Track");
    }

    // MEDIUM #4: Note Learn rejects non-note controls (e.g. macro) and HLD Note Off releases Mute even after disabling Note Learn!
    {
        SliderRhythmMidiProcessor nlProc;
        nlProc.prepareToPlay (48000.0, 512);
        nlProc.setNoteLearnEnabled (true);
        nlProc.armMidiLearn (0, SliderRhythmMidiProcessor::macro);
        {
            juce::AudioBuffer<float> audio (0, 256);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 48, static_cast<juce::uint8> (100)), 0);
            nlProc.processBlock (audio, midi);
        }
        require (nlProc.getLearnTarget() >= 0, "MEDIUM #4: Macro must NOT bind to Note On during Note Learn");

        // Bind Mute in HLD mode to Note 25, hold it down, turn NOTE learn OFF, then release Note 25 -> must unmute!
        nlProc.setLearnTriggerMode (SliderRhythmMidiProcessor::TriggerMode::momentary);
        nlProc.armMidiLearn (0, SliderRhythmMidiProcessor::mute);
        {
            juce::AudioBuffer<float> audio (0, 256);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 25, static_cast<juce::uint8> (100)), 0);
            nlProc.processBlock (audio, midi); // Learn
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOn (1, 25, static_cast<juce::uint8> (100)), 0);
            nlProc.processBlock (audio, midi); // Hold -> Mute ON
            require (nlProc.snapshot()->sequence.tracks[0].muted, "HLD Note On should mute Track 0");

            nlProc.setNoteLearnEnabled (false);
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOff (1, 25), 0);
            nlProc.processBlock (audio, midi); // Release -> must unmute even though noteLearnEnabled is now false!
            require (! nlProc.snapshot()->sequence.tracks[0].muted, "MEDIUM #4: HLD Note Off must release Mute even after Note Learn toggle is turned OFF");
        }
    }

    // Editor creation/destruction smoke test (Braun RootCanvas, scopes, fader, and SharedResourcePointer teardown).
    {
        SliderRhythmMidiProcessor uiProc;
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (uiProc.createEditor());
            require (ed != nullptr, "Failed to create editor");
            ed->setSize (1140, 680);
        }

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
                const std::array<sliderseq::EndZoneMode, 6> tops {
                    sliderseq::EndZoneMode::thru, sliderseq::EndZoneMode::thru, sliderseq::EndZoneMode::off,
                    sliderseq::EndZoneMode::thru, sliderseq::EndZoneMode::thru, sliderseq::EndZoneMode::block
                };
                const std::array<sliderseq::MidRouteMode, 6> mids {
                    sliderseq::MidRouteMode::gen, sliderseq::MidRouteMode::genPlusThru, sliderseq::MidRouteMode::gen,
                    sliderseq::MidRouteMode::gen, sliderseq::MidRouteMode::gen, sliderseq::MidRouteMode::gen
                };
                const std::array<sliderseq::EndZoneMode, 6> bots {
                    sliderseq::EndZoneMode::block, sliderseq::EndZoneMode::block, sliderseq::EndZoneMode::thru,
                    sliderseq::EndZoneMode::block, sliderseq::EndZoneMode::block, sliderseq::EndZoneMode::off
                };
                const std::array<int, 6> slots { 5, 6, 3, 13, 0, 6 }; // T4 = TOP THRU (13), T5 = 0 BLOCK (0)
                const std::array<int, 6> swings { 100, 75, 0, 100, 50, 85 };
                const std::array<sliderseq::MacroMode, 6> macroModes {
                    sliderseq::MacroMode::build, sliderseq::MacroMode::density, sliderseq::MacroMode::density,
                    sliderseq::MacroMode::roll, sliderseq::MacroMode::human, sliderseq::MacroMode::human
                };
                const std::array<int, 6> macroVals { 65, 84, 20, 52, 85, 18 };
                const std::array<sliderseq::ModShape, 6> shapes {
                    sliderseq::ModShape::rampUp, sliderseq::ModShape::cos8, sliderseq::ModShape::rampDown,
                    sliderseq::ModShape::random, sliderseq::ModShape::cos16, sliderseq::ModShape::rampUp
                };
                for (int i = 0; i < 6; ++i) {
                    auto& t = s.sequence.tracks[static_cast<size_t> (i)];
                    t.name = names[static_cast<size_t> (i)];
                    t.activeZones = zones[static_cast<size_t> (i)];
                    t.topMode = tops[static_cast<size_t> (i)];
                    t.midMode = mids[static_cast<size_t> (i)];
                    t.bottomMode = bots[static_cast<size_t> (i)];
                    t.slot = slots[static_cast<size_t> (i)];
                    t.groove = swings[static_cast<size_t> (i)];
                    t.macroMode = macroModes[static_cast<size_t> (i)];
                    t.macroValue = macroVals[static_cast<size_t> (i)];
                    t.modShape = shapes[static_cast<size_t> (i)];
                    t.modAmount = 70;
                    t.velocity = 110;
                    if (i >= 4)
                        t.patterns[5] = sliderseq::makePattern ("Perc 1/8", 16, { 0, 3, 6, 10, 14 });
                }
            });
            const auto savedPrefs = uiprefs::load();
            auto renderMode = [&] (bool dark, const char* fileName) {
                uiprefs::Settings tmp = savedPrefs;
                tmp.darkMode = dark;
                tmp.scalePercent = 100;
                uiprefs::save (tmp);
                std::unique_ptr<juce::AudioProcessorEditor> snapEd (uiProc.createEditor());
                snapEd->setSize (1132, 680);
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
