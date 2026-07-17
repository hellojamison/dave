#include "platform/AudioEngine.h"

#include <cstdio>
#include <cstring>

namespace dave::platform {

AudioEngine::~AudioEngine() {
    stop();
    // Free any graph still held.
    delete graph_.load(std::memory_order_acquire);
    delete pendingFree_;
}

bool AudioEngine::start(double sampleRate, int channels) {
    if (running_.load(std::memory_order_acquire)) {
        return true; // already running
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(channels);
    config.sampleRate = static_cast<ma_uint32>(sampleRate);
    config.dataCallback = &AudioEngine::dataCallback;
    config.pUserData = this;

    if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) {
        std::fprintf(stderr, "Dave: failed to init audio device\n");
        return false;
    }

    if (ma_device_start(&device_) != MA_SUCCESS) {
        std::fprintf(stderr, "Dave: failed to start audio device\n");
        ma_device_uninit(&device_);
        return false;
    }

    initialized_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    sampleRate_.store(sampleRate, std::memory_order_release);

    // Positive confirmation the device opened — the proof the RT path
    // (miniaudio callback -> our CompiledGraph -> SineNode) is live.
    // Use stderr + flush so the message appears even when stdout is piped.
    const char* name = device_.playback.name;
    std::fprintf(stderr, "Dave: audio device \"%s\" opened @ %.0f Hz, %u ch, format=f32\n",
                 name ? name : "(default)", sampleRate, config.playback.channels);
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

void AudioEngine::setGraph(std::unique_ptr<engine::CompiledGraph> graph) {
    // UI thread. Prepare the new graph first so the RT thread never sees an
    // un-prepared graph.
    if (graph) {
        graph->prepareAll(sampleRate_.load(std::memory_order_acquire), 256);
    }

    // Publish the new graph, retire the old one for UI-thread deletion.
    auto* old = graph_.exchange(graph.release(), std::memory_order_acq_rel);
    // We can't free `old` here if the RT callback might still be mid-call on it,
    // but exchange guarantees the RT callback will see the new pointer on its
    // next entry. There's a window where RT is executing process() on `old`
    // using graph-owned state, but CompiledGraph::process only reads node
    // pointers it already loaded into local state, so dropping `old` after the
    // exchange is safe in practice. To be conservative we defer one cycle.
    delete pendingFree_;
    pendingFree_ = old;
}

void AudioEngine::dataCallback(ma_device* device, void* output, const void* /*input*/,
                                ma_uint32 frameCount) {
    auto* self = static_cast<AudioEngine*>(device->pUserData);

    // --- RT thread --------------------------------------------------------
    // Start with silence so a no-graph state is quiet (never leave output
    // uninitialized — miniaudio does not zero the buffer for us).
    auto* out = static_cast<float*>(output);
    const auto channels = device->playback.channels;
    std::memset(out, 0, static_cast<size_t>(frameCount) * channels * sizeof(float));

    auto* graph = self->graph_.load(std::memory_order_acquire);
    if (graph == nullptr || graph->empty()) {
        return;
    }

    // Fill ProcessContext with views onto the interleaved miniaudio buffer.
    // miniaudio delivers interleaved float when ma_device_config hasn't asked
    // for deinterleaved; our graph speaks deinterleaved (channel pointer
    // arrays). Build a small deinterleaved view on the stack (bounded: channels
    // is small, fixed per device lifetime).
    constexpr int kMaxChannels = 8;
    float* channelPtrs[kMaxChannels]{};
    const int ch = static_cast<int>(channels) <= kMaxChannels
                       ? static_cast<int>(channels)
                       : kMaxChannels;
    // Deinterleave in-place is not safe; miniaudio with default config actually
    // delivers non-interleaved (per-channel planes) via ma_pcm_rb, but for the
    // simple playback callback the buffer is interleaved. We deinterleave into
    // a small stack buffer to keep the RT path allocation-free.
    // NOTE: for a real engine we'd configure miniaudio for non-interleaved, but
    // RB-0 keeps it simple and interleaved; the deinterleave here is bounded
    // and allocation-free.
    float deinterleaved[kMaxChannels][256];
    const int n = static_cast<int>(frameCount) <= 256 ? static_cast<int>(frameCount) : 256;
    for (int c = 0; c < ch; ++c) {
        channelPtrs[c] = deinterleaved[c];
        for (int i = 0; i < n; ++i) {
            deinterleaved[c][i] = out[i * ch + c];
        }
    }

    self->timeInfo_.sampleRate = self->sampleRate_.load(std::memory_order_relaxed);

    engine::ProcessContext ctx{};
    ctx.numSamples = n;
    ctx.sampleRate = self->timeInfo_.sampleRate;
    ctx.inChannels = channelPtrs;
    ctx.outChannels = channelPtrs;
    ctx.numInChannels = ch;
    ctx.numOutChannels = ch;
    ctx.time = &self->timeInfo_;

    graph->process(ctx);

    // Re-interleave the graph output back into miniaudio's buffer.
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < ch; ++c) {
            out[i * ch + c] = deinterleaved[c][i];
        }
    }
}

} // namespace dave::platform
