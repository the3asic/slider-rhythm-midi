# Windows VST3 preview

Approved scope: Windows x64 VST3 preview sharing the current sequencer, editor, MIDI Learn and beat preset implementation. Build with Visual Studio on GitHub Actions and distribute a ZIP with installation instructions and corresponding source. Windows DAW routing and recording remain unverified until tested in a real host.

Approach: use a Windows hosted runner to produce a native binary. Reuse JUCE and CMake; enable AU only on Apple platforms. A separate implementation would duplicate the existing engine, and cross-compilation would not run the Windows tests.

Acceptance: Windows Release build produces an x64 VST3 bundle; sequencer assertions, MIDI Learn and preset/project round-trip tests pass; download archive contains the complete bundle, license and bilingual install instructions. Preserve macOS AU/VST3 configuration.
