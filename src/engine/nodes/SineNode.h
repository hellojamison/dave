#pragma once

#include "engine/graph/Node.h"

#include <atomic>
#include <cmath>

namespace dave::engine {

// SineNode — a sine-wave generator. 0 input pins, 1 output pin (stereo).
// A pure tone source; useful as a test signal and (later) a calibration node.
class SineNode : public Node {
public:
    SineNode() : Node("sine") {}

    void setFrequency(double hz) { frequency_.store(hz, std::memory_order_relaxed); }
    void setGain(double g) { gain_.store(g, std::memory_order_relaxed); }

    int numOutputPins() const override { return 1; }
    int channelsPerPin() const override { return 2; }

    void prepare(double sampleRate, int /*maxBlock*/) override {
        sampleRate_ = sampleRate;
        phase_ = 0.0;
    }

    void process(NodeProcessContext& ctx) override {
        // Generator: no inputs. Write a sine into both output channels.
        const double delta = twoPi * frequency_.load(std::memory_order_relaxed)
                             / ctx.sampleRate;
        const double gain = gain_.load(std::memory_order_relaxed);
        double phase = phase_.load(std::memory_order_relaxed);

        const int chans = ctx.output.numChannels;
        const int n = ctx.numSamples;
        for (int i = 0; i < n; ++i) {
            const float s = static_cast<float>(gain * std::sin(phase));
            phase += delta;
            if (phase >= twoPi) {
                phase -= twoPi;
            }
            for (int c = 0; c < chans; ++c) {
                ctx.output.channels[c][i] = s;
            }
        }
        phase_.store(phase, std::memory_order_relaxed);
    }

private:
    static constexpr double twoPi = 6.28318530717958647692;

    double sampleRate_ = 48000.0;
    std::atomic<double> frequency_{440.0};
    std::atomic<double> gain_{0.2};
    std::atomic<double> phase_{0.0};
};

} // namespace dave::engine
