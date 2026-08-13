// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/Types.h"
#include "engine/util/SpscRing.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace dave::engine {

// RT-side capture router. It is deliberately outside the node graph: recording
// receives the device's native input before playback processing, and a graph
// rebuild must not interrupt or re-route an active take.
class RecordController {
public:
    static constexpr double kRingSeconds = 8.0;

    struct ArmedTrackConfig {
        SpscRing* ring = nullptr;
        int channelCount = 1;
        std::array<int, 2> inputChannels{0, -1};

        static ArmedTrackConfig mono(SpscRing& destination, int inputChannel) {
            return {&destination, 1, {inputChannel, -1}};
        }

        static ArmedTrackConfig stereo(SpscRing& destination, int leftInput,
                                       int rightInput) {
            return {&destination, 2, {leftInput, rightInput}};
        }
    };

    RecordController() = default;
    RecordController(const RecordController&) = delete;
    RecordController& operator=(const RecordController&) = delete;

    // UI thread, before capture starts. Rings are owned by DiskWriter and must
    // remain alive and un-reset until processBlock has quiesced. All callback
    // storage is allocated here.
    [[nodiscard]] bool prepare(std::vector<ArmedTrackConfig> armedTracks,
                               int nativeInputChannels,
                               std::size_t maxBlockFrames);
    void clear();

    // Audio thread. Input is device-native interleaved f32. Oversized device
    // blocks are divided into maxBlockFrames chunks so scratch and ring writes
    // remain bounded. Null/missing input channels become silence, preserving
    // the take's sample length. No allocation, lock, file I/O, or syscall.
    void processBlock(const float* interleavedInput, int inputChannels,
                      std::size_t frames, const TimeInfo& time) noexcept;

    bool isPrepared() const noexcept { return prepared_; }
    std::size_t armedTrackCount() const noexcept { return armedTracks_.size(); }
    std::size_t maxBlockFrames() const noexcept { return maxBlockFrames_; }
    int nativeInputChannels() const noexcept { return nativeInputChannels_; }
    int64_t firstSamplePosition() const noexcept {
        return firstSamplePosition_.load(std::memory_order_acquire);
    }

    // DiskWriter accepts its ring size in frames and SpscRing rounds it to the
    // next power of two. Eight seconds rides out a long filesystem stall.
    static std::size_t ringFramesForSampleRate(double sampleRate) noexcept;

private:
    float* scratchChannel(int channel) noexcept {
        return scratch_.data() +
               static_cast<std::size_t>(channel) * maxBlockFrames_;
    }
    const float* scratchChannel(int channel) const noexcept {
        return scratch_.data() +
               static_cast<std::size_t>(channel) * maxBlockFrames_;
    }

    std::vector<ArmedTrackConfig> armedTracks_;
    // One plane per prepared native input plus a permanent zero plane used if
    // a callback reports fewer channels than the device advertised.
    std::vector<float> scratch_;
    int nativeInputChannels_ = 0;
    int zeroChannel_ = 0;
    std::size_t maxBlockFrames_ = 0;
    bool prepared_ = false;
    std::atomic<int64_t> firstSamplePosition_{-1};
};

} // namespace dave::engine
