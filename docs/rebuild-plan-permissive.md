# Dave — Rebuild Plan (Permissive Stack)

**Purpose:** a concrete, costed plan to rebuild Dave on a permissively-licensed
foundation so the commercial (closed-source) option stays open permanently.
This replaces the JUCE + Tracktion foundation decided in Phase 0.

**Trigger:** JUCE (AGPL) and Tracktion (GPL) can never be relicensed. If we link
either, Dave is GPL forever and the commercial option dies. Every dependency
below is MIT/BSD/Apache/ISC or LGPL (dynamic-link only).

## The stack

| Layer | Library | License | Status |
|---|---|---|---|
| Audio I/O (CoreAudio/WASAPI) | **miniaudio** | Unlicense/MIT (single-header) | Active, maintained |
| MIDI I/O | **PortMidi** | MIT | Active (Dec 2025 release) |
| GUI | **Dear ImGui** (docking+viewport branch) + OpenGL3 backend | MIT | Active; branch is beta but production-used |
| GUI: waveforms/spectra | **ImPlot** | MIT | Mature |
| GUI: routing grid | **imgui-node-editor** | MIT | Popular, v0.9.x |
| RT graph engine | **Ours** (grow from Phase 0's Node/CompiledGraph) | Ours (GPL+commercial) | Build |
| Plugin host | **Ours** against VST3 SDK + CLAP + AU | Ours | Build |
| ↳ VST3 | **VST3 SDK** (Steinberg) | MIT (since Oct 2025!) | Active |
| ↳ CLAP | **CLAP** | MIT | Active |
| ↳ Audio Unit | **AudioUnitSDK** (Apple) | Platform SDK (link, don't ship) | Active |
| Time/pitch | **Signalsmith Stretch** | MIT (header-only) | Active |
| Audio formats (WAV/FLAC/MP3) | **dr_libs** + libFLAC | Unlicense/MIT + BSD | Active |
| Video | **FFmpeg (LGPL-only build)** + OpenH264 for encode | LGPL (dynamic) + BSD | Active |
| JSON | **nlohmann/json** | MIT | Mature |
| Crash | **Crashpad** | Apache-2.0 | Active |
| Tests | **Catch2** | BSL-1.0 | Mature |
| Build | **CMake** | BSD-3 | Mature |

**Everything is permissive. The commercial option is preserved. We own the
graph engine, the plugin host, the Edit model, markers, video, and the UI.**

## What we lose vs JUCE+Tracktion (honest)

1. **JUCE's unified everything.** Audio+MIDI+GUI+formats+hosting in one box,
   with consistent APIs. We now compose ~10 libraries. More integration glue.
2. **Tracktion's mature RT graph** — latency comp, MT player, all nodes. We
   build them. (Phase 0 already started; the contract is proven.)
3. **JUCE's plugin host code** — the single biggest loss. We write a VST3/AU/CLAP
   host against the raw SDKs. This is months of careful work; it's the line item
   that most extends the timeline.
4. **A turnkey GUI toolkit.** ImGui is immediate-mode; we hand-roll the timeline,
   waveform, piano-roll, mixer. No DAW ships on ImGui yet (Lumix is the closest,
   WIP). We're trailblazing — high control, real effort.
5. **Video encode quality.** LGPL FFmpeg has no x264/x265; we use OpenH264 (BSD,
   lower quality) or AV1 for encode. Decode is fine.

## What we gain

1. **Full ownership + clean dual licensing.** Sell a closed Dave later, no
   relicensing gymnastics, no contributor-agreement headaches for *other people's*
   code. Every byte is ours or permissively licensed.
2. **No Tracktion API churn risk.** Our graph is ours.
3. **Smaller, tighter binary** — no JUCE's kitchen sink linked in.
4. **Pedigree:** this is the same shape of decision Ardour, Reaper (WDL), and
   Bitwig made — own your engine, license it as you see fit.

## Timeline impact (rough, honest)

Relative to the original JUCE+Tracktion plan:

| Phase | Original estimate | Rebuild estimate | Delta |
|---|---|---|---|
| Phase 0 (skeleton) | done (JUCE) | **redo** (~1 wk) | +1 wk |
| Phase 1 (engine: graph + clips) | 2 mo | 3 mo (grow our graph + latency comp) | +1 mo |
| Phase 2 (editing NLE) | 2 mo | 2.5 mo (custom timeline widgets) | +0.5 mo |
| Phase 3 (plugins) | 2 mo | **5-6 mo** (write the host from scratch) | **+3-4 mo** |
| Phase 4 (markers) | 2 mo | 2 mo | 0 |
| Phase 5 (video playback) | 2 mo | 2.5 mo | +0.5 mo |
| Phase 6 (video NLE) | 4 mo | 4 mo | 0 |
| Phase 7 (polish/1.0) | 3 mo | 3 mo | 0 |
| **Total** | ~17 mo | **~20-21 mo** | **+~4 mo** |

The plugin host is the dominant added cost (~3-4 months). Everything else is
incremental. If plugin hosting could be deferred past 1.0 (ship a 1.0 that only
hosts CLAP, add VST3/AU in 1.1), the delta shrinks to ~+2 months.

## Phased rebuild (replaces the old Phase 0-7)

### RB-0: Permissive skeleton (replaces Phase 0)
- CMake: drop JUCE/Tracktion submodules; add miniaudio, ImGui (docking branch),
  PortMidi, nlohmann/json, Catch2 as submodules/fetches.
- App: ImGui + OpenGL3 backend window, miniaudio callback driving our existing
  `CompiledGraph` + `SineNode` (these survive — they're already ours).
- **Deliverable:** window opens, miniaudio device @ 48k, sine plays through our
  graph. License-clean.

### RB-1: Engine + clip playback
- Grow `CompiledGraph`: real topological sort, summing node, gain/pan node,
  send/return node, channel remap.
- **Latency compensation** (the hard part) — a `LatencyNode` + AudioFifo.
- `AudioClipNode` — plays a WAV/AIFF via dr_libs + a streaming reader, with
  sample-accurate start/length and fades.
- `Transport` — play/stop/seek, `TimeInfo` threaded through ProcessContext.
- Unit tests (Catch2) for graph correctness; RT-safety assertions.

### RB-2: Editing + UI
- ImGui timeline widget: tracks, clips, drag/trim/split, scroll/zoom, beat-snap.
- Peak mipmap + FBO waveform renderer (OpenGL) for dense waveforms.
- Command pattern + undo journal (ours, from the original plan).
- Document model: content-addressed JSON, atomic saves, crash recovery.
- Mixer panel (faders derived from the graph).

### RB-3: Plugin host (the long pole)
- VST3 host: scanner (out-of-process), loader, processor wrapper, editor window
  embedding (HWND/NSView into ImGui), param normalization. Against VST3 SDK.
- AU host (macOS only): equivalent against AudioUnitSDK.
- CLAP host: against CLAP SDK (cleanest of the three).
- Format-agnostic `ParamId` layer for automation/MIDI-learn.
- **Defer option:** ship CLAP-only in 1.0, VST3/AU in 1.1 — cuts ~2 months.

### RB-4: Markers (your strength — parallelize)
- Multi-track markers, all position modes (sample/SMPTE/musical/clip-anchored).
- Marker-aware editing commands.
- Import/export (CSV, Avid XML, FCPXML, OSC, DAWproject).

### RB-5: Video playback
- FFmpeg (LGPL) decode, frame cache, A/V sync (audio master), preview panel.
- ImGui video display via GL texture from decoded RGBA.

### RB-6: Video NLE
- Video tracks, transitions, GPU compositor, offline render/export (OpenH264).

### RB-7: Polish / 1.0
- Theming, a11y, crash (Crashpad), packaging (mac notarize, Win installer),
  DAWproject interchange, perf pass, docs.

## Key decisions to make before starting RB-0

1. **CLAP-only 1.0 or full VST3+AU+CLAP?** Deferring VST3/AU saves ~2 months
   but limits early usefulness (most users have VST3 plugins). Your call.
2. **ImGui docking branch commit pin** — we depend on a beta branch. Accept the
   risk (most do) or stay on master docking-less (much worse UX). I recommend
   pin a known-good commit.
3. **Multi-threaded graph player in RB-1 or later?** Single-threaded first is
   simpler; MT comes when CPU budget demands it. Recommend single-threaded to
   start, MT in RB-7 polish.
4. **Keep the Phase 0 commit history?** It's JUCE-based and will be largely
   rewritten. Options: (a) keep it for archaeology, (b) squash/start fresh on a
   new main. I'd keep it — it documents the contract proof — but it's cosmetic.

## Recommendation

Do the rebuild. The commercial option is worth +4 months and full ownership,
given that you explicitly want it. The single most important risk is the plugin
host (RB-3); if we de-scope VST3/AU from 1.0, the rebuild delta drops to ~+2
months and we still ship a usable, dual-licensable DAW with CLAP support.
