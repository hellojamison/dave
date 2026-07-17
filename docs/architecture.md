# Dave — Architecture

This document is the source of truth for Dave's design. It expands the plan in
the root README. When code and doc disagree, trust the code, then fix the doc.

## Threading model (the most important contract)

```
┌─────────────────────────────────────────────────────────────┐
│ UI / Message thread (1)                                     │
│ - All JUCE Components, user input, edit commands.           │
│ - The ONLY thread that mutates the Edit document.           │
└───────────┬─────────────────────────────────────────────────┘
            │ lock-free SPSC queues (param ramps, state)
            ▼
┌─────────────────────────────────────────────────────────────┐
│ Real-time audio thread (1) — THE RT THREAD                  │
│ - Runs the compiled node graph, fixed block size.           │
│ - NEVER allocates, locks, does I/O, or calls the OS.        │
└────┬───────────────┬────────────────────────────────────────┘
     │               │
     ▼               ▼
┌────────────────┐ ┌─────────────────────────────────────────┐
│ Disk I/O pool  │ │ Video decode pool (N threads)           │
│ streams audio  │ │ decodes frames ahead of the playhead;   │
│ pre-empted by  │ │ pushes RGBA to RT via ring buffer.       │
│ priority       │ │ RT never blocks on either.               │
└────────────────┘ └─────────────────────────────────────────┘
```

### RT iron rules (enforced by CI sanitizer runs)

1. No `malloc` / `new` / `delete`. Pre-allocate every buffer in `prepareToPlay`.
2. No contended locks. Only atomics and single-producer/single-consumer queues.
3. No syscalls — no file I/O, no `printf`. Log via an SPSC ring drained on the
   UI thread.
4. Bounded work per block. Worst case is measured in CI.

## Layered architecture

```
GUI Layer      JUCE Components, OpenGL, theming
Application    commands, undo, view-models
Document       Edit model, serialization, assets, recovery
Engine         RT Graph | Plugins | MIDI/NoteEx | Video pipeline
Platform       JUCE: audio/MIDI devices, window, GL
```

Dependency rule: layers call *down* only. The Engine knows nothing about GUI.
This is what lets the engine run headless in tests.

## Real-time audio engine & node graph

Two-phase execution keeps the RT thread cheap:

1. **Compile (UI thread, on every graph edit):** validate, cycle-detect,
   topologically sort, allocate buffer pools, produce an immutable
   `CompiledGraph`.
2. **Render (RT):** walks the compiled order. No decisions, no allocation. The
   RT thread swaps to a new `CompiledGraph` at a block boundary via an atomic
   pointer.

Cycles: zero-delay loops are forbidden (reported as a routing error). Delayed
feedback is allowed via explicit **feedback nodes** that introduce a one-block
delay — the Bitwig trick for glitch-free feedback networks.

### Node interface

```cpp
struct ProcessContext {
    int numSamples;
    double sampleRate;
    AudioBus in, out;          // multi-channel, pre-allocated
    MidiEventList midiIn, midiOut;
    ParamRamp params;          // sample-accurate parameter changes
    TimeInfo time;             // samplePos, bpm, ppqPosition, isPlaying…
};

struct Node {
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    virtual void process(ProcessContext&) = 0;   // RT-safe only
    virtual void release() = 0;
};
```

Built-in node types (filled in over the phases): `audioClip`, `midiClip`,
`plugin`, `instrument`, `trackSum`, `bus`, `send`, `gain/pan`, `splitter`,
`merger`, `feedback`, `hardwareIn`, `hardwareOut`, `freeze`.

## Phasing

Phase 0 (now) proves the contract with the thinnest possible slice: a `SineNode`
driven by a `CompiledGraph`, rendered by a JUCE `AudioSource` on the audio
thread, with a window. No Tracktion Edit yet — that enters in Phase 1 once the
graph is proven.
