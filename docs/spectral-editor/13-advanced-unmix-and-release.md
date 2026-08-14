# Spectral Phase 13 — Advanced Unmix and Release Hardening

## Objective

Expand only after the D/M/E module proves useful, then harden the entire
spectral program for supported-platform release.

## Entry gate

- Phase 12 quality, licensing, packaging, project, and live acceptance gates
  are complete.
- Real user sessions identify which additional module is worth its permanent
  maintenance and download cost.

## Module sequencing

Evaluate and ship modules one at a time, each with a separate model audit,
corpus, metrics, listening review, resource budget, documentation, and rollback
plan. Candidate order for Dave's post-production focus:

1. Noisy speech separation or dialogue isolation.
2. Multiple-voice separation.
3. Crowd-noise separation.
4. Music stem separation.
5. Drum or arbitrary-instrument separation.
6. Speech enhancement or reconstruction only with careful disclosure.

Do not ship a checklist of weak modules. A smaller set of reliable post tools
is preferable to nominal feature parity.

## Product hardening

- Batch queue with resource-aware concurrency, pause/resume, retry, and
  deterministic output naming.
- Presets with schema versions and migration.
- Model/package update rollback and projects pinned to output provenance.
- Keyboard-accessible spectral tools and full high-DPI/minimum-size review.
- Cache/model/disk usage management and understandable cleanup categories.
- Crash recovery for jobs, manifests, sidecars, previews, and finalized outputs.
- Performance profiling across minimum/recommended Mac and Windows hardware.
- Long-duration, high-sample-rate, multichannel, Unicode-path, removable-drive,
  network-volume, and low-disk-space testing.
- User documentation for destructive-sounding operations, limitations,
  artifacts, privacy, model packages, and project portability.
- Release packaging, notices, SBOM, reproducible model hashes, and vulnerability
  response process.

## Regression and acceptance program

- Golden DSP and model-output suites pinned to exact algorithm/model versions.
- Migration tests for every shipped spectral sidecar and project schema.
- Fuzzing for manifests, tiles, masks, and model metadata.
- Sanitizers and repeated concurrency/RT-safety tests.
- Full headless suite and app/package builds on every supported architecture.
- Screenshot matrix inspected at minimum/normal/Retina sizes and all themes.
- Live long-form post sessions on physical audio hardware.
- Independent license/security review of the final dependency and model set.
- Documented quality review with working examples and known failure cases.

## Completion gate

Dave can call the spectral program complete when a user can move a project
between supported systems, rebuild all regenerable caches, reopen every
canonical edit, audition and render without model availability, and obtain the
same aligned deliverable without violating RT, memory, licensing, or recovery
contracts.

## Stop conditions

Stop adding modules when maintenance, package size, quality variance, or model
rights exceed the demonstrated user value. Do not use feature count as the
release criterion.

## Out of scope

Cloud processing, model training from user audio, forensic-authenticity claims,
or compatibility claims for platforms that have not passed the complete
acceptance program.
