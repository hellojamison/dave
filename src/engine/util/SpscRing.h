// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/util/SpscQueue.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dave::engine {

// A single-producer / single-consumer lock-free ring of interleaved f32 frames.
//
// The audio thread is the producer, a disk-writer thread the consumer. Only
// those two, and only in those roles — two producers or two consumers break the
// index arithmetic silently.
//
// Positions are monotonic 64-bit counters that are never wrapped, only masked
// on access. If a block does not fit, it is dropped whole: a partial block
// would break the interleave. A gap record carries the exact ideal stream
// position so the consumer can put silence where the missing block belonged.
class SpscRing {
public:
    struct Gap {
        uint64_t streamPos = 0;
        uint64_t frames = 0;
    };
    static_assert(sizeof(Gap) == 16);

    // One acquire snapshot for the consumer. `publishedWritePos` and the gap
    // observation belong together; read(view) never consumes samples that
    // were published after the view was acquired.
    struct ConsumerView {
        uint64_t publishedWritePos = 0;
        bool hasGap = false;
        Gap gap;
    };

    SpscRing() = default;

    // Sized in frames, rounded up to a power of two so wrapping is a mask.
    // UI thread only. Not safe to call while either side is running.
    void reset(size_t frames, int channels, size_t gapRecords = 4096) {
        channels_ = std::max(1, channels);
        size_t capacity = 1;
        while (capacity < std::max<size_t>(frames, 2)) capacity <<= 1;
        capacityFrames_ = capacity;
        mask_ = capacity - 1;
        data_.assign(capacity * static_cast<size_t>(channels_), 0.0f);
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
        droppedFrames_.store(0, std::memory_order_relaxed);
        overrunBlocks_.store(0, std::memory_order_relaxed);
        unlocatedDropBlocks_.store(0, std::memory_order_relaxed);
        pendingUnlocatedFrames_ = 0;
        gaps_.reset(gapRecords);
    }

    size_t capacityFrames() const { return capacityFrames_; }
    int channels() const { return channels_; }

    // Producer (RT thread). `src` is planar: src[c][i]. Returns false if the
    // block did not fit, in which case nothing is written but the loss and its
    // stream position are recorded. Allocation-free, lock-free, bounded.
    bool write(const float* const* src, int numChannels, size_t frames) {
        if (frames == 0) return true;

        const uint64_t w = writePos_.load(std::memory_order_relaxed);
        if (capacityFrames_ == 0) {
            recordDrop(w, frames);
            return false;
        }

        const uint64_t r = readPos_.load(std::memory_order_acquire);
        const size_t used = static_cast<size_t>(w - r);
        if (capacityFrames_ - used < frames) {
            recordDrop(w, frames);
            return false;
        }

        const int chans = std::min(numChannels, channels_);
        for (size_t i = 0; i < frames; ++i) {
            float* dst = &data_[(static_cast<size_t>((w + i) & mask_)) *
                                static_cast<size_t>(channels_)];
            for (int c = 0; c < channels_; ++c) {
                dst[c] = (c < chans && src[c] != nullptr) ? src[c][i] : 0.0f;
            }
        }
        writePos_.store(w + frames, std::memory_order_release);
        return true;
    }

    // Consumer (writer thread).
    ConsumerView consumerView() const {
        ConsumerView view;
        // This acquire MUST happen before peeking at gaps. The producer
        // publishes a gap record before it publishes any post-gap samples. If
        // the order were reversed, the consumer could observe those samples,
        // miss their gap, and drain across it on weakly ordered CPUs.
        view.publishedWritePos = writePos_.load(std::memory_order_acquire);
        view.hasGap = gaps_.peek(view.gap);
        return view;
    }

    size_t availableFrames() const {
        return static_cast<size_t>(writePos_.load(std::memory_order_acquire) -
                                   readPos_.load(std::memory_order_relaxed));
    }

    // Convenience read for consumers that do not place gaps themselves.
    size_t read(float* dst, size_t maxFrames) {
        return read(dst, maxFrames, consumerView());
    }

    // Reads only samples covered by `view`. This prevents a second acquire
    // from seeing post-gap samples newer than the paired gap observation.
    size_t read(float* dst, size_t maxFrames, const ConsumerView& view) {
        const uint64_t r = readPos_.load(std::memory_order_relaxed);
        if (view.publishedWritePos <= r) return 0;
        const size_t available =
            static_cast<size_t>(view.publishedWritePos - r);
        const size_t frames = std::min(available, maxFrames);
        for (size_t i = 0; i < frames; ++i) {
            const float* srcFrame =
                &data_[(static_cast<size_t>((r + i) & mask_)) *
                       static_cast<size_t>(channels_)];
            std::memcpy(dst + i * static_cast<size_t>(channels_), srcFrame,
                        static_cast<size_t>(channels_) * sizeof(float));
        }
        readPos_.store(r + frames, std::memory_order_release);
        return frames;
    }

    bool popGap() { return gaps_.pop(); }

    // Accounting (either thread).
    uint64_t droppedFrames() const {
        return droppedFrames_.load(std::memory_order_acquire);
    }
    uint64_t framesConsumed() const {
        return readPos_.load(std::memory_order_acquire);
    }
    uint64_t overrunBlocks() const {
        return overrunBlocks_.load(std::memory_order_relaxed);
    }
    uint64_t unlocatedDropBlocks() const {
        return unlocatedDropBlocks_.load(std::memory_order_relaxed);
    }
    uint64_t framesProduced() const {
        return writePos_.load(std::memory_order_acquire) + droppedFrames();
    }

private:
    void recordDrop(uint64_t writePos, size_t frames) {
        const uint64_t dropped =
            droppedFrames_.load(std::memory_order_relaxed);
        const uint64_t idealGapPos = writePos + dropped;

        // If the queue was previously full, fold those unlocated frames into
        // the next record that can be published. Their silence lands late,
        // but subtracting it from the record position makes the final `frames`
        // portion start at its exact ideal position and restores alignment for
        // everything after it. If no later record can be published, the disk
        // writer makes the remainder good at EOF.
        const Gap gap{idealGapPos - pendingUnlocatedFrames_,
                      pendingUnlocatedFrames_ + frames};
        if (gaps_.push(gap)) {
            pendingUnlocatedFrames_ = 0;
        } else {
            pendingUnlocatedFrames_ += frames;
            unlocatedDropBlocks_.fetch_add(1, std::memory_order_relaxed);
        }

        // Publish accounting only after the gap record. Later successful
        // samples are likewise published after it by writePos_'s release.
        droppedFrames_.fetch_add(frames, std::memory_order_release);
        overrunBlocks_.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<float> data_;
    size_t capacityFrames_ = 0;
    size_t mask_ = 0;
    int channels_ = 1;

    alignas(64) std::atomic<uint64_t> writePos_{0};
    alignas(64) std::atomic<uint64_t> readPos_{0};
    alignas(64) std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<uint64_t> overrunBlocks_{0};
    std::atomic<uint64_t> unlocatedDropBlocks_{0};

    SpscQueue<Gap> gaps_;
    // Producer-only. Folded into the next successfully published gap.
    uint64_t pendingUnlocatedFrames_ = 0;
};

} // namespace dave::engine
