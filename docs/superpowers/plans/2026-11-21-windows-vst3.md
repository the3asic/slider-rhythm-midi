# Windows VST3 implementation plan

Goal: ship a downloadable Windows x64 VST3 preview.
Architecture: reuse existing JUCE 9.0.2 code and CMake with a Windows GitHub Actions runner.
Spec: ../specs/2026-11-21-windows-vst3-design.md

- [ ] Add a manually triggered Windows workflow, configure with MSVC x64 and run existing tests. Confirm current AU format fails on Windows before correcting the format list.
- [ ] Make AU Apple-only, expose the engine test to CMake, keep its assertions enabled in Release. Build native Windows VST3 and run both tests.
- [ ] Package the entire VST3 bundle, LICENSE and bilingual install instructions. Record exact source commit.
- [ ] Verify macOS targets, review diff, publish preview ZIP and matching source with installation links.
- [ ] State Windows host playback/routing has not been verified.
