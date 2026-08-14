# Spectral Phase 06 — Selections and Basic Editing

## Objective

Add persistent time/frequency selections and the first nondestructive spectral
operations: gain, attenuation, and erase.

## Entry gate

- Phase 05 is visually and headlessly accepted.
- Coordinate transforms and analysis configuration identity are stable.

## Document and sidecar model

- Introduce stable `SpectralDocumentId`, `SpectralLayerId`, `SpectralSelectionId`,
  and `SpectralOperationId` values.
- Attach a spectral document to a source asset/clip without changing the source
  asset.
- Store compact metadata in `project.json`; store large raster masks in
  checksummed, versioned binary sidecars.
- A selection is not automatically an edit. Selection undo and edit undo must
  remain understandable and merge only when explicitly intended.
- Use sparse tiled masks and copy-on-write revisions; never snapshot a complete
  multigigabyte spectral buffer for undo.

## Tools

- Rectangular time/frequency selection.
- Freehand lasso.
- Configurable soft selection brush.
- Replace, add, subtract, and intersect selection modes.
- Feather/fade selection boundaries.
- Clear, invert, and select-all within clip/source bounds.
- Apply gain, attenuation, or full erase to the selected bins.

All tools operate in source-sample/frequency coordinates so zoom level and tile
resolution do not change the result.

## Interaction requirements

- Show tool cursor, radius, feather, and operation preview before commit.
- One pointer gesture creates one undo entry and one document notification.
- Escape cancels an in-progress gesture without mutating the document.
- Tool changes during a gesture cancel safely.
- Channel mode is explicit: current channel, linked channels, or all channels.
- Edits outside the clip's used source range are refused or clearly scoped; do
  not silently modify unrelated uses of the same source.

## Tests

- Coordinate-independent selection masks across zoom and tile levels.
- Rectangle/lasso/brush geometry, feathering, and boolean modes.
- Channel isolation and linked-channel behavior.
- One notification and one undo step per gesture.
- Stable IDs across undo/redo and save/load.
- Sparse sidecar corruption and missing-sidecar errors.
- Gain/erase bin math, including DC and Nyquist.
- Cancelling or switching tools leaves no partial edit.
- Memory usage scales with touched tiles, not source duration.

## Acceptance

- A user can select a noise or tone and nondestructively attenuate it.
- Reopening the project reproduces the same selection and edit geometry.
- Clearing regenerable analysis caches does not remove edits.
- Screenshots show every tool and state at minimum and normal sizes and are
  individually inspected.
- Focused document/DSP/UI suites, full headless suite, app link build, and
  `git diff --check` pass.

## Stop conditions

Stop if editing depends on screen pixels, full-buffer copies, or mutable source
files, or if an undo can restore metadata without restoring its sidecar state.

## Out of scope

No audio render back to the timeline, real-time preview, spectral layer mixer,
repair interpolation, intelligent selection, or AI.
