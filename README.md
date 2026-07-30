# Dave

**D.A.V.E. — Digital Audio & Video Environment**

An open-source, cross-platform digital audio workstation with a focus on
post-production and audio-for-picture: full video NLE, a first-class marker
system, free node-graph routing, and VST3 / AU / CLAP plugin hosting.

[![License: GPL v3](https://img.shields.io/badge/License-GPL_v3-blue.svg)](LICENSE)

## Status

🚧 **Early, but it runs.** macOS only so far. Not ready for real work — test
coverage is thin, and nothing here should be trusted with a session you care
about.

**Works today:** audio playback through our own node graph, with device
selection and a transport. A timeline with tracks, clips, drag editing, peak
waveforms, selection, snapping and undo/redo. VST3 hosting — scanning, loading,
native plugin editor windows, bypass and state save/restore. A multi-track
marker lane with Reaper CSV import/export and loop regions. Video playback with
a preview panel synced to the audio clock, plus a video lane with thumbnails.
Projects save and load as `.dave` bundles with content-addressed assets.

**Not yet:** Windows (untested), automation, video editing, AU and CLAP
hosting, MIDI, and time/pitch. See the roadmap.

## Goals

- **Native** macOS (arm64 + x86_64) and Windows (x64). Linux later.
- **Audio + video** in one timeline — full non-linear video editing, not just
  a reference track. Audio is the master clock.
- **Markers as a first-class system** — multi-track, sample / SMPTE / musical /
  clip-anchored positions, marker-aware editing, cue-list and show-control
  integration.
- **Free node-graph routing** — Reaper/Bitwig-style modular signal flow with
  delayed feedback.
- **Open by default** — GPL-3.0+, text-based diffable project format,
  content-addressed assets, DAWproject interchange.

## Foundation

Built on a **permissively-licensed** stack so the project stays dual-licensable
(GPL-3.0+ now, with a commercial option preserved). See
[`docs/rebuild-plan-permissive.md`](docs/rebuild-plan-permissive.md) for the
rationale and full dependency matrix.

**In use today:**

- [miniaudio](https://github.com/mackron/miniaudio) + dr_wav — audio I/O and WAV decoding.
- [Dear ImGui](https://github.com/ocornut/imgui) + GLFW + OpenGL — GUI.
- **Our own** real-time node-graph engine and plugin host.
- [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) (MIT, Oct 2025) — plugin hosting.
- [FFmpeg](https://ffmpeg.org/) (LGPL-only build, run as a subprocess) — video decode.
- [nlohmann/json](https://github.com/nlohmann/json) — project serialization.
- [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) — file dialogs.

**Planned, not yet integrated:** [PortMidi](https://github.com/PortMidi/portmidi)
(MIDI I/O), [CLAP](https://github.com/free-audio/clap) and Audio Unit hosting,
[Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch)
(time/pitch), libFLAC, OpenH264 (H.264/265 encode),
[DAWproject](https://github.com/bitwig/dawproject) interchange.

## Build

Requires CMake ≥ 3.25, a C++20 compiler (Apple Clang 15+ / MSVC 2022+), and
the platform SDK (Xcode / Visual Studio).

```bash
git clone --recurse-submodules https://github.com/hellojamison/dave.git
cd dave
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/Dave
```

Already cloned without submodules? `git submodule update --init --recursive`.

The app can also capture itself, which is useful for UI work in environments
where screen recording isn't available:

```bash
./build/Dave --screenshot out.png --frames 25 [--demo-audio file.wav]
```

## Contributing

Pull requests welcome — see [`CONTRIBUTING.md`](CONTRIBUTING.md). Two constraints
are worth knowing before you write code: every dependency must be permissively
licensed (no GPL/AGPL, for the reason in the roadmap note above), and nothing on
the audio thread may allocate, lock, or do I/O.

Contributors sign a [CLA](docs/cla.md) — you keep your copyright, and your work
stays available under the project's licence.

## Roadmap

| Phase | Goal | State |
|---|---|---|
| 0 — Skeleton | CMake; window; audio device; sine node through our graph | ✅ |
| 1 — Audio engine | Our node graph drives playback; mixer; transport | ✅ |
| 2 — Editing | Track/clip NLE; commands; undo; peak waveforms | ✅ |
| 3 — Plugins | VST3 hosting; scanner; editor windows; state save | ✅ VST3 only — AU and CLAP pending |
| 4 — Markers | Multi-track markers; import/export; loop regions | 🟡 Reaper CSV in/out; not all position modes; marker-aware editing pending |
| 5 — Video playback | FFmpeg decode; A/V sync; preview; video lane | ✅ |
| 6 — Video NLE | Video tracks; transitions; compositor; render/export | ⬜ |
| — | Automation, MIDI, time/pitch | ⬜ |
| — | Test suite + CI | 🟡 Unit tests + macOS CI; RT-safety checks (TSan/ASan, no allocs or locks on the audio thread) pending |
| 7 — Polish / 1.0 | Theming; a11y; crash reporting; packaging; DAWproject | 🟡 Theming done; rest pending |

The original Phase 0 targeted JUCE and Tracktion Engine. Both were dropped —
they're GPL-family, which would have foreclosed the dual-licensing option this
project is built to preserve. The node graph and plugin host are our own as a
direct result. See [`AGENTS.md`](AGENTS.md) for the constraints that follow from
that decision.

## License

Copyright © 2026 Jamison Rabbe. Licensed under the
[GNU General Public License v3.0 or later](LICENSE).
