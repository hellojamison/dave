# Spectral Phase 07 — Render and Derived Assets

## Objective

Turn spectral operations into sample-aligned audio and integrate the result as
a normal, nondestructive Dave asset and clip revision.

## Entry gate

- Phase 06 document edits and masks are stable across reopen and undo/redo.
- Phase 03 reconstruction gates remain green with no-op masks.

## Implementation changes

- Add a cancellable offline render job that reads source pages, applies ordered
  spectral operations, performs iSTFT overlap-add, and writes through Dave's
  hardened WAV/RF64 path.
- Extend render regions beyond edit boundaries by the exact analysis context
  required for seamless overlap-add, then trim to the requested source range.
- Hash the finalized file on the worker thread and register it as a
  content-addressed `AudioAsset` only after successful close and validation.
- Add an undoable `CommitSpectralRenderCommand` that keeps source identity,
  timeline position, source offset, length, fades, gain, routing, and stable
  clip identity consistent.
- Define whether commit replaces the selected clip revision or creates a new
  take/alternate; make the choice explicit in the UI.
- Keep failed/cancelled renders out of the Edit and expose recoverable cleanup
  for temporary files.
- Reuse existing streaming playback for the derived asset.
- Record source spectral document/revision provenance on the derived asset.

## Public interfaces

- `SpectralRenderRequest`
- `SpectralRenderResult`
- `SpectralRenderService`
- `CommitSpectralRenderCommand`
- Derived-asset provenance fields

## Tests

- No-op render nulls against the source within the Phase 00 numerical gate.
- Exact sample count, channel count, sample rate, timeline placement, source
  offset, fades, and boundary handling.
- Edited-region boundaries have no clicks or gain discontinuity.
- Partial-region render agrees with the equivalent full render.
- Cancel, disk-full, short-write, close failure, hash failure, and missing
  source produce no document mutation.
- Undo/redo preserves stable clip and asset identities.
- Save As copies required derived assets and keeps provenance valid.
- Long render memory remains bounded and RF64 promotion remains readable.

## Acceptance

- A gain/erase edit can be rendered, auditioned through Dave's existing graph,
  undone, redone, saved, reopened, and Save-As copied without touching source.
- Failed render leaves the previous playable graph and clip unchanged.
- Render progress and errors remain responsive while playback continues.
- Focused render/project/undo suites, full headless suite, app link build,
  `git diff --check`, and a live audible before/after/null check pass.

## Stop conditions

Stop if rendered audio shifts by even one unexplained sample, if failed output
can enter the document, or if Save As can leave a derived clip pointing into
the old bundle.

## Out of scope

No low-latency live spectral synthesis, spectral layers, repair tools, or ML.
