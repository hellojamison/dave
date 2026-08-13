// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/Node.h"

#include <algorithm>
#include <atomic>

namespace dave::engine {

// Selects a contiguous native capture span. Mono is duplicated into Dave's
// stereo software path; unavailable channels produce silence.
class HardwareInputNode final : public Node {
public:
    HardwareInputNode(int firstChannel, int channelCount)
        : Node("hardwareInput"), first_(std::max(0, firstChannel)),
          count_(std::clamp(channelCount, 1, 2)) {}

    void process(NodeProcessContext& ctx) override {
        if (!ctx.hardwareInput || !ctx.hardwareInput->isValid() ||
            first_ >= ctx.hardwareInput->numChannels) return;
        const auto& input = *ctx.hardwareInput;
        const float* left = input.channels[first_];
        const float* right = count_ == 2 && first_ + 1 < input.numChannels
            ? input.channels[first_ + 1] : left;
        for (int sample = 0; sample < ctx.numSamples; ++sample) {
            ctx.output.channels[0][sample] = left[sample];
            ctx.output.channels[1][sample] = right[sample];
        }
    }

private:
    int first_ = 0;
    int count_ = 1;
};

// A send has gain but no pan law. Applying GainNode at center would attenuate
// both channels by 3 dB, which is correct for a pan pot and wrong for a send.
class SendLevelNode final : public Node {
public:
    explicit SendLevelNode(double gain) : Node("sendLevel"), gain_(gain) {}
    int numInputPins() const override { return 1; }

    void process(NodeProcessContext& ctx) override {
        if (ctx.numInputs == 0) return;
        const float gain = static_cast<float>(gain_.load(std::memory_order_relaxed));
        for (int channel = 0; channel < 2; ++channel) {
            for (int sample = 0; sample < ctx.numSamples; ++sample) {
                ctx.output.channels[channel][sample] =
                    ctx.inputs[0].channels[channel][sample] * gain;
            }
        }
    }

private:
    std::atomic<double> gain_{0.0};
};

// Maps a stereo software channel into one physical-output pair. Stereo routes
// preserve L/R. Mono routes fold L+R with -3 dB per channel and place the sum
// into the chosen physical side of the pair.
class HardwareRouteNode final : public Node {
public:
    HardwareRouteNode(int channelWithinPair, int channelCount)
        : Node("hardwareRoute"), channel_(std::clamp(channelWithinPair, 0, 1)),
          count_(std::clamp(channelCount, 1, 2)) {}
    int numInputPins() const override { return 1; }

    void process(NodeProcessContext& ctx) override {
        if (ctx.numInputs == 0) return;
        const auto& input = ctx.inputs[0];
        if (count_ == 2) {
            for (int sample = 0; sample < ctx.numSamples; ++sample) {
                ctx.output.channels[0][sample] = input.channels[0][sample];
                ctx.output.channels[1][sample] = input.channels[1][sample];
            }
            return;
        }
        constexpr float kMinus3dB = 0.7071067811865476f;
        for (int sample = 0; sample < ctx.numSamples; ++sample) {
            ctx.output.channels[channel_][sample] =
                (input.channels[0][sample] + input.channels[1][sample]) *
                kMinus3dB;
        }
    }

private:
    int channel_ = 0;
    int count_ = 2;
};

// Final graph root. Each stereo input pin is one independent hardware pair;
// the flattened output buses become physical channels in AudioEngine.
class HardwareOutputNode final : public Node {
public:
    explicit HardwareOutputNode(int pairCount)
        : Node("hardwareOutput"), pairs_(std::max(1, pairCount)) {}
    int numInputPins() const override { return pairs_; }
    int numOutputPins() const override { return pairs_; }

    void process(NodeProcessContext& ctx) override {
        for (int pin = 0; pin < std::min(ctx.numInputs, pairs_); ++pin) {
            for (int channel = 0; channel < 2; ++channel) {
                float* output = ctx.output.channels[pin * 2 + channel];
                const float* input = ctx.inputs[pin].channels[channel];
                for (int sample = 0; sample < ctx.numSamples; ++sample) {
                    output[sample] = input[sample];
                }
            }
        }
    }

private:
    int pairs_ = 1;
};

} // namespace dave::engine
