# Spectral Phase 12 — Unmix Soundtrack MVP

## Objective

Ship Dave's first production ML module: separate a selected soundtrack region
into Dialogue, Music, and Effects results suited to post-production workflows.

## Entry gate

- Phase 11 runtime, package security, resource limits, and model audit are
  accepted on macOS and Windows.
- The chosen D/M/E model weights have explicit redistribution and commercial
  rights.
- A legally usable evaluation corpus and blind-listening protocol are approved.

## Product behavior

- Operate on a selected clip source range or explicit timeline selection.
- Show estimated resource requirements before starting.
- Run offline with progress, cancellation, and a clearly named model/version.
- Create three new spectral layers and optionally render them to three new Dave
  audio tracks routed through Main.
- Preserve exact source/timeline alignment, channels, and total duration.
- Keep the original clip unchanged and grouped with outputs for comparison.
- Provide solo, mute, recombine, bypass, and one-step undo of the committed
  result.
- Expose a small set of meaningful quality controls; do not leak raw model
  tensors or invent parameters that the model does not support.
- Warn that separation is estimation and may contain artifacts or leakage.

## Processing requirements

- Chunk long inputs with model-appropriate overlap and deterministic edge
  blending.
- Resample only inside the worker and return to the source/session rate with
  measured alignment.
- Preserve multichannel policy explicitly. If the model is stereo-only, define
  and test downmix/upmix behavior instead of silently discarding channels.
- Normalize only when explicitly requested; recombined stems should retain the
  model's natural relationship and never clip through hidden gain changes.
- Hash and validate finalized output files before document mutation.

## Evaluation

- Objective separation metrics where valid, plus peak, loudness, phase,
  alignment, leakage, and recombination measurements.
- Blind listening across clean dialogue, music-heavy, effects-heavy, reverberant,
  noisy, clipped, mono, stereo, and long-form material.
- Compare at least one established external reference offline, without copying
  its code, assets, or UI.
- Record accepted limitations and failure examples in user documentation.

## Tests

- Range and channel mapping, chunk-boundary continuity, resample alignment, and
  exact output lengths.
- Cancellation, crash, model removal, disk-full, corrupt output, and stale
  project revision.
- Stable layer/track IDs through undo/redo and Save As.
- No document mutation until all required outputs finalize successfully.
- Repeated inference/resource tests and RT isolation while playback continues.

## Acceptance

- D/M/E output meets the approved quality threshold on the owned corpus and is
  judged useful in documented post-production listening sessions.
- Rendered tracks reopen and play without the model installed.
- Model provenance and licensing notices ship with the package.
- Focused inference/project suites, full headless suite, app link/package builds
  on macOS and Windows, `git diff --check`, inspected screenshots, and separate
  live acceptance pass.

## Stop conditions

Stop rather than ship if rights remain ambiguous, dialogue intelligibility is
regularly damaged, chunk seams are audible, outputs shift in time, or partial
results can enter the Edit.

## Out of scope

No song-stem, drum-stem, multi-speaker, transcription, cloud, or user-trained
models.
