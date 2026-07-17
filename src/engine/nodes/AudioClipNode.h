#pragma once

#include "engine/graph/Node.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace dave::engine {

// AudioClipNode — plays a loaded audio buffer at a timeline position.
// Transport-aware: reads ctx.time->samplePos to decide what (if anything) to
// play each block.
//
// Loading happens on the UI thread via loadFromFile(); the decoded samples land
// in `buffer_` (deinterleaved, per-channel vectors). The RT process() only
// reads from buffer_ + transport state — no file I/O, no allocation.
//
// Clip placement (transport-relative):
//   - clipStart_ : timeline sample where the clip begins
//   - The clip plays buffer_ for its full duration, then falls silent.
// Fades (RB-1: linear):
//   - fadeSamples_ applied at the clip's head and tail to avoid clicks.
class AudioClipNode : public Node {
public:
    AudioClipNode() : Node("audioClip") {}

    int numOutputPins() const override { return 1; }
    int channelsPerPin() const override { return 2; }

    // --- UI thread ---------------------------------------------------------
    // Load a WAV file. Returns true on success. Clears any previous content.
    // The path is opened with dr_wav; decoded into per-channel float vectors.
    bool loadFromFile(const std::string& path);

    // Place the clip at this timeline position (samples).
    void setStart(int64_t timelineSample) { clipStart_.store(timelineSample, std::memory_order_relaxed); }
    int64_t start() const { return clipStart_.load(std::memory_order_relaxed); }

    int64_t lengthSamples() const { return buffer_.empty() ? 0 : static_cast<int64_t>(buffer_[0].size()); }
    int numChannels() const { return static_cast<int>(buffer_.size()); }
    double sampleRate() const { return sourceSampleRate_; }

    void prepare(double /*sampleRate*/, int /*maxBlock*/) override {}

    // --- RT thread ---------------------------------------------------------
    void process(NodeProcessContext& ctx) override {
        const int n = ctx.numSamples;
        const int outChans = ctx.output.numChannels;
        if (buffer_.empty()) {
            return; // nothing loaded; output stays zero (host pre-zeroes)
        }

        const int64_t clipStart = clipStart_.load(std::memory_order_relaxed);
        const int64_t pos = ctx.time ? ctx.time->samplePos : 0;
        const bool playing = ctx.time ? ctx.time->isPlaying : false;
        if (!playing) {
            return;
        }

        const int64_t clipEnd = clipStart + lengthSamples();

        for (int i = 0; i < n; ++i) {
            int64_t timelineSample = pos + i;
            if (timelineSample < clipStart || timelineSample >= clipEnd) {
                continue; // outside clip; leave output zero
            }
            int64_t bufIdx = timelineSample - clipStart;
            // Linear fade in/out over the first/last fadeSamples_ samples.
            float fadeGain = 1.0f;
            if (bufIdx < fadeSamples_) {
                fadeGain = static_cast<float>(bufIdx) / static_cast<float>(fadeSamples_);
            } else if (bufIdx >= lengthSamples() - fadeSamples_) {
                int64_t intoTail = lengthSamples() - 1 - bufIdx;
                fadeGain = static_cast<float>(intoTail) / static_cast<float>(fadeSamples_);
            }
            const int inChans = static_cast<int>(buffer_.size());
            for (int c = 0; c < outChans; ++c) {
                // If the source has fewer channels than output, duplicate
                // (mono→stereo) or drop (surround→stereo) as needed.
                int srcC = (inChans == 1) ? 0 : std::min(c, inChans - 1);
                ctx.output.channels[c][i] = buffer_[srcC][bufIdx] * fadeGain;
            }
        }
    }

private:
    // Decoded source: one vector per channel (deinterleaved). Loaded on UI
    // thread; read-only from RT thread after load completes. A swap-then-
    // release pattern (replace buffer_ atomically) would be safer for
    // concurrent reload; RB-1 loads once before the graph runs.
    std::vector<std::vector<float>> buffer_;
    double sourceSampleRate_ = 48000.0;
    std::atomic<int64_t> clipStart_{0};
    int64_t fadeSamples_ = 64; // ~1.3ms at 48k
};

} // namespace dave::engine
