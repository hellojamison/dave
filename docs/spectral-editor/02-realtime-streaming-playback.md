# Spectral Phase 02 — Real-Time Streaming Playback

## Objective

Make long audio assets playable without whole-file decode while preserving
Dave's real-time contract and current sample-accurate clip behavior.

## Entry gate

- Phase 01 is accepted with bounded-memory and concurrency proof.
- Existing eager playback behavior has deterministic reference renders.

## Implementation changes

- Replace `GraphBuilder`'s whole-file `assetCache_` decode with persistent
  streaming source handles owned outside graph rebuilds.
- Add a background prefetch coordinator driven by transport position, clip
  source range, playback direction, loop bounds, and expected callback size.
- Publish immutable, preallocated read windows to `AudioClipNode` through a
  proven lock-free handoff.
- Keep graph compilation free of source decoding and long blocking work.
- Preserve clip source offsets, trimming, fades, gain, looping, seeks, sample
  rate behavior, overlapping clips, and routing.
- Define underrun behavior: output bounded silence, increment a visible
  counter, recover alignment on the next available page, and never replay late
  samples at the wrong position.
- Prioritize active playback over waveform, thumbnail, and later spectral jobs.
- Keep a narrow fallback for tiny assets only if benchmarks justify it; one
  implementation path is preferable to divergent semantics.
- Remove the 4 GiB playback refusal only after long-file playback is actually
  safe. Import hashing must also be streaming before making that claim.

## Public interfaces

- `StreamingAudioSource`
- `AudioPrefetchCoordinator`
- `AudioReadWindow`
- `AudioStreamStats`
- A streaming buffer input for `AudioClipNode`

## Tests

- Eager-versus-streamed sample equality across clip boundaries.
- Seek, stop/return, loop wrap, reverse request rejection if reverse playback
  is still unsupported, and rapid scrub request replacement.
- Overlapping clips sharing one source without duplicate unbounded caches.
- Forced slow-reader underruns produce silence at the exact missing timeline
  range and recover without phase shift.
- Long sparse/generated files maintain bounded RSS.
- Graph rebuilds do not reopen or re-decode unchanged assets.
- Audio callback instrumentation proves no allocation, lock, syscall, logging,
  or unbounded retry.

## Acceptance

- A multi-hour generated WAV plays, seeks, and loops with bounded RAM.
- Current short-session renders remain numerically equivalent within the
  recorded tolerance.
- Playback remains audible when lower-priority analysis workers are saturated.
- Underruns are visible in diagnostics and never corrupt transport alignment.
- Full headless suite, repeated RT/concurrency tests, app link build, and
  `git diff --check` pass.
- Separate live macOS playback is performed with short, long, looping, and
  multichannel assets. Windows remains explicitly unverified until run there.

## Stop conditions

Stop if correctness depends on the audio callback waiting for a worker or disk,
or if retiring streaming buffers cannot be proven safe against callback use.

## Out of scope

No spectral analysis, spectral UI, source-format expansion, reverse playback,
or ML inference.
