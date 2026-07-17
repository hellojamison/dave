# Dave

**D.A.V.E. — Digital Audio & Video Environment**

An open-source, cross-platform digital audio workstation with a focus on
post-production and audio-for-picture: full video NLE, a first-class marker
system, free node-graph routing, and VST3 / AU / CLAP plugin hosting.

[![License: GPL v3](https://img.shields.io/badge/License-GPL_v3-blue.svg)](LICENSE)

## Status

🚧 **Phase 0 — Skeleton.** Not yet functional. See the roadmap below.

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

- [JUCE 8](https://github.com/juce-framework/JUCE) — cross-platform audio I/O,
  plugin hosting, GUI.
- [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) — DAW
  primitives (tracks, clips, transport, automation).
- [FFmpeg](https://ffmpeg.org/) — video decode/encode.
- [CLAP](https://github.com/free-audio/clap) via
  [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions) —
  modern plugin format.
- [Rubber Band](https://github.com/breakfastquay/rubberband) — time/pitch.
- [DAWproject](https://github.com/bitwig/dawproject) — open interchange format.

## Build

Requires CMake ≥ 3.25, a C++20 compiler (Apple Clang 15+ / MSVC 2022+), and
the platform SDK (Xcode / Visual Studio).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Roadmap

| Phase | Goal |
|---|---|
| 0 — Skeleton | CMake + JUCE + Tracktion linked; window; audio device; sine node through our graph |
| 1 — Audio engine | Our node graph drives playback; mixer; transport |
| 2 — Editing | Track/clip NLE; commands; undo; peak waveforms; automation |
| 3 — Plugins | VST3 + AU + CLAP hosting; param layer; scanner; MIDI-learn |
| 4 — Markers | Multi-track markers; all position modes; import/export; marker-aware editing |
| 5 — Video playback | FFmpeg decode; frame cache; A/V sync; preview |
| 6 — Video NLE | Video tracks; transitions; compositor; offline render/export |
| 7 — Polish / 1.0 | Theming; a11y; crash reporting; packaging; DAWproject interchange |

## License

Copyright © 2026 Jamison Rabbe. Licensed under the
[GNU General Public License v3.0 or later](LICENSE).
