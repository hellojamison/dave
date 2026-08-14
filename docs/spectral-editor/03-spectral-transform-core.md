# Spectral Phase 03 — Spectral Transform Core

## Objective

Add a deterministic, channel-independent STFT/iSTFT library with proven
reconstruction and bounded chunk processing. This phase has no production UI.

## Entry gate

- Phases 00–02 are accepted.
- The FFT strategy and license record from Phase 00 are still valid at the
  exact revision being integrated.

## Implementation changes

- Introduce an FFT backend abstraction so platform optimization does not leak
  into spectral document or UI code.
- Add real-input forward transforms and inverse transforms for the approved FFT
  sizes.
- Implement explicit analysis/synthesis windows and overlap-add
  normalization. Do not assume library-specific scaling.
- Represent complex bins with a stable internal type and documented DC/Nyquist
  conventions.
- Process bounded frame chunks from `AudioSourceReader`; never require a whole
  clip buffer.
- Define deterministic zero-padding and boundary behavior at source head/tail
  and selection boundaries.
- Support independent mono, stereo, and multichannel analysis.
- Add magnitude/dB conversion utilities for visualization without discarding
  the complex data required for reconstruction.
- Record an algorithm version used later in cache keys.

## Public interfaces

- `FftBackend`
- `SpectralConfig`
- `ComplexBin`
- `StftAnalyzer`
- `StftSynthesizer`
- `SpectralFrame`
- `spectralAlgorithmVersion()`

## Tests and benchmarks

- Phase 00's full procedural corpus at all supported FFT/hop/window settings.
- Round-trip alignment and SNR, including odd lengths and sub-window regions.
- Impulse position and amplitude preservation.
- Stereo channel isolation and multichannel independence.
- DC, Nyquist, denormal, NaN, infinity, clipping, and silence behavior.
- Chunk-size invariance: different worker chunk sizes produce the same result.
- Deterministic output across repeated runs on one platform.
- Benchmarks for analysis and synthesis throughput on supported FFT sizes.
- Sanitizer runs and fuzzed configuration rejection.

## Acceptance

- Every supported configuration meets Phase 00's numerical reconstruction
  gates.
- Processing memory is bounded by configured chunk and FFT sizes, not duration.
- Unsupported configurations fail before allocating large buffers.
- Backend licensing and notices are added correctly.
- Focused DSP suite, repeated benchmarks, full headless suite, app link build,
  and `git diff --check` pass.

## Stop conditions

Stop if no window/hop combination meets the agreed reconstruction target, if
cross-platform scaling differs without an explicit normalization fix, or if
the dependency audit changes.

## Out of scope

No persistent analysis cache, GPU textures, selections, editing masks, preview,
or document persistence.
