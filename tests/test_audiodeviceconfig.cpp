// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/AudioDeviceConfig.h"

#include <catch2/catch_test_macros.hpp>

using dave::platform::InputDeviceSelection;
using dave::platform::makeAudioDeviceOpenPlan;

TEST_CASE("input Off opens playback only at the session format",
          "[audio-device-config]") {
    const auto plan = makeAudioDeviceOpenPlan(
        InputDeviceSelection::off(), 96000, 6);

    REQUIRE(plan.count == 1);
    CHECK_FALSE(plan.attempts[0].duplex);
    CHECK(plan.attempts[0].sampleRate == 96000);
    CHECK(plan.attempts[0].playbackChannels == 0);
}

TEST_CASE("an input selection requests native capture channels then fallback",
          "[audio-device-config]") {
    const auto plan = makeAudioDeviceOpenPlan(
        InputDeviceSelection::device(3), 48000, 2);

    REQUIRE(plan.count == 2);
    CHECK(plan.attempts[0].duplex);
    CHECK(plan.attempts[0].captureChannels == 0);
    CHECK(plan.attempts[0].sampleRate == 48000);
    CHECK_FALSE(plan.attempts[1].duplex);
    CHECK(plan.attempts[1].sampleRate == 48000);
    CHECK(plan.attempts[1].playbackChannels == 0);
}

TEST_CASE("the default input follows the same duplex fallback policy",
          "[audio-device-config]") {
    const auto plan = makeAudioDeviceOpenPlan(
        InputDeviceSelection::defaultDevice(), 44100, 2);

    REQUIRE(plan.count == 2);
    CHECK(plan.attempts[0].duplex);
    CHECK_FALSE(plan.attempts[1].duplex);
}
