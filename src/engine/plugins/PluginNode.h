// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/Node.h"
#include "engine/plugins/PluginInstance.h"

#include <algorithm>
#include <memory>

namespace dave::engine {

// PluginNode wraps a PluginInstance as a graph Node: 1 stereo input pin ->
// plugin -> 1 stereo output pin. This is how a VST3 sits in the signal chain.
//
// The PluginInstance is owned externally (typically by the GraphBuilder /
// Edit) and shared here; the node just calls instance->process() each block.
// Parameters can be set on the instance directly (RT-safe queue, later).
class PluginNode : public Node {
public:
    explicit PluginNode(std::shared_ptr<PluginInstance> instance)
        : Node("plugin"), instance_(std::move(instance)) {}

    int numInputPins() const override { return 1; }
    int numOutputPins() const override { return 1; }
    int channelsPerPin() const override { return 2; }

    void prepare(double sampleRate, int maxBlock) override {
        if (instance_) instance_->prepare(sampleRate, maxBlock);
    }

    void process(NodeProcessContext& ctx) override {
        if (!instance_ || !instance_->isLoaded() || ctx.numInputs == 0) {
            // No instance or no input: pass through silence (host pre-zeroes
            // output, so just return).
            return;
        }
        // Feed the input bus into the plugin; it writes our output.
        // The host has already summed any edges into input pin 0.
        const auto& in = ctx.inputs[0];
        instance_->process(in.channels, ctx.output.channels,
                           std::min(in.numChannels, ctx.output.numChannels),
                           ctx.numSamples,
                           ctx.time ? *ctx.time : TimeInfo{});
    }

private:
    std::shared_ptr<PluginInstance> instance_;
};

} // namespace dave::engine
