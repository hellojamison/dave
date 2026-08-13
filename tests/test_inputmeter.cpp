// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/InputMeterBank.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Approx;
using dave::audio::InputMeterBank;

TEST_CASE("input meters report per-channel peak and RMS", "[input-meter]") {
    InputMeterBank meters;
    meters.reset(2);

    // Two interleaved stereo frames: left is below full scale; right touches
    // both positive and negative full scale.
    const float input[] = {-0.5f, 1.0f, 0.25f, -1.0f};
    meters.processInterleaved(input, 2, 2);

    const auto left = meters.snapshot(0);
    CHECK(left.peak == Approx(0.5f));
    CHECK(left.rms == Approx(std::sqrt((0.25f + 0.0625f) / 2.0f)));
    CHECK_FALSE(left.clipped);

    const auto right = meters.snapshot(1);
    CHECK(right.peak == Approx(1.0f));
    CHECK(right.rms == Approx(1.0f));
    CHECK(right.clipped);
}

TEST_CASE("input meter levels decay on silence and remain bounded",
          "[input-meter]") {
    InputMeterBank meters;
    meters.reset(1);
    const float signal[] = {0.8f, -0.4f};
    meters.processInterleaved(signal, 2, 1);
    const auto before = meters.snapshot(0);

    const float silence[] = {0.0f, 0.0f};
    meters.processInterleaved(silence, 2, 1);
    const auto after = meters.snapshot(0);
    const float peakRelease =
        1.0f - 2.0f / (48000.0f * InputMeterBank::kPeakDecaySeconds);
    const float rmsRelease =
        1.0f - 2.0f / (48000.0f * InputMeterBank::kRmsDecaySeconds);
    CHECK(after.peak == Approx(before.peak * peakRelease));
    CHECK(after.rms == Approx(before.rms * rmsRelease));
    CHECK(after.peak >= 0.0f);
    CHECK(after.peak < before.peak);
    CHECK(after.rms >= 0.0f);
    CHECK(after.rms < before.rms);
}

TEST_CASE("input meter clip indication latches until cleared",
          "[input-meter]") {
    InputMeterBank meters;
    meters.reset(1);
    const float clipped[] = {-1.0f};
    meters.processInterleaved(clipped, 1, 1);
    REQUIRE(meters.snapshot(0).clipped);

    const float quiet[] = {0.1f};
    meters.processInterleaved(quiet, 1, 1);
    CHECK(meters.snapshot(0).clipped);

    meters.clearClip(0);
    CHECK_FALSE(meters.snapshot(0).clipped);
}

TEST_CASE("null input behaves as silence and zero frames do nothing",
          "[input-meter]") {
    InputMeterBank meters;
    meters.reset(1);
    const float signal[] = {0.5f};
    meters.processInterleaved(signal, 1, 1);
    const auto signalLevel = meters.snapshot(0);

    meters.processInterleaved(nullptr, 0, 1);
    CHECK(meters.snapshot(0).peak == signalLevel.peak);
    CHECK(meters.snapshot(0).rms == signalLevel.rms);

    meters.processInterleaved(nullptr, 32, 1);
    const float peakRelease =
        1.0f - 32.0f / (48000.0f * InputMeterBank::kPeakDecaySeconds);
    const float rmsRelease =
        1.0f - 32.0f / (48000.0f * InputMeterBank::kRmsDecaySeconds);
    CHECK(meters.snapshot(0).peak ==
          Approx(signalLevel.peak * peakRelease));
    CHECK(meters.snapshot(0).rms ==
          Approx(signalLevel.rms * rmsRelease));
}

TEST_CASE("input meter decay follows elapsed frames rather than callback count",
          "[input-meter]") {
    InputMeterBank oneBlock;
    InputMeterBank twoBlocks;
    oneBlock.reset(1, 48000.0f);
    twoBlocks.reset(1, 48000.0f);
    const float signal[] = {0.8f};
    oneBlock.processInterleaved(signal, 1, 1);
    twoBlocks.processInterleaved(signal, 1, 1);

    oneBlock.processInterleaved(nullptr, 512, 1);
    twoBlocks.processInterleaved(nullptr, 256, 1);
    twoBlocks.processInterleaved(nullptr, 256, 1);

    CHECK(twoBlocks.snapshot(0).peak ==
          Approx(oneBlock.snapshot(0).peak).margin(0.0001f));
    CHECK(twoBlocks.snapshot(0).rms ==
          Approx(oneBlock.snapshot(0).rms).margin(0.0005f));
}

TEST_CASE("clear and reset remove stale meter state", "[input-meter]") {
    InputMeterBank meters;
    meters.reset(2);
    const float input[] = {0.5f, 1.25f};
    meters.processInterleaved(input, 1, 2);
    REQUIRE(meters.snapshot(0).peak > 0.0f);
    REQUIRE(meters.snapshot(1).clipped);

    meters.clear();
    CHECK(meters.snapshot(0).peak == 0.0f);
    CHECK(meters.snapshot(0).rms == 0.0f);
    CHECK_FALSE(meters.snapshot(1).clipped);

    meters.processInterleaved(input, 1, 2);
    meters.reset(1);
    CHECK(meters.channelCount() == 1);
    CHECK(meters.snapshot(0).peak == 0.0f);
    CHECK(meters.snapshot(1).peak == 0.0f);
    CHECK_FALSE(meters.snapshot(1).clipped);
}

TEST_CASE("input meter channel access is bounded by miniaudio's maximum",
          "[input-meter]") {
    InputMeterBank meters;
    meters.reset(InputMeterBank::kMaxChannels + 1);
    REQUIRE(meters.channelCount() == InputMeterBank::kMaxChannels);

    std::vector<float> frame(InputMeterBank::kMaxChannels + 1, 0.0f);
    frame[InputMeterBank::kMaxChannels - 1] = 0.75f;
    // The malformed extra channel is ignored and is not used as the stride.
    frame[InputMeterBank::kMaxChannels] = 1.5f;
    meters.processInterleaved(frame.data(), 1,
                              InputMeterBank::kMaxChannels + 1);

    CHECK(meters.snapshot(InputMeterBank::kMaxChannels - 1).peak ==
          Approx(0.75f));
    const auto outOfRange = meters.snapshot(InputMeterBank::kMaxChannels);
    CHECK(outOfRange.peak == 0.0f);
    CHECK(outOfRange.rms == 0.0f);
    CHECK_FALSE(outOfRange.clipped);
    meters.clearClip(InputMeterBank::kMaxChannels); // no-op, no OOB access
}
