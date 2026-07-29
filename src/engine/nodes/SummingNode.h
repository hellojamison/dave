// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/Node.h"

#include <algorithm>
#include <vector>

namespace dave::engine {

// SummingNode — mixes N input pins into 1 output pin. The number of input pins
// is fixed at construction (set to match how many tracks/edges feed this bus).
//
// The host's CompiledGraph already sums multiple edges that hit the SAME pin
// (see Graph.cpp input gathering). SummingNode exists for the common case of
// "mix these N distinct sources" where the UI wants a single node rather than
// N edges into a single pin. process() just copies each input into the output
// (the host has NOT summed across pins — only across edges within a pin).
class SummingNode : public Node {
public:
    explicit SummingNode(int inputPinCount = 2) : Node("sum"), inputs_(inputPinCount) {}

    int numInputPins() const override { return inputs_; }
    int numOutputPins() const override { return 1; }
    int channelsPerPin() const override { return 2; }

    void process(NodeProcessContext& ctx) override {
        const int n = ctx.numSamples;
        const int outChans = ctx.output.numChannels;
        // Output is pre-zeroed by the host; we add each input in.
        for (int pin = 0; pin < ctx.numInputs; ++pin) {
            const int inChans = std::min(ctx.inputs[pin].numChannels, outChans);
            for (int c = 0; c < inChans; ++c) {
                const float* in = ctx.inputs[pin].channels[c];
                float* out = ctx.output.channels[c];
                for (int i = 0; i < n; ++i) {
                    out[i] += in[i];
                }
            }
        }
    }

private:
    int inputs_;
};

} // namespace dave::engine
