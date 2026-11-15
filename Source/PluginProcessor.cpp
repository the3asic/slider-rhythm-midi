#include "PluginProcessor.h"
#include "PluginEditor.h"

SliderRhythmMidiProcessor::SliderRhythmMidiProcessor()
    : AudioProcessor (BusesProperties()), state (std::make_shared<const Snapshot>()) {}

void SliderRhythmMidiProcessor::prepareToPlay (double sampleRate, int) {
    currentSampleRate = sampleRate;
    wasPlaying = false;
    generatedNotes.store (0);
}

void SliderRhythmMidiProcessor::edit (const std::function<void (Snapshot&)>& change) {
    auto previous = std::atomic_load (&state);
    for (;;) {
        auto next = std::make_shared<Snapshot> (*previous);
        change (*next);
        std::shared_ptr<const Snapshot> immutable = next;
        if (std::atomic_compare_exchange_weak (&state, &previous, immutable)) break;
    }
}

void SliderRhythmMidiProcessor::armMidiLearn (int track, Control control) {
    if (track < 0 || track >= sliderseq::maxTracks) return;
    learnTarget.store (track * controlCount + static_cast<int> (control));
}

template<typename Sample>
void SliderRhythmMidiProcessor::renderBlock (juce::AudioBuffer<Sample>& audio, juce::MidiBuffer& midi) {
    audio.clear();
    for (const auto metadata : midi) {
        const auto message = metadata.getMessage();
        if (! message.isController()) continue;
        const int cc = message.getControllerNumber(), channel = message.getChannel(), value = message.getControllerValue();
        const int target = learnTarget.exchange (-1);
        if (target >= 0) {
            const int track = target / controlCount, control = target % controlCount;
            edit ([=] (Snapshot& next) {
                for (auto& trackBindings : next.bindings)
                    for (auto& binding : trackBindings)
                        if (binding.cc == cc && binding.channel == channel) binding = {};
                next.bindings[static_cast<size_t> (track)][static_cast<size_t> (control)] = { cc, channel };
            });
            continue;
        }
        const auto bindingsNow = std::atomic_load (&state);
        bool mapped = false;
        for (int t = 0; t < bindingsNow->sequence.trackCount && ! mapped; ++t)
            for (int control = 0; control < controlCount; ++control) {
                const auto& binding = bindingsNow->bindings[static_cast<size_t> (t)][static_cast<size_t> (control)];
                if (binding.cc == cc && binding.channel == channel) { mapped = true; break; }
            }
        if (! mapped) continue;
        edit ([=] (Snapshot& next) {
            const std::array<double, 5> rates { 1.0 / 3.0, .5, 1.0, 2.0, 3.0 };
            for (int t = 0; t < next.sequence.trackCount; ++t) {
                auto& track = next.sequence.tracks[static_cast<size_t> (t)];
                for (int control = 0; control < controlCount; ++control) {
                    const auto& binding = next.bindings[static_cast<size_t> (t)][static_cast<size_t> (control)];
                    if (binding.cc != cc || binding.channel != channel) continue;
                    switch (static_cast<Control> (control)) {
                        case slot: track.slot = static_cast<int> (std::lround (value * 11.0 / 127.0)); break;
                        case rate: track.rate = rates[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (value * 4.0 / 127.0)), 0, 4))]; break;
                        case velocity: track.velocity = std::clamp (value, 1, 127); break;
                        case groove: track.groove = static_cast<int> (std::lround (value * 100.0 / 127.0)); break;
                        case modAmount: track.modAmount = static_cast<int> (std::lround (value * 100.0 / 127.0)); break;
                        case mute: track.muted = value >= 64; break;
                        case modMode: track.modMode = static_cast<int> (std::lround (value * 2.0 / 127.0)); break;
                        case note: track.midiNote = value; break;
                        case SliderRhythmMidiProcessor::channel: track.midiChannel = 1 + static_cast<int> (std::lround (value * 15.0 / 127.0)); break;
                        case length: track.patterns[static_cast<size_t> (std::clamp (track.slot, 0, 11))].length = 1 + static_cast<int> (std::lround (value * 15.0 / 127.0)); break;
                        case grid: {
                            const std::array<int, 5> grids { 12, 16, 32, 48, 64 };
                            track.patterns[static_cast<size_t> (std::clamp (track.slot, 0, 11))].grid = grids[static_cast<size_t> (std::clamp (static_cast<int> (std::lround (value * 4.0 / 127.0)), 0, 4))];
                            break;
                        }
                        case controlCount: break;
                    }
                }
            }
        });
    }
    const auto mode = std::atomic_load (&state);
    if (mode->playRecordedMidi) {
        if (wasPlaying)
            for (int t = 0; t < mode->sequence.trackCount; ++t)
                midi.addEvent (juce::MidiMessage::allNotesOff (mode->sequence.tracks[static_cast<size_t> (t)].midiChannel), 0);
        wasPlaying = false;
        return; // Let the recorded MIDI region reach Ultrabeat without generating notes again.
    }
    midi.clear(); // This is a generator: avoid forwarding input notes into the drum instrument.

    const auto snapshotNow = std::atomic_load (&state);
    const auto* playhead = getPlayHead();
    const auto position = playhead != nullptr ? playhead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};
    // Logic may report recording without setting isPlaying on a MIDI FX playhead.
    const bool playing = position && (position->getIsPlaying() || position->getIsRecording());
    hostPlaying.store (playing);
    hostRecording.store (position && position->getIsRecording());
    hostPositionAvailable.store (position && position->getPpqPosition() && position->getBpm());
    if (! playing || ! position || ! position->getPpqPosition() || ! position->getBpm()) {
        if (wasPlaying) {
            for (int t = 0; t < snapshotNow->sequence.trackCount; ++t)
                midi.addEvent (juce::MidiMessage::allNotesOff (snapshotNow->sequence.tracks[static_cast<size_t> (t)].midiChannel), 0);
        }
        wasPlaying = false;
        return;
    }
    wasPlaying = true;
    const double bpm = *position->getBpm(), startTick = *position->getPpqPosition() * sliderseq::ticksPerQuarter;
    if (bpm <= 0.0 || currentSampleRate <= 0.0) return;
    const double ticksPerSample = bpm * sliderseq::ticksPerQuarter / (60.0 * currentSampleRate);
    const auto events = sliderseq::render (snapshotNow->sequence, startTick, startTick + ticksPerSample * audio.getNumSamples());
    for (const auto& event : events) {
        const int sample = std::clamp (static_cast<int> (std::lround ((event.absoluteTick - startTick) / ticksPerSample)), 0, audio.getNumSamples() - 1);
        if (event.noteOn) {
            midi.addEvent (juce::MidiMessage::noteOn (event.channel, event.note, static_cast<juce::uint8> (event.velocity)), sample);
            generatedNotes.fetch_add (1, std::memory_order_relaxed);
        } else
            midi.addEvent (juce::MidiMessage::noteOff (event.channel, event.note), sample);
    }
}

void SliderRhythmMidiProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) { renderBlock (audio, midi); }
void SliderRhythmMidiProcessor::processBlock (juce::AudioBuffer<double>& audio, juce::MidiBuffer& midi) { renderBlock (audio, midi); }

void SliderRhythmMidiProcessor::getStateInformation (juce::MemoryBlock& destination) {
    const auto current = std::atomic_load (&state);
    juce::XmlElement root ("SliderRhythmMidi");
    root.setAttribute ("version", 1);
    root.setAttribute ("trackCount", current->sequence.trackCount);
    root.setAttribute ("playRecordedMidi", current->playRecordedMidi);
    for (int i = 0; i < sliderseq::maxTracks; ++i) {
        const auto& track = current->sequence.tracks[static_cast<size_t> (i)];
        auto* element = root.createNewChildElement ("Track");
        element->setAttribute ("index", i);
        element->setAttribute ("name", track.name);
        element->setAttribute ("slot", track.slot);
        element->setAttribute ("rate", track.rate);
        element->setAttribute ("note", track.midiNote);
        element->setAttribute ("channel", track.midiChannel);
        element->setAttribute ("velocity", track.velocity);
        element->setAttribute ("groove", track.groove);
        element->setAttribute ("modMode", track.modMode);
        element->setAttribute ("modAmount", track.modAmount);
        element->setAttribute ("muted", track.muted);
        element->setAttribute ("solo", track.solo);
        for (int slot = 0; slot < sliderseq::slotsPerTrack; ++slot) {
            const auto& pattern = track.patterns[static_cast<size_t> (slot)];
            auto* child = element->createNewChildElement ("Pattern");
            child->setAttribute ("slot", slot);
            child->setAttribute ("name", pattern.name);
            child->setAttribute ("grid", pattern.grid);
            child->setAttribute ("length", pattern.length);
            juce::String steps;
            for (int step = 0; step < 64; ++step) steps += pattern.steps[static_cast<size_t> (step)] ? "1" : "0";
            child->setAttribute ("steps", steps);
        }
        for (int control = 0; control < controlCount; ++control) {
            const auto& binding = current->bindings[static_cast<size_t> (i)][static_cast<size_t> (control)];
            if (binding.cc < 0) continue;
            auto* child = element->createNewChildElement ("Binding");
            child->setAttribute ("control", control);
            child->setAttribute ("cc", binding.cc);
            child->setAttribute ("channel", binding.channel);
        }
    }
    copyXmlToBinary (root, destination);
}

void SliderRhythmMidiProcessor::setStateInformation (const void* data, int size) {
    const auto xml = getXmlFromBinary (data, size);
    if (! xml || ! xml->hasTagName ("SliderRhythmMidi")) return;
    Snapshot restored;
    restored.sequence.trackCount = std::clamp (xml->getIntAttribute ("trackCount", 4), 1, sliderseq::maxTracks);
    restored.playRecordedMidi = xml->getBoolAttribute ("playRecordedMidi", false);
    for (auto* element : xml->getChildIterator()) {
        if (! element->hasTagName ("Track")) continue;
        const int index = element->getIntAttribute ("index", -1);
        if (index < 0 || index >= sliderseq::maxTracks) continue;
        auto& track = restored.sequence.tracks[static_cast<size_t> (index)];
        track.name = element->getStringAttribute ("name", track.name).toStdString();
        track.slot = std::clamp (element->getIntAttribute ("slot", track.slot), 0, 11);
        const std::array<double, 5> rates { 1.0 / 3.0, .5, 1.0, 2.0, 3.0 };
        const auto requestedRate = element->getDoubleAttribute ("rate", 1.0);
        track.rate = *std::min_element (rates.begin(), rates.end(), [=] (double a, double b) { return std::abs (a - requestedRate) < std::abs (b - requestedRate); });
        track.midiNote = std::clamp (element->getIntAttribute ("note", track.midiNote), 0, 127);
        track.midiChannel = std::clamp (element->getIntAttribute ("channel", track.midiChannel), 1, 16);
        track.velocity = std::clamp (element->getIntAttribute ("velocity", track.velocity), 1, 127);
        track.groove = std::clamp (element->getIntAttribute ("groove", track.groove), 0, 100);
        track.modMode = std::clamp (element->getIntAttribute ("modMode", track.modMode), 0, 2);
        track.modAmount = std::clamp (element->getIntAttribute ("modAmount", track.modAmount), 0, 100);
        track.muted = element->getBoolAttribute ("muted", false);
        track.solo = element->getBoolAttribute ("solo", false);
        for (auto* child : element->getChildIterator()) {
            if (child->hasTagName ("Pattern")) {
                const int slot = child->getIntAttribute ("slot", -1);
                if (slot < 0 || slot >= sliderseq::slotsPerTrack) continue;
                auto& pattern = track.patterns[static_cast<size_t> (slot)];
                pattern.name = child->getStringAttribute ("name", pattern.name).toStdString();
                pattern.grid = std::clamp (child->getIntAttribute ("grid", 16), 1, 64);
                pattern.length = std::clamp (child->getIntAttribute ("length", 16), 1, 16);
                const auto steps = child->getStringAttribute ("steps");
                for (int step = 0; step < 64; ++step) pattern.steps[static_cast<size_t> (step)] = step < steps.length() && steps[step] == '1';
            } else if (child->hasTagName ("Binding")) {
                const int control = child->getIntAttribute ("control", -1);
                if (control < 0 || control >= controlCount) continue;
                auto& binding = restored.bindings[static_cast<size_t> (index)][static_cast<size_t> (control)];
                binding.cc = std::clamp (child->getIntAttribute ("cc", -1), -1, 127);
                binding.channel = std::clamp (child->getIntAttribute ("channel", 0), 0, 16);
            }
        }
    }
    std::atomic_store (&state, std::make_shared<const Snapshot> (std::move (restored)));
}

juce::AudioProcessorEditor* SliderRhythmMidiProcessor::createEditor() { return new SliderRhythmMidiEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SliderRhythmMidiProcessor(); }
