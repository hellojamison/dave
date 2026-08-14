# Spectral Phase 04 — Analysis Cache and Background Jobs

## Objective

Analyze long assets into persistent, corruption-detecting spectral tiles using
cancellable, prioritized background jobs and hard resource budgets.

## Entry gate

- Phase 03 is accepted at the FFT backend revision used here.
- Phase 02 playback has priority and underrun instrumentation available.

## Implementation changes

- Add a bounded worker pool shared by spectral tasks, with priorities below
  active audio prefetch.
- Add cancellable job handles, monotonic progress, structured errors, and clean
  shutdown/join semantics.
- Define a versioned binary tile format containing configuration identity,
  source identity, channel, frame range, dimensions, encoding, checksum, and
  payload length.
- Store full-resolution complex analysis tiles where later editing requires
  them, but keep them regenerable from the immutable source. Store
  display-oriented magnitude mip levels separately so visualization does not
  decode full-resolution complex data.
- Write to a temporary file, flush/close, validate, and atomically publish.
- Detect partial, corrupt, stale, wrong-endian, and wrong-version tiles and
  regenerate them without changing the document.
- Implement memory and OpenGL-upload staging budgets with deterministic LRU
  eviction.
- Expose cache location, size, clear/rebuild controls, and per-job status to
  application code without drawing UI in the service.
- Ensure Save As and project relocation do not make regenerable caches
  canonical project dependencies.

## Public interfaces

- `BackgroundJobService`
- `JobHandle`, `JobProgress`, and `JobError`
- `SpectralTileKey`
- `SpectralTileStore`
- `SpectralAnalysisService`
- `SpectralCacheStats`

## Tests

- Tile round-trip, checksum failure, truncation, version mismatch, and invalid
  dimensions.
- Atomic publication under simulated interruption.
- Cancellation at queue, read, FFT, encode, and write stages.
- Priority test proving playback prefetch is not starved.
- Duplicate analysis request coalescing.
- Tiny RAM/disk-budget eviction with pinned visible tiles.
- Source SHA/config/version changes create distinct keys.
- Multi-thread stress and clean app shutdown with jobs in flight.
- Multi-hour generated source stays within RAM limits.

## Acceptance

- Analysis can be killed at any point without leaving a tile that is treated as
  valid.
- Reopening an unchanged asset reuses valid tiles without reanalysis.
- Deleting the cache loses no user-authored work.
- Playback underrun tests remain green while analysis saturates workers.
- Focused corruption/concurrency suites, full headless suite, app link build,
  and `git diff --check` pass.

## Stop conditions

Stop if any canonical edit data is placed only in the regenerable cache, or if
worker priority cannot prevent analysis from disrupting playback.

## Out of scope

No spectrogram workspace, user selections, spectral modifications, or project
spectral-document fields.
