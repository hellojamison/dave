#pragma once

#include "engine/graph/Node.h"

#include <algorithm>
#include <atomic>

namespace dave::engine {

// GainNode — applies a gain (volume) to its 1 input pin → 1 output pin.
// Gain is an atomic, set from the UI thread; process() reads it RT-safely.
// Includes a one-pole smoother to avoid clicks when gain changes abruptly.
class GainNode : public Node {
public:
    GainNode() : Node("gain") {}

    int numInputPins() const override { return 1; }
    int numOutputPins() const override { return 1; }
    int channelsPerPin() const override { return 2; }

    void setGain(double g) { targetGain_.store(g, std::memory_order_relaxed); }
    double gain() const { return targetGain_.load(std::memory_order_relaxed); }

    void prepare(double /*sampleRate*/, int /*maxBlock*/) override {
        currentGain_ = targetGain_.load(std::memory_order_relaxed);
    }

    void process(NodeProcessContext& ctx) override {
        const int n = ctx.numSamples;
        const int chans = std::min(ctx.inputs[0].numChannels, ctx.output.numChannels);
        double target = targetGain_.load(std::memory_order_relaxed);
        // One-pole smoother coefficient: ~10ms time-constant at 48k.
        const double coef = 0.999;
        double g = currentGain_;

        for (int i = 0; i < n; ++i) {
            g += (target - g) * (1.0 - coef);
            for (int c = 0; c < chans; ++c) {
                ctx.output.channels[c][i] =
                    static_cast<float>(ctx.inputs[0].channels[c][i] * g);
            }
        }
        currentGain_ = g;
    }

private:
    std::atomic<double> targetGain_{1.0};
    double currentGain_ = 1.0;
};

} // namespace dave::engine
