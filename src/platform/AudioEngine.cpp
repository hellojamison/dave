#include "platform/AudioEngine.h"

#include <cstdio>
#include <cstring>

namespace dave::platform {

AudioEngine::~AudioEngine() {
    stop();
    delete graph_.load(std::memory_order_acquire);
    delete pendingFree_;
}

bool AudioEngine::start(double sampleRate, int channels) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(channels);
    config.sampleRate = static_cast<ma_uint32>(sampleRate);
    config.dataCallback = &AudioEngine::dataCallback;
    config.pUserData = this;
    // Request a bounded period so our scratch buffer has a known max size.
    config.periodSizeInFrames = 256;

    if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) {
        std::fprintf(stderr, "Dave: failed to init audio device\n");
        return false;
    }
    if (ma_device_start(&device_) != MA_SUCCESS) {
        std::fprintf(stderr, "Dave: failed to start audio device\n");
        ma_device_uninit(&device_);
        return false;
    }

    // Pre-allocate the non-interleaved scratch (one buffer per channel, sized
    // to a generous max block). 8196 frames covers any reasonable period.
    constexpr int kMaxBlock = 8196;
    scratchStorage_.assign(channels, std::vector<float>(kMaxBlock, 0.0f));
    scratchChannelPtrs_.assign(channels, nullptr);
    for (int c = 0; c < channels; ++c) {
        scratchChannelPtrs_[c] = scratchStorage_[c].data();
    }

    initialized_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    sampleRate_.store(sampleRate, std::memory_order_release);

    const char* name = device_.playback.name;
    std::fprintf(stderr, "Dave: audio device \"%s\" opened @ %.0f Hz, %d ch\n",
                 name ? name : "(default)", sampleRate, channels);
    std::fflush(stderr);
    return true;
}

void AudioEngine::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    ma_device_stop(&device_);
    running_.store(false, std::memory_order_release);
    if (initialized_.load(std::memory_order_acquire)) {
        ma_device_uninit(&device_);
        initialized_.store(false, std::memory_order_release);
    }
}

void AudioEngine::setCompiledGraph(std::unique_ptr<engine::CompiledGraph> graph) {
    auto* old = graph_.exchange(graph.release(), std::memory_order_acq_rel);
    delete pendingFree_;
    pendingFree_ = old;
}

void AudioEngine::dataCallback(ma_device* device, void* output, const void* /*input*/,
                                ma_uint32 frameCount) {
    auto* self = static_cast<AudioEngine*>(device->pUserData);

    // --- RT thread --------------------------------------------------------
    auto* out = static_cast<float*>(output);
    const int channels = static_cast<int>(device->playback.channels);
    const int n = static_cast<int>(frameCount);
    // Always silence first.
    std::memset(out, 0, static_cast<size_t>(n) * channels * sizeof(float));

    auto* graph = self->graph_.load(std::memory_order_acquire);
    if (graph == nullptr || graph->empty()) {
        return;
    }

    // Deinterleave miniaudio's output buffer into our scratch (in place: we
    // read nothing from it, so just point scratch at zeroed memory).
    for (int c = 0; c < channels; ++c) {
        std::memset(self->scratchChannelPtrs_[c], 0,
                    static_cast<size_t>(n) * sizeof(float));
    }

    // Advance transport and fill TimeInfo for this block.
    self->transport_.advanceAndFill(self->timeInfo_, n,
                                    self->sampleRate_.load(std::memory_order_relaxed));

    // Run the graph into the scratch buffers.
    engine::AudioBus rootBus;
    rootBus.channels = self->scratchChannelPtrs_.data();
    rootBus.numChannels = channels;
    rootBus.numSamples = n;
    graph->process(rootBus, self->timeInfo_);

    // Re-interleave the graph output into miniaudio's buffer.
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < channels; ++c) {
            out[i * channels + c] = self->scratchChannelPtrs_[c][i];
        }
    }
}

} // namespace dave::platform
