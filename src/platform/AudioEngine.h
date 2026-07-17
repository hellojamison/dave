#pragma once

#include "engine/graph/CompiledGraph.h"

#include <miniaudio.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace dave::platform {

// AudioEngine owns the miniaudio device and drives our CompiledGraph from the
// miniaudio data callback (which runs on miniaudio's audio thread == our RT
// thread). This is the single seam between the OS audio device and our graph.
//
// Threading contract:
// - start()/stop()/setGraph() run on the UI/main thread.
// - The miniaudio data callback runs on the RT audio thread and must obey the
//   RT iron rules: no allocation, no locks, no syscalls.
//
// Graph handover: setGraph() prepares the new graph on the UI thread, then
// publishes it via an atomic pointer. The RT callback picks it up at the next
// block boundary. The previous graph is dropped on the UI thread (we can't
// free graph memory from the RT thread).
class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Open the default output device at the given sample rate, stereo out.
    // Returns true on success. Safe to call once.
    bool start(double sampleRate = 48000.0, int channels = 2);

    // Close the device. Idempotent.
    void stop();

    // Publish a new compiled graph. Prepared on the UI thread, swapped in at
    // the next RT block boundary. Pass nullptr to silence output.
    void setGraph(std::unique_ptr<engine::CompiledGraph> graph);

    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    double sampleRate() const { return sampleRate_.load(std::memory_order_acquire); }

private:
    // miniaudio data callback — RT thread. static because miniaudio wants a
    // C function pointer; we recover `this` from the pUserData field.
    static void dataCallback(ma_device* device, void* output, const void* input,
                             ma_uint32 frameCount);

    // The graph the RT thread renders. Owned here; swapped atomically.
    // We use a raw atomic pointer + a pending-free slot so the RT thread only
    // does a single relaxed load and never frees memory.
    std::atomic<engine::CompiledGraph*> graph_{nullptr};
    engine::CompiledGraph* pendingFree_{nullptr}; // freed on UI thread in setGraph

    ma_device device_{};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRate_{48000.0};

    engine::TimeInfo timeInfo_{}; // reused per block; written from RT thread only
};

} // namespace dave::platform
