// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/Node.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace dave::engine {

// GainNode — applies gain + pan to its 1 input pin → 1 output pin.
// Gain and pan are atomics, set from the UI thread; process() reads them
// RT-safely. Gain uses a one-pole smoother to avoid clicks.
// Pan uses a constant-power law: pan=-1 → full left, 0 → center, +1 → full right.
class GainNode : public Node {
public:
    struct MeterSnapshot {
        float peak = 0.0f;
        float rms = 0.0f;
        bool clipped = false;
        // The peak-hold marker: the highest peak within the hold window,
        // which sits still while the bar underneath it falls. A meter whose
        // marker chases the bar cannot answer "what did that transient
        // actually reach", which is the only reason to read a peak.
        float holdPeak = 0.0f;
        // The highest peak since the last clip clear, undecayed. The decaying
        // peak answers "how loud now"; this answers "how far over did it go",
        // which is the question a 32-bit float session actually needs — the
        // cost of running hot is paid at the bottom of the range, long after
        // the transient that caused it has gone.
        float maxPeak = 0.0f;
    };

    struct AutomationPoint {
        int64_t sample = 0;
        double db = 0.0;
    };

    struct PanAutomationPoint {
        int64_t sample = 0;
        double pan = 0.0;
    };

    GainNode() : Node("gain") {}

    int numInputPins() const override { return 1; }
    int numOutputPins() const override { return 1; }
    int channelsPerPin() const override { return 2; }

    // A tap node: metering only, audio passed through untouched. Reusing
    // GainNode rather than adding a second metering node is deliberate — the
    // ballistics, the clip latch and the pre/post banks are all here, and a
    // second implementation of them would drift from this one and put two
    // meters that disagree next to each other.
    // How long the peak marker sits at its maximum before it falls back.
    // Zero lets it follow the bar; a negative or non-finite value holds it
    // until the clip latch is cleared, which is the default.
    void setPeakHoldSeconds(float seconds) {
        peakHoldSeconds_.store(seconds, std::memory_order_relaxed);
    }
    float peakHoldSeconds() const {
        return peakHoldSeconds_.load(std::memory_order_relaxed);
    }

    void setMeterTapOnly(bool tapOnly) {
        tapOnly_.store(tapOnly, std::memory_order_relaxed);
    }
    bool meterTapOnly() const {
        return tapOnly_.load(std::memory_order_relaxed);
    }

    void setGain(double g) { targetGain_.store(g, std::memory_order_relaxed); }
    double gain() const { return targetGain_.load(std::memory_order_relaxed); }
    void setPan(double p) { targetPan_.store(p, std::memory_order_relaxed); }
    double pan() const { return targetPan_.load(std::memory_order_relaxed); }
    void setVolumeAutomation(std::vector<AutomationPoint> points) {
        std::sort(points.begin(), points.end(),
                  [](const auto& a, const auto& b) {
                      return a.sample < b.sample;
                  });
        automation_ = std::move(points);
    }
    const std::vector<AutomationPoint>& volumeAutomation() const {
        return automation_;
    }
    void setPanAutomation(std::vector<PanAutomationPoint> points) {
        for (auto& point : points) {
            point.pan = std::clamp(point.pan, -1.0, 1.0);
        }
        std::sort(points.begin(), points.end(),
                  [](const auto& a, const auto& b) {
                      return a.sample < b.sample;
                  });
        panAutomation_ = std::move(points);
    }
    const std::vector<PanAutomationPoint>& panAutomation() const {
        return panAutomation_;
    }

    void prepare(double sampleRate, int /*maxBlock*/) override {
        currentGain_ = targetGain_.load(std::memory_order_relaxed);
        currentPan_ = targetPan_.load(std::memory_order_relaxed);
        meterSampleRate_ = std::isfinite(sampleRate) && sampleRate > 0.0
            ? static_cast<float>(sampleRate) : 48000.0f;
        for (auto* bank : {&meters_, &preMeters_}) {
        for (auto& meter : *bank) {
            meter.peak.store(0.0f, std::memory_order_relaxed);
            meter.rms.store(0.0f, std::memory_order_relaxed);
            meter.clipped.store(0, std::memory_order_relaxed);
            meter.maxPeak.store(0.0f, std::memory_order_relaxed);
            meter.holdPeak.store(0.0f, std::memory_order_relaxed);
            meter.holdFrames = 0;
        }
        }
    }

    // `preFader` reads the signal as it arrived, before gain, pan and
    // automation — what the source is actually delivering, independent of how
    // the track is currently balanced.
    [[nodiscard]] MeterSnapshot meter(int channel,
                                      bool preFader = false) const noexcept {
        if (channel < 0 || channel >= static_cast<int>(meters_.size())) return {};
        const auto& state = preFader
            ? preMeters_[static_cast<size_t>(channel)]
            : meters_[static_cast<size_t>(channel)];
        return {state.peak.load(std::memory_order_relaxed),
                state.rms.load(std::memory_order_relaxed),
                state.clipped.load(std::memory_order_acquire) != 0,
                state.holdPeak.load(std::memory_order_relaxed),
                state.maxPeak.load(std::memory_order_relaxed)};
    }

    void clearMeterClips() noexcept {
        // The over-hold clears with the clip latch. They answer the same
        // question — "did this go over, and how far" — so resetting one and
        // not the other leaves the meter contradicting itself.
        for (auto* bank : {&meters_, &preMeters_}) {
            for (auto& meter : *bank) {
                meter.maxPeak.store(0.0f, std::memory_order_relaxed);
                meter.holdPeak.store(0.0f, std::memory_order_relaxed);
                meter.holdFrames = 0;
                meter.clipped.store(0, std::memory_order_release);
            }
        }
    }

    void process(NodeProcessContext& ctx) override {
        const int n = ctx.numSamples;
        const int chans = std::min(ctx.inputs[0].numChannels, ctx.output.numChannels);
        double target = targetGain_.load(std::memory_order_relaxed);
        double panTarget = targetPan_.load(std::memory_order_relaxed);
        const double coef = 0.999;
        double g = currentGain_;
        double p = currentPan_;
        const bool tapOnly = tapOnly_.load(std::memory_order_relaxed);
        std::array<float, 2> blockPeak{};
        std::array<double, 2> squareSum{};
        std::array<bool, 2> blockClipped{};
        // The pre-fader tap costs one extra max and one multiply-add per
        // sample. Publishing both every block means switching the meter's
        // source is a UI decision with no round trip to the audio thread.
        std::array<float, 2> preBlockPeak{};
        std::array<double, 2> preSquareSum{};
        std::array<bool, 2> preBlockClipped{};
        const int64_t blockStart = ctx.time != nullptr ? ctx.time->samplePos : 0;
        size_t nextAutomation = static_cast<size_t>(std::lower_bound(
            automation_.begin(), automation_.end(), blockStart,
            [](const AutomationPoint& point, int64_t sample) {
                return point.sample < sample;
            }) - automation_.begin());
        size_t nextPanAutomation = static_cast<size_t>(std::lower_bound(
            panAutomation_.begin(), panAutomation_.end(), blockStart,
            [](const PanAutomationPoint& point, int64_t sample) {
                return point.sample < sample;
            }) - panAutomation_.begin());

        for (int i = 0; i < n; ++i) {
            g += (target - g) * (1.0 - coef);
            p += (panTarget - p) * (1.0 - coef);
            double automationGain = 1.0;
            if (!automation_.empty()) {
                const int64_t sample = blockStart + i;
                while (nextAutomation < automation_.size() &&
                       automation_[nextAutomation].sample <= sample) {
                    ++nextAutomation;
                }
                double db = 0.0;
                if (nextAutomation == 0) {
                    db = automation_.front().db;
                } else if (nextAutomation >= automation_.size()) {
                    db = automation_.back().db;
                } else {
                    const auto& before = automation_[nextAutomation - 1];
                    const auto& after = automation_[nextAutomation];
                    const int64_t span = after.sample - before.sample;
                    const double amount = span > 0
                        ? static_cast<double>(sample - before.sample) /
                              static_cast<double>(span)
                        : 1.0;
                    db = before.db + (after.db - before.db) * amount;
                }
                automationGain = std::pow(10.0, db / 20.0);
            }
            double automatedPan = p;
            if (!panAutomation_.empty()) {
                const int64_t sample = blockStart + i;
                while (nextPanAutomation < panAutomation_.size() &&
                       panAutomation_[nextPanAutomation].sample <= sample) {
                    ++nextPanAutomation;
                }
                if (nextPanAutomation == 0) {
                    automatedPan = panAutomation_.front().pan;
                } else if (nextPanAutomation >= panAutomation_.size()) {
                    automatedPan = panAutomation_.back().pan;
                } else {
                    const auto& before =
                        panAutomation_[nextPanAutomation - 1];
                    const auto& after = panAutomation_[nextPanAutomation];
                    const int64_t span = after.sample - before.sample;
                    const double amount = span > 0
                        ? static_cast<double>(sample - before.sample) /
                              static_cast<double>(span)
                        : 1.0;
                    automatedPan = before.pan +
                        (after.pan - before.pan) * amount;
                }
            }
            // Constant-power pan law: L gain = cos((p+1)*pi/4), R = sin(...).
            // A tap bypasses it entirely: at centre the law is 0.707 a side,
            // so even a unity-gain pass would quietly cost the chain 3 dB.
            double angle = (automatedPan + 1.0) * 0.7853981633974483;
            const double effectiveGain = tapOnly ? 1.0 : g * automationGain;
            double lGain = tapOnly ? 1.0 : effectiveGain * std::cos(angle);
            double rGain = tapOnly ? 1.0 : effectiveGain * std::sin(angle);
            if (chans >= 2) {
                accumulateMeterSample(0, ctx.inputs[0].channels[0][i],
                                      preBlockPeak, preSquareSum,
                                      preBlockClipped);
                accumulateMeterSample(1, ctx.inputs[0].channels[1][i],
                                      preBlockPeak, preSquareSum,
                                      preBlockClipped);
                const float left = static_cast<float>(
                    ctx.inputs[0].channels[0][i] * lGain);
                const float right = static_cast<float>(
                    ctx.inputs[0].channels[1][i] * rGain);
                ctx.output.channels[0][i] = left;
                ctx.output.channels[1][i] = right;
                accumulateMeterSample(0, left, blockPeak, squareSum,
                                      blockClipped);
                accumulateMeterSample(1, right, blockPeak, squareSum,
                                      blockClipped);
            } else if (chans == 1) {
                accumulateMeterSample(0, ctx.inputs[0].channels[0][i],
                                      preBlockPeak, preSquareSum,
                                      preBlockClipped);
                const float mono = static_cast<float>(
                    ctx.inputs[0].channels[0][i] * effectiveGain);
                ctx.output.channels[0][i] = mono;
                accumulateMeterSample(0, mono, blockPeak, squareSum,
                                      blockClipped);
            }
        }
        publishMeters(n, chans, blockPeak, squareSum, blockClipped, meters_);
        publishMeters(n, chans, preBlockPeak, preSquareSum, preBlockClipped,
                      preMeters_);
        currentGain_ = g;
        currentPan_ = p;
    }

private:
    struct MeterState {
        std::atomic<float> peak{0.0f};
        std::atomic<float> rms{0.0f};
        std::atomic<std::uint32_t> clipped{0};
        std::atomic<float> maxPeak{0.0f};
        std::atomic<float> holdPeak{0.0f};
        // Frames since the marker was last renewed. RT-thread only; the UI
        // never reads it, so it needs no atomicity of its own.
        int holdFrames = 0;
    };

    static void accumulateMeterSample(
        size_t channel, float sample, std::array<float, 2>& blockPeak,
        std::array<double, 2>& squareSum,
        std::array<bool, 2>& blockClipped) noexcept {
        float magnitude = std::fabs(sample);
        if (std::isnan(magnitude)) {
            magnitude = 0.0f;
        } else if (std::isinf(magnitude)) {
            magnitude = std::numeric_limits<float>::max();
        }
        blockPeak[channel] = std::max(blockPeak[channel], magnitude);
        const double value = static_cast<double>(magnitude);
        squareSum[channel] += value * value;
        blockClipped[channel] = blockClipped[channel] || magnitude >= 1.0f;
    }

    // The marker renews on any new maximum, sits still for the hold window,
    // and then falls to whatever the bar is showing — rather than decaying on
    // its own, which would leave it hanging in mid-air below a peak that has
    // already gone.
    static void updatePeakHold(MeterState& state, float blockPeak, int frames,
                               float holdFrames) noexcept {
        const float held = state.holdPeak.load(std::memory_order_relaxed);
        if (blockPeak >= held) {
            state.holdPeak.store(blockPeak, std::memory_order_relaxed);
            state.holdFrames = 0;
            return;
        }
        // Negative or non-finite means hold until cleared.
        if (!(holdFrames >= 0.0f)) return;
        state.holdFrames += frames;
        if (static_cast<float>(state.holdFrames) >= holdFrames) {
            state.holdPeak.store(state.peak.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
            // Pinned at the threshold rather than reset: once the window has
            // expired the marker rides the bar down until a new maximum
            // renews it. Resetting would make it hold again at the lower
            // value and stagger downwards in steps.
            state.holdFrames = static_cast<int>(holdFrames);
        }
    }

    void publishMeters(
        int frames, int channels, const std::array<float, 2>& blockPeak,
        const std::array<double, 2>& squareSum,
        const std::array<bool, 2>& blockClipped,
        std::array<MeterState, 2>& target) noexcept {
        if (frames <= 0) return;
        const float holdSetting =
            peakHoldSeconds_.load(std::memory_order_relaxed);
        const float holdSeconds = std::isfinite(holdSetting)
            ? holdSetting * meterSampleRate_ : -1.0f;
        constexpr float peakDecaySeconds = 0.75f;
        constexpr float rmsDecaySeconds = 0.30f;
        const float peakRelease = std::max(
            0.0f, 1.0f - static_cast<float>(frames) /
                (meterSampleRate_ * peakDecaySeconds));
        const float rmsRelease = std::max(
            0.0f, 1.0f - static_cast<float>(frames) /
                (meterSampleRate_ * rmsDecaySeconds));
        for (size_t channel = 0; channel < target.size(); ++channel) {
            const bool active = static_cast<int>(channel) < channels;
            const float blockRms = active
                ? static_cast<float>(std::sqrt(
                      squareSum[channel] / static_cast<double>(frames)))
                : 0.0f;
            auto& state = target[channel];
            const float releasedPeak =
                state.peak.load(std::memory_order_relaxed) * peakRelease;
            const float releasedRms =
                state.rms.load(std::memory_order_relaxed) * rmsRelease;
            state.peak.store(
                std::max(active ? blockPeak[channel] : 0.0f, releasedPeak),
                std::memory_order_relaxed);
            state.rms.store(std::max(blockRms, releasedRms),
                            std::memory_order_relaxed);
            if (active) {
                const float held =
                    state.maxPeak.load(std::memory_order_relaxed);
                if (blockPeak[channel] > held) {
                    state.maxPeak.store(blockPeak[channel],
                                        std::memory_order_relaxed);
                }
                updatePeakHold(state, blockPeak[channel], frames, holdSeconds);
            }
            if (active && blockClipped[channel]) {
                state.clipped.store(1, std::memory_order_release);
            }
        }
    }

    static_assert(std::atomic<float>::is_always_lock_free,
                  "track metering requires lock-free float atomics");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "track metering requires lock-free integer atomics");

    std::atomic<double> targetGain_{1.0};
    std::atomic<double> targetPan_{0.0};
    std::atomic<bool> tapOnly_{false};
    // Negative: hold until cleared. Matches the shipped behaviour, so a
    // preference nobody sets changes nothing.
    std::atomic<float> peakHoldSeconds_{-1.0f};
    // Copied into the node before the graph is published. The RT thread only
    // reads this immutable storage; edits build and publish a new graph.
    std::vector<AutomationPoint> automation_;
    std::vector<PanAutomationPoint> panAutomation_;
    std::array<MeterState, 2> meters_{};
    std::array<MeterState, 2> preMeters_{};
    float meterSampleRate_ = 48000.0f;
    double currentGain_ = 1.0;
    double currentPan_ = 0.0;
};

} // namespace dave::engine
