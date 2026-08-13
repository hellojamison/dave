// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

namespace dave::platform {

namespace {

std::string miniaudioError(const char* action, ma_result result) {
    std::string message(action);
    message += ": ";
    message += ma_result_description(result);
    return message;
}

} // namespace

AudioEngine::~AudioEngine() {
    stop();
    if (contextInited_) {
        ma_context_uninit(&context_);
    }
    delete graph_.load(std::memory_order_acquire);
}

bool AudioEngine::ensureContext() {
    if (contextInited_) {
        return true;
    }
    const ma_result result = ma_context_init(nullptr, 0, nullptr, &context_);
    if (result != MA_SUCCESS) {
        std::fprintf(stderr, "Dave: ma_context_init failed: %s\n",
                     ma_result_description(result));
        return false;
    }
    contextInited_ = true;
    return true;
}

bool AudioEngine::start(double sampleRate, int playbackChannels) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!ensureContext()) {
        return false;
    }
    if (deviceLists_.playbackNames.empty() && playbackIds_.empty()) {
        enumerateDevices();
    }
    return selectDevices(-1, InputDeviceSelection::off(), sampleRate,
                         playbackChannels);
}

DeviceLists AudioEngine::enumerateDevices() {
    deviceLists_ = {};
    playbackIds_.clear();
    captureIds_.clear();
    if (!ensureContext()) {
        return deviceLists_;
    }

    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playbackCount = 0;
    ma_uint32 captureCount = 0;
    const ma_result result = ma_context_get_devices(
        &context_, &playback, &playbackCount, &capture, &captureCount);
    if (result != MA_SUCCESS) {
        std::fprintf(stderr, "Dave: device enumeration failed: %s\n",
                     ma_result_description(result));
        return deviceLists_;
    }

    deviceLists_.playbackNames.reserve(playbackCount);
    playbackIds_.reserve(playbackCount);
    for (ma_uint32 index = 0; index < playbackCount; ++index) {
        const char* name = playback[index].name;
        deviceLists_.playbackNames.emplace_back(
            name != nullptr && name[0] != '\0' ? name : "(unnamed)");
        playbackIds_.push_back(playback[index].id);
    }

    deviceLists_.captureNames.reserve(captureCount);
    captureIds_.reserve(captureCount);
    for (ma_uint32 index = 0; index < captureCount; ++index) {
        const char* name = capture[index].name;
        deviceLists_.captureNames.emplace_back(
            name != nullptr && name[0] != '\0' ? name : "(unnamed)");
        captureIds_.push_back(capture[index].id);
    }
    return deviceLists_;
}

bool AudioEngine::validOutputIndex(int index) const {
    return index == -1 ||
           (index >= 0 && static_cast<std::size_t>(index) < playbackIds_.size());
}

bool AudioEngine::validCaptureSelection(InputDeviceSelection selection) const {
    if (selection.kind != InputDeviceSelection::Kind::Device) {
        return true;
    }
    return selection.deviceIndex >= 0 &&
           static_cast<std::size_t>(selection.deviceIndex) < captureIds_.size();
}

bool AudioEngine::selectDevices(int outputIndex,
                                InputDeviceSelection inputSelection,
                                double sampleRate, int playbackChannels) {
    if (!ensureContext()) {
        return false;
    }
    if (deviceLists_.playbackNames.empty() && playbackIds_.empty()) {
        enumerateDevices();
    }
    if (!validOutputIndex(outputIndex)) {
        captureAvailable_.store(false, std::memory_order_release);
        captureError_ = "Selected output device is unavailable";
        return false;
    }

    currentPlaybackIndex_ = outputIndex;
    currentInputSelection_ = inputSelection;
    captureAvailable_.store(false, std::memory_order_release);
    captureChannelCount_.store(0, std::memory_order_release);
    captureError_.clear();

    const auto safeRate = static_cast<std::uint32_t>(std::clamp(
        std::llround(sampleRate), 1LL,
        static_cast<long long>(std::numeric_limits<std::uint32_t>::max())));
    // Zero asks miniaudio for the selected playback device's native channel
    // count. The legacy argument remains source-compatible but no longer
    // forces every interface to stereo.
    (void)playbackChannels;
    const std::uint32_t safePlaybackChannels = 0;
    const auto plan = makeAudioDeviceOpenPlan(
        inputSelection, safeRate, safePlaybackChannels);

    stop();

    if (!validCaptureSelection(inputSelection)) {
        captureError_ = "Selected input device is unavailable";
        std::string playbackError;
        if (openAttempt(outputIndex, InputDeviceSelection::off(),
                        plan.attempts[plan.count - 1], playbackError)) {
            return true;
        }
        captureError_ += "; playback fallback failed: " + playbackError;
        return false;
    }

    std::string firstError;
    if (openAttempt(outputIndex, inputSelection, plan.attempts[0], firstError)) {
        return true;
    }

    if (plan.count == 1) {
        captureError_ = firstError;
        return false;
    }

    captureError_ = "Capture unavailable (" + firstError + ")";
    std::string fallbackError;
    if (openAttempt(outputIndex, InputDeviceSelection::off(), plan.attempts[1],
                    fallbackError)) {
        std::fprintf(stderr, "Dave: %s; continuing playback-only\n",
                     captureError_.c_str());
        return true;
    }

    captureError_ += "; playback fallback failed: " + fallbackError;
    std::fprintf(stderr, "Dave: %s\n", captureError_.c_str());
    return false;
}

bool AudioEngine::openAttempt(int outputIndex,
                              InputDeviceSelection inputSelection,
                              const AudioDeviceOpenAttempt& attempt,
                              std::string& error) {
    ma_device_config config = ma_device_config_init(
        attempt.duplex ? ma_device_type_duplex : ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = attempt.playbackChannels;
    config.sampleRate = attempt.sampleRate;
    config.dataCallback = &AudioEngine::dataCallback;
    config.pUserData = this;
    config.periodSizeInFrames = 256;

    if (outputIndex >= 0) {
        config.playback.pDeviceID = &playbackIds_[outputIndex];
    }
    if (attempt.duplex) {
        config.capture.format = ma_format_f32;
        config.capture.channels = attempt.captureChannels;
        if (inputSelection.kind == InputDeviceSelection::Kind::Device) {
            config.capture.pDeviceID = &captureIds_[inputSelection.deviceIndex];
        }
    }

    ma_result result = ma_device_init(&context_, &config, &device_);
    if (result != MA_SUCCESS) {
        error = miniaudioError(attempt.duplex ? "duplex init failed"
                                              : "playback init failed",
                               result);
        return false;
    }
    initialized_.store(true, std::memory_order_release);

    const auto playbackChannels = device_.playback.channels;
    playbackChannelCount_.store(playbackChannels, std::memory_order_release);
    scratchStorage_.assign(
        playbackChannels, std::vector<float>(kMaxBlock, 0.0f));
    scratchChannelPtrs_.resize(playbackChannels);
    for (ma_uint32 channel = 0; channel < playbackChannels; ++channel) {
        scratchChannelPtrs_[channel] = scratchStorage_[channel].data();
    }

    const ma_uint32 captureChannels =
        attempt.duplex ? device_.capture.channels : 0;
    captureScratchStorage_.assign(
        captureChannels, std::vector<float>(kMaxBlock, 0.0f));
    captureScratchChannelPtrs_.resize(captureChannels);
    for (ma_uint32 channel = 0; channel < captureChannels; ++channel) {
        captureScratchChannelPtrs_[channel] =
            captureScratchStorage_[channel].data();
    }
    inputMeters_.reset(captureChannels,
                       static_cast<float>(device_.sampleRate));
    captureChannelCount_.store(captureChannels, std::memory_order_release);
    sampleRate_.store(static_cast<double>(device_.sampleRate),
                      std::memory_order_release);

    result = ma_device_start(&device_);
    if (result != MA_SUCCESS) {
        error = miniaudioError(attempt.duplex ? "duplex start failed"
                                              : "playback start failed",
                               result);
        initialized_.store(false, std::memory_order_release);
        ma_device_uninit(&device_);
        scratchStorage_.clear();
        scratchChannelPtrs_.clear();
        captureScratchStorage_.clear();
        captureScratchChannelPtrs_.clear();
        inputMeters_.reset(0);
        captureChannelCount_.store(0, std::memory_order_release);
        playbackChannelCount_.store(0, std::memory_order_release);
        return false;
    }

    running_.store(true, std::memory_order_release);
    captureAvailable_.store(attempt.duplex, std::memory_order_release);
    if (attempt.duplex) {
        captureError_.clear();
    }

    const char* outputName = device_.playback.name;
    if (attempt.duplex) {
        const char* inputName = device_.capture.name;
        std::fprintf(stderr,
                     "Dave: output \"%s\" + input \"%s\" opened @ %u Hz "
                     "(%u out, %u in)\n",
                     outputName ? outputName : "(default)",
                     inputName ? inputName : "(default)", device_.sampleRate,
                     playbackChannels, captureChannels);
    } else {
        std::fprintf(stderr, "Dave: output \"%s\" opened @ %u Hz, %u ch\n",
                     outputName ? outputName : "(default)", device_.sampleRate,
                     playbackChannels);
    }
    std::fflush(stderr);
    return true;
}

void AudioEngine::stop() {
    const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
    if (wasRunning) {
        ma_device_stop(&device_);
    }
    if (initialized_.exchange(false, std::memory_order_acq_rel)) {
        ma_device_uninit(&device_);
    }
    captureAvailable_.store(false, std::memory_order_release);
    captureChannelCount_.store(0, std::memory_order_release);
    playbackChannelCount_.store(0, std::memory_order_release);
    inputMeters_.reset(0);
    scratchStorage_.clear();
    scratchChannelPtrs_.clear();
    captureScratchStorage_.clear();
    captureScratchChannelPtrs_.clear();
}

void AudioEngine::setCompiledGraph(std::unique_ptr<engine::CompiledGraph> graph) {
    auto* old = graph_.exchange(graph.release(), std::memory_order_seq_cst);
    // The callback announces before loading under the same sequentially
    // consistent order. It therefore either becomes a reader before this
    // exchange (and we wait) or loads only the replacement afterward.
    while (graphReaders_.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }
    delete old;
}

InputMeterSnapshot AudioEngine::inputMeter(std::uint32_t channel) const {
    const auto meter = inputMeters_.snapshot(channel);
    return {meter.peak, meter.rms, meter.clipped};
}

void AudioEngine::clearInputClips() {
    const auto channels = inputMeters_.channelCount();
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        inputMeters_.clearClip(channel);
    }
}

bool AudioEngine::publishRecordController(
    engine::RecordController* controller) noexcept {
    if (controller == nullptr || !controller->isPrepared()) return false;
    engine::RecordController* expected = nullptr;
    return recordController_.compare_exchange_strong(
        expected, controller, std::memory_order_seq_cst,
        std::memory_order_seq_cst);
}

bool AudioEngine::retireRecordController(
    std::chrono::milliseconds timeout) noexcept {
    recordController_.store(nullptr, std::memory_order_seq_cst);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (recordReaders_.load(std::memory_order_seq_cst) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

void AudioEngine::dataCallback(ma_device* device, void* output,
                               const void* input, ma_uint32 frameCount) {
    auto* self = static_cast<AudioEngine*>(device->pUserData);
    auto* out = static_cast<float*>(output);
    const auto* in = static_cast<const float*>(input);
    const int outputChannels = static_cast<int>(device->playback.channels);
    const int inputChannels = static_cast<int>(device->capture.channels);
    const int total = static_cast<int>(frameCount);

    if (out != nullptr) {
        std::memset(out, 0,
                    static_cast<std::size_t>(total) * outputChannels *
                        sizeof(float));
    }

    // Announce before loading. In the global seq_cst order a publisher either
    // sees this reader and waits, or completes before this callback loads the
    // new pointer. This avoids both the load-before-announce race and ABA.
    self->graphReaders_.fetch_add(1, std::memory_order_seq_cst);
    auto* graph = self->graph_.load(std::memory_order_seq_cst);
    const bool renderGraph = out != nullptr && graph != nullptr && !graph->empty();
    const double sampleRate =
        self->sampleRate_.load(std::memory_order_relaxed);

    // Both capture metering and graph playback are bounded by kMaxBlock even
    // when a backend delivers a larger-than-requested callback.
    for (int offset = 0; offset < total; offset += kMaxBlock) {
        const int frames = std::min(kMaxBlock, total - offset);

        // Transport time is the single source of truth for both capture and
        // playback. Fill it before the capture tap so recording does not
        // depend on a compiled graph being present.
        self->transport_.advanceAndFill(self->timeInfo_, frames, sampleRate);

        if (inputChannels > 0) {
            const float* inputSlice = in == nullptr
                ? nullptr
                : in + static_cast<std::size_t>(offset) * inputChannels;
            self->inputMeters_.processInterleaved(
                inputSlice, static_cast<std::uint32_t>(frames),
                static_cast<std::uint32_t>(inputChannels));

            for (int channel = 0; channel < inputChannels; ++channel) {
                float* planar = self->captureScratchChannelPtrs_[channel];
                if (inputSlice == nullptr) {
                    std::memset(planar, 0,
                                static_cast<size_t>(frames) * sizeof(float));
                } else {
                    for (int frame = 0; frame < frames; ++frame) {
                        planar[frame] = inputSlice[
                            static_cast<size_t>(frame) * inputChannels + channel];
                    }
                }
            }
        }

        // Announce before loading, exactly like the graph reader above. This
        // closes load-before-announce and address-reuse (ABA) races without a
        // lock or a wait on the real-time thread.
        self->recordReaders_.fetch_add(1, std::memory_order_seq_cst);
        engine::RecordController* recorder =
            self->recordController_.load(std::memory_order_seq_cst);
        if (recorder != nullptr) {
            const float* inputSlice = in == nullptr
                ? nullptr
                : in + static_cast<std::size_t>(offset) * inputChannels;
            recorder->processBlock(inputSlice, inputChannels,
                                   static_cast<std::size_t>(frames),
                                   self->timeInfo_);
        }
        self->recordReaders_.fetch_sub(1, std::memory_order_seq_cst);

        if (!renderGraph) {
            self->rtGeneration_.fetch_add(1, std::memory_order_release);
            continue;
        }

        for (int channel = 0; channel < outputChannels; ++channel) {
            std::memset(self->scratchChannelPtrs_[channel], 0,
                        static_cast<std::size_t>(frames) * sizeof(float));
        }

        engine::AudioBus rootBus;
        rootBus.channels = self->scratchChannelPtrs_.data();
        rootBus.numChannels = outputChannels;
        rootBus.numSamples = frames;
        engine::AudioBus hardwareInput;
        hardwareInput.channels = self->captureScratchChannelPtrs_.data();
        hardwareInput.numChannels = inputChannels;
        hardwareInput.numSamples = frames;
        graph->process(rootBus, hardwareInput, self->timeInfo_);

        float* outputSlice =
            out + static_cast<std::size_t>(offset) * outputChannels;
        for (int frame = 0; frame < frames; ++frame) {
            for (int channel = 0; channel < outputChannels; ++channel) {
                outputSlice[frame * outputChannels + channel] =
                    self->scratchChannelPtrs_[channel][frame];
            }
        }
        self->rtGeneration_.fetch_add(1, std::memory_order_release);
    }
    self->graphReaders_.fetch_sub(1, std::memory_order_seq_cst);
}

} // namespace dave::platform
