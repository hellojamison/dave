// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/DecodedAudioAsset.h"
#include "document/Fade.h"
#include "engine/graph/Node.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>
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

    // Per-clip linear gain, and whether the clip is muted (silent).
    void setGain(float gain) { gain_ = gain; }
    void setMuted(bool muted) { muted_ = muted; }

    // Fade in/out lengths (samples) and their curve shapes.
    void setFades(int64_t fadeIn, int64_t fadeOut,
                  document::FadeShape fadeInShape = document::FadeShape::Linear,
                  document::FadeShape fadeOutShape = document::FadeShape::Linear) {
        fadeIn_ = fadeIn;
        fadeOut_ = fadeOut;
        fadeInShape_ = fadeInShape;
        fadeOutShape_ = fadeOutShape;
    }

    // Timeline intervals where a clip drawn on top of this one masks it, so
    // overlapping clips don't sum. Set at graph-build time (the node is not
    // live yet), the same as the other setters.
    void setMuteIntervals(std::vector<std::pair<int64_t, int64_t>> intervals) {
        muteIntervals_ = std::move(intervals);
    }

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
        if (!playing || muted_) return;

        const int64_t len = lengthSamples();
        const int64_t clipEnd = clipStart + len;
        const int inChans = static_cast<int>(buffer_->size());
        const int64_t bufLen = static_cast<int64_t>((*buffer_)[0].size());

        for (int i = 0; i < n; ++i) {
            int64_t timelineSample = pos + i;
            if (timelineSample < clipStart || timelineSample >= clipEnd) continue;
            // A clip on top masks this one here, so the two don't sum.
            bool masked = false;
            for (const auto& m : muteIntervals_) {
                if (timelineSample >= m.first && timelineSample < m.second) {
                    masked = true;
                    break;
                }
            }
            if (masked) continue;
            int64_t bufIdx = sourceOffset_ + (timelineSample - clipStart);
            if (bufIdx < 0 || bufIdx >= bufLen) continue;

            // Shaped fade in/out. The ratio is the same one the timeline draws;
            // the shape functions live in document/Fade.h so paint and playback
            // never disagree.
            int64_t intoClip = timelineSample - clipStart;
            float fadeGain = 1.0f;
            if (fadeIn_ > 0 && intoClip < fadeIn_) {
                const float t =
                    static_cast<float>(intoClip) / static_cast<float>(fadeIn_);
                fadeGain = document::fadeInGain(fadeInShape_, t);
            } else if (fadeOut_ > 0 && intoClip >= len - fadeOut_) {
                // intoTail runs fadeOut_-1 .. 0 across the tail, so the ratio
                // falls 1 -> 0; the fade-in curve read on the remaining
                // fraction is the mirrored fade-out.
                int64_t intoTail = len - 1 - intoClip;
                const float t =
                    static_cast<float>(intoTail) / static_cast<float>(fadeOut_);
                fadeGain = document::fadeInGain(fadeOutShape_, t);
            }
            const float sampleGain = fadeGain * gain_;
            for (int c = 0; c < outChans; ++c) {
                int srcC = (inChans == 1) ? 0 : std::min(c, inChans - 1);
                ctx.output.channels[c][i] = (*buffer_)[srcC][bufIdx] * sampleGain;
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
    float gain_ = 1.0f;
    bool muted_ = false;
    int64_t fadeIn_ = 64;
    int64_t fadeOut_ = 64;
    document::FadeShape fadeInShape_ = document::FadeShape::Linear;
    document::FadeShape fadeOutShape_ = document::FadeShape::Linear;
    std::vector<std::pair<int64_t, int64_t>> muteIntervals_;
};

} // namespace dave::engine
