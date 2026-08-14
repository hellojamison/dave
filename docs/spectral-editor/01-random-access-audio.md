# Spectral Phase 01 — Random-Access Audio and Decoded-Page Cache

## Objective

Replace the assumption that an audio asset must be decoded completely into RAM
with a bounded, worker-thread random-access service. Do not change RT playback
yet.

## Entry gate

- Phase 00 is accepted.
- The chosen decoder and cache policies are documented and license-approved.
- Current import, hashing, recording, and playback tests are green before edits.

## Implementation changes

- Add an `AudioSourceReader` abstraction that exposes metadata and reads a
  caller-specified frame range into caller-owned planar float buffers.
- Implement WAV first with 64-bit file offsets. Preserve the existing encoded
  file guard until streaming playback is complete.
- Add an `AudioPageCache` keyed by asset SHA, channel, and page index.
- Decode pages only on background workers. UI callers may request and poll;
  they may not block the audio callback.
- Bound decoded RAM with an explicit byte budget and deterministic LRU
  eviction. Pinning must also have a hard limit.
- Coalesce duplicate in-flight requests and prioritize visible/audible ranges.
- Detect source replacement or modification and invalidate pages safely.
- Surface short reads, malformed headers, unsupported encodings, missing files,
  and cancellation as structured errors.
- Keep recorded RF64 files readable through the same abstraction.

## Public interfaces

- `AudioSourceInfo`
- `AudioReadRequest`
- `AudioReadResult`
- `AudioSourceReader`
- `AudioPageKey`
- `AudioPageCache`
- `AudioPageCache::Stats`

Exact names may change during implementation, but the separation between
random-access decode and cache policy must remain testable.

## Tests

- Exact frame reads at start, middle, and EOF.
- Mono, stereo, multichannel, PCM16/24/32, float32, RIFF, and RF64.
- Odd frame counts, empty files, truncated payloads, and malformed headers.
- Requests spanning page boundaries and EOF.
- LRU eviction under a tiny deterministic budget.
- Duplicate request coalescing and cancellation.
- Source modification invalidation.
- Multiple worker threads requesting the same and different assets.
- Peak memory remains within the configured budget for a sparse or generated
  long file.

## Acceptance

- Long-file random reads do not allocate proportional to source duration.
- No API reachable from the audio callback performs file I/O.
- All failures are reported without exceptions escaping worker boundaries.
- Existing import, recording, project, and graph tests remain green.
- Full headless suite, repeated concurrency tests, app link build, and
  `git diff --check` pass.

## Stop conditions

Stop if the decoder cannot seek with 64-bit offsets on both supported
platforms, or if the cache cannot prove a hard upper memory bound.

## Out of scope

No `AudioClipNode` migration, prefetch scheduling from transport, FFT analysis,
spectrogram UI, or project-format change.
