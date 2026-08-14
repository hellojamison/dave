# Spectral Phase 11 — ML Runtime and Model Governance

## Objective

Create an optional, license-safe, versioned inference platform before shipping
any AI unmixing feature.

## Entry gate

- Phases 00–10 are accepted.
- Product leadership explicitly approves distributing optional model packages.
- Legal/licensing review is available for runtime and model artifacts.

## Governance first

For every runtime and model package, record:

- Exact source and immutable version/hash.
- Code license and required notices.
- Weight license, redistribution, modification, and commercial-use rights.
- Training dataset disclosure and any known downstream restrictions.
- Patent or export-control statements where applicable.
- Supported architectures, providers, precision, memory, and expected speed.
- Package size, update channel, rollback policy, and end-of-life plan.
- Input-data privacy: inference must be local unless a later plan explicitly
  authorizes a network service.

Reject packages whose rights are ambiguous. An MIT inference repository does
not make separately licensed weights safe.

## Runtime implementation

- Keep inference optional and outside the core Dave app bundle where practical.
- Add signed/checksummed model manifests and resumable user-initiated downloads.
- Verify package integrity before loading and prevent path traversal or
  executable payload substitution.
- Add provider abstraction for CPU and approved hardware acceleration on macOS
  and Windows, with deterministic fallback and visible capability reporting.
- Run inference in a cancellable worker/process boundary that cannot block audio
  or corrupt the Edit if it crashes.
- Bound input chunk, overlap, model memory, output staging, and concurrent job
  counts.
- Store model ID/version/provider/parameters as output provenance.
- Add remove-model and clear-model-cache actions that never remove project
  outputs.

## Public interfaces

- `ModelManifest`
- `ModelPackageStore`
- `InferenceProvider`
- `InferenceRequest`, `InferenceProgress`, and `InferenceResult`
- `ModelAuditRecord`
- GUI-only model management view model

## Tests

- Manifest validation, hash mismatch, interrupted download, rollback, removal,
  unsupported architecture, and disk-full handling.
- Malicious paths and malformed metadata.
- Provider selection and deterministic CPU fallback.
- Worker crash, cancellation, app shutdown, and stale result rejection.
- Hard RAM/concurrency budgets.
- Output provenance survives save/load and Save As.
- Offline operation after an installed package has been verified.

## Acceptance

- At least one tiny redistribution-safe test model exercises the complete
  package/runtime path; it need not perform production unmixing.
- No production model ships without a completed audit record.
- Removing a model does not make already rendered project audio unplayable.
- Focused security/concurrency tests, full headless suite, app link builds on
  macOS and Windows, `git diff --check`, and package-notice review pass.

## Stop conditions

Stop if commercial redistribution of the selected runtime or weights is not
explicit, if inference can escape resource limits, or if model removal can
invalidate rendered project deliverables.

## Out of scope

No production separation model, cloud inference, telemetry, training pipeline,
or automatic background model download.
