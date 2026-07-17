#pragma once

#include "engine/graph/Types.h"

#include <cstdint>
#include <string>

namespace dave::engine {

// NodeProcessContext is what a Node sees each block. All buffers are
// pre-allocated by CompiledGraph — process() must never allocate.
//
// `inputs` is indexed by input pin: inputs[pinIndex] is the AudioBus for that
// pin (already summed across all edges feeding it). `output` is this node's
// own output bus, pre-zeroed; the node writes (or adds) into it.
struct NodeProcessContext {
    int numSamples = 0;
    double sampleRate = 48000.0;
    const TimeInfo* time = nullptr;
    // Per-pin input buses. For a generator (no inputs) this is empty.
    const AudioBus* inputs = nullptr;
    int numInputs = 0;
    // This node's output. Pre-zeroed by the host before process() is called.
    AudioBus output;
};

// Node is the base DSP unit. A node declares how many input/output pins it has
// (each pin is a stereo bus for RB-1) and implements process(). It does NOT
// know its sources or destinations — routing is the Graph's job, expressed as
// edges between pins. This keeps nodes unit-testable and the graph
// serializable.
//
// Threading: prepare()/release() and any setters run on the UI thread.
// process() runs on the RT audio thread and must obey the RT iron rules:
// no allocation, no locks, no syscalls.
class Node {
public:
    virtual ~Node() = default;

    // Called on the UI thread before the graph goes live (or when the sample
    // rate / max block changes). Allocate per-node state here.
    virtual void prepare(double sampleRate, int maxBlock) {}

    // Called on the UI thread when the node leaves the active graph.
    virtual void release() {}

    // --- Pin topology (fixed for the node's lifetime) ----------------------
    // Number of input pins (each is a stereo bus for RB-1). Generators return 0.
    virtual int numInputPins() const { return 0; }
    // Number of output pins. Most nodes have 1.
    virtual int numOutputPins() const { return 1; }
    // Channels per pin. RB-1 fixes this at 2 (stereo); overridable for mono
    // sources / surround later.
    virtual int channelsPerPin() const { return 2; }

    // --- RT processing -----------------------------------------------------
    // Called on the RT thread for every block. Read from ctx.inputs, write into
    // ctx.output. RT-safe only.
    virtual void process(NodeProcessContext& ctx) = 0;

    // Stable identifier for serialization. Subclasses set this in their ctor.
    const std::string& typeName() const { return typeName_; }

protected:
    explicit Node(std::string typeName) : typeName_(std::move(typeName)) {}

private:
    std::string typeName_;
};

} // namespace dave::engine
