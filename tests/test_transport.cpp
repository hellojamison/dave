// SPDX-License-Identifier: GPL-3.0-or-later
//
// Transport play/stop semantics.
//
// The rule under test is the Pro Tools one: stopping returns the cursor to
// wherever the pass started. It is easy to get subtly wrong, because the
// position the UI thread can see (currentPosition_) lags the position a
// pending seek has already committed to — press play immediately after a
// click and a naive implementation records the pre-click position.
#include "document/Edit.h"
#include "document/ProjectFile.h"
#include "engine/transport/Transport.h"

#include <catch2/catch_test_macros.hpp>

using namespace dave;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

// One block of "audio", which is what actually advances the transport and
// commits a pending seek.
void pump(engine::Transport& t, int blocks = 1) {
    engine::TimeInfo time{};
    for (int i = 0; i < blocks; ++i) t.advanceAndFill(time, kBlock, kSr);
}

} // namespace

TEST_CASE("stop returns the cursor to where playback started", "[transport]") {
    engine::Transport t;
    t.seek(96000);
    pump(t);
    REQUIRE(t.position() == 96000);

    t.play();
    pump(t, 10);
    REQUIRE(t.position() > 96000);   // it really moved

    t.stop();
    pump(t);
    CHECK_FALSE(t.isPlaying());
    CHECK(t.position() == 96000);
}

TEST_CASE("play right after a seek returns to the seeked position",
          "[transport]") {
    // No pump between the seek and the play: the seek is still queued, so
    // reading currentPosition_ here would record 0 and stop would rewind to
    // the top of the session.
    engine::Transport t;
    t.seek(240000);
    t.play();
    CHECK(t.returnPosition() == 240000);

    pump(t, 20);
    t.stop();
    pump(t);
    CHECK(t.position() == 240000);
}

TEST_CASE("the space-bar toggle follows the same rule", "[transport]") {
    engine::Transport t;
    t.seek(48000);
    pump(t);

    t.toggle();                       // play
    REQUIRE(t.isPlaying());
    pump(t, 8);
    t.toggle();                       // stop
    pump(t);

    CHECK_FALSE(t.isPlaying());
    CHECK(t.position() == 48000);
}

TEST_CASE("a seek during playback becomes the new return point",
          "[transport]") {
    // Jumping the playhead mid-pass is the user saying "play from here".
    // Throwing them back to the position before the jump would be a rewind
    // they never asked for.
    engine::Transport t;
    t.seek(48000);
    pump(t);
    t.play();
    pump(t, 5);

    t.seek(400000);
    pump(t, 5);
    REQUIRE(t.isPlaying());

    t.stop();
    pump(t);
    CHECK(t.position() == 400000);
}

TEST_CASE("stopping an already-stopped transport does not move the cursor",
          "[transport]") {
    // Stop is wired to a menu item and a button; pressing it twice must not
    // rewind to a return point left over from an earlier pass.
    engine::Transport t;
    t.seek(48000);
    pump(t);
    t.play();
    pump(t, 5);
    t.stop();
    pump(t);
    REQUIRE(t.position() == 48000);

    t.seek(300000);
    pump(t);
    t.stop();                         // already stopped
    pump(t);
    CHECK(t.position() == 300000);
}

TEST_CASE("stop and rewind still lands at zero", "[transport]") {
    // DaveApp's rewind button is stop() followed by seek(0); the return-to-
    // start must not fight it.
    engine::Transport t;
    t.seek(96000);
    pump(t);
    t.play();
    pump(t, 5);

    t.stop();
    t.seek(0);
    pump(t);
    CHECK(t.position() == 0);
}

// ─── Session format ─────────────────────────────────────────────────────────

TEST_CASE("session sample rate and bit depth round-trip through the project",
          "[session]") {
    document::Edit edit;
    // Defaults match what pre-existing projects meant.
    CHECK(edit.sampleRate() == 48000);
    CHECK(edit.bitDepth() == 24);

    edit.addTrack("Audio 1");
    edit.setSampleRate(96000);
    edit.setBitDepth(32);

    const std::string text = document::serializeEdit(edit);
    document::Edit reloaded;
    const auto r = document::deserializeEdit(text, reloaded);
    REQUIRE(r.ok);
    CHECK(reloaded.sampleRate() == 96000);
    CHECK(reloaded.bitDepth() == 32);
}

TEST_CASE("a project written before the session format existed still loads",
          "[session]") {
    // No bitDepth key, and the old hardcoded rate. Both defaults have to mean
    // what that file meant, or every legacy project reopens detuned.
    const std::string legacy =
        R"({"format":"dave.doc/v1","sampleRate":48000,"tracks":[]})";
    document::Edit edit;
    const auto r = document::deserializeEdit(legacy, edit);
    REQUIRE(r.ok);
    CHECK(edit.sampleRate() == 48000);
    CHECK(edit.bitDepth() == 24);
}

TEST_CASE("setting the session rate notifies listeners", "[session]") {
    // DaveApp reopens the audio device off this notification; without it the
    // engine keeps running at the old rate.
    document::Edit edit;
    int notifications = 0;
    edit.setChangeListener([&] { ++notifications; });

    edit.setSampleRate(96000);
    CHECK(notifications == 1);
    edit.setSampleRate(96000);          // no-op
    CHECK(notifications == 1);
    edit.setBitDepth(16);
    CHECK(notifications == 2);
    edit.setSampleRate(0);              // rejected
    CHECK(notifications == 2);
    CHECK(edit.sampleRate() == 96000);
}

// ─── Loop playback ──────────────────────────────────────────────────────────

TEST_CASE("playback wraps inside the loop range", "[transport]") {
    engine::Transport t;
    constexpr int64_t kLoopStart = 48000;
    constexpr int64_t kLoopEnd = 96000;
    t.setLoop(kLoopStart, kLoopEnd);
    t.seek(kLoopStart);
    pump(t);
    t.play();

    // Long enough to cross the loop end several times over.
    for (int i = 0; i < 400; ++i) {
        pump(t);
        INFO("block " << i << " position " << t.position());
        REQUIRE(t.position() >= kLoopStart);
        REQUIRE(t.position() < kLoopEnd);
    }
}

TEST_CASE("clearing the loop lets playback run past the old end",
          "[transport]") {
    engine::Transport t;
    t.setLoop(48000, 96000);
    t.seek(48000);
    pump(t);
    t.play();
    pump(t, 200);
    REQUIRE(t.position() < 96000);

    t.clearLoop();
    pump(t, 400);
    CHECK(t.position() > 96000);
}

TEST_CASE("stopping a looping pass returns to where it started",
          "[transport]") {
    // The two transport rules have to compose: the loop wraps the playhead
    // repeatedly, and stop still returns to the position play began from —
    // not to the loop start, and not to wherever the wrap left it.
    engine::Transport t;
    t.setLoop(48000, 96000);
    t.seek(60000);
    pump(t);
    t.play();
    pump(t, 300);          // several wraps
    REQUIRE(t.position() != 60000);

    t.stop();
    pump(t);
    CHECK(t.position() == 60000);
}

TEST_CASE("a loop shorter than one block still advances", "[transport]") {
    // A range narrower than the audio block is a plausible mis-drag. The wrap
    // arithmetic must not leave the playhead pinned or send it negative.
    engine::Transport t;
    t.setLoop(1000, 1100);
    t.seek(1000);
    pump(t);
    t.play();
    for (int i = 0; i < 20; ++i) {
        pump(t);
        REQUIRE(t.position() >= 0);
    }
}
