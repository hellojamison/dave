# Dave

**D.A.V.E. — Digital Audio & Video Environment.** An open-source, cross-platform
(macOS arm64/x64 + Windows x64) digital audio workstation with a post-production
focus: full video NLE, a first-class marker system, free node-graph routing, and
VST3 / AU / CLAP plugin hosting. GPL-3.0-or-later. See `README.md` for the full
goals and roadmap.

**Status: Phase 0 — Skeleton.** Not yet functional. The app launches, opens an
audio device, and renders a sine node through our own RT graph to prove the
threading contract. Tracks, clips, plugins, video, and markers do not exist yet.

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
- `third_party/` — git submodules (JUCE, tracktion_engine, clap,
  clap-juce-extensions, rubberband). FFmpeg is linked system/prebuilt, not
  vendored as source. Not yet populated in Phase 0.
- `tests/` — Catch2 unit tests + headless engine harness (not yet present).
- `packaging/` — macOS .app bundle + notarization, Windows installer (later).
- `docs/` — `architecture.md` (the full design), contributing guide.
- `Project Notes/` — Obsidian notes vault (installed via notes-graph-kit).

## Commands

- Configure (first run is slow — fetches/builds JUCE + Tracktion):
  `cmake -B build -DCMAKE_BUILD_TYPE=Debug`
- Build: `cmake --build build`
- Run (macOS): `open build/Dave.app` or `./build/Dave.app/Contents/MacOS/Dave`
- Test: `ctest --test-dir build` (no tests yet in Phase 0)
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

- **License hygiene: GPL-3.0-or-later core.** Do not link GPL-2-only libraries
  (incompatible). LGPL-2.1+ and permissive (MIT/BSD/Apache/ISC) are fine.
  FFmpeg must be linked as shared LGPL libraries; GPL-enabled FFmpeg components
  (libx264/libx265/libfdk_aac) require the whole app to stay GPL-compatible —
  it does, but flag any dependency that would force a conflict.
- **ASIO is opt-in and dynamically loaded**, never statically linked — the
  Steinberg SDK license is not GPL-clean. Keep it behind a runtime dlopen.
- **Do not embed or link openDAW code.** openDAW (AGPL-3.0) is architectural
  inspiration only. No code, no fork, no shared format. We borrow ideas, not
  source. This is a hard licensing boundary.
- **openDAW the project is unrelated to Dave.** Do not confuse them; the names
  are similar but the projects are independent.
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
