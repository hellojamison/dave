# Dave

**D.A.V.E. — Digital Audio & Video Environment.** An open-source, cross-platform
(macOS arm64/x64 + Windows x64) digital audio workstation with a post-production
focus: full video NLE, a first-class marker system, free node-graph routing, and
VST3 / AU / CLAP plugin hosting. GPL-3.0-or-later. See `README.md` for the full
goals and roadmap.

**Status: RB-0 — Permissive skeleton (in progress).** Rebuilding on a
permissive-licensed stack (miniaudio + ImGui + our own graph/host) to keep the
commercial licensing option open. The JUCE-based Phase 0 is superseded; our
`Node`/`CompiledGraph`/`SineNode` survive into RB-0. See
`docs/rebuild-plan-permissive.md`.

Toolchain: CMake ≥ 3.25, C++20, Apple Clang 15+ (Xcode 16+) on macOS, MSVC 2022
on Windows. This repo was created on an Apple Silicon Mac.

## Repo map

- `src/` — application source. Layered (see `docs/architecture.md`):
  - `src/platform/` — thin JUCE wrappers (audio/MIDI device, window, GL).
  - `src/engine/` — real-time audio engine. `graph/` (Node, CompiledGraph,
    scheduler), `nodes/` (built-in node types), `midi/`, `plugins/`, `video/`.
  - `src/document/` — Edit model, JSON serialization, content-addressed assets,
    crash recovery, DAWproject interchange.
  - `src/markers/` — marker model, import/export, behaviors.
  - `src/editing/` — command pattern, transactions, undo journal.
  - `src/application/` — app, transport, view-models, commands.
  - `src/gui/` — JUCE Components, theming, OpenGL.
- `third_party/` — git submodules: miniaudio, imgui (docking branch), PortMidi,
  implot, imgui-node-editor, signalsmith-stretch, dr_libs, nlohmann_json,
  Catch2, vst3sdk, clap, Crashpad. FFmpeg/OpenH264 linked as system/dynamic
  libs, not vendored as source. Populated during RB-0.
- `tests/` — Catch2 unit tests + headless engine harness (not yet present).
- `packaging/` — macOS .app bundle + notarization, Windows installer (later).
- `docs/` — `architecture.md` (the full design), contributing guide.
- `Project Notes/` — Obsidian notes vault (installed via notes-graph-kit).

## Commands

- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Debug`
- Build: `cmake --build build`
- Run (macOS): `./build/Dave` (or the .app bundle once packaging lands)
- Test: `ctest --test-dir build`
- Notes (after notes-graph-kit install):
  - `npm run notes:route -- "<task>"`
  - `npm run notes:new -- --title "<title>" --process <alias> --summary "<goal>"`
  - `npm run notes:closeout -- --note "<path>" --working "..." --verified "..."`
  - `npm run notes:validate`

## Testing and verification

- **Phase 0 verification is build + launch + audible sine.** No automated tests
  exist yet. The RT-safety CI (TSan/ASan asserting no allocs/locks on the audio
  thread) is a Phase 1+ goal, not yet wired up.
- The audio thread contract is the most important invariant in the codebase.
  Anything that allocates, locks, or does I/O on the RT thread is a bug.
  See `src/engine/graph/` and `docs/architecture.md` §1.

## Hard rules and gotchas

- **License hygiene: permissive-only.** Dave is built to be dual-licensed
  (GPL-3.0+ now, closed/commercial option preserved for later). Therefore
  **every dependency must be MIT/BSD/Apache/ISC or LGPL (dynamic-link only)**.
  GPL/AGPL dependencies are FORBIDDEN — they can never be relicensed and would
  kill the commercial option permanently. Before adding ANY dependency, verify
  its SPDX license. See `docs/architecture.md` §Foundational decision and
  `docs/rebuild-plan-permissive.md`.
- **Do NOT add JUCE or Tracktion Engine.** Both are GPL-family and were rejected
  for the licensing reason above, despite being the obvious technical choice.
  Do not "just use JUCE for X" — it's not an option here.
- **Do not embed or link openDAW code.** openDAW (AGPL-3.0) is architectural
  inspiration only. No code, no fork, no shared format. We borrow ideas, not source.
- **openDAW the project is unrelated to Dave.** Do not confuse them; the names
  are similar but the projects are independent.
- **VST3 SDK is MIT as of October 2025** (Steinberg relicensed) — no fee, no
  signed agreement. CLAP is MIT. Audio Unit is a platform SDK (link, don't
  redistribute). All three are hostable in a permissively-licensed app.
- **FFmpeg must be an LGPL-only build** (no `--enable-gpl`, no libx264/libx265).
  H.264/H.265 encode uses OpenH264 (BSD) instead. Dynamic-link FFmpeg.
- **Never allocate on the RT thread.** Pre-allocate all buffers in
  `prepareToPlay`. Use lock-free SPSC queues for UI↔RT communication.
- The text-based `project.json` document format is intentional for VCS-friendliness
  — do not "optimize" it into a binary blob without discussion.

## Project notes

<!-- Installed by notes-graph-kit. See the `## Project Notes Graph` section that
     the installer appends below, and `Project Notes/_Codex/Start Here.md` as the
     graph entrypoint. -->

Notes are a map, not proof — reverify stale facts against the repo, build, or
runtime before relying on them.

## Skills

<!-- Route to obsidian-markdown / obsidian-bases skills for notes work, if
     available in this environment. -->
