// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/SmfReader.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace dave;
using dave::engine::midi::parseSmf;
using dave::engine::midi::ticksToSamples;

namespace {

constexpr int kSampleRate = 48000;

// Fixtures are built as byte vectors in-test rather than checked in as .mid
// files: the interesting cases here (a fifth VLQ continuation byte, a running
// status run, a note left open at end of track) are precisely the ones no
// sequencer will write for us, and a hex blob in a test is far easier to
// reason about than a binary file nobody can read in review.
struct Builder {
    std::vector<uint8_t> bytes;

    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
    void u32(uint32_t v) {
        u8(uint8_t(v >> 24)); u8(uint8_t(v >> 16)); u8(uint8_t(v >> 8)); u8(uint8_t(v));
    }
    void raw(const char* s, size_t n) {
        for (size_t i = 0; i < n; ++i) u8(static_cast<uint8_t>(s[i]));
    }
    void vlq(uint32_t v) {
        uint8_t buf[4];
        int n = 0;
        buf[n++] = uint8_t(v & 0x7F);
        while ((v >>= 7) != 0) buf[n++] = uint8_t((v & 0x7F) | 0x80);
        for (int i = n - 1; i >= 0; --i) u8(buf[i]);
    }
};

// Header + a single MTrk whose body is `body`.
std::vector<uint8_t> file(uint16_t format, uint16_t numTracks, int16_t division,
                          const std::vector<std::vector<uint8_t>>& trackBodies) {
    Builder b;
    b.raw("MThd", 4);
    b.u32(6);
    b.u16(format);
    b.u16(numTracks);
    b.u16(static_cast<uint16_t>(division));
    for (const auto& body : trackBodies) {
        b.raw("MTrk", 4);
        b.u32(static_cast<uint32_t>(body.size()));
        for (uint8_t v : body) b.u8(v);
    }
    return b.bytes;
}

// One note on channel 0: on at `onTick`, off `durTicks` later.
std::vector<uint8_t> oneNoteTrack(uint32_t onTick, uint32_t durTicks,
                                  uint8_t pitch = 60, uint8_t vel = 100) {
    Builder b;
    b.vlq(onTick); b.u8(0x90); b.u8(pitch); b.u8(vel);
    b.vlq(durTicks); b.u8(0x80); b.u8(pitch); b.u8(0);
    b.vlq(0); b.u8(0xFF); b.u8(0x2F); b.u8(0);   // end of track
    return b.bytes;
}

} // namespace

// ─── ticksToSamples ─────────────────────────────────────────────────────────

TEST_CASE("ticks convert at the SMF default of 120 bpm when no tempo is given",
          "[smf][tempo]") {
    // 480 ticks = one quarter = 0.5 s at 120 bpm = 24000 samples.
    CHECK(ticksToSamples(480, {}, 480, kSampleRate) == 24000);
    CHECK(ticksToSamples(960, {}, 480, kSampleRate) == 48000);
    CHECK(ticksToSamples(0, {}, 480, kSampleRate) == 0);
    CHECK(ticksToSamples(-1, {}, 480, kSampleRate) == 0);
}

TEST_CASE("a tempo change only affects ticks after it", "[smf][tempo]") {
    // 240 bpm from tick 480 on: the first quarter still takes 0.5 s, the
    // second takes 0.25 s.
    std::vector<document::TempoEvent> tempi{{480, 250000}};
    CHECK(ticksToSamples(480, tempi, 480, kSampleRate) == 24000);
    CHECK(ticksToSamples(960, tempi, 480, kSampleRate) == 24000 + 12000);
}

TEST_CASE("an invalid ppq or sample rate converts to zero rather than dividing by it",
          "[smf][tempo]") {
    CHECK(ticksToSamples(480, {}, 0, kSampleRate) == 0);
    CHECK(ticksToSamples(480, {}, 480, 0) == 0);
}

// ─── Parsing ────────────────────────────────────────────────────────────────

TEST_CASE("a one-note format 0 file parses to one baked note", "[smf]") {
    auto bytes = file(0, 1, 480, {oneNoteTrack(0, 480)});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks.size() == 1);
    REQUIRE(smf.tracks[0].notes.size() == 1);
    const auto& n = smf.tracks[0].notes[0];
    CHECK(n.startSample == 0);
    CHECK(n.lengthSamples == 24000);   // one quarter at 120 bpm
    CHECK(n.pitch == 60);
    CHECK(n.velocity == 100);
    CHECK(n.channel == 0);
}

TEST_CASE("a note-on with velocity 0 ends the note", "[smf]") {
    Builder b;
    b.vlq(0);   b.u8(0x90); b.u8(64); b.u8(90);
    b.vlq(480); b.u8(0x90); b.u8(64); b.u8(0);   // note-off spelled as on/vel-0
    b.vlq(0);   b.u8(0xFF); b.u8(0x2F); b.u8(0);
    auto bytes = file(0, 1, 480, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks[0].notes.size() == 1);
    CHECK(smf.tracks[0].notes[0].lengthSamples == 24000);
}

TEST_CASE("running status carries the note-on across events", "[smf]") {
    // One 0x90 status byte, then three bare data pairs: on 60, on 62, off 60.
    Builder b;
    b.vlq(0);   b.u8(0x90); b.u8(60); b.u8(100);
    b.vlq(0);               b.u8(62); b.u8(100);
    b.vlq(480);             b.u8(60); b.u8(0);
    b.vlq(0);               b.u8(62); b.u8(0);
    b.vlq(0);   b.u8(0xFF); b.u8(0x2F); b.u8(0);
    auto bytes = file(0, 1, 480, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks[0].notes.size() == 2);
    CHECK(smf.tracks[0].notes[0].pitch == 60);
    CHECK(smf.tracks[0].notes[1].pitch == 62);
    CHECK(smf.tracks[0].notes[0].lengthSamples == 24000);
    CHECK(smf.tracks[0].notes[1].lengthSamples == 24000);
}

TEST_CASE("a multi-byte delta time is decoded as a VLQ", "[smf]") {
    // 0x8F 0x00 == 1920 ticks == four quarters == 2 s == 96000 samples.
    Builder b;
    b.u8(0x8F); b.u8(0x00); b.u8(0x90); b.u8(60); b.u8(100);
    b.vlq(480); b.u8(0x80); b.u8(60); b.u8(0);
    b.vlq(0);   b.u8(0xFF); b.u8(0x2F); b.u8(0);
    auto bytes = file(0, 1, 480, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks[0].notes.size() == 1);
    CHECK(smf.tracks[0].notes[0].startSample == 96000);
}

TEST_CASE("a fifth VLQ continuation byte is rejected rather than shifted in",
          "[smf]") {
    // Five bytes with the high bit set is not a legal delta time. The track
    // stops there; the file itself still parses (the chunk length resyncs us).
    Builder b;
    b.u8(0x80); b.u8(0x80); b.u8(0x80); b.u8(0x80); b.u8(0x80); b.u8(0x00);
    b.u8(0x90); b.u8(60); b.u8(100);
    auto bytes = file(0, 1, 480, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    CHECK(smf.tracks[0].notes.empty());
}

TEST_CASE("a mid-file tempo change halves the position of later notes",
          "[smf][tempo]") {
    // Tempo 120 for the first quarter, then 240. Note A at tick 0, note B at
    // tick 960 — two quarters in, but the second of them runs twice as fast.
    Builder b;
    b.vlq(0);   b.u8(0xFF); b.u8(0x51); b.u8(3); b.u8(0x07); b.u8(0xA1); b.u8(0x20);
    b.vlq(0);   b.u8(0x90); b.u8(60); b.u8(100);
    b.vlq(480); b.u8(0x80); b.u8(60); b.u8(0);
    // At tick 480, switch to 250000 us/quarter (240 bpm).
    b.vlq(0);   b.u8(0xFF); b.u8(0x51); b.u8(3); b.u8(0x03); b.u8(0xD0); b.u8(0x90);
    b.vlq(480); b.u8(0x90); b.u8(62); b.u8(100);
    b.vlq(480); b.u8(0x80); b.u8(62); b.u8(0);
    b.vlq(0);   b.u8(0xFF); b.u8(0x2F); b.u8(0);
    auto bytes = file(0, 1, 480, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks[0].notes.size() == 2);
    CHECK(smf.tracks[0].notes[0].startSample == 0);
    CHECK(smf.tracks[0].notes[0].lengthSamples == 24000);   // quarter at 120
    CHECK(smf.tracks[0].notes[1].startSample == 24000 + 12000);
    CHECK(smf.tracks[0].notes[1].lengthSamples == 12000);   // quarter at 240
    REQUIRE(smf.tempi.size() == 2);
    CHECK(smf.tempi[1].tick == 480);
}

TEST_CASE("a tempo meta in a later track still applies to earlier tracks",
          "[smf][tempo]") {
    // Format 1 conventionally puts tempo in track 0, but nothing requires it.
    // The map is file-global, so a tempo in track 1 must move track 0's notes.
    Builder tempoTrack;
    tempoTrack.vlq(0); tempoTrack.u8(0xFF); tempoTrack.u8(0x51); tempoTrack.u8(3);
    tempoTrack.u8(0x03); tempoTrack.u8(0xD0); tempoTrack.u8(0x90);  // 240 bpm
    tempoTrack.vlq(0); tempoTrack.u8(0xFF); tempoTrack.u8(0x2F); tempoTrack.u8(0);

    auto bytes = file(1, 2, 480, {oneNoteTrack(480, 480), tempoTrack.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks.size() == 2);
    REQUIRE(smf.tracks[0].notes.size() == 1);
    CHECK(smf.tracks[0].notes[0].startSample == 12000);   // quarter at 240 bpm
}

TEST_CASE("a note left open at end of track is closed there", "[smf]") {
    Builder b;
    b.vlq(0);   b.u8(0x90); b.u8(60); b.u8(100);
    b.vlq(960); b.u8(0xFF); b.u8(0x2F); b.u8(0);   // end of track, no note-off
    auto bytes = file(0, 1, 480, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks[0].notes.size() == 1);
    CHECK(smf.tracks[0].notes[0].startSample == 0);
    CHECK(smf.tracks[0].notes[0].lengthSamples == 48000);  // two quarters
}

TEST_CASE("a second note-on for a sounding key ends the first", "[smf]") {
    Builder b;
    b.vlq(0);   b.u8(0x90); b.u8(60); b.u8(100);
    b.vlq(480); b.u8(0x90); b.u8(60); b.u8(100);
    b.vlq(480); b.u8(0x80); b.u8(60); b.u8(0);
    b.vlq(0);   b.u8(0xFF); b.u8(0x2F); b.u8(0);
    auto bytes = file(0, 1, 480, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks[0].notes.size() == 2);
    CHECK(smf.tracks[0].notes[0].lengthSamples == 24000);
    CHECK(smf.tracks[0].notes[1].startSample == 24000);
    CHECK(smf.tracks[0].notes[1].lengthSamples == 24000);
}

TEST_CASE("notes come back sorted by start position", "[smf]") {
    // Two channels interleaved so the note-ON order and the completion order
    // differ; the reader must sort by start, not by when the note closed.
    Builder b;
    b.vlq(0);   b.u8(0x90); b.u8(60); b.u8(100);   // starts first, ends last
    b.vlq(240); b.u8(0x91); b.u8(67); b.u8(100);
    b.vlq(240); b.u8(0x81); b.u8(67); b.u8(0);     // ends first
    b.vlq(480); b.u8(0x80); b.u8(60); b.u8(0);
    b.vlq(0);   b.u8(0xFF); b.u8(0x2F); b.u8(0);
    auto bytes = file(0, 1, 480, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks[0].notes.size() == 2);
    CHECK(smf.tracks[0].notes[0].startSample <= smf.tracks[0].notes[1].startSample);
    CHECK(smf.tracks[0].notes[0].pitch == 60);
    CHECK(smf.tracks[0].notes[1].channel == 1);
}

TEST_CASE("a track name meta becomes the track's name", "[smf]") {
    Builder b;
    b.vlq(0); b.u8(0xFF); b.u8(0x03); b.u8(5);
    b.raw("Piano", 5);
    b.vlq(0); b.u8(0x90); b.u8(60); b.u8(100);
    b.vlq(480); b.u8(0x80); b.u8(60); b.u8(0);
    b.vlq(0); b.u8(0xFF); b.u8(0x2F); b.u8(0);
    auto bytes = file(0, 1, 480, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    CHECK(smf.tracks[0].name == "Piano");
}

TEST_CASE("control changes and pitch bends are skipped without derailing notes",
          "[smf]") {
    Builder b;
    b.vlq(0);   b.u8(0xB0); b.u8(7);  b.u8(100);   // CC volume
    b.vlq(0);   b.u8(0xE0); b.u8(0);  b.u8(64);    // pitch bend
    b.vlq(0);   b.u8(0xC0); b.u8(42);              // program change (1 byte)
    b.vlq(0);   b.u8(0x90); b.u8(60); b.u8(100);
    b.vlq(480); b.u8(0x80); b.u8(60); b.u8(0);
    b.vlq(0);   b.u8(0xFF); b.u8(0x2F); b.u8(0);
    auto bytes = file(0, 1, 480, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks[0].notes.size() == 1);
    CHECK(smf.tracks[0].notes[0].startSample == 0);
}

TEST_CASE("SMF format 2 is rejected", "[smf]") {
    auto bytes = file(2, 1, 480, {oneNoteTrack(0, 480)});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    CHECK_FALSE(smf.ok);
    CHECK_FALSE(smf.error.empty());
}

TEST_CASE("a file that isn't a MIDI file is rejected", "[smf]") {
    const char junk[] = "RIFF....WAVEfmt this is a wav, not a mid";
    auto smf = parseSmf(reinterpret_cast<const uint8_t*>(junk), sizeof(junk),
                        kSampleRate);
    CHECK_FALSE(smf.ok);
}

TEST_CASE("a truncated file is rejected rather than read past the end", "[smf]") {
    auto bytes = file(0, 1, 480, {oneNoteTrack(0, 480)});
    bytes.resize(bytes.size() - 6);   // chop the tail off the track chunk
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);
    CHECK_FALSE(smf.ok);
}

TEST_CASE("a zero division is rejected instead of dividing by it", "[smf]") {
    auto bytes = file(0, 1, 0, {oneNoteTrack(0, 480)});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);
    CHECK_FALSE(smf.ok);
}

TEST_CASE("an SMPTE division gives absolute time and ignores tempo metas",
          "[smf][tempo]") {
    // -25 fps, 40 ticks per frame == 1000 ticks per second. A note at tick
    // 1000 is one second in, whatever any tempo meta claims.
    const int16_t division = static_cast<int16_t>((-25 << 8) | 40);
    Builder b;
    b.vlq(0);    b.u8(0xFF); b.u8(0x51); b.u8(3); b.u8(0x03); b.u8(0xD0); b.u8(0x90);
    b.vlq(1000); b.u8(0x90); b.u8(60); b.u8(100);
    b.vlq(1000); b.u8(0x80); b.u8(60); b.u8(0);
    b.vlq(0);    b.u8(0xFF); b.u8(0x2F); b.u8(0);
    auto bytes = file(0, 1, division, {b.bytes});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    // The file's 240 bpm meta is dropped; the only entry is the synthetic
    // "one quarter == one second" that makes the tick rate absolute.
    REQUIRE(smf.tempi.size() == 1);
    CHECK(smf.tempi[0].microsecondsPerQuarter == 1000000);
    REQUIRE(smf.tracks[0].notes.size() == 1);
    CHECK(smf.tracks[0].notes[0].startSample == kSampleRate);
    CHECK(smf.tracks[0].notes[0].lengthSamples == kSampleRate);
}

TEST_CASE("the file length spans the longest track", "[smf]") {
    auto bytes = file(1, 2, 480, {oneNoteTrack(0, 480), oneNoteTrack(0, 1920)});
    auto smf = parseSmf(bytes.data(), bytes.size(), kSampleRate);

    REQUIRE(smf.ok);
    REQUIRE(smf.tracks.size() == 2);
    CHECK(smf.lengthSamples == 96000);   // four quarters at 120 bpm
}
