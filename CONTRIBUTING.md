# Contributing

Thanks for helping improve Slider Rhythm MIDI. Please search existing issues before opening a new one. For a bug, include the plug-in commit or version, macOS and DAW versions, instrument/kit, exact steps, expected result, and actual result. For MIDI mapping issues, include MIDI note numbers and the target instrument's selected mapping preset.

For code changes, describe the behavior change, build AU and VST3 on macOS, and run the MIDI Learn and sequencer tests. Keep audio-thread work bounded and avoid introducing allocations in `processBlock`.

By contributing, you agree that your contribution is licensed under the repository's AGPL-3.0-only license.
