// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/transport/Transport.h"

#include <catch2/catch_test_macros.hpp>

using dave::engine::TimeInfo;
using dave::engine::Transport;

TEST_CASE("transport publishes recording state into every time block",
          "[transport][recording]") {
    Transport transport;
    TimeInfo time;

    CHECK_FALSE(transport.isRecording());
    transport.advanceAndFill(time, 64, 48000.0);
    CHECK_FALSE(time.isRecording);

    transport.setRecording(true);
    CHECK(transport.isRecording());
    transport.advanceAndFill(time, 64, 48000.0);
    CHECK(time.isRecording);

    transport.setRecording(false);
    transport.advanceAndFill(time, 64, 48000.0);
    CHECK_FALSE(transport.isRecording());
    CHECK_FALSE(time.isRecording);
}

TEST_CASE("recording state is independent of playback state",
          "[transport][recording]") {
    Transport transport;
    TimeInfo time;

    transport.setRecording(true);
    transport.advanceAndFill(time, 64, 48000.0);
    CHECK(time.isRecording);
    CHECK_FALSE(time.isPlaying);

    transport.play();
    transport.advanceAndFill(time, 64, 48000.0);
    CHECK(time.isRecording);
    CHECK(time.isPlaying);
}

TEST_CASE("record and roll anchors the take at the pending seek",
          "[transport][recording]") {
    Transport transport;
    TimeInfo time;
    transport.seek(12345);

    CHECK(transport.beginRecordingAndPlay() == 12345);
    transport.advanceAndFill(time, 128, 48000.0);
    CHECK(time.samplePos == 12345);
    CHECK(time.isPlaying);
    CHECK(time.isRecording);

    transport.setRecording(false);
    transport.stop();
    transport.advanceAndFill(time, 64, 48000.0);
    CHECK(time.samplePos == 12345);
}

TEST_CASE("seeks are ignored during a linear record pass",
          "[transport][recording]") {
    Transport transport;
    TimeInfo time;
    transport.seek(400);
    transport.beginRecordingAndPlay();
    transport.advanceAndFill(time, 64, 48000.0);
    transport.seek(9000);
    transport.advanceAndFill(time, 64, 48000.0);
    CHECK(time.samplePos == 464);

    transport.setRecording(false);
    transport.stop();
    transport.advanceAndFill(time, 64, 48000.0);
    CHECK(time.samplePos == 400);
}
