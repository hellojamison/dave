# Spectral Phase 00 — Product Contract and Architecture

## Objective

Lock the product boundary and technical contracts before adding an FFT library
or document fields. The deliverable is an implementation-ready architecture,
not a partially connected spectral UI.

## Entry gate

- Read `AGENTS.md`, `docs/architecture.md`,
  `docs/rebuild-plan-permissive.md`, and the program index.
- Inspect the current audio asset, graph publication, project bundle, undo,
  worker-thread, OpenGL, and screenshot paths. Treat older design notes as
  hypotheses until verified against source.
- Preserve any unrelated dirty-tree changes.

## Product boundary

Define the first shippable product as a clip-oriented spectral restoration
workspace for audio post-production:

- Spectrogram navigation and audition.
- Time/frequency selections.
- Gain, attenuation, erase, and basic repair.
- Nondestructive rendering back to an ordinary Dave audio clip.

Explicitly defer AI unmixing, transcription, batch processing, external-editor
integration, 3D display, and feature parity with any commercial editor.

## Required decisions

Write `docs/spectral-editor/architecture.md` covering:

1. Ownership boundaries among document, application, background workers,
   OpenGL UI, disk cache, and RT graph.
2. `SpectralDocument` identity and its relationship to `AudioAsset` and
   `AudioClip`.
3. Canonical versus regenerable data. The source, edit recipe, and required
   masks are canonical; analysis textures and preview renders are caches.
4. Binary sidecar versioning, byte order, checksums, atomic replacement, and
   recovery after interrupted writes.
5. STFT configuration identity: source SHA, channel, sample rate, window, FFT
   size, hop size, and algorithm/cache version.
6. Worker cancellation, progress, priority, and app-shutdown behavior.
7. RT preview publication without locks, allocation, or I/O.
8. CPU/RAM/GPU/disk budgets for minimum and recommended hardware.
9. Maximum supported source length, channel count, sample rate, and FFT size.
10. Failure behavior for missing source, corrupt sidecar, full disk, unsupported
    format, job cancellation, and stale cache.

## Dependency and model policy

- Evaluate at least two FFT implementation strategies against Dave's macOS and
  Windows targets.
- Record SPDX license, copyright notice obligations, commercial-use status,
  architecture support, SIMD behavior, and maintenance state.
- Benchmark representative FFT sizes rather than choosing on reputation.
- Do not add an inference runtime or model in this phase.
- Add a reusable model-package audit checklist covering code, weights, training
  data, patents, redistribution, commercial use, attribution, and privacy.

## Test corpus and budgets

Define a procedurally generated, redistribution-safe corpus containing:

- Impulse at the beginning, middle, and end.
- Silence and denormally small signals.
- Bin-centered and off-bin sine waves.
- Log sweep, chirp, broadband noise, transient train, and clipped samples.
- Mono, stereo with isolated channels, and multichannel fixtures.
- Odd source lengths and regions shorter than one analysis window.

Record numerical gates for reconstruction SNR, alignment, deterministic output,
peak preservation, cache corruption detection, maximum resident memory, and
job cancellation latency.

## Acceptance

- Architecture and dependency-decision documents exist and have no unresolved
  P0/P1 questions.
- Budgets are numerical rather than words such as "fast" or "small."
- Test fixtures can be generated from source without checked-in copyrighted
  audio.
- The plan states which data must survive project transfer and which data may
  be deleted and rebuilt.
- `git diff --check` passes.

## Stop conditions

Stop and request a decision if the FFT options cannot satisfy Dave's licensing
policy, or if the proposed canonical sidecars would make `.dave` bundles
non-portable or silently lossy.

## Out of scope

No production FFT integration, spectrogram rendering, document migration,
audio graph changes, ML runtime, or model download.
