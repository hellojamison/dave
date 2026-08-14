# Spectral Phase 10 — Intelligent Selections

## Objective

Add algorithm-assisted selections that accelerate manual restoration while
remaining deterministic, inspectable, and editable by the user.

## Entry gate

- Phase 09 restoration operations and the Phase 06 selection algebra are
  accepted.
- Quality fixtures exist for tonal, transient, harmonic, and noisy material.

## Selection tools

- Frequency-track selection that follows a local spectral ridge through time.
- Harmonic selection from a chosen fundamental with tolerance, harmonic count,
  and gap controls.
- Transient selection using spectral flux/onset evidence with adjustable
  pre/post range.
- Similar-pattern selection from a user-defined template region.
- Magic-wand region growth based on time/frequency neighborhood similarity.
- Selection sharpen, expand/contract, and time/frequency feather controls.

Every result becomes the normal editable sparse selection mask. Algorithms may
suggest a mask but do not bypass the document/undo model.

## Interaction requirements

- Display confidence/strength as preview, not as an unexplained binary answer.
- Allow add/subtract refinement with the existing brush and lasso.
- Keep parameter changes cancellable and debounce obsolete jobs.
- Persist accepted masks and parameters; transient previews remain
  regenerable cache.
- Explain when a selection cannot be resolved because signal evidence is weak.

## Tests and evaluation

- Known synthetic fundamentals/harmonics, chirps, vibrato, crossing tones,
  transients, noise bursts, and silence.
- Precision/recall against hand-authored fixture masks where meaningful.
- Coordinate and result invariance across UI zoom and display mip levels.
- Deterministic results at fixed algorithm versions.
- Cancellation, stale-preview rejection, undo/redo, and persistence.
- Bounded memory/time on long selections and pathological dense spectra.
- Listening checks after applying Phase 09 tools through suggested masks.

## Acceptance

- Assisted tools materially reduce manual selection work on the accepted corpus
  without hiding or locking the generated mask.
- Results are versioned and deterministic or explicitly baked into canonical
  mask data.
- No algorithm executes on the RT or drawing thread.
- Focused algorithm/UI tests, full headless suite, app link build,
  `git diff --check`, inspected screenshots, and documented user evaluation
  pass.

## Stop conditions

Stop if a tool's apparent quality depends on display pixels, if its result
cannot be represented by the normal selection model, or if false selections
cannot be corrected manually.

## Out of scope

No neural inference, stem separation, dialogue reconstruction, or automatic
application of restoration without user confirmation.
