// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/InputMeterBank.h"
#include "engine/graph/Graph.h"
#include "engine/graph/Types.h"
#include "engine/record/RecordController.h"
#include "engine/transport/Transport.h"
#include "platform/AudioDeviceConfig.h"

#include <miniaudio.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace dave::platform {

struct DeviceLists {
    std::vector<std::string> playbackNames;
    std::vector<std::string> captureNames;
};

struct InputMeterSnapshot {
    float peak = 0.0f;
    float rms = 0.0f;
    bool clipped = false;
};

// AudioEngine owns the miniaudio device, the live CompiledGraph, and drives
// the Transport + graph from miniaudio's real-time callback.
//
// Device enumeration and reopen operations are UI-thread work. The callback
// only touches preallocated graph scratch and a fixed-capacity atomic meter
// bank; it never allocates, locks, logs, or performs I/O.
class AudioEngine {
public:
    static constexpr int kMaxBlock = 1024;

    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool start(double sampleRate = 48000.0, int playbackChannels = 2);
    void stop();

    // Refresh both native device catalogs. Indices stay valid until the next
    // call to enumerateDevices(). -1 selects the system-default output.
    DeviceLists enumerateDevices();

    // Reopen at the session sample rate. Named input selections are never
    // replaced with the default input. If duplex fails, the same output is
    // retried playback-only and captureError() explains the degraded state.
    bool selectDevices(int outputIndex, InputDeviceSelection inputSelection,
                       double sampleRate = 48000.0,
                       int playbackChannels = 2);

    // Compatibility wrapper for callers that only change playback.
    bool selectDevice(int outputIndex, double sampleRate = 48000.0,
                      int playbackChannels = 2) {
        return selectDevices(outputIndex, currentInputSelection_, sampleRate,
                             playbackChannels);
    }

    void setCompiledGraph(std::unique_ptr<engine::CompiledGraph> graph);

    engine::Transport& transport() { return transport_; }

    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    double sampleRate() const {
        return sampleRate_.load(std::memory_order_acquire);
    }
    static constexpr int maxBlockSize() { return kMaxBlock; }

    int currentDeviceIndex() const { return currentPlaybackIndex_; }
    int currentPlaybackIndex() const { return currentPlaybackIndex_; }
    InputDeviceSelection currentInputSelection() const {
        return currentInputSelection_;
    }

    bool captureAvailable() const {
        return captureAvailable_.load(std::memory_order_acquire);
    }
    std::uint32_t captureChannelCount() const {
        return captureChannelCount_.load(std::memory_order_acquire);
    }
    std::uint32_t playbackChannelCount() const {
        return playbackChannelCount_.load(std::memory_order_acquire);
    }
    const std::string& captureError() const { return captureError_; }
    InputMeterSnapshot inputMeter(std::uint32_t channel) const;
    void clearInputClips();

    // Publish a fully prepared capture router to the real-time callback. The
    // caller retains ownership and must keep it (and its DiskWriter rings)
    // alive until retireRecordController() succeeds.
    [[nodiscard]] bool publishRecordController(
        engine::RecordController* controller) noexcept;
    [[nodiscard]] bool retireRecordController(
        std::chrono::milliseconds timeout =
            std::chrono::milliseconds(250)) noexcept;

private:
    static void dataCallback(ma_device* device, void* output, const void* input,
                             ma_uint32 frameCount);

    bool ensureContext();
    bool openAttempt(int outputIndex, InputDeviceSelection inputSelection,
                     const AudioDeviceOpenAttempt& attempt,
                     std::string& error);
    bool validOutputIndex(int index) const;
    bool validCaptureSelection(InputDeviceSelection selection) const;

    std::atomic<engine::CompiledGraph*> graph_{nullptr};
    std::atomic<std::uint32_t> graphReaders_{0};

    ma_device device_{};
    ma_context context_{};
    bool contextInited_ = false;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRate_{48000.0};
    int currentPlaybackIndex_ = -1;
    InputDeviceSelection currentInputSelection_ = InputDeviceSelection::off();

    DeviceLists deviceLists_;
    std::vector<ma_device_id> playbackIds_;
    std::vector<ma_device_id> captureIds_;

    std::atomic<bool> captureAvailable_{false};
    std::atomic<std::uint32_t> captureChannelCount_{0};
    std::atomic<std::uint32_t> playbackChannelCount_{0};
    std::string captureError_;
    audio::InputMeterBank inputMeters_;

    // The callback announces entry before loading the published controller.
    // Sequential consistency makes retirement orderable: it either waits for
    // that reader, or completes before the reader can load the old pointer.
    // rtGeneration_ remains for diagnostics; reclamation uses the reader ack.
    std::atomic<engine::RecordController*> recordController_{nullptr};
    std::atomic<std::uint32_t> recordReaders_{0};
    std::atomic<std::uint64_t> rtGeneration_{0};

    engine::Transport transport_;
    engine::TimeInfo timeInfo_{};

    // Non-interleaved graph scratch. Allocated before ma_device_start and
    // reused for every callback chunk.
    std::vector<std::vector<float>> scratchStorage_;
    std::vector<float*> scratchChannelPtrs_;
    std::vector<std::vector<float>> captureScratchStorage_;
    std::vector<float*> captureScratchChannelPtrs_;
};

} // namespace dave::platform
