# Spectral Phase 09 — Restoration Tools

## Objective

Make the editor genuinely useful for dialogue and post-production restoration
without machine-learning separation.

## Entry gate

- Phase 08 layer, preview, render, undo, and persistence paths are accepted.
- A curated, redistribution-safe restoration corpus and objective metrics are
  defined before tuning algorithms.

## Tools

Implement as independent, testable offline modules that consume a selection and
produce a new operation/layer revision:

1. Frequency repair using surrounding time/frequency context.
2. Clone/transfer from an explicitly selected source region.
3. Spectral attenuation brush with pressure/strength accumulation.
4. Noise-print capture and stationary spectral denoise.
5. Broadband transient/click interpolation.
6. Deplosive processing focused on low-frequency short-duration energy.
7. Hum removal with fundamental and harmonic controls.

Each module must expose deterministic parameters, preview, bypass, cancellation,
progress, presets, and a clear statement of affected channels/range.

## Quality rules

- Never label interpolation as recovered original audio.
- Default settings must avoid musical-noise artifacts and phase discontinuity.
- Module output must be bounded and finite for pathological input.
- Preserve ambience outside the selection and crossfade processed boundaries.
- Keep processing sample-aligned and independent of UI zoom.
- Store module parameters and algorithm version for deterministic reopen or
  explicit migration.

## Tests and evaluation

- Synthetic tones/noise/transients with known removable components.
- Dialogue fixtures containing hum, click, plosive, stationary noise, and room
  tone, with redistribution rights recorded.
- Objective residual, distortion, boundary, channel-leakage, and peak metrics.
- No-op/bypass null tests.
- Parameter extremes, silence, clipping, NaN, tiny selections, and EOF.
- Repeatability, cancellation, undo/redo, save/load, and stale preview.
- Blind listening protocol comparing source, processed, and reference where a
  reference exists; retain results as evidence, not as unit tests.

## Acceptance

- Every tool passes objective safety/alignment tests and documented listening
  acceptance on representative dialogue.
- Tool failure keeps the previous layer revision and reports an actionable
  error.
- Presets are versioned, portable, and contain no machine-specific paths.
- Focused DSP tests repeated under sanitizers, full headless suite, app link
  build, `git diff --check`, inspected UI screenshots, and live listening pass.

## Stop conditions

Stop a tool rather than shipping it if blind listening consistently prefers
the untreated source, if it produces unstable output, or if its result cannot
be reproduced from saved parameters and canonical data.

## Out of scope

No ML inference, source separation, speech reconstruction, transcription, or
claims of forensic authenticity.
