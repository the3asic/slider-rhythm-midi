#include "PluginProcessor.h"
#include "MappingPresets.h"
#include "MidiExporter.h"
#include "PatternPresets.h"
#include "UiPreferences.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <vector>

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

struct ScopedTestPrefsEnv final {
    juce::File tempFile;
    ScopedTestPrefsEnv()
        : tempFile (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("slider-rhythm-test-uiprefs-" + juce::Uuid().toString() + ".xml")) {
        const auto p = tempFile.getFullPathName().toStdString();
#if JUCE_WINDOWS
        _putenv_s ("SLIDER_UI_PREFS_PATH", p.c_str());
#else
        setenv ("SLIDER_UI_PREFS_PATH", p.c_str(), 1);
#endif
    }
    ~ScopedTestPrefsEnv() {
#if JUCE_WINDOWS
        _putenv_s ("SLIDER_UI_PREFS_PATH", "");
#else
        unsetenv ("SLIDER_UI_PREFS_PATH");
#endif
        tempFile.deleteFile();
    }
};

static void runAuditV2RegressionTests();

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    ScopedTestPrefsEnv isolatedPrefs;
    auto processorPtr = std::make_unique<SliderRhythmMidiProcessor>();
    auto& processor = *processorPtr;
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

    // Slot CC maps 0..127 -> minSlot..maxSlot (default TOP=THRU, BOT=BLOCK -> 0..13 for 12 zones, default slot=4).
    processor.armMidiLearn (1, SliderRhythmMidiProcessor::slot);
    sendCC (processor, 21, 64);
    require (processor.snapshot()->bindings[1][SliderRhythmMidiProcessor::slot].cc == 21, "Track 2 did not learn CC 21");
    require (processor.snapshot()->sequence.tracks[0].slot == sliderseq::defaultTrackSlot, "Track 1 changed during Track 2 learn");
    sendCC (processor, 21, 127);
    require (processor.snapshot()->sequence.tracks[1].slot == 13, "Track 2 fader did not reach TOP THRU detent (13)");
    sendCC (processor, 21, 0);
    require (processor.snapshot()->sequence.tracks[1].slot == 0, "Track 2 CC 0 did not reach 0 BLOCK floor");
    require (processor.snapshot()->sequence.tracks[0].slot == sliderseq::defaultTrackSlot, "Track 1 responded to Track 2 CC");

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
    auto reopenedPtr = std::make_unique<SliderRhythmMidiProcessor>();
    auto& reopened = *reopenedPtr;
    reopened.setStateInformation (projectState.getData(), static_cast<int> (projectState.getSize()));
    const auto restored = reopened.snapshot()->sequence.tracks[1].patterns[4];
    require (restored.name == beat.name && restored.grid == beat.grid
        && restored.length == beat.length && restored.subdivision == 16 && restored.steps == beat.steps,
        "Project state lost the loaded slot");

    // Real-time zero-allocation test under heavy load (16 tracks, 64th notes, rate=3, CC flood, 4096-sample blocks).
    {
        auto rtProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& rtProc = *rtProcPtr;
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
        auto noteProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& noteProc = *noteProcPtr;
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
        auto migratedPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& migrated = *migratedPtr;
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
        auto apvtsProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& apvtsProc = *apvtsProcPtr;
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
        auto reloadedPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& reloaded = *reloadedPtr;
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
        auto routeProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& routeProc = *routeProcPtr;
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
        auto mpProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& mpProc = *mpProcPtr;
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
        auto wfProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& wfProc = *wfProcPtr;
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
        auto ccSaveProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& ccSaveProc = *ccSaveProcPtr;
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

        auto ccReloadedPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& ccReloaded = *ccReloadedPtr;
        ccReloaded.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        const auto rs = ccReloaded.snapshot();
        require (rs->sequence.tracks[0].velocity == 42, "CRITICAL #1: CC-edited velocity lost on save/reload");
        require (rs->sequence.tracks[0].slot == 13, "CRITICAL #1: CC-edited slot lost on save/reload");
        require (rs->sequence.tracks[0].macroValue == static_cast<int> (std::lround (90.0 * 100.0 / 127.0)),
                 "CRITICAL #1: CC-edited macroValue lost on save/reload");
    }

    // CRITICAL #2: >256 MIDI messages in a single block AND SysEx > 3 bytes must NEVER be dropped!
    {
        auto floodProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& floodProc = *floodProcPtr;
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
        auto bpProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& bpProc = *bpProcPtr;
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
        auto pedProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& pedProc = *pedProcPtr;
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
        auto edgeProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& edgeProc = *edgeProcPtr;
        require (edgeProc.supportsDoublePrecisionProcessing(), "Processor must support 64-bit double precision");
        TestPlayHead playHead;
        playHead.bpm = 125.0;
        playHead.playing = true;
        edgeProc.setPlayHead (&playHead);
        edgeProc.prepareToPlay (48000.0, 512);
        edgeProc.edit ([] (auto& s) {
            s.sequence.trackCount = 1;
            s.sequence.tracks[0].slot = sliderseq::defaultTrackSlot; // Four on floor: 0, 288, 576, 864
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
        auto chProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& chProc = *chProcPtr;
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
        auto nlProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& nlProc = *nlProcPtr;
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

            // L3: HLD Note Off arriving while in ALL THRU must still release Mute cleanly.
            nlProc.setNoteLearnEnabled (true);
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOn (1, 25, static_cast<juce::uint8> (100)), 0);
            nlProc.processBlock (audio, midi);
            require (nlProc.snapshot()->sequence.tracks[0].muted, "HLD Note On should re-mute Track 0");

            nlProc.edit ([] (auto& s) { s.playbackMode = sliderseq::PlaybackMode::thru; });
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOff (1, 25), 0);
            nlProc.processBlock (audio, midi);
            nlProc.edit ([] (auto& s) { s.playbackMode = sliderseq::PlaybackMode::gen; });
            require (! nlProc.snapshot()->sequence.tracks[0].muted, "L3: HLD Note Off during ALL THRU must release Mute");

            // L4: MIDI Learn must refuse sustain/sostenuto/panic CCs (64, 66, 120, 121, 123).
            nlProc.armMidiLearn (0, SliderRhythmMidiProcessor::velocity);
            midi.clear();
            midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
            midi.addEvent (juce::MidiMessage::controllerEvent (1, 123, 0), 1);
            nlProc.processBlock (audio, midi);
            require (nlProc.getLearnTarget() >= 0, "L4: MIDI Learn must ignore sustain/panic CCs");
            nlProc.cancelMidiLearn();
        }
    }

    // C1: DAW Loop playback must continue firing on every loop pass (8 passes of 4-beat loop = 32 Kick hits) and after Bypass.
    {
        auto loopProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& loopProc = *loopProcPtr;
        TestPlayHead ph;
        ph.bpm = 120.0;
        ph.looping = true;
        ph.loopStartPpq = 0.0;
        ph.loopEndPpq = 4.0;
        loopProc.setPlayHead (&ph);
        loopProc.prepareToPlay (48000.0, 512);
        loopProc.edit ([] (auto& s) { s.sequence.trackCount = 1; s.sequence.tracks[0].slot = sliderseq::defaultTrackSlot; });

        const double ppqPerBlock = 512.0 * ph.bpm / (60.0 * 48000.0);
        int kickOns = 0;
        double ppq = 0.0;
        const int blocksFor8Bars = static_cast<int> (std::ceil (32.0 / ppqPerBlock));
        juce::AudioBuffer<float> audio (0, 512);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocksFor8Bars; ++b) {
            ph.ppq = ppq;
            midi.clear();
            loopProc.processBlock (audio, midi);
            for (const auto meta : midi)
                if (meta.getMessage().isNoteOn() && meta.getMessage().getNoteNumber() == 36) ++kickOns;
            ppq += ppqPerBlock;
            while (ppq >= ph.loopEndPpq) ppq -= (ph.loopEndPpq - ph.loopStartPpq);
        }
        const auto loopMsg = "C1: 8 passes of 4-beat loop should emit exactly 32 Kick notes, got " + std::to_string (kickOns);
        require (kickOns == 32, loopMsg.c_str());

        // Bypass for one full 4-beat loop pass, then return to ACTIVE and verify Kick resumes immediately.
        const int blocksFor1Bar = static_cast<int> (std::ceil (4.0 / ppqPerBlock));
        for (int b = 0; b < blocksFor1Bar; ++b) {
            ph.ppq = ppq;
            midi.clear();
            loopProc.processBlockBypassed (audio, midi);
            ppq += ppqPerBlock;
            while (ppq >= ph.loopEndPpq) ppq -= (ph.loopEndPpq - ph.loopStartPpq);
        }
        int resumedKickOns = 0;
        for (int b = 0; b < blocksFor1Bar * 2; ++b) {
            ph.ppq = ppq;
            midi.clear();
            loopProc.processBlock (audio, midi);
            for (const auto meta : midi)
                if (meta.getMessage().isNoteOn() && meta.getMessage().getNoteNumber() == 36) ++resumedKickOns;
            ppq += ppqPerBlock;
            while (ppq >= ph.loopEndPpq) ppq -= (ph.loopEndPpq - ph.loopStartPpq);
        }
        require (resumedKickOns >= 7 && resumedKickOns <= 9, "C1: Loop playback must resume emitting notes immediately after Bypass");
    }

    // M1: Audio thread preempting during edit() / setStateInformation() APVTS sync must NOT roll back edited parameters.
    {
        struct PreemptListener final : public juce::AudioProcessorListener {
            SliderRhythmMidiProcessor& proc;
            TestPlayHead& ph;
            bool armed = false;
            bool fired = false;
            int observedVel = -1;
            PreemptListener (SliderRhythmMidiProcessor& p, TestPlayHead& head) : proc (p), ph (head) {}
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {
                if (! armed || fired) return;
                fired = true;
                juce::AudioBuffer<float> a (0, 256);
                juce::MidiBuffer m;
                ph.ppq = 0.0;
                proc.processBlock (a, m);
                for (const auto meta : m)
                    if (meta.getMessage().isNoteOn() && meta.getMessage().getNoteNumber() == 36) {
                        observedVel = meta.getMessage().getVelocity();
                        break;
                    }
            }
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        };

        auto raceProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& raceProc = *raceProcPtr;
        TestPlayHead ph;
        ph.bpm = 120.0;
        raceProc.setPlayHead (&ph);
        raceProc.prepareToPlay (48000.0, 256);
        raceProc.edit ([] (auto& s) {
            s.sequence.trackCount = 1;
            s.sequence.tracks[0].slot = sliderseq::defaultTrackSlot;
            s.sequence.tracks[0].modAmount = 0;
            s.sequence.tracks[0].velocity = 100;
        });
        PreemptListener listener (raceProc, ph);
        raceProc.addListener (&listener);
        listener.armed = true;
        raceProc.edit ([] (auto& s) { s.sequence.tracks[0].velocity = 40; });
        raceProc.removeListener (&listener);
        require (listener.fired && listener.observedVel == 40, "M1: Audio block during edit() APVTS sync rolled back velocity to stale value");
    }

    // M2: Heavy 16-track 64th 3x density across 4096-sample blocks with choke must match unbounded renderOnsetsVector exactly.
    {
        auto denseProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& denseProc = *denseProcPtr;
        TestPlayHead ph;
        ph.bpm = 120.0;
        denseProc.setPlayHead (&ph);
        denseProc.prepareToPlay (48000.0, 4096);
        denseProc.edit ([] (auto& s) {
            s.sequence.trackCount = sliderseq::maxTracks;
            for (int i = 0; i < sliderseq::maxTracks; ++i) {
                auto& t = s.sequence.tracks[static_cast<size_t> (i)];
                t.slot = 1;
                t.rate = 3.0;
                t.midiNote = 36 + i;
                t.midiChannel = 10;
                t.chokeMask = (i == 3) ? static_cast<uint16_t> (1u << 2) : 0u;
                auto& pat = t.patterns[0];
                pat.grid = pat.subdivision = pat.length = 64;
                pat.steps.fill (true);
            }
        });
        const auto snap = denseProc.snapshot();
        const auto renderSt = sliderseq::toRenderState (snap->sequence, sliderseq::PlaybackMode::gen);
        const double ppqPerBlock = 4096.0 * ph.bpm / (60.0 * 48000.0);
        const int blocks = 4;
        const double endTick = blocks * ppqPerBlock * sliderseq::ticksPerQuarter;
        const auto expected = sliderseq::renderOnsetsVector (renderSt, 0.0, endTick);

        int rtNoteOns = 0;
        juce::AudioBuffer<float> audio (0, 4096);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b) {
            ph.ppq = b * ppqPerBlock;
            midi.clear();
            denseProc.processBlock (audio, midi);
            for (const auto meta : midi)
                if (meta.getMessage().isNoteOn()) ++rtNoteOns;
        }
        require (static_cast<size_t> (rtNoteOns) == expected.size(),
                 "M2: Dense 16-track 64th 3x 4096-sample blocks truncated onsets vs renderOnsetsVector");
    }

    // M3: Switching Macro mode to direct -> VEL / -> RATE must preserve track velocity and rate instead of resetting to vel=1 / rate=1/3.
    {
        auto macroDirectProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& macroDirectProc = *macroDirectProcPtr;
        macroDirectProc.setTrackMacroMode (0, sliderseq::MacroMode::velocity, true);
        {
            const auto s = macroDirectProc.snapshot();
            const auto eff = sliderseq::computeEffectiveTrackForPattern (s->sequence.tracks[0], s->sequence.tracks[0].patterns[0]);
            require (std::abs (eff.velocity - 100) <= 1, "M3: Switching to -> VEL must preserve current track velocity");
        }
        macroDirectProc.setTrackMacroMode (0, sliderseq::MacroMode::rate, true);
        {
            const auto s = macroDirectProc.snapshot();
            const auto eff = sliderseq::computeEffectiveTrackForPattern (s->sequence.tracks[0], s->sequence.tracks[0].patterns[0]);
            require (std::abs (eff.rate - 1.0) < 1e-6, "M3: Switching to -> RATE must preserve current track rate");
        }
    }

    // Editor creation/destruction smoke test, H1 fader tick-to-click alignment, and Debug paint zero-jassert check.
    {
        auto uiProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
        auto& uiProc = *uiProcPtr;
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (uiProc.createEditor());
            require (ed != nullptr, "Failed to create editor");
            ed->setSize (1140, 680);

            // GLM L-1: Render full dark-mode snapshot to verify zero jasserts in Debug builds.
            const auto smokeImg = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            require (smokeImg.isValid() && smokeImg.getWidth() == 1140, "Editor snapshot render failed");

            // H1: Locate Track 1 vertical 3-zone fader and verify clicking every single detent yForVal(z) selects exact slot z (0..13).
            juce::Slider* track0Fader = nullptr;
            std::function<void (juce::Component&)> findFader = [&] (juce::Component& c) {
                if (track0Fader != nullptr) return;
                if (auto* sl = dynamic_cast<juce::Slider*> (&c)) {
                    if (sl->getSliderStyle() == juce::Slider::LinearVertical) { track0Fader = sl; return; }
                }
                for (int i = 0; i < c.getNumChildComponents(); ++i)
                    findFader (*c.getChildComponent (i));
            };
            findFader (*ed);
            require (track0Fader != nullptr, "H1: Could not locate Track 0 vertical fader");

            // Adaptive vertical height verification: resizing taller must expand TrackStrip and fader height with zero dead bottom void.
            ed->setSize (1120, 782);
            const int faderHNormal = track0Fader->getHeight();
            const int stripHNormal = track0Fader->getParentComponent()->getHeight();
            ed->setSize (1120, 1040);
            const int faderHTall = track0Fader->getHeight();
            const int stripHTall = track0Fader->getParentComponent()->getHeight();
            require (stripHTall >= stripHNormal + 240, "Adaptive height: TrackStrip failed to expand vertically with window");
            require (faderHTall >= faderHNormal + 140, "Adaptive height: Fader bay failed to expand vertically with window");
            ed->setSize (1120, 782);

            const auto layout = track0Fader->getLookAndFeel().getSliderLayout (*track0Fader);
            const auto sb = layout.sliderBounds.toFloat();
            for (int z = 0; z <= 13; ++z) {
                const float prop = static_cast<float> (z) / 13.0f;
                const float clickY = sb.getBottom() - sb.getHeight() * prop;
                const float clickX = sb.getCentreX();
                const auto fakePos = juce::Point<float> (clickX, clickY);
                const auto src = juce::Desktop::getInstance().getMainMouseSource();
                juce::MouseEvent down (src, fakePos, juce::ModifierKeys::leftButtonModifier,
                                       0.0f, 0.0f, 0.0f, 0.0f, 0.0f, track0Fader, track0Fader,
                                       juce::Time::getCurrentTime(), fakePos, juce::Time::getCurrentTime(), 1, false);
                track0Fader->mouseDown (down);
                track0Fader->mouseUp (down);
                const int got = uiProc.snapshot()->sequence.tracks[0].slot;
                const auto msg = "H1: Clicking fader tick " + std::to_string (z) + " landed on slot " + std::to_string (got);
                require (got == z, msg.c_str());
            }
        }

        if (const char* dumpDir = std::getenv ("SLIDER_DUMP_UI_PNG_DIR"); dumpDir != nullptr && dumpDir[0] != '\0') {
            const auto outDir = juce::File (dumpDir);
            outDir.createDirectory();
            auto renderSnap = [&] (SliderRhythmMidiProcessor& p, bool dark, int w, int h, const char* fileName) {
                uiprefs::Settings tmp {};
                tmp.darkMode = dark;
                tmp.scalePercent = 100;
                uiprefs::save (tmp);
                std::unique_ptr<juce::AudioProcessorEditor> snapEd (p.createEditor());
                snapEd->setSize (w, h);
                auto img = snapEd->createComponentSnapshot (snapEd->getLocalBounds(), true, 2.0f);
                juce::FileOutputStream fos (outDir.getChildFile (fileName));
                if (fos.openedOk()) {
                    fos.setPosition (0);
                    fos.truncate();
                    juce::PNGImageFormat png;
                    png.writeImageToStream (img, fos);
                }
            };
            // Default fresh 4-track kit (Dark default, tall adaptive Dark, Light mode)
            auto freshProcPtr = std::make_unique<SliderRhythmMidiProcessor>();
            freshProcPtr->setGlobalSwingValue (26);
            renderSnap (*freshProcPtr, true, 1120, 782, "v2-default-dark-4trk.png");
            renderSnap (*freshProcPtr, true, 1120, 1020, "v2-tall-dark-4trk.png");
            renderSnap (*freshProcPtr, false, 1120, 782, "v2-default-light-4trk.png");
        }
    }

    runAuditV2RegressionTests();

    std::cout << "MIDI Learn, real-time, Mapping Presets, Braun UI, and Note-Off checks passed\n";
}

static void runAuditV2RegressionTests() {
    // H1: Same-pitch live Note On + generated Note On in MID:GEN+THRU stay strictly chronological and balanced.
    {
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        TestPlayHead ph;
        ph.bpm = 120.0;
        p->setPlayHead (&ph);
        p->prepareToPlay (48000.0, 256);
        p->edit ([] (auto& s) {
            s.sequence.trackCount = 1;
            s.sequence.tracks[0].slot = sliderseq::defaultTrackSlot; // Kick beat 1 at sample 0
            s.sequence.tracks[0].midMode = sliderseq::MidRouteMode::genPlusThru;
            s.sequence.tracks[0].gateTicks = 144.0; // long gate (> 80 samples)
        });
        std::map<std::pair<int, int>, int> active;
        juce::AudioBuffer<float> audio (0, 256);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (10, 36, static_cast<juce::uint8> (110)), 80);
        ph.ppq = 0.0;
        p->processBlock (audio, midi);
        trackMidiBalance (midi, active);

        std::vector<std::tuple<int, bool>> seq;
        for (const auto m : midi)
            if (m.getMessage().isNoteOnOrOff() && m.getMessage().getNoteNumber() == 36)
                seq.emplace_back (m.samplePosition, m.getMessage().isNoteOn());
        require (seq.size() == 3
                 && std::get<0> (seq[0]) == 0 && std::get<1> (seq[0]) == true
                 && std::get<0> (seq[1]) == 80 && std::get<1> (seq[1]) == false
                 && std::get<0> (seq[2]) == 80 && std::get<1> (seq[2]) == true,
                 "H1: Expected chronological On@0 -> Off@80 -> On@80 for same-pitch generated + live Note On");

        midi.clear();
        midi.addEvent (juce::MidiMessage::noteOff (10, 36), 10);
        ph.ppq = 256.0 * 120.0 / (60.0 * 48000.0);
        p->processBlock (audio, midi);
        trackMidiBalance (midi, active);
        require (active[{ 10, 36 }] == 0, "H1: Live Note Off in following block was dropped");
    }

    // H2: Multi-wrap short loops have zero cumulative rounding drift, exact gate duration, and bounded sub-sample cost.
    {
        // (a) 10.5-sample loop over 128 samples -> exact onset samples {0, 10, 21, 31, 42, 52, ...}
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        TestPlayHead ph;
        ph.bpm = 120.0;
        ph.looping = true;
        const double ppqPerSample = ph.bpm / (60.0 * 48000.0);
        ph.loopStartPpq = 0.0;
        ph.loopEndPpq = 10.5 * ppqPerSample;
        p->setPlayHead (&ph);
        p->prepareToPlay (48000.0, 128);
        p->edit ([] (auto& s) {
            s.sequence.trackCount = 1;
            s.sequence.tracks[0].slot = 1;
            s.sequence.tracks[0].patterns[0] = sliderseq::makePattern ("One", 16, { 0 });
        });
        juce::AudioBuffer<float> audio (0, 128);
        juce::MidiBuffer midi;
        ph.ppq = 0.0;
        p->processBlock (audio, midi);
        std::vector<int> onSamples;
        for (const auto m : midi)
            if (m.getMessage().isNoteOn() && m.getMessage().getNoteNumber() == 36)
                onSamples.push_back (m.samplePosition);
        require (onSamples.size() >= 6
                 && onSamples[0] == 0 && onSamples[1] == 10 && onSamples[2] == 21
                 && onSamples[3] == 31 && onSamples[4] == 42 && onSamples[5] == 52,
                 "H2: Multi-wrap 10.5-sample loop accumulated rounding drift");

        // (b) Nominal 2,000-sample gate across 1-sample blocks with 2.5-sample loop -> Note Off at sample 2,000 (not 1,668!)
        auto pGate = std::make_unique<SliderRhythmMidiProcessor>();
        TestPlayHead phGate;
        phGate.bpm = 120.0;
        phGate.looping = true;
        const double ticksPerSample = phGate.bpm * sliderseq::ticksPerQuarter / (60.0 * 48000.0);
        phGate.loopStartPpq = 0.0;
        phGate.loopEndPpq = 2.5 * ppqPerSample;
        pGate->setPlayHead (&phGate);
        pGate->prepareToPlay (48000.0, 1);
        pGate->edit ([ticksPerSample] (auto& s) {
            s.sequence.trackCount = 1;
            s.sequence.tracks[0].slot = 1;
            s.sequence.tracks[0].gateTicks = 2000.0 * ticksPerSample; // ~24 ticks = exactly 2,000 samples at 120 BPM / 48 kHz
            s.sequence.tracks[0].patterns[0] = sliderseq::makePattern ("One", 16, { 0 });
        });
        // Trigger Note On at sample 0, then clear step 0 (without seeking) so the 2,000-sample gate rings undisturbed
        // across ~800 wraps of the 2.5-sample loop in 1-sample blocks.
        juce::AudioBuffer<float> a1 (0, 1);
        juce::MidiBuffer m1;
        phGate.ppq = 0.0;
        pGate->processBlock (a1, m1);
        bool sawInitialOn = false;
        for (const auto m : m1)
            if (m.getMessage().isNoteOn()) sawInitialOn = true;
        require (sawInitialOn, "H2: Expected initial Note On at sample 0");

        SliderRhythmMidiProcessor::StateOp clearStep;
        clearStep.kind = SliderRhythmMidiProcessor::OpKind::setStep;
        clearStep.track = 0;
        clearStep.slotIndex = 0;
        clearStep.arg0 = 0;
        clearStep.barg = false;
        pGate->applyUiOp (clearStep);

        double curPpq = ppqPerSample;
        int offAtSample = -1;
        for (int s = 1; s <= 2100; ++s) {
            phGate.ppq = curPpq;
            m1.clear();
            pGate->processBlock (a1, m1);
            for (const auto m : m1)
                if (m.getMessage().isNoteOff() && offAtSample < 0)
                    offAtSample = s;
            curPpq += ppqPerSample;
            while (curPpq >= phGate.loopEndPpq) curPpq -= (phGate.loopEndPpq - phGate.loopStartPpq);
        }
        const auto gateMsg = "H2: Short loop altered 2,000-sample gate, Note Off landed at sample " + std::to_string (offAtSample);
        require (offAtSample == 2000, gateMsg.c_str());

        // (c) Degenerate 0.125-sample sub-sample loop over 128 samples must stay bounded well under real-time deadline.
        auto pSub = std::make_unique<SliderRhythmMidiProcessor>();
        TestPlayHead phSub;
        phSub.bpm = 120.0;
        phSub.looping = true;
        phSub.loopStartPpq = 0.0;
        phSub.loopEndPpq = 0.125 * ppqPerSample;
        pSub->setPlayHead (&phSub);
        pSub->prepareToPlay (48000.0, 128);
        juce::AudioBuffer<float> a128 (0, 128);
        juce::MidiBuffer m128;
        const auto t0 = std::chrono::steady_clock::now();
        pSub->processBlock (a128, m128);
        const auto elapsedMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        require (elapsedMs < 5.0, "H2: Sub-sample degenerate loop exceeded real-time budget");
    }

    // H3: VST3 2,048-event compaction preserves all Tier-1 Note Offs even under 2,048 earlier poly-aftertouch messages.
    {
        juce::MidiBuffer src, dst;
        for (int i = 0; i < 2048; ++i)
            src.addEvent (juce::MidiMessage::aftertouchChange (1, 60, i % 128), i % 100);
        src.addEvent (juce::MidiMessage::noteOff (10, 36), 200);
        src.addEvent (juce::MidiMessage::noteOff (10, 42), 201);
        SliderRhythmMidiProcessor::compactVst3OutputBuffer (src, dst, 256, 2048);
        int total = 0, noteOffs = 0;
        for (const auto m : dst) {
            ++total;
            if (m.getMessage().isNoteOff()) ++noteOffs;
        }
        require (total == 2048 && noteOffs == 2, "H3: VST3 compaction dropped expiring Note Offs under dense poly-aftertouch");
    }

    // H4: Re-entrant host callback modifying APVTS during edit() or setHostParamValue() must NOT be hidden from snapshot/save.
    {
        struct ReentrantHostListener final : public juce::AudioProcessorListener {
            SliderRhythmMidiProcessor& proc;
            bool armed = false;
            bool fired = false;
            explicit ReentrantHostListener (SliderRhythmMidiProcessor& pr) : proc (pr) {}
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {
                if (! armed || fired) return;
                fired = true;
                if (auto* vp = proc.parameters().getParameter (SliderRhythmMidiProcessor::paramId (0, "velocity")))
                    vp->setValueNotifyingHost (vp->convertTo0to1 (77.0f));
            }
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        };

        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        ReentrantHostListener listener (*p);
        p->addListener (&listener);
        listener.armed = true;
        p->edit ([] (auto& s) { s.sequence.tracks[0].velocity = 40; });
        p->removeListener (&listener);
        require (listener.fired, "H4: Re-entrant listener did not fire during edit()");
        require (p->snapshot()->sequence.tracks[0].velocity == 77, "H4: Snapshot missed re-entrant host value 77 after edit()");
        require (! p->hasPendingHostOrAudioChanges(), "H4: Snapshot should already be synced with 77");

        juce::MemoryBlock saved;
        p->getStateInformation (saved);
        auto p2 = std::make_unique<SliderRhythmMidiProcessor>();
        p2->setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        require (p2->snapshot()->sequence.tracks[0].velocity == 77, "H4: Saved state lost re-entrant host value 77");

        // Same check on single-param setHostParamValue:
        listener.fired = false;
        p->addListener (&listener);
        p->setHostParamValue (0, "velocity", 40.0f);
        p->removeListener (&listener);
        require (listener.fired && p->snapshot()->sequence.tracks[0].velocity == 77,
                 "H4: setHostParamValue hid re-entrant host value 77");
    }

    // H5: Hardware CC followed immediately by unrelated edit() (e.g. rename) must NOT rebound to old APVTS velocity on next block.
    {
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        TestPlayHead ph;
        ph.bpm = 120.0;
        p->setPlayHead (&ph);
        p->prepareToPlay (48000.0, 256);
        p->edit ([] (auto& s) {
            s.sequence.trackCount = 1;
            s.sequence.tracks[0].slot = sliderseq::defaultTrackSlot;
            s.sequence.tracks[0].modAmount = 0;
            s.sequence.tracks[0].velocity = 100;
            s.bindings[0][SliderRhythmMidiProcessor::velocity] = { 70, 1 };
        });
        ph.playing = false;
        sendCC (*p, 70, 40, 1); // Audio CC changes velocity 100 -> 40 while APVTS is still 100
        p->edit ([] (auto& s) { s.sequence.tracks[0].name = "KICK RENAMED"; }); // Unrelated bulk edit

        ph.playing = true;
        ph.ppq = 0.0;
        juce::AudioBuffer<float> audio (0, 256);
        juce::MidiBuffer midi;
        p->processBlock (audio, midi);
        int playedVel = -1;
        for (const auto m : midi)
            if (m.getMessage().isNoteOn() && m.getMessage().getNoteNumber() == 36)
                playedVel = m.getMessage().getVelocity();
        require (playedVel == 40, "H5: Unrelated edit() after hardware CC caused next block to rebound to stale velocity 100");
    }

    // H6: 520 learned velocity CCs with editor closed (overflowing 512-entry audioToUiFifo) must retain final value 12, not older FIFO value 3.
    {
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        p->prepareToPlay (48000.0, 256);
        p->edit ([] (auto& s) {
            s.bindings[0][SliderRhythmMidiProcessor::velocity] = { 71, 1 };
        });
        for (int i = 1; i <= 520; ++i) {
            const int v = (i == 520) ? 12 : ((i % 5) + 1);
            sendCC (*p, 71, v, 1);
        }
        require (p->snapshot()->sequence.tracks[0].velocity == 12, "H6: Overflow recovery replayed stale FIFO ops over final velocity 12");
        juce::MemoryBlock saved;
        p->getStateInformation (saved);
        auto pReload = std::make_unique<SliderRhythmMidiProcessor>();
        pReload->setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        require (pReload->snapshot()->sequence.tracks[0].velocity == 12, "H6: Saved project lost final overflow velocity 12");
    }

    // H7 / GLM H-1: ACTIVE -> ALL THRU -> return inside DAW loop (or seek back to beat 0) must emit all 4 kicks immediately.
    {
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        TestPlayHead ph;
        ph.bpm = 120.0;
        ph.looping = true;
        ph.loopStartPpq = 0.0;
        ph.loopEndPpq = 4.0;
        p->setPlayHead (&ph);
        p->prepareToPlay (48000.0, 512);
        p->edit ([] (auto& s) {
            s.sequence.trackCount = 1;
            s.sequence.tracks[0].slot = sliderseq::defaultTrackSlot;
        });
        const double ppqPerBlock = 512.0 * 120.0 / (60.0 * 48000.0);
        const int blocksFor2Bars = static_cast<int> (std::lround (8.0 / ppqPerBlock)); // Exact 375 blocks = 8.0 beats
        const int blocksPerBar = static_cast<int> (std::ceil (4.0 / ppqPerBlock));
        juce::AudioBuffer<float> audio (0, 512);
        juce::MidiBuffer midi;
        double ppq = 0.0;

        auto runBlocks = [&] (int count) {
            int kicks = 0;
            for (int b = 0; b < count; ++b) {
                ph.ppq = ppq;
                midi.clear();
                p->processBlock (audio, midi);
                for (const auto m : midi)
                    if (m.getMessage().isNoteOn() && m.getMessage().getNoteNumber() == 36) ++kicks;
                ppq += ppqPerBlock;
                while (ppq >= ph.loopEndPpq) ppq -= (ph.loopEndPpq - ph.loopStartPpq);
            }
            return kicks;
        };

        require (runBlocks (blocksFor2Bars) == 8, "H7: Initial 2 loop passes should emit 8 kicks");
        SliderRhythmMidiProcessor::StateOp thruOp;
        thruOp.kind = SliderRhythmMidiProcessor::OpKind::setPlaybackMode;
        thruOp.arg0 = static_cast<int> (sliderseq::PlaybackMode::thru);
        p->applyUiOp (thruOp);
        require (runBlocks (226) == 0, "H7: ALL THRU must emit 0 generated kicks");

        SliderRhythmMidiProcessor::StateOp activeOp;
        activeOp.kind = SliderRhythmMidiProcessor::OpKind::setPlaybackMode;
        activeOp.arg0 = static_cast<int> (sliderseq::PlaybackMode::gen);
        p->applyUiOp (activeOp);
        const int resumed = runBlocks (blocksPerBar);
        require (resumed == 4, "H7: Returning from ALL THRU inside DAW loop dropped kicks");

        // Also test explicit seek back to beat 0 during ALL THRU -> immediate Beat 1 kick on return!
        p->applyUiOp (thruOp);
        runBlocks (1);
        ppq = 0.0;
        p->applyUiOp (activeOp);
        ph.ppq = 0.0;
        midi.clear();
        p->processBlock (audio, midi);
        bool sawBeat1 = false;
        for (const auto m : midi)
            if (m.getMessage().isNoteOn() && m.getMessage().getNoteNumber() == 36 && m.samplePosition == 0)
                sawBeat1 = true;
        require (sawBeat1, "H7: Seeking to beat 0 during ALL THRU then returning to ACTIVE dropped Beat 1");
    }

    // M1: Multi-field edit() and setStateInformation() must NEVER expose torn (old note 36 + new vel 40) state mid-sync.
    {
        struct AtomicCheckListener final : public juce::AudioProcessorListener {
            SliderRhythmMidiProcessor& proc;
            TestPlayHead& ph;
            bool armed = false;
            bool fired = false;
            int obsNote = -1, obsVel = -1;
            AtomicCheckListener (SliderRhythmMidiProcessor& pr, TestPlayHead& head) : proc (pr), ph (head) {}
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {
                if (! armed || fired) return;
                fired = true;
                juce::AudioBuffer<float> a (0, 256);
                juce::MidiBuffer m;
                ph.ppq = 0.0;
                proc.processBlock (a, m);
                for (const auto meta : m)
                    if (meta.getMessage().isNoteOn()) {
                        obsNote = meta.getMessage().getNoteNumber();
                        obsVel = meta.getMessage().getVelocity();
                        break;
                    }
            }
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        };

        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        TestPlayHead ph;
        ph.bpm = 120.0;
        p->setPlayHead (&ph);
        p->prepareToPlay (48000.0, 256);
        p->edit ([] (auto& s) {
            s.sequence.trackCount = 1;
            s.sequence.tracks[0].slot = sliderseq::defaultTrackSlot;
            s.sequence.tracks[0].modAmount = 0;
            s.sequence.tracks[0].midiNote = 36;
            s.sequence.tracks[0].velocity = 100;
        });

        AtomicCheckListener listener (*p, ph);
        p->addListener (&listener);
        listener.armed = true;
        p->edit ([] (auto& s) {
            s.sequence.tracks[0].midiNote = 50;
            s.sequence.tracks[0].velocity = 40;
        });
        p->removeListener (&listener);
        require (listener.fired && listener.obsNote == 50 && listener.obsVel == 40,
                 "M1: edit() exposed torn (old note + new velocity) state to audio thread");

        juce::MemoryBlock stateB;
        p->getStateInformation (stateB);

        // Reset back to (36, 100), then restore (50, 40) via setStateInformation while preempting on first param callback.
        p->edit ([] (auto& s) {
            s.sequence.tracks[0].midiNote = 36;
            s.sequence.tracks[0].velocity = 100;
        });
        listener.fired = false;
        listener.obsNote = listener.obsVel = -1;
        p->addListener (&listener);
        p->setStateInformation (stateB.getData(), static_cast<int> (stateB.getSize()));
        p->removeListener (&listener);
        require (listener.fired && listener.obsNote == 50 && listener.obsVel == 40,
                 "M1: setStateInformation() exposed torn state to audio thread");
    }

    // M2: Releasing a held HLD Note-Learn Mute pad during host Bypass must unmute cleanly.
    {
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        p->prepareToPlay (48000.0, 256);
        p->setNoteLearnEnabled (true);
        p->setLearnTriggerMode (SliderRhythmMidiProcessor::TriggerMode::momentary);
        p->edit ([] (auto& s) {
            s.bindings[0][SliderRhythmMidiProcessor::mute] = {
                25, 1, SliderRhythmMidiProcessor::BindingKind::note, SliderRhythmMidiProcessor::TriggerMode::momentary, false
            };
        });
        juce::AudioBuffer<float> audio (0, 256);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 25, static_cast<juce::uint8> (100)), 0);
        p->processBlock (audio, midi);
        require (p->snapshot()->sequence.tracks[0].muted, "M2: HLD Note On failed to mute");

        midi.clear();
        midi.addEvent (juce::MidiMessage::noteOff (1, 25), 5);
        p->processBlockBypassed (audio, midi);
        require (! p->snapshot()->sequence.tracks[0].muted, "M2: Releasing HLD Note during host Bypass left track muted");
    }

    // M3: Legacy saved bindings to CC 64 / 66 / 120 / 121 / 123 must be stripped on load and never swallow pedals/panic.
    {
        juce::XmlElement xml ("SliderRhythmMidi");
        xml.setAttribute ("version", 5);
        xml.setAttribute ("trackCount", 4);
        auto* tr = xml.createNewChildElement ("Track");
        tr->setAttribute ("index", 0);
        tr->setAttribute ("slot", 4);
        auto* b64 = tr->createNewChildElement ("Binding");
        b64->setAttribute ("control", static_cast<int> (SliderRhythmMidiProcessor::velocity));
        b64->setAttribute ("cc", 64);
        b64->setAttribute ("channel", 1);
        b64->setAttribute ("kind", 0);
        auto* b123 = tr->createNewChildElement ("Binding");
        b123->setAttribute ("control", static_cast<int> (SliderRhythmMidiProcessor::mute));
        b123->setAttribute ("cc", 123);
        b123->setAttribute ("channel", 1);
        b123->setAttribute ("kind", 0);

        juce::MemoryBlock bin;
        SliderRhythmMidiProcessor::copyXmlToBinary (xml, bin);
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        p->setStateInformation (bin.getData(), static_cast<int> (bin.getSize()));
        require (p->snapshot()->bindings[0][SliderRhythmMidiProcessor::velocity].cc == -1
                 && p->snapshot()->bindings[0][SliderRhythmMidiProcessor::mute].cc == -1,
                 "M3: Protected CC 64/123 bindings must be dropped during state load");

        // Even if injected directly in memory, CC 64 and CC 123 must still pass through unconsumed.
        p->edit ([] (auto& s) {
            s.bindings[0][SliderRhythmMidiProcessor::velocity] = { 64, 1 };
        });
        juce::AudioBuffer<float> audio (0, 256);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
        p->processBlock (audio, midi);
        bool saw64 = false;
        for (const auto m : midi)
            if (m.getMessage().isController() && m.getMessage().getControllerNumber() == 64) saw64 = true;
        require (saw64, "M3: Protected CC 64 was swallowed by binding");
    }

    // M6 & L1: Legacy v3 mixed-sign grooves + v4 GEN+THRU mute-bypass + sparse v4 missing slot fallback to 1.
    {
        juce::XmlElement v3 ("SliderRhythmMidi");
        v3.setAttribute ("version", 3);
        v3.setAttribute ("trackCount", 2);
        v3.setAttribute ("playbackMode", static_cast<int> (sliderseq::PlaybackMode::genPlusThru));
        auto* t0 = v3.createNewChildElement ("Track");
        t0->setAttribute ("index", 0);
        t0->setAttribute ("slot", 0);
        t0->setAttribute ("groove", 75); // +50 8th swing
        t0->setAttribute ("muted", true);
        auto* t1 = v3.createNewChildElement ("Track");
        t1->setAttribute ("index", 1);
        t1->setAttribute ("slot", 0);
        t1->setAttribute ("groove", 25); // -50 16th shuffle

        juce::MemoryBlock bin;
        SliderRhythmMidiProcessor::copyXmlToBinary (v3, bin);
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        p->setStateInformation (bin.getData(), static_cast<int> (bin.getSize()));
        juce::MemoryBlock resaved;
        p->getStateInformation (resaved);
        auto p2 = std::make_unique<SliderRhythmMidiProcessor>();
        p2->setStateInformation (resaved.getData(), static_cast<int> (resaved.getSize()));
        const auto s2 = p2->snapshot();
        require (s2->sequence.globalSwing == 50
                 && s2->sequence.tracks[0].groove == 100 && ! s2->sequence.tracks[0].invertSwingSign
                 && s2->sequence.tracks[1].groove == 100 && s2->sequence.tracks[1].invertSwingSign,
                 "M6: Mixed-sign v3 grooves did not round-trip across re-save");
        require (s2->sequence.legacyThruBypassesMuteSolo, "M6: Legacy GEN+THRU mute/solo bypass flag was not preserved");

        // Muted Track 0 still passes live Kick input through because legacyThruBypassesMuteSolo == true.
        p2->prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> audio (0, 256);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (10, 36, static_cast<juce::uint8> (100)), 0);
        p2->processBlock (audio, midi);
        bool sawThru = false;
        for (const auto m : midi)
            if (m.getMessage().isNoteOn() && m.getMessage().getNoteNumber() == 36) sawThru = true;
        require (sawThru, "M6: Legacy GEN+THRU muted track dropped passthrough note");

        // L1: Sparse v4 XML omitting slot falls back to legacyDefaultTrackSlot (1), not 4.
        juce::XmlElement sparseV4 ("SliderRhythmMidi");
        sparseV4.setAttribute ("version", 4);
        sparseV4.setAttribute ("trackCount", 1);
        auto* st0 = sparseV4.createNewChildElement ("Track");
        st0->setAttribute ("index", 0);
        juce::MemoryBlock sparseBin;
        SliderRhythmMidiProcessor::copyXmlToBinary (sparseV4, sparseBin);
        auto pSparse = std::make_unique<SliderRhythmMidiProcessor>();
        pSparse->setStateInformation (sparseBin.getData(), static_cast<int> (sparseBin.getSize()));
        require (pSparse->snapshot()->sequence.tracks[0].slot == 1, "L1: Sparse v4 missing slot should default to 1");
    }

    // M9: Persisted Strict channel match takes effect on fresh processor BEFORE opening any editor.
    {
        uiprefs::Settings strictPrefs;
        strictPrefs.inputChannelMatch = sliderseq::InputChannelMatch::strict;
        require (uiprefs::save (strictPrefs), "M9: Failed to write temp prefs");
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        require (p->snapshot()->sequence.inputChannelMatch == sliderseq::InputChannelMatch::strict,
                 "M9: Persisted Strict inputChannelMatch did not apply before opening editor");
        // Restore default prefs for subsequent checks.
        uiprefs::save (uiprefs::Settings {});
    }

    // M12: ALL THRU passes overlapping same-pitch Note Ons/Offs and unpaired Note Offs 100% verbatim.
    {
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        p->prepareToPlay (48000.0, 256);
        p->edit ([] (auto& s) { s.playbackMode = sliderseq::PlaybackMode::thru; });
        juce::AudioBuffer<float> audio (0, 256);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (90)), 10);
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 20);
        midi.addEvent (juce::MidiMessage::noteOff (1, 60), 30);
        midi.addEvent (juce::MidiMessage::noteOff (1, 64), 40); // Unpaired standalone Note Off
        p->processBlock (audio, midi);
        int ons60 = 0, offs60 = 0, offs64 = 0;
        for (const auto m : midi) {
            const auto msg = m.getMessage();
            if (msg.getNoteNumber() == 60 && msg.isNoteOn()) ++ons60;
            if (msg.getNoteNumber() == 60 && msg.isNoteOff()) ++offs60;
            if (msg.getNoteNumber() == 64 && msg.isNoteOff()) ++offs64;
        }
        require (ons60 == 2 && offs60 == 2 && offs64 == 1,
                 "M12: ALL THRU altered overlapping Note On/Off sequence or dropped unpaired Note Off");
    }

    // M13: MIDI exporter supports all power-of-two denominators including 3/32 (4 bars = 432 ticks).
    {
        auto st = sliderseq::makeDefaultState();
        st.trackCount = 1;
        const auto f = midiexporter::buildMidiFile (st, 4, 120.0, 3, 32);
        const auto* seq = f.getTrack (0);
        require (seq != nullptr && std::abs (seq->getEndTime() - 432.0) < 1.0,
                 "M13: 4-bar 3/32 export length should be 432 ticks, not 3/4");
        bool saw332 = false;
        for (int i = 0; i < seq->getNumEvents(); ++i) {
            const auto& msg = seq->getEventPointer (i)->message;
            if (msg.isTimeSignatureMetaEvent()) {
                int num = 0, den = 0;
                msg.getTimeSignatureInfo (num, den);
                if (num == 3 && den == 32) saw332 = true;
            }
        }
        require (saw332, "M13: Exported MIDI missing 3/32 time signature meta event");
    }

    // M14: CC123 (All Notes Off) must NOT clear Sustain/Sostenuto pedal state so subsequent CC64=0 passes under OTHER:BLK.
    {
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        p->prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> audio (0, 256);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 123, 0), 5);
        p->processBlock (audio, midi);

        SliderRhythmMidiProcessor::StateOp blk;
        blk.kind = SliderRhythmMidiProcessor::OpKind::setUnmappedMode;
        blk.arg0 = static_cast<int> (sliderseq::UnmappedRouteMode::block);
        p->applyUiOp (blk);

        midi.clear();
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 10);
        p->processBlock (audio, midi);
        bool sawRelease = false;
        for (const auto m : midi)
            if (m.getMessage().isController() && m.getMessage().getControllerNumber() == 64 && m.getMessage().getControllerValue() == 0)
                sawRelease = true;
        require (sawRelease, "M14: CC123 cleared Sustain pedal tracking and caused CC64=0 release to be dropped under OTHER:BLK");
    }

    // M7, M10, L3: Small-screen 125% CFG accessibility, ThemeToggleButton hover/down ink visibility, and 4-track compact zero H-scroll.
    {
        uiprefs::Settings s125;
        s125.darkMode = true;
        s125.scalePercent = 125;
        uiprefs::save (s125);
        auto p = std::make_unique<SliderRhythmMidiProcessor>();
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
        ed->setSize (1240, 700); // Simulated 1280px screen at 125% zoom
        juce::TextButton* cfgBtn = nullptr;
        juce::TextButton* themeBtn = nullptr;
        juce::Viewport* vp = nullptr;
        std::function<void (juce::Component&)> walk = [&] (juce::Component& c) {
            if (auto* b = dynamic_cast<juce::TextButton*> (&c)) {
                if (b->getButtonText() == "CFG") cfgBtn = b;
                if (b->getButtonText().contains ("DARK")) themeBtn = b;
            }
            if (auto* v = dynamic_cast<juce::Viewport*> (&c)) vp = v;
            for (int i = 0; i < c.getNumChildComponents(); ++i)
                walk (*c.getChildComponent (i));
        };
        walk (*ed);
        require (cfgBtn != nullptr && themeBtn != nullptr && vp != nullptr, "UI lookup failed for CFG/Theme/Viewport");
        const auto cfgInEd = ed->getLocalArea (cfgBtn, cfgBtn->getLocalBounds());
        require (cfgInEd.getRight() <= ed->getWidth() && cfgInEd.getWidth() > 20,
                 "M7: CFG button clipped outside 1240px window at 125% zoom");

        // M10: Paint ThemeToggleButton in normal, over, and down states -> orange ink must remain visible in all 3!
        for (int state = 0; state < 3; ++state) {
            const bool over = (state == 1);
            const bool down = (state == 2);
            juce::Image img (juce::Image::ARGB, std::max (1, themeBtn->getWidth()), std::max (1, themeBtn->getHeight()), true);
            juce::Graphics g (img);
            themeBtn->paintButton (g, over, down);
            int orangePixels = 0;
            for (int y = 0; y < img.getHeight(); ++y)
                for (int x = 0; x < img.getWidth(); ++x) {
                    const auto px = img.getPixelAt (x, y);
                    if (px.getRed() > 150 && px.getBlue() < 90) ++orangePixels;
                }
            require (orangePixels > 20, "M10: ThemeToggleButton text obscured by background in hover/down state");
        }

        // L3: 4 tracks at 100% scale in 1080x540 must NOT spawn a spurious horizontal scrollbar.
        ed.reset(); // Release SharedResourcePointer<LookAndFeelResources> so 100% prefs reload cleanly
        uiprefs::Settings s100;
        s100.darkMode = true;
        s100.scalePercent = 100;
        uiprefs::save (s100);
        std::unique_ptr<juce::AudioProcessorEditor> ed100 (p->createEditor());
        ed100->setSize (1080, 540);
        juce::Viewport* vp100 = nullptr;
        std::function<void (juce::Component&)> findVp = [&] (juce::Component& c) {
            if (auto* v = dynamic_cast<juce::Viewport*> (&c)) vp100 = v;
            for (int i = 0; i < c.getNumChildComponents(); ++i)
                findVp (*c.getChildComponent (i));
        };
        findVp (*ed100);
        require (vp100 != nullptr, "L3: Missing viewport");
        require (vp100->getVerticalScrollBar().isVisible() && ! vp100->getHorizontalScrollBar().isVisible(),
                 "L3: 4 tracks at 1080x540 unexpectedly triggered visible horizontal scrollbar");
    }
}
