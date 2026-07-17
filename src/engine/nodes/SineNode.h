#pragma once

#include "engine/graph/Node.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace dave::engine {

// SineNode — a sine-wave generator. Phase 0's "make a sound" proof node.
//
// A pure generator: no inputs, two outputs (stereo). Its frequency and gain
// are set from the UI thread before prepare()/process(). Later phases will add
// sample-accurate parameter ramps via the ParamRamp in ProcessContext.
class SineNode : public Node {
public:
    SineNode() = default;

    void setFrequency(double hz) { frequency_ = hz; }
    void setGain(double g) { gain_ = g; }

    void prepare(double sampleRate, int /*maxBlock*/) override {
        sampleRate_ = sampleRate;
        // phase increment per sample = 2π·f / sr
        updatePhaseDelta();
        phase_ = 0.0;
    }

    void process(ProcessContext& ctx) override {
        // We have no inputs; write a sine into each output channel.
        const int outs = std::min(ctx.numOutChannels, 2);
        if (outs == 0)
            return;

        const double delta = phaseDelta_.load(std::memory_order_relaxed);
        const double gain = gain_.load(std::memory_order_relaxed);
        double phase = phase_.load(std::memory_order_relaxed);

        for (int i = 0; i < ctx.numSamples; ++i) {
            const float sample = static_cast<float>(gain * std::sin(phase));
            phase += delta;
            // keep phase bounded to [0, 2π) to avoid precision drift over time
            if (phase >= twoPi)
                phase -= twoPi;
            for (int c = 0; c < outs; ++c)
                ctx.outChannels[c][i] = sample;
        }

        phase_.store(phase, std::memory_order_relaxed);
    }

    void release() override {
        phase_ = 0.0;
    }

    int numInputs() const override { return 0; }
    int numOutputs() const override { return 2; }

private:
    void updatePhaseDelta() {
        // frequency_ and sampleRate_ are set from the UI thread before prepare.
        const double f = frequency_.load(std::memory_order_relaxed);
        const double sr = sampleRate_;
        phaseDelta_.store(twoPi * f / sr, std::memory_order_relaxed);
    }

    static constexpr double twoPi = 6.28318530717958647692;

    double sampleRate_ = 44100.0;
    std::atomic<double> frequency_{440.0};
    std::atomic<double> gain_{0.2};
    std::atomic<double> phaseDelta_{0.0};
    std::atomic<double> phase_{0.0};
};

} // namespace dave::engine
