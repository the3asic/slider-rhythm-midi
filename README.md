# Slider Rhythm MIDI

Maintained by **Slider Rhythm Lab**.

A performance-first MIDI drum sequencer for Logic Pro and Ableton Live. Instead of programming every beat from scratch on stage, select rhythmic patterns with a fader, switch clock divisions, and shape variations in real time. Each track has 12 pattern slots, including three fill slots, and an editable 1–16 step grid.

This plug-in generates MIDI; it has no built-in sounds or audio effects. Put it before a drum instrument and use the instrument's sounds. The project is currently an **early preview**, and broader host compatibility testing is still in progress.

## Features

- 1–16 independent tracks; Kick, Snare, Closed Hat, and Open Hat start presets
- Twelve selectable patterns per track, including three fills
- Division choices: 1/3×, 1/2×, 1×, 2×, 3×
- Editable pattern length (1–16 steps), Shuffle/Swing, modulation, velocity, Mute, and Solo
- MIDI Learn: click MIDI LEARN, click a control, then move your hardware controller
- AU MIDI FX for Logic Pro; VST3 for hosts that support plug-in MIDI output
- Host-synced tempo and project-state recall
- MIDI note map presets: GM-compatible Logic/Groove Agent/AD2 GM, XLN Addictive Drums 2 Standard (12 tracks), XLN XO eight slots, and editable chromatic pads. XO note mapping needs verification against XO itself

## Logic Pro quick start

1. Create a Software Instrument track and insert Ultrabeat as the instrument.
2. Insert Slider Rhythm MIDI in the MIDI FX slot above Ultrabeat.
3. Press Play in `LIVE GENERATE` mode. The default notes are 36, 38, 42, and 46 on MIDI channel 10; adjust track notes to match your kit.
4. To record generated notes into a MIDI region, choose `Record MIDI to Track Here` from the plug-in slot's menu, arm the track, and record.
5. For playback of that region, switch to `PLAY RECORDED MIDI` to prevent double triggering.

A project tester confirmed that Logic records MIDI when `Record MIDI to Track Here` is enabled. A reproducible release test across a clean Logic project is still pending.

## Build from source

Requirements: macOS, Xcode with its license accepted, CMake 3.22 or newer, and JUCE 9.0.2. CMake fetches JUCE 9.0.2 unless `JUCE_DIR` points to a local checkout.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

Build artifacts are under `build/SliderRhythmMidi_artefacts/Release/`. For local use, copy the AU component into `~/Library/Audio/Plug-Ins/Components/` and rescan Logic. You can validate AU discovery with `auval -v aumi SlrM Slrh`.

## Status and limitations

- AU format validation passed on one Apple Silicon Mac with macOS 27.2 and Logic Pro 13 installed.
- AU and VST3 currently build for arm64 on that Mac. Intel macOS and other hosts are unverified.
- A user confirmed Logic MIDI recording with `Record MIDI to Track Here`, but repeatable clean-project testing and Ableton Live routing are still pending.
- This preview is intended as a **source release**. Distributed macOS binaries require a signing and notarization plan.

## License

This project is licensed under **AGPL-3.0-only**. See [LICENSE](LICENSE). JUCE 9.0.2 is used under its AGPLv3 option.

## Contributing

Bug reports with macOS, host and plug-in versions, reproduction steps, expected behavior, and actual behavior are welcome after the first public release.
