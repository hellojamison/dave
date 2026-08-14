// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/nodes/GainNode.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <vector>

using Catch::Approx;

namespace {

void process(dave::engine::GainNode& node,
             std::vector<std::vector<float>>& input,
             std::vector<std::vector<float>>& output,
             int64_t samplePos = 0) {
    std::vector<float*> inputPointers;
    std::vector<float*> outputPointers;
    for (auto& channel : input) inputPointers.push_back(channel.data());
    for (auto& channel : output) outputPointers.push_back(channel.data());
    dave::engine::AudioBus inputBus{
        inputPointers.data(), static_cast<int>(inputPointers.size()),
        static_cast<int>(input.front().size())};
    dave::engine::TimeInfo time;
    time.samplePos = samplePos;
    dave::engine::NodeProcessContext context;
    context.numSamples = static_cast<int>(input.front().size());
    context.time = &time;
    context.inputs = &inputBus;
    context.numInputs = 1;
    context.output = dave::engine::AudioBus{
        outputPointers.data(), static_cast<int>(outputPointers.size()),
        context.numSamples};
    node.process(context);
}

} // namespace

TEST_CASE("track meters measure the post-fader stereo output independently",
          "[track-meter][graph]") {
    dave::engine::GainNode node;
    node.prepare(48000.0, 64);
    std::vector<std::vector<float>> input(
        2, std::vector<float>(64, 0.0f));
    std::vector<std::vector<float>> output(
        2, std::vector<float>(64, 0.0f));
    std::fill(input[0].begin(), input[0].end(), 1.0f);
    process(node, input, output);

    const auto left = node.meter(0);
    const auto right = node.meter(1);
    CHECK(left.peak == Approx(0.70710678f).margin(1.0e-5));
    CHECK(left.rms == Approx(0.70710678f).margin(1.0e-5));
    CHECK(right.peak == 0.0f);
    CHECK(right.rms == 0.0f);
    CHECK_FALSE(left.clipped);
    CHECK_FALSE(right.clipped);
}

TEST_CASE("track meter clips latch, clear, and levels decay on silence",
          "[track-meter][graph]") {
    dave::engine::GainNode node;
    node.setGain(2.0);
    node.prepare(48000.0, 64);
    std::vector<std::vector<float>> input(1, std::vector<float>(64, 1.0f));
    std::vector<std::vector<float>> output(1, std::vector<float>(64, 0.0f));
    process(node, input, output);
    const auto loud = node.meter(0);
    CHECK(loud.peak == Approx(2.0f));
    CHECK(loud.rms == Approx(2.0f));
    CHECK(loud.clipped);

    node.clearMeterClips();
    CHECK_FALSE(node.meter(0).clipped);
    std::fill(input[0].begin(), input[0].end(), 0.0f);
    process(node, input, output, 64);
    const auto released = node.meter(0);
    CHECK(released.peak < loud.peak);
    CHECK(released.peak > 0.0f);
    CHECK(released.rms < loud.rms);
    CHECK_FALSE(released.clipped);

    node.prepare(48000.0, 64);
    CHECK(node.meter(0).peak == 0.0f);
    CHECK(node.meter(0).rms == 0.0f);
}

TEST_CASE("track meter decay follows elapsed time instead of callback size",
          "[track-meter][graph]") {
    dave::engine::GainNode at48k;
    dave::engine::GainNode at96k;
    at48k.prepare(48000.0, 960);
    at96k.prepare(96000.0, 1920);

    std::vector<std::vector<float>> impulse48(1, std::vector<float>(1, 1.0f));
    std::vector<std::vector<float>> impulse96(1, std::vector<float>(1, 1.0f));
    std::vector<std::vector<float>> out48(1, std::vector<float>(1));
    std::vector<std::vector<float>> out96(1, std::vector<float>(1));
    process(at48k, impulse48, out48);
    process(at96k, impulse96, out96);

    std::vector<std::vector<float>> silence48(1, std::vector<float>(480));
    std::vector<std::vector<float>> silence96(1, std::vector<float>(960));
    std::vector<std::vector<float>> silentOut48(1, std::vector<float>(480));
    std::vector<std::vector<float>> silentOut96(1, std::vector<float>(960));
    process(at48k, silence48, silentOut48, 1);
    process(at96k, silence96, silentOut96, 1);

    CHECK(at48k.meter(0).peak ==
          Approx(at96k.meter(0).peak).margin(1.0e-6));
    CHECK(at48k.meter(0).rms ==
          Approx(at96k.meter(0).rms).margin(1.0e-6));
}
