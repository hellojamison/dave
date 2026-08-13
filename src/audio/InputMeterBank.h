// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dave::audio {

// Fixed-capacity input metering shared by the audio callback and the UI.
//
// The callback side is deliberately boring: fixed arrays, arithmetic and
// lock-free atomics only. reset() chooses how many of those slots are active
// before a device starts; processInterleaved() never allocates, locks or does
// I/O. The UI may read snapshots and clear clip latches concurrently.
class InputMeterBank {
public:
    struct Snapshot {
        float peak = 0.0f;
        float rms = 0.0f;
        bool clipped = false;
    };

    static constexpr std::uint32_t kMaxChannels = MA_MAX_CHANNELS;

    // Attack is immediate. Release is frame- and sample-rate-aware so changing
    // a backend's callback size does not visibly change meter ballistics.
    static constexpr float kPeakDecaySeconds = 0.75f;
    static constexpr float kRmsDecaySeconds = 0.30f;

    InputMeterBank() noexcept { clear(); }

    InputMeterBank(const InputMeterBank&) = delete;
    InputMeterBank& operator=(const InputMeterBank&) = delete;

    // Device-control thread. Values above miniaudio's hard limit are clamped.
    // Atomic storage makes an accidental concurrent UI read data-race-free,
    // but callers should still reset only while the device is stopped.
    void reset(std::uint32_t channels, float sampleRate = 48000.0f) noexcept {
        clear();
        sampleRate_ = std::isfinite(sampleRate) && sampleRate > 0.0f
            ? sampleRate
            : 48000.0f;
        channelCount_.store(std::min(channels, kMaxChannels),
                            std::memory_order_release);
    }

    // Clears levels and clip latches for every slot, including inactive ones,
    // so increasing the channel count cannot reveal stale device state.
    void clear() noexcept {
        for (auto& channel : channels_) {
            channel.peak.store(0.0f, std::memory_order_relaxed);
            channel.rms.store(0.0f, std::memory_order_relaxed);
            channel.clipped.store(0, std::memory_order_relaxed);
        }
    }

    void clearClip(std::uint32_t channel) noexcept {
        if (channel >= channelCount()) return;
        channels_[channel].clipped.store(0, std::memory_order_release);
    }

    [[nodiscard]] std::uint32_t channelCount() const noexcept {
        return channelCount_.load(std::memory_order_acquire);
    }

    // Out-of-range channels intentionally read as silence. Each member is an
    // atomic observation; strict cross-field coherence is unnecessary for a
    // visual meter and would require a retry loop on the UI thread.
    [[nodiscard]] Snapshot snapshot(std::uint32_t channel) const noexcept {
        if (channel >= channelCount()) return {};
        const auto& state = channels_[channel];
        return {state.peak.load(std::memory_order_relaxed),
                state.rms.load(std::memory_order_relaxed),
                state.clipped.load(std::memory_order_acquire) != 0};
    }

    // RT audio thread. `input` is interleaved f32. A null buffer is silence;
    // configured channels not supplied by the device also decay as silence.
    // `inputChannels` is clamped before it is used as the frame stride so a
    // malformed count cannot index beyond miniaudio's supported channel span.
    void processInterleaved(const float* input, std::uint32_t frames,
                            std::uint32_t inputChannels) noexcept {
        if (frames == 0) return;

        const std::uint32_t active = channelCount();
        const std::uint32_t stride = std::min(inputChannels, kMaxChannels);
        const std::uint32_t readable = input == nullptr
            ? 0
            : std::min(active, stride);
        const float peakRelease = std::max(
            0.0f, 1.0f - static_cast<float>(frames) /
                (sampleRate_ * kPeakDecaySeconds));
        const float rmsRelease = std::max(
            0.0f, 1.0f - static_cast<float>(frames) /
                (sampleRate_ * kRmsDecaySeconds));

        for (std::uint32_t channel = 0; channel < active; ++channel) {
            float blockPeak = 0.0f;
            double squareSum = 0.0;
            bool blockClipped = false;

            if (channel < readable) {
                for (std::uint32_t frame = 0; frame < frames; ++frame) {
                    const float sample = input[
                        static_cast<std::size_t>(frame) * stride + channel];
                    float magnitude = std::fabs(sample);
                    if (std::isnan(magnitude)) {
                        // A NaN must not poison the meter's persistent state.
                        magnitude = 0.0f;
                    } else if (std::isinf(magnitude)) {
                        magnitude = std::numeric_limits<float>::max();
                    }
                    blockPeak = std::max(blockPeak, magnitude);
                    const double value = static_cast<double>(magnitude);
                    squareSum += value * value;
                    blockClipped = blockClipped || magnitude >= 1.0f;
                }
            }

            const float blockRms = static_cast<float>(
                std::sqrt(squareSum / static_cast<double>(frames)));
            auto& state = channels_[channel];
            const float releasedPeak =
                state.peak.load(std::memory_order_relaxed) * peakRelease;
            const float releasedRms =
                state.rms.load(std::memory_order_relaxed) * rmsRelease;
            state.peak.store(std::max(blockPeak, releasedPeak),
                             std::memory_order_relaxed);
            state.rms.store(std::max(blockRms, releasedRms),
                            std::memory_order_relaxed);
            if (blockClipped) {
                state.clipped.store(1, std::memory_order_release);
            }
        }
    }

private:
    struct ChannelState {
        std::atomic<float> peak{0.0f};
        std::atomic<float> rms{0.0f};
        std::atomic<std::uint32_t> clipped{0};
    };

    static_assert(std::atomic<float>::is_always_lock_free,
                  "input metering requires lock-free float atomics");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "input metering requires lock-free integer atomics");

    std::array<ChannelState, kMaxChannels> channels_{};
    std::atomic<std::uint32_t> channelCount_{0};
    // Set only while the device is stopped, then read by the sole RT writer.
    float sampleRate_ = 48000.0f;
};

} // namespace dave::audio
