// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/DecodedAudioAsset.h"
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

    // Set the decoded buffer directly (used by GraphBuilder, which caches
    // decoded assets). Copies the buffer reference (no decode). The caller
    // must keep `buffer` alive for the node's lifetime.
    void setBuffer(audio::DecodedAudioAssetPtr buffer) {
        decodedAsset_ = std::move(buffer);
        buffer_ = decodedAsset_ ? &decodedAsset_->channels : nullptr;
        sourceSampleRate_ = decodedAsset_ ? decodedAsset_->sampleRate : 48000.0;
    }

    // Clip placement on the timeline (samples).
    void setStart(int64_t timelineSample) { clipStart_.store(timelineSample, std::memory_order_relaxed); }
    int64_t start() const { return clipStart_.load(std::memory_order_relaxed); }

    // Where in the source file to start reading (samples). Default 0.
    void setSourceOffset(int64_t offset) { sourceOffset_ = offset; }

    // How many samples to play. 0 = play the whole buffer from sourceOffset.
    void setLength(int64_t length) { length_ = length; }

    // Fade in/out lengths (samples).
    void setFades(int64_t fadeIn, int64_t fadeOut) { fadeIn_ = fadeIn; fadeOut_ = fadeOut; }

    int64_t lengthSamples() const {
        if (buffer_ == nullptr || buffer_->empty()) return 0;
        int64_t bufLen = static_cast<int64_t>((*buffer_)[0].size()) - sourceOffset_;
        return length_ > 0 ? std::min(length_, bufLen) : bufLen;
    }
    int numChannels() const { return buffer_ ? static_cast<int>(buffer_->size()) : 0; }
    double sampleRate() const { return sourceSampleRate_; }

    void prepare(double /*sampleRate*/, int /*maxBlock*/) override {}

    // --- RT thread ---------------------------------------------------------
    void process(NodeProcessContext& ctx) override {
        const int n = ctx.numSamples;
        const int outChans = ctx.output.numChannels;
        if (buffer_ == nullptr || buffer_->empty()) {
            return;
        }
        const int64_t clipStart = clipStart_.load(std::memory_order_relaxed);
        const int64_t pos = ctx.time ? ctx.time->samplePos : 0;
        const bool playing = ctx.time ? ctx.time->isPlaying : false;
        if (!playing) return;

        const int64_t len = lengthSamples();
        const int64_t clipEnd = clipStart + len;
        const int inChans = static_cast<int>(buffer_->size());
        const int64_t bufLen = static_cast<int64_t>((*buffer_)[0].size());

        for (int i = 0; i < n; ++i) {
            int64_t timelineSample = pos + i;
            if (timelineSample < clipStart || timelineSample >= clipEnd) continue;
            int64_t bufIdx = sourceOffset_ + (timelineSample - clipStart);
            if (bufIdx < 0 || bufIdx >= bufLen) continue;

            // Linear fade in/out.
            int64_t intoClip = timelineSample - clipStart;
            float fadeGain = 1.0f;
            if (fadeIn_ > 0 && intoClip < fadeIn_) {
                fadeGain = static_cast<float>(intoClip) / static_cast<float>(fadeIn_);
            } else if (fadeOut_ > 0 && intoClip >= len - fadeOut_) {
                int64_t intoTail = len - 1 - intoClip;
                fadeGain = static_cast<float>(intoTail) / static_cast<float>(fadeOut_);
            }
            for (int c = 0; c < outChans; ++c) {
                int srcC = (inChans == 1) ? 0 : std::min(c, inChans - 1);
                ctx.output.channels[c][i] = (*buffer_)[srcC][bufIdx] * fadeGain;
            }
        }
    }

private:
    // Retained off-thread when the node is built; callbacks only dereference
    // the raw channel pointer and never touch shared ownership.
    audio::DecodedAudioAssetPtr decodedAsset_;
    const std::vector<std::vector<float>>* buffer_ = nullptr;
    double sourceSampleRate_ = 48000.0;
    std::atomic<int64_t> clipStart_{0};
    int64_t sourceOffset_ = 0;
    int64_t length_ = 0;
    int64_t fadeIn_ = 64;
    int64_t fadeOut_ = 64;
};

} // namespace dave::engine
