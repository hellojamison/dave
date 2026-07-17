#pragma once

#include "engine/graph/Types.h"

#include <atomic>

namespace dave::engine {

// Transport owns playback state: play/stop, the timeline position, and loop
// points. The UI thread mutates play state and requests seeks; the RT thread
// advances the sample counter and writes the current TimeInfo each block.
//
// The transport is NOT part of the node graph — it's a peer that the AudioEngine
// reads each block to fill TimeInfo before calling CompiledGraph::process().
// Nodes that care about time (AudioClipNode) read ctx.time->samplePos; nodes
// that don't (SineNode) ignore it.
class Transport {
public:
    Transport() = default;

    // --- UI thread ---------------------------------------------------------
    void play() { playing_.store(true, std::memory_order_release); }
    void stop() { playing_.store(false, std::memory_order_release); }
    void toggle() {
        bool expected = playing_.load(std::memory_order_acquire);
        playing_.store(!expected, std::memory_order_release);
    }
    void seek(int64_t samplePos) { nextPosition_.store(samplePos, std::memory_order_release); }
    bool isPlaying() const { return playing_.load(std::memory_order_acquire); }
    int64_t position() const { return currentPosition_.load(std::memory_order_acquire); }

    void setLoop(int64_t start, int64_t end) {
        loopStart_ = start;
        loopEnd_ = end;
        looping_.store(start < end, std::memory_order_release);
    }
    void clearLoop() {
        looping_.store(false, std::memory_order_release);
    }

    // --- RT thread ---------------------------------------------------------
    // Called once per block by the AudioEngine BEFORE graph processing. Advances
    // the position by numSamples (honoring loop), fills `out` for the graph.
    void advanceAndFill(TimeInfo& out, int numSamples, double sampleRate) {
        // Pick up a pending seek (UI thread requested it via seek()).
        int64_t requested = nextPosition_.load(std::memory_order_acquire);
        int64_t pos = currentPosition_.load(std::memory_order_relaxed);
        if (requested >= 0 && requested != pos) {
            pos = requested;
            nextPosition_.store(-1, std::memory_order_release); // consume
        }

        out.sampleRate = sampleRate;
        out.samplePos = pos;
        out.isPlaying = playing_.load(std::memory_order_acquire);
        out.isLooping = looping_.load(std::memory_order_acquire);
        out.loopStart = loopStart_;
        out.loopEnd = loopEnd_;

        // Advance position for the NEXT block (this block covers [pos, pos+n)).
        if (out.isPlaying) {
            int64_t next = pos + numSamples;
            if (out.isLooping && next >= out.loopEnd) {
                next = out.loopStart + (next - out.loopEnd);
            }
            currentPosition_.store(next, std::memory_order_relaxed);
        }
    }

private:
    std::atomic<bool> playing_{false};
    std::atomic<int64_t> currentPosition_{0};
    std::atomic<int64_t> nextPosition_{-1}; // >=0 means a seek is pending
    std::atomic<bool> looping_{false};
    int64_t loopStart_ = 0;
    int64_t loopEnd_ = 0;
};

} // namespace dave::engine
