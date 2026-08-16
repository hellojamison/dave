// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/nodes/GainNode.h"
#include "gui/LevelMeter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
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

// ─── 32-bit float headroom ─────────────────────────────────────────────────
//
// Going above 0 dBFS in a float session is not a clip: nothing was truncated
// and pulling the fader down recovers the signal exactly. The cost lands at
// the other end of the meter — the same number of dB of quiet detail is pushed
// below what a fixed-point render can represent, and that part does not come
// back. So the over reads as headroom and the loss it implies reads as damage.

TEST_CASE("only a level above 0 dBFS counts as headroom", "[trackmeter]") {
    CHECK(dave::gui::meterOverDb(1.0f) == 0.0f);
    CHECK(dave::gui::meterOverDb(0.5f) == 0.0f);
    CHECK(dave::gui::meterOverDb(0.0f) == 0.0f);
    // A meter that treats a NaN as "infinitely over" would paint the whole
    // scale red on the first denormal that escapes a plugin.
    CHECK(dave::gui::meterOverDb(
              std::numeric_limits<float>::quiet_NaN()) == 0.0f);

    CHECK(dave::gui::meterOverDb(2.0f) == Approx(6.0206f).margin(0.001f));
    CHECK(dave::gui::meterOverDb(4.0f) == Approx(12.041f).margin(0.001f));
}

TEST_CASE("the doomed noise floor is as deep as the over is high",
          "[trackmeter]") {
    // 66 dB of scale: 6 dB over costs the quietest 6 dB, which is 6/66 of the
    // bar. The two numbers are the same because they are the same decision —
    // how much of the range you spent on headroom you cannot spend on detail.
    constexpr float span =
        dave::gui::kMeterCeilingDb - dave::gui::kMeterFloorDb;
    CHECK(span == 66.0f);
    CHECK(dave::gui::noiseFloorLossFraction(6.0f) ==
          Approx(6.0f / 66.0f).margin(1e-6f));
    CHECK(dave::gui::noiseFloorLossFraction(33.0f) == Approx(0.5f));

    // Never negative, never more than the whole bar.
    CHECK(dave::gui::noiseFloorLossFraction(0.0f) == 0.0f);
    CHECK(dave::gui::noiseFloorLossFraction(-6.0f) == 0.0f);
    CHECK(dave::gui::noiseFloorLossFraction(600.0f) == 1.0f);
}

TEST_CASE("float headroom is a property of the session's format",
          "[trackmeter]") {
    CHECK_FALSE(dave::gui::sessionHasFloatHeadroom(16));
    CHECK_FALSE(dave::gui::sessionHasFloatHeadroom(24));
    CHECK(dave::gui::sessionHasFloatHeadroom(32));
}

TEST_CASE("the over-hold survives the decay that the peak does not",
          "[trackmeter]") {
    // The bar falls back as soon as the transient passes. The question the
    // headroom warning answers is not "how loud now" but "how far over did
    // this ever go", so it needs a latch rather than the decaying peak.
    dave::engine::GainNode node;
    node.setGain(1.0);
    node.prepare(48000.0, 64);

    auto runBlock = [&](float amplitude) {
        std::array<float, 64> in{};
        std::array<float, 64> out{};
        in.fill(amplitude);
        float* inPtr = in.data();
        float* outPtr = out.data();
        dave::engine::AudioBus inputBus{&inPtr, 1, 64};
        dave::engine::TimeInfo time;
        time.samplePos = 0;
        dave::engine::NodeProcessContext ctx;
        ctx.numSamples = 64;
        ctx.time = &time;
        ctx.inputs = &inputBus;
        ctx.numInputs = 1;
        ctx.output = dave::engine::AudioBus{&outPtr, 1, 64};
        node.process(ctx);
    };

    runBlock(2.0f);
    const auto loud = node.meter(0, true);
    CHECK(loud.maxPeak == Approx(2.0f).margin(1e-4f));

    // Quiet again for a long time. The displayed peak decays; the hold must
    // not.
    // 0.75 s of release per unit, so this is a few seconds of quiet.
    for (int i = 0; i < 2000; ++i) runBlock(0.001f);
    const auto quiet = node.meter(0, true);
    CHECK(quiet.peak < 0.5f);
    CHECK(quiet.maxPeak == Approx(2.0f).margin(1e-4f));
    CHECK(dave::gui::meterOverDb(quiet.maxPeak) == Approx(6.0206f).margin(0.01f));

    // Clearing the clip latch clears the hold with it — they answer the same
    // question, and resetting one alone leaves the meter contradicting itself.
    node.clearMeterClips();
    CHECK(node.meter(0, true).maxPeak == 0.0f);
}

// ─── Peak hold ─────────────────────────────────────────────────────────────
//
// The bar falls fast so you can read the current level; the marker sits still
// so you can read what the transient actually reached. How long it sits is the
// preference — a marker that chases the bar answers neither question.

namespace {

// Runs `blocks` blocks of a constant amplitude through a node and returns it,
// so the hold cases read as "loud, then quiet for N seconds".
void runBlocks(dave::engine::GainNode& node, float amplitude, int blocks) {
    for (int b = 0; b < blocks; ++b) {
        std::array<float, 64> in{};
        std::array<float, 64> out{};
        in.fill(amplitude);
        float* inPtr = in.data();
        float* outPtr = out.data();
        dave::engine::AudioBus inputBus{&inPtr, 1, 64};
        dave::engine::TimeInfo time;
        time.samplePos = 0;
        dave::engine::NodeProcessContext ctx;
        ctx.numSamples = 64;
        ctx.time = &time;
        ctx.inputs = &inputBus;
        ctx.numInputs = 1;
        ctx.output = dave::engine::AudioBus{&outPtr, 1, 64};
        node.process(ctx);
    }
}

// 64-frame blocks at 48 kHz.
constexpr int kBlocksPerSecond = 750;

} // namespace

TEST_CASE("the peak marker holds for the configured time", "[trackmeter]") {
    dave::engine::GainNode node;
    node.setGain(1.0);
    node.setPeakHoldSeconds(2.0f);
    node.prepare(48000.0, 64);

    runBlocks(node, 0.8f, 1);
    CHECK(node.meter(0, true).holdPeak == Approx(0.8f).margin(1e-4f));

    // One second of quiet: the bar has fallen, the marker has not.
    runBlocks(node, 0.001f, kBlocksPerSecond);
    const auto holding = node.meter(0, true);
    CHECK(holding.peak < 0.8f);
    CHECK(holding.holdPeak == Approx(0.8f).margin(1e-4f));

    // Past two seconds it drops to whatever the bar is showing, rather than
    // hanging in mid-air below a peak that has already gone.
    runBlocks(node, 0.001f, kBlocksPerSecond * 2);
    const auto released = node.meter(0, true);
    CHECK(released.holdPeak < 0.8f);
    CHECK(released.holdPeak == Approx(released.peak).margin(1e-3f));
}

TEST_CASE("a hold of zero lets the marker follow the bar", "[trackmeter]") {
    dave::engine::GainNode node;
    node.setGain(1.0);
    node.setPeakHoldSeconds(0.0f);
    node.prepare(48000.0, 64);

    runBlocks(node, 0.8f, 1);
    runBlocks(node, 0.001f, 4);
    const auto snapshot = node.meter(0, true);
    CHECK(snapshot.holdPeak == Approx(snapshot.peak).margin(1e-3f));
}

TEST_CASE("a negative hold keeps the marker until it is cleared",
          "[trackmeter]") {
    // The shipped behaviour, and the default — so a user who never opens the
    // preference sees no change.
    dave::engine::GainNode node;
    node.setGain(1.0);
    CHECK(node.peakHoldSeconds() < 0.0f);
    node.prepare(48000.0, 64);

    runBlocks(node, 0.8f, 1);
    runBlocks(node, 0.001f, kBlocksPerSecond * 10);
    CHECK(node.meter(0, true).holdPeak == Approx(0.8f).margin(1e-4f));

    node.clearMeterClips();
    CHECK(node.meter(0, true).holdPeak == 0.0f);
}

TEST_CASE("a new maximum renews the hold immediately", "[trackmeter]") {
    dave::engine::GainNode node;
    node.setGain(1.0);
    node.setPeakHoldSeconds(1.0f);
    node.prepare(48000.0, 64);

    runBlocks(node, 0.4f, 1);
    runBlocks(node, 0.001f, kBlocksPerSecond / 2);   // half the window gone
    runBlocks(node, 0.9f, 1);
    CHECK(node.meter(0, true).holdPeak == Approx(0.9f).margin(1e-4f));

    // The window restarted, so it is still up half a second later.
    runBlocks(node, 0.001f, kBlocksPerSecond / 2);
    CHECK(node.meter(0, true).holdPeak == Approx(0.9f).margin(1e-4f));
}
