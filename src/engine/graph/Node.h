#pragma once

#include <cstdint>

namespace dave::engine {

// Forward declarations — fleshed out in later phases.
struct TimeInfo {
    double sampleRate = 44100.0;
    int64_t samplePos = 0;
    double bpm = 120.0;
    double ppqPosition = 0.0;
    bool isPlaying = false;
    bool isRecording = false;
    // numSamples lives on ProcessContext, not here.
};

// ProcessContext is everything a Node sees each block.
// All buffers are pre-allocated by the host in prepareToPlay — the RT thread
// must never allocate. See docs/architecture.md §Threading model.
struct ProcessContext {
    int numSamples = 0;
    double sampleRate = 44100.0;
    const float* const* inChannels = nullptr;  // read-only input
    float* const* outChannels = nullptr;       // write output
    int numInChannels = 0;
    int numOutChannels = 0;
    const TimeInfo* time = nullptr;
};

// Node is the base DSP unit. Every node implements prepare/process/release.
// process() runs on the RT audio thread and must obey the RT iron rules.
class Node {
public:
    virtual ~Node() = default;

    // Called on the UI/message thread before processing starts (or when the
    // graph is recompiled). Allocate here, never in process().
    virtual void prepare(double sampleRate, int maxBlock) = 0;

    // Called on the RT thread for every block. RT-safe only.
    virtual void process(ProcessContext& ctx) = 0;

    // Called on the UI thread when the node leaves the active graph.
    virtual void release() = 0;

    // Number of audio input/output channels this node exposes. Fixed at
    // compile time so the host can pre-wire buffers.
    virtual int numInputs() const { return 0; }
    virtual int numOutputs() const { return 0; }
};

} // namespace dave::engine
