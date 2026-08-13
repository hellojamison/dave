// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/record/RecordController.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using dave::engine::RecordController;
using dave::engine::SpscRing;
using dave::engine::TimeInfo;

namespace {

std::vector<float> interleavedRamp(std::size_t frames, int channels) {
    std::vector<float> input(frames * static_cast<std::size_t>(channels));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            input[frame * static_cast<std::size_t>(channels) + channel] =
                static_cast<float>(frame * 10 + static_cast<std::size_t>(channel));
        }
    }
    return input;
}

TimeInfo recordingTime() {
    TimeInfo time;
    time.isRecording = true;
    time.isPlaying = true;
    time.samplePos = 1234;
    return time;
}

} // namespace

TEST_CASE("record controller maps native input channels to mono and stereo rings",
          "[recordcontroller]") {
    SpscRing mono;
    SpscRing stereo;
    mono.reset(64, 1);
    stereo.reset(64, 2);

    RecordController controller;
    REQUIRE(controller.prepare(
        {RecordController::ArmedTrackConfig::mono(mono, 2),
         RecordController::ArmedTrackConfig::stereo(stereo, 3, 1)},
        4, 16));

    const auto input = interleavedRamp(8, 4);
    controller.processBlock(input.data(), 4, 8, recordingTime());
    CHECK(controller.firstSamplePosition() == 1234);

    std::vector<float> monoOut(8, -1.0f);
    REQUIRE(mono.read(monoOut.data(), 8) == 8);
    std::vector<float> stereoOut(16, -1.0f);
    REQUIRE(stereo.read(stereoOut.data(), 8) == 8);
    for (std::size_t frame = 0; frame < 8; ++frame) {
        CHECK(monoOut[frame] == static_cast<float>(frame * 10 + 2));
        CHECK(stereoOut[frame * 2] == static_cast<float>(frame * 10 + 3));
        CHECK(stereoOut[frame * 2 + 1] == static_cast<float>(frame * 10 + 1));
    }
}

TEST_CASE("oversized capture blocks preserve order across bounded chunks",
          "[recordcontroller]") {
    constexpr std::size_t kFrames = 23;
    SpscRing ring;
    ring.reset(64, 2);

    RecordController controller;
    REQUIRE(controller.prepare(
        {RecordController::ArmedTrackConfig::stereo(ring, 0, 1)}, 2, 5));
    const auto input = interleavedRamp(kFrames, 2);
    controller.processBlock(input.data(), 2, kFrames, recordingTime());

    std::vector<float> output(kFrames * 2, -1.0f);
    REQUIRE(ring.read(output.data(), kFrames) == kFrames);
    CHECK(output == input);
    CHECK(ring.droppedFrames() == 0);
}

TEST_CASE("null and absent input become silence without shortening a take",
          "[recordcontroller]") {
    SpscRing ring;
    ring.reset(64, 2);
    RecordController controller;
    REQUIRE(controller.prepare(
        {RecordController::ArmedTrackConfig::stereo(ring, 0, 1)}, 2, 4));

    controller.processBlock(nullptr, 0, 7, recordingTime());
    std::vector<float> output(14, 1.0f);
    REQUIRE(ring.read(output.data(), 7) == 7);
    for (float sample : output) CHECK(sample == 0.0f);
}

TEST_CASE("capture is a no-op outside recording and for zero frames",
          "[recordcontroller]") {
    SpscRing ring;
    ring.reset(16, 1);
    RecordController controller;
    REQUIRE(controller.prepare(
        {RecordController::ArmedTrackConfig::mono(ring, 0)}, 1, 4));

    const auto input = interleavedRamp(4, 1);
    TimeInfo stopped;
    controller.processBlock(input.data(), 1, 4, stopped);
    controller.processBlock(input.data(), 1, 0, recordingTime());
    CHECK(ring.availableFrames() == 0);
}

TEST_CASE("missing callback channels silence only the unavailable mapping",
          "[recordcontroller]") {
    SpscRing ring;
    ring.reset(16, 2);
    RecordController controller;
    REQUIRE(controller.prepare(
        {RecordController::ArmedTrackConfig::stereo(ring, 0, 1)}, 2, 8));

    const auto monoInput = interleavedRamp(5, 1);
    controller.processBlock(monoInput.data(), 1, 5, recordingTime());

    std::vector<float> output(10, -1.0f);
    REQUIRE(ring.read(output.data(), 5) == 5);
    for (std::size_t frame = 0; frame < 5; ++frame) {
        CHECK(output[frame * 2] == monoInput[frame]);
        CHECK(output[frame * 2 + 1] == 0.0f);
    }
}

TEST_CASE("record preparation rejects invalid fixed mappings",
          "[recordcontroller]") {
    SpscRing mono;
    mono.reset(16, 1);
    RecordController controller;

    CHECK_FALSE(controller.prepare(
        {RecordController::ArmedTrackConfig::stereo(mono, 0, 1)}, 2, 8));
    CHECK_FALSE(controller.isPrepared());
    CHECK_FALSE(controller.prepare(
        {RecordController::ArmedTrackConfig::mono(mono, 2)}, 2, 8));
    CHECK_FALSE(controller.prepare(
        {RecordController::ArmedTrackConfig::mono(mono, 0)}, 2, 0));
}

TEST_CASE("record ring sizing provides eight seconds",
          "[recordcontroller]") {
    CHECK(RecordController::ringFramesForSampleRate(48000.0) == 384000);
    CHECK(RecordController::ringFramesForSampleRate(44100.0) == 352800);
    CHECK(RecordController::ringFramesForSampleRate(0.0) == 0);
}
