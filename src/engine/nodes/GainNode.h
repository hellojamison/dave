// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/Node.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace dave::engine {

// GainNode — applies gain + pan to its 1 input pin → 1 output pin.
// Gain and pan are atomics, set from the UI thread; process() reads them
// RT-safely. Gain uses a one-pole smoother to avoid clicks.
// Pan uses a constant-power law: pan=-1 → full left, 0 → center, +1 → full right.
class GainNode : public Node {
public:
    GainNode() : Node("gain") {}

    int numInputPins() const override { return 1; }
    int numOutputPins() const override { return 1; }
    int channelsPerPin() const override { return 2; }

    void setGain(double g) { targetGain_.store(g, std::memory_order_relaxed); }
    double gain() const { return targetGain_.load(std::memory_order_relaxed); }
    void setPan(double p) { targetPan_.store(p, std::memory_order_relaxed); }
    double pan() const { return targetPan_.load(std::memory_order_relaxed); }

    void prepare(double /*sampleRate*/, int /*maxBlock*/) override {
        currentGain_ = targetGain_.load(std::memory_order_relaxed);
        currentPan_ = targetPan_.load(std::memory_order_relaxed);
    }

    void process(NodeProcessContext& ctx) override {
        const int n = ctx.numSamples;
        const int chans = std::min(ctx.inputs[0].numChannels, ctx.output.numChannels);
        double target = targetGain_.load(std::memory_order_relaxed);
        double panTarget = targetPan_.load(std::memory_order_relaxed);
        const double coef = 0.999;
        double g = currentGain_;
        double p = currentPan_;

        for (int i = 0; i < n; ++i) {
            g += (target - g) * (1.0 - coef);
            p += (panTarget - p) * (1.0 - coef);
            // Constant-power pan law: L gain = cos((p+1)*pi/4), R = sin(...).
            double angle = (p + 1.0) * 0.7853981633974483; // (p+1) * pi/4
            double lGain = g * std::cos(angle);
            double rGain = g * std::sin(angle);
            if (chans >= 2) {
                ctx.output.channels[0][i] = static_cast<float>(
                    ctx.inputs[0].channels[0][i] * lGain);
                ctx.output.channels[1][i] = static_cast<float>(
                    ctx.inputs[0].channels[1][i] * rGain);
            } else if (chans == 1) {
                ctx.output.channels[0][i] = static_cast<float>(
                    ctx.inputs[0].channels[0][i] * g);
            }
        }
        currentGain_ = g;
        currentPan_ = p;
    }

private:
    std::atomic<double> targetGain_{1.0};
    std::atomic<double> targetPan_{0.0};
    double currentGain_ = 1.0;
    double currentPan_ = 0.0;
};

} // namespace dave::engine
