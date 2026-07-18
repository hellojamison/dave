#pragma once

#include "engine/graph/Graph.h"
#include "engine/graph/Types.h"
#include "engine/transport/Transport.h"

#include <miniaudio.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace dave::platform {

// AudioEngine owns the miniaudio device, the live CompiledGraph, and drives
// the Transport + graph from the miniaudio data callback (the RT thread).
//
// Threading:
// - start()/stop()/setGraph()/transport() run on the UI/main thread.
// - The miniaudio data callback runs on the RT audio thread (RT iron rules).
//
// Graph handover: setCompiledGraph() publishes a new CompiledGraph via atomic
// pointer; the RT callback picks it up at the next block. The previous graph
// is retired and freed on the UI thread.
class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool start(double sampleRate = 48000.0, int channels = 2);
    void stop();

    // Switch the active playback device. Restarts the device with the given
    // device index (from enumerateDevices()). Returns true on success.
    bool selectDevice(int deviceIndex, double sampleRate = 48000.0, int channels = 2);

    // Enumerate available playback devices. Returns names; indices match the
    // deviceIndex used by selectDevice().
    std::vector<std::string> enumerateDevices();

    // Publish a freshly-compiled graph. Prepared on the UI thread, swapped at
    // the next RT block boundary. nullptr = silence.
    void setCompiledGraph(std::unique_ptr<engine::CompiledGraph> graph);

    engine::Transport& transport() { return transport_; }

    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    double sampleRate() const { return sampleRate_.load(std::memory_order_acquire); }
    int currentDeviceIndex() const { return currentDeviceIndex_; }

private:
    static void dataCallback(ma_device* device, void* output, const void* input,
                             ma_uint32 frameCount);

    // Open (or reopen) a specific device by index. -1 = system default.
    bool openDevice(int deviceIndex, double sampleRate, int channels);

    std::atomic<engine::CompiledGraph*> graph_{nullptr};
    engine::CompiledGraph* pendingFree_{nullptr};

    ma_device device_{};
    ma_context context_{};
    bool contextInited_ = false;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRate_{48000.0};
    int currentDeviceIndex_ = -1;

    engine::Transport transport_;
    engine::TimeInfo timeInfo_{};

    // Scratch for handing the graph a non-interleaved view of miniaudio's
    // (interleaved) output buffer. Allocated once in start(); reused every
    // block — NO RT allocation. Sized to [channels][maxBlock].
    std::vector<std::vector<float>> scratchStorage_;
    std::vector<float*> scratchChannelPtrs_;
};

} // namespace dave::platform
