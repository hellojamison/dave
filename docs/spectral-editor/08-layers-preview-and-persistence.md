# Spectral Phase 08 — Layers, Preview, and Persistence

## Objective

Add a spectral layer stack and responsive region preview while preserving
deterministic project portability and ordinary Dave playback.

## Entry gate

- Phase 07 renders are sample-aligned and project-safe.
- The spectral sidecar format has an explicit migration/version policy.

## Layer model

- Add ordered `SpectralLayer` objects with stable IDs, name, color, visibility,
  mute, solo, polarity, gain, channel scope, operation stack, and optional
  group parent.
- Define additive and subtractive behavior mathematically. Muting every edit
  layer must recover the original source.
- Separate spectral layers from Dave mixer tracks and buses. Provide explicit
  `Render Layer to New Track` and `Render Mix to Clip` actions instead of
  conflating the two models.
- Refuse dangling group/layer references and preserve stable IDs through
  reorder, undo, redo, save, load, and Save As.

## Preview

- Add a background preview renderer for the current loop/selection and a
  bounded preview cache keyed by source revision, layer state, range, and
  config.
- Preview publication to playback must be immutable and RT-safe.
- If a changed region is not ready, keep playing the last complete preview or
  the original and show that the preview is stale; never mix partially rendered
  revisions.
- Bypass must switch predictably between original and the same sample-aligned
  preview range.
- Cancel obsolete previews aggressively while giving active playback requests
  priority.

## Persistence and recovery

- Save canonical manifests and mask/layer sidecars transactionally.
- Reopen with missing regenerable preview/analysis caches.
- Detect missing canonical sidecars as a visible project error; do not silently
  flatten or discard edits.
- Save As copies or hardlinks canonical sidecars and required derived assets,
  then rebases in-memory paths.
- Add orphan discovery that distinguishes recoverable canonical work from
  deletable caches and temporary renders.

## Tests

- Layer sum, mute, solo, polarity, gain, grouping, and ordering.
- Stable identity under lifecycle commands.
- Preview revision atomicity and rapid-edit cancellation.
- Bypass null/alignment and loop-boundary continuity.
- Missing cache recovery versus missing canonical-sidecar failure.
- Interrupted save, Save As collision, relocation, and orphan classification.
- Bounded preview memory and no RT allocations/I/O.

## Acceptance

- Multiple spectral layers can be edited, auditioned, bypassed, rendered to a
  clip or separate Dave tracks, saved, reopened, and moved to a new bundle.
- Playback never consumes half of a newly rendered revision.
- Project recovery explains what is missing and never silently changes sound.
- Focused layer/preview/persistence suites, full headless suite, app link build,
  `git diff --check`, inspected screenshots, and live audition pass.

## Stop conditions

Stop if spectral layers create a second routing graph, if preview swaps are not
sample-aligned and atomic, or if canonical and cache files cannot be reliably
distinguished during recovery.

## Out of scope

No advanced repair, harmonic/transient selection, inference runtime, or AI
unmixing.
