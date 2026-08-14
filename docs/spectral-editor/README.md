# Dave Spectral Editor Program

This folder breaks the built-in spectral editor into ordered implementation
plans. Run one plan at a time, in filename order. Each plan is intentionally
bounded so it can be handed to a fresh Codex task without implicitly
authorizing later phases.

The product goal is a Dave-native spectral restoration workspace for
post-production. It is not permission to copy SpectraLayers code, assets,
branding, proprietary behavior, or model weights.

## How to run the program

1. Start with the first unchecked plan below.
2. Ask Codex to read `AGENTS.md`, this index, and that plan, then implement only
   that plan.
3. Do not skip a plan's entry gate or acceptance gate.
4. Keep unrelated dirty-tree work intact. Nothing is committed or pushed
   unless explicitly requested.
5. After a phase passes, mark it complete here in a separate, explicitly
   requested documentation change.
6. Treat build, headless tests, visual inspection, live audio, and ML quality as
   separate forms of evidence.

## Non-negotiable program rules

- The audio callback never allocates, locks, logs, or performs file I/O.
- Spectral analysis, editing, inference, and rendering run off the real-time
  thread.
- Original audio assets are immutable. Every destructive-sounding operation is
  represented nondestructively and renders to a derived asset.
- Large PCM and spectral data are streamed and tiled; they are never loaded as
  a complete session-sized buffer.
- `project.json` remains text and diffable. Large masks, tiles, and caches use
  versioned binary sidecars.
- Dependencies, model runtimes, model weights, and datasets must preserve
  Dave's commercial relicensing option. GPL/AGPL code and non-commercial model
  licenses are not acceptable.
- Every user mutation has stable identity and undo/redo behavior.
- A failed background job keeps the prior playable result and reports a visible
  error.

## Sequence

- Plans 00–07 deliver the first useful end-to-end spectral editor.
- Plans 08–10 turn it into a mature non-ML restoration workspace.
- Plans 11–13 are the optional AI program and should begin only after the
  non-ML editor proves valuable.

| Status | Plan | Result |
|---|---|---|
| [ ] | [00 — Product contract and architecture](00-product-contract-and-architecture.md) | Scope, budgets, formats, test corpus, and dependency decisions are locked |
| [ ] | [01 — Random-access audio and decoded-page cache](01-random-access-audio.md) | Worker-thread, bounded-memory source access |
| [ ] | [02 — Real-time streaming playback](02-realtime-streaming-playback.md) | Long files play without whole-file decoding or RT I/O |
| [ ] | [03 — Spectral transform core](03-spectral-transform-core.md) | Tested STFT/iSTFT with deterministic reconstruction |
| [ ] | [04 — Analysis cache and background jobs](04-analysis-cache-and-jobs.md) | Persistent tiled analysis with cancellation and progress |
| [ ] | [05 — Spectrogram workspace](05-spectrogram-workspace.md) | Integrated GPU spectral viewer synchronized with Dave |
| [ ] | [06 — Selections and basic editing](06-selections-and-basic-editing.md) | Rectangle, lasso, brush, gain, attenuate, and erase |
| [ ] | [07 — Render and derived assets](07-render-and-derived-assets.md) | Spectral edits render nondestructively into the timeline |
| [ ] | [08 — Layers, preview, and persistence](08-layers-preview-and-persistence.md) | Spectral layers, loop preview, reopen, and crash safety |
| [ ] | [09 — Restoration tools](09-restoration-tools.md) | Repair, clone, noise print, denoise, and deplosive tools |
| [ ] | [10 — Intelligent selections](10-intelligent-selections.md) | Frequency, harmonic, transient, and similarity assistance |
| [ ] | [11 — ML runtime and model governance](11-ml-runtime-and-model-governance.md) | Safe optional inference platform with audited model packages |
| [ ] | [12 — Unmix Soundtrack MVP](12-unmix-soundtrack-mvp.md) | Dialogue, music, and effects separation into Dave channels |
| [ ] | [13 — Advanced unmix and release hardening](13-advanced-unmix-and-release.md) | Broader modules, packaging, performance, and release evidence |

## Program completion definition

The program is complete only when:

- Long-form sessions remain bounded in RAM during playback and editing.
- Spectral edits survive save, close, reopen, Save As, missing-cache recovery,
  and undo/redo.
- Preview and committed renders stay sample-aligned with the original clip.
- Every supported operation has deterministic DSP tests and visible error
  handling.
- The app passes the full headless suite and link build on macOS and Windows.
- Live multichannel playback and representative restoration sessions are
  separately accepted on physical hardware.
- Every shipped dependency and model artifact has a recorded redistribution
  and commercial-use audit.
