// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/record/RecordController.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dave::engine {

bool RecordController::prepare(std::vector<ArmedTrackConfig> armedTracks,
                               int nativeInputChannels,
                               std::size_t maxBlockFrames) {
    clear();
    if (nativeInputChannels < 0 || maxBlockFrames == 0) return false;

    for (const auto& track : armedTracks) {
        if (track.ring == nullptr ||
            (track.channelCount != 1 && track.channelCount != 2) ||
            track.ring->channels() != track.channelCount) {
            return false;
        }
        for (int channel = 0; channel < track.channelCount; ++channel) {
            const int source = track.inputChannels[channel];
            if (source < 0 || source >= nativeInputChannels) return false;
        }
    }

    nativeInputChannels_ = nativeInputChannels;
    zeroChannel_ = nativeInputChannels;
    maxBlockFrames_ = maxBlockFrames;
    armedTracks_ = std::move(armedTracks);
    scratch_.assign((static_cast<std::size_t>(nativeInputChannels_) + 1) *
                        maxBlockFrames_,
                    0.0f);
    firstSamplePosition_.store(-1, std::memory_order_release);
    prepared_ = true;
    return true;
}

void RecordController::clear() {
    prepared_ = false;
    armedTracks_.clear();
    scratch_.clear();
    nativeInputChannels_ = 0;
    zeroChannel_ = 0;
    maxBlockFrames_ = 0;
    firstSamplePosition_.store(-1, std::memory_order_release);
}

void RecordController::processBlock(const float* interleavedInput,
                                    int inputChannels, std::size_t frames,
                                    const TimeInfo& time) noexcept {
    if (!prepared_ || !time.isRecording || frames == 0 || armedTracks_.empty()) {
        return;
    }

    int64_t unset = -1;
    firstSamplePosition_.compare_exchange_strong(
        unset, time.samplePos, std::memory_order_release,
        std::memory_order_relaxed);

    const int callbackChannels = std::max(0, inputChannels);
    for (std::size_t offset = 0; offset < frames;) {
        const std::size_t chunk = std::min(maxBlockFrames_, frames - offset);

        // Deinterleave only the native channels the UI prepared. A callback
        // with no input (or fewer channels) produces silence for the missing
        // planes instead of shortening every armed take.
        for (int channel = 0; channel < nativeInputChannels_; ++channel) {
            float* destination = scratchChannel(channel);
            if (interleavedInput != nullptr && channel < callbackChannels) {
                for (std::size_t frame = 0; frame < chunk; ++frame) {
                    destination[frame] =
                        interleavedInput[(offset + frame) *
                                             static_cast<std::size_t>(callbackChannels) +
                                         static_cast<std::size_t>(channel)];
                }
            } else {
                std::fill_n(destination, chunk, 0.0f);
            }
        }
        std::fill_n(scratchChannel(zeroChannel_), chunk, 0.0f);

        for (const auto& track : armedTracks_) {
            const float* selected[2] = {
                scratchChannel(track.inputChannels[0]),
                track.channelCount == 2
                    ? scratchChannel(track.inputChannels[1])
                    : scratchChannel(zeroChannel_),
            };
            track.ring->write(selected, track.channelCount, chunk);
        }
        offset += chunk;
    }
}

std::size_t RecordController::ringFramesForSampleRate(
    double sampleRate) noexcept {
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) return 0;
    const long double frames =
        static_cast<long double>(sampleRate) * kRingSeconds;
    if (frames >= static_cast<long double>(
                      std::numeric_limits<std::size_t>::max())) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(std::ceil(frames));
}

} // namespace dave::engine
