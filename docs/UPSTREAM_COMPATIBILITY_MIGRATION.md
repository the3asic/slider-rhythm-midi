# Upstream Compatibility & Migration Reference

This document summarizes the parameter and project-state compatibility boundaries introduced in the 3ASiC fork (`main`), with reference conversion helpers isolated in [`Source/UpstreamCompatibility.h`](../Source/UpstreamCompatibility.h) for upstream maintainers merging features selectively.

## 1. AUv2 `ParameterID` Version Hints (Logic Pro / AU Hosts)

JUCE's AUv2 wrapper sorts exposed parameters by `(ParameterID.versionHint, hashedParamID)`. Adding new parameters with `versionHint = 1` inserts them into the middle of existing AU parameter indices, which can shift saved Logic Pro automation lanes.

The fork assigns strictly monotonic hints per released parameter generation (see `upstreamcompat::kAuHintGen*`):

| Generation | Version Hint | Parameter IDs |
| --- | --- | --- |
| Gen 1 (Core per-track) | `1` | `t01_slot`..`t16_slot`, `mute`, `solo`, `velocity`, `rate`, `groove`, `mod_amt`, `loop_steps` (128 params) |
| Gen 2 (Global Swing) | `2` | `global_swing` (1 param) |
| Gen 3 (Per-track Macro) | `3` | `t01_macro`..`t16_macro` (16 params) |

## 2. VST3 `tXX_slot` Normalized Range (`0..12` vs `0..13`)

Extending `tXX_slot` from `0..12` (13 steps: `EMPTY` + `1..12`) to `0..13` (14 steps: `0 BOT` + `1..12` + `TOP`) changes the normalized float-to-integer quantization (`value / 12` vs `value / 13`).

- For fresh fork builds without pre-existing VST3 automation to preserve, `tXX_slot` uses `0..13` directly with reversible text formatting (`0 BOT`, `1..12`, `TOP`).
- If an upstream release already shipped `tXX_slot` at `0..12` and must preserve saved host automation curves verbatim, either:
  1. Keep `tXX_slot` at `0..12` and introduce a new parameter ID (e.g., `tXX_zone_slot` at `versionHint = 4`) for the extended `0..13` fader, OR
  2. Use `upstreamcompat::legacyV4NormalizedSlotToV5()` when upgrading automation streams.

## 3. Legacy Project XML Migration (`v1`–`v4` → `v5`)

`upstreamcompat::migrateLegacyRouting()` and `upstreamcompat::migrateLegacyGrooves()` isolate the exact migration mapping:

- **Routing (`v1`–`v4`)**:
  - Old `GEN` (`playbackMode = 0`) blocked unconsumed MIDI → migrates to `unmappedMode = BLOCK`, `midMode = GEN`, `topMode = OFF`, `bottomMode = BLOCK` (v4) / `OFF` (v1–v3).
  - Old `GEN+THRU` (`playbackMode = 1`) forwarded all input → migrates to `playbackMode = ACTIVE`, `unmappedMode = THRU`, `midMode = GEN+THRU`, `topMode = OFF`, `bottomMode = THRU` (v4) / `OFF` (v1–v3), with `legacyThruBypassesMuteSolo` flagged if exact pre-v5 M/S behavior is required.
  - Old `THRU` (`playbackMode = 2`) → migrates to `playbackMode = ALL THRU`.
- **Groove (`v1`–`v3`)**:
  - `migrateLegacyGrooves()` extracts dominant `globalSwing` + per-track `trackSwingPercent`, and flags `hasMixedSigns` (setting opposite-sign tracks to `0%` instead of silently flipping their swing direction from 16th Shuffle to 8th Swing). Exact per-track signed values are preserved in `signedPerTrackGroove` if an upstream merge wants dual-sign legacy playback.
