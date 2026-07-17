# RB-1 Graph Engine — Design Notes

## The problem with RB-0's graph

RB-0's `Node::process(ProcessContext&)` works for a generator (SineNode) but
cannot express *routing*. There's no concept of inputs, edges, or summing.
The `CompiledGraph` is a flat list walked in insertion order — no topology.

## RB-1 model

**Three types:**

- `Node` — a DSP unit. Declares how many input/output **pins** (channels) it
  exposes. Implement `process(NodeProcessContext&)`. Does NOT know its sources
  or destinations — it just fills its output buffer and reads its input buffer.
- `Graph` — mutable, UI-thread only. Holds `Node`s and `Edge`s (source pin →
  destination pin). `compile()` validates (cycle detection), topologically
  sorts, allocates a buffer pool, returns an immutable `CompiledGraph`.
- `CompiledGraph` — immutable, RT-thread. Pre-allocated buffers per (node,
  channel). Walks nodes in topo order; for each node, gathers its input
  channels (by following edges, summing when multiple edges hit the same
  destination pin), then calls `node->process()` writing into the node's
  output channels.

### Pins and channels

A node has `numInputPins()` × channels-per-pin and `numOutputPins()` × channels
per pin. For RB-1 every pin is stereo (2 channels) — we'll add per-pin channel
counts later if needed. An edge connects `(srcNode, srcPin) → (dstNode, dstPin)`.
Multiple edges into one dst pin **sum** (mix). This gives us bus-style routing
for free: a SummingNode is just a node with 1 output pin fed by N edges.

### Buffer ownership

`CompiledGraph` owns one `AudioBuffer` per node (sized `maxBlock × outChannels`).
The RT walk:
1. Zero the node's output buffer.
2. For each input pin, find source nodes (via the compiled edge table), copy
   their output into a scratch input buffer (summing if multiple sources).
3. Call `node->process({inputBuf, outputBuf, time})`.

No allocation on RT: buffers allocated in `compile()`.

### Transport

`Transport` is separate from the graph — it owns play state and a sample
position counter. Each block, the AudioEngine advances the transport and writes
the current position/time into `TimeInfo`, which is passed to every node via
`NodeProcessContext.time`. AudioClipNode reads `time->samplePos` to decide what
to play. This keeps the graph transport-agnostic (a generator ignores time).

### Why this shape (vs Tracktion's)

Tracktion bakes topology into the object tree (node owns inputs). We separate
`Graph` (nodes+edges) from `Node` (pure DSP) so that:
- The graph is **serializable** to our JSON `graph.{nodes,edges}` directly.
- Nodes are **unit-testable** without building a graph (hand them buffers).
- The **routing editor UI** edits `Graph`, not node internals.
- We can **recompile** (UI thread) and swap atomically (RT thread) — already
  the pattern from RB-0's AudioEngine.

## RB-1 scope (what to build)

1. New `Node` base with pins + `process(NodeProcessContext&)`.
2. `Graph` (mutable) + `Edge` + `compile()` with Kahn's-algorithm topo sort and
   cycle detection.
3. `CompiledGraph` with pre-allocated per-node buffers and the RT walk.
4. Nodes: `SummingNode` (1 out, N in), `GainNode`, `PanNode`, `ChannelRemapNode`.
5. `AudioClipNode` — plays a WAV (dr_libs) with start/length/fades, transport-
   aware.
6. `Transport` — play/stop/seek, sample counter, TimeInfo writer.
7. AudioEngine: switch miniaudio to non-interleaved; remove RB-0 deinterleave
   hack.
8. UI: load-file button, transport bar, basic clip list.
