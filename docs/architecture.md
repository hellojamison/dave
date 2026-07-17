# Dave — Architecture

This document is the source of truth for Dave's design. When code and doc
disagree, trust the code, then fix the doc.

## Foundational decision: permissive stack (dual-licensable)

Dave is built to be **dual-licensed**: GPL-3.0+ open source now, with a
closed-source commercial option preserved for the future. To keep that option
alive, **every dependency must be permissively licensed** (MIT/BSD/Apache/ISC)
or LGPL (dynamic-link only). GPL/AGPL dependencies are forbidden — they can
never be relicensed and would foreclose the commercial path permanently.

This is why we do **not** use JUCE or Tracktion Engine (both GPL-family),
despite them being the obvious/fastest choice. The tradeoff is documented in
`docs/rebuild-plan-permissive.md`: we own more code (~+4 months to 1.0, dominated
by writing our own plugin host), in exchange for full ownership and licensing
freedom.

## Dependency stack

| Layer | Library | License |
|---|---|---|
| Audio I/O | miniaudio | Unlicense/MIT (single-header) |
| MIDI I/O | PortMidi | MIT |
| GUI | Dear ImGui (docking+viewport branch) + OpenGL3 | MIT |
| Plotting/meters | ImPlot | MIT |
| Routing grid | imgui-node-editor | MIT |
| RT graph engine | **ours** | ours |
| Plugin host | **ours** | ours |
| ↳ VST3 | VST3 SDK (Steinberg) | MIT (since Oct 2025) |
| ↳ CLAP | CLAP | MIT |
| ↳ Audio Unit | AudioUnitSDK (Apple) | platform SDK |
| Time/pitch | Signalsmith Stretch | MIT |
| Audio formats | dr_libs + libFLAC | Unlicense/MIT + BSD |
| Video | FFmpeg (LGPL build) + OpenH264 (encode) | LGPL (dynamic) + BSD |
| JSON | nlohmann/json | MIT |
| Crash | Crashpad | Apache-2.0 |
| Tests | Catch2 | BSL-1.0 |

## Threading model (the most important contract)

```
┌─────────────────────────────────────────────────────────────┐
│ UI / Message thread (1) — ImGui render loop                 │
│ - All UI, user input, edit commands.                        │
│ - The ONLY thread that mutates the Edit document.           │
└───────────┬─────────────────────────────────────────────────┘
            │ lock-free SPSC queues (param ramps, state)
            ▼
┌─────────────────────────────────────────────────────────────┐
│ Real-time audio thread (1) — THE RT THREAD                  │
│ - miniaudio callback drives the compiled node graph.        │
│ - NEVER allocates, locks, does I/O, or calls the OS.        │
└────┬───────────────┬────────────────────────────────────────┘
     │               │
     ▼               ▼
┌────────────────┐ ┌─────────────────────────────────────────┐
│ Disk I/O pool  │ │ Video decode pool (N threads)           │
│ streams audio  │ │ decodes frames ahead of the playhead;   │
│ pre-empted by  │ │ pushes RGBA to a GL texture; RT never    │
│ priority       │ │ blocks on either.                        │
└────────────────┘ └─────────────────────────────────────────┘
```

### RT iron rules (enforced by CI sanitizer runs)

1. No `malloc`/`new`/`delete`. Pre-allocate every buffer in `prepareToPlay`.
2. No contended locks. Only atomics and SPSC queues.
3. No syscalls — no file I/O, no `printf`. Log via an SPSC ring drained on the
   UI thread.
4. Bounded work per block. Worst case measured in CI.

## Layered architecture

```
GUI Layer      Dear ImGui + OpenGL, custom timeline/waveform/mixer widgets
Application    commands, undo, view-models, transport
Document       Edit model, JSON serialization, content-addressed assets, recovery
Engine         RT Graph (ours) | Plugin Host (ours) | MIDI/NoteEx | Video pipeline
Platform       miniaudio (audio), PortMidi, window/GL, ImGui backends
```

Dependency rule: layers call *down* only. The engine knows nothing about GUI.

## Real-time audio engine & node graph (ours)

We own this end to end. Routing is **DAG-only for 1.0** (no feedback cycles).

Two-phase execution keeps the RT thread cheap:
1. **Compile (UI thread, on every graph edit):** validate, cycle-detect,
   topologically sort, allocate buffer pools, produce an immutable
   `CompiledGraph`.
2. **Render (RT):** walks the compiled order. No decisions, no allocation.
   The RT thread swaps to a new `CompiledGraph` at a block boundary via an
   atomic pointer.

### Node interface

```cpp
struct ProcessContext {
    int numSamples;
    double sampleRate;
    const float* const* inChannels;   // read-only input
    float* const* outChannels;        // write output
    int numInChannels, numOutChannels;
    const TimeInfo* time;             // samplePos, bpm, ppqPosition, isPlaying…
};

class Node {
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    virtual void process(ProcessContext&) = 0;   // RT-safe only
    virtual void release() = 0;
};
```

Built-in node types (filled in over the phases): `audioClip`, `midiClip`,
`plugin`, `instrument`, `sum`, `bus`, `send`, `return`, `gain/pan`, `latency`,
`splitter`, `merger`, `hardwareIn`, `hardwareOut`, `freeze`.

Latency compensation: a `LatencyNode` + AudioFifo aligns parallel paths by the
max reported latency. (Tracktion's was the reference; we reimplement.)

## Phasing (rebuild phases RB-0..RB-7)

See `docs/rebuild-plan-permissive.md` for the full phase-by-phase breakdown.
Phase 0's JUCE work is superseded by RB-0 (permissive skeleton). Our
`Node`/`CompiledGraph`/`SineNode` from Phase 0 survive into RB-0 — they're
already ours and are the seed of the engine.
