# Contributing to Dave

Thanks for your interest. Dave is early — see the roadmap in `README.md` for what
works today and what doesn't.

## How changes get in

Nobody pushes directly to `main`, including maintainers. Everything goes through a
pull request:

1. Fork the repository and create a branch off `main`.
2. Make your change.
3. Open a pull request describing what it does and how you verified it.

Issues are the right place to propose something before building it. For anything
larger than a bug fix, open an issue first — Dave has strong architectural
constraints (below) and it's better to find out early that an approach won't work.

## Contributor License Agreement

Dave is licensed GPL-3.0-or-later, and the project intends to keep a commercial
relicensing option open. That only works if the project can license contributed
code under other terms, so **every contributor must sign the CLA** before their
first pull request is merged.

Dave uses a [Harmony](https://www.harmonyagreements.org/) contributor **licence
agreement** — not a copyright assignment. Concretely:

- **You keep your copyright.** Ownership of what you write stays yours, and you
  remain free to use your own contribution anywhere else, including in other
  projects or commercially.
- **You grant Dave the right to license your contribution under other terms**,
  including proprietary ones. This is what preserves the dual-licensing option
  described in `README.md`.
- **You grant a patent licence** covering your contribution, so the project can't
  be ambushed later by a patent claim over code that was contributed freely.

We've stated the relicensing permission explicitly rather than leaving it to be
inferred from a broad sublicensing clause. If you'd rather not grant it, please
don't submit code — but issues, testing, reproductions and documentation feedback
are all genuinely welcome and need no agreement at all.

The full text is in [`docs/cla.md`](docs/cla.md). Signing is a one-time comment on
your first pull request; a bot posts the instructions automatically and remembers
you afterwards.

## Building

Requires CMake ≥ 3.25, a C++20 compiler (Apple Clang 15+ / MSVC 2022+), and the
platform SDK (Xcode / Visual Studio).

```
git clone --recurse-submodules <your fork>
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/Dave
```

If you already cloned without submodules: `git submodule update --init --recursive`.

To capture the UI without a display grab:

```
./build/Dave --screenshot out.png --frames 25 [--demo-audio file.wav]
```

## Hard constraints

These are not style preferences. A pull request that breaks one of them cannot be
merged, however good it is otherwise.

### Licensing: permissive dependencies only

Every dependency must be MIT / BSD / Apache / ISC, or LGPL **dynamically linked**.
GPL and AGPL dependencies are forbidden — they can never be relicensed, and would
permanently foreclose the commercial option the CLA exists to preserve. Verify a
dependency's SPDX identifier before proposing it, and say what it is in the PR.

In particular: do not add JUCE or Tracktion Engine (both GPL-family), and do not
embed or link openDAW code (AGPL-3.0). openDAW is architectural inspiration only.
FFmpeg must be an LGPL-only build — no `--enable-gpl`, no libx264/libx265.

### The audio thread contract

Nothing on the real-time audio thread may allocate, lock, or perform I/O. This is
the most important invariant in the codebase. If your change touches
`src/engine/`, be explicit in the PR about which thread the new code runs on.

See `docs/architecture.md` §1 and `AGENTS.md` for the full rationale on both.

## Code style

Match the surrounding code. Comments explain *why*, not *what* — the existing
source is a good guide to the expected density and tone.

New source files should carry an SPDX header:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

## Verifying your change

There is no automated test suite yet — that's a known gap, not a standard we're
relaxing for you. Until it exists, verification means saying concretely what you
ran and what you observed:

- `cmake --build build` completes clean, with no new warnings.
- The app launches and audio still starts.
- For UI changes, attach a screenshot (`--screenshot`, above).
- For engine changes, describe the audible or measured result — not the code path
  that ought to produce it.

Claims like "should work" or "looks correct" aren't verification. If you couldn't
test something, say so explicitly; unverified is fine, silently-assumed is not.
