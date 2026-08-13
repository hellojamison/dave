// SPDX-License-Identifier: GPL-3.0-or-later
//
// The writer thread, driven the way the audio thread will drive it: frames
// pushed into a ring from another thread, drained to disk in the background.
// No device involved — the producer here stands in for the RT callback.
#include "engine/record/DiskWriter.h"
#include "document/Sha256.h"

#define DR_WAV_NO_IMPLEMENTATION
#include <dr_wav.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using dave::engine::DiskWriter;
using dave::engine::WavWriter;

namespace {

struct TempDir {
    TempDir() {
        path = std::filesystem::temp_directory_path() / "dave_diskwriter_test";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

std::unique_ptr<DiskWriter::Track> makeTrack(const std::string& path,
                                             const std::string& id,
                                             int channels) {
    auto t = std::make_unique<DiskWriter::Track>();
    t->path = path;
    t->trackId = id;
    t->channels = channels;
    return t;
}

// Push `frames` of a constant value the way the RT thread would: planar.
void pushConstant(dave::engine::SpscRing& ring, int channels, size_t frames,
                  float value) {
    std::vector<std::vector<float>> planar(channels,
                                           std::vector<float>(frames, value));
    std::vector<const float*> ptrs(channels);
    for (int c = 0; c < channels; ++c) ptrs[c] = planar[c].data();
    ring.write(ptrs.data(), channels, frames);
}

drwav_uint64 frameCountOf(const std::string& path) {
    drwav wav;
    if (!drwav_init_file(&wav, path.c_str(), nullptr)) return 0;
    const drwav_uint64 frames = wav.totalPCMFrameCount;
    drwav_uninit(&wav);
    return frames;
}

std::vector<float> samplesOf(const std::string& path) {
    drwav wav;
    if (!drwav_init_file(&wav, path.c_str(), nullptr)) return {};
    std::vector<float> samples(
        static_cast<size_t>(wav.totalPCMFrameCount) * wav.channels);
    const auto decoded = drwav_read_pcm_frames_f32(
        &wav, wav.totalPCMFrameCount, samples.data());
    samples.resize(static_cast<size_t>(decoded) * wav.channels);
    drwav_uninit(&wav);
    return samples;
}

} // namespace

TEST_CASE("frames pushed from another thread reach the file", "[diskwriter]") {
    TempDir dir;
    const std::string path = (dir.path / "take1.wav").string();

    std::vector<std::unique_ptr<DiskWriter::Track>> tracks;
    tracks.push_back(makeTrack(path, "track_1", 2));

    DiskWriter writer;
    REQUIRE(writer.start(std::move(tracks), 48000, WavWriter::Format::Pcm24,
                         8192));
    REQUIRE(writer.isRunning());

    // Feed in blocks, the way a callback would, with real time passing so the
    // writer thread actually gets to run.
    constexpr size_t kBlocks = 40;
    constexpr size_t kBlockFrames = 512;
    for (size_t i = 0; i < kBlocks; ++i) {
        pushConstant(*writer.ring(0), 2, kBlockFrames, 0.5f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    writer.stop();

    CHECK(frameCountOf(path) == kBlocks * kBlockFrames);
    const auto stats = writer.stats(0);
    CHECK(stats.framesWritten == kBlocks * kBlockFrames);
    CHECK(stats.droppedFrames == 0);
}

TEST_CASE("a ring overrun becomes silence, not a shorter file",
          "[diskwriter]") {
    // The alignment guarantee. If dropped frames simply vanished, every take
    // after the dropout would sit earlier than it was performed — a whole
    // session quietly out of sync rather than one audible hole.
    TempDir dir;
    const std::string path = (dir.path / "overrun.wav").string();

    std::vector<std::unique_ptr<DiskWriter::Track>> tracks;
    tracks.push_back(makeTrack(path, "track_1", 1));

    DiskWriter writer;
    // A deliberately tiny ring, and no pauses, so the producer outruns the
    // writer immediately.
    REQUIRE(writer.start(std::move(tracks), 48000, WavWriter::Format::Pcm24,
                         256));

    constexpr size_t kBlocks = 200;
    constexpr size_t kBlockFrames = 256;
    for (size_t i = 0; i < kBlocks; ++i) {
        pushConstant(*writer.ring(0), 1, kBlockFrames, 0.25f);
    }
    const uint64_t produced = writer.ring(0)->framesProduced();
    writer.stop();

    const auto stats = writer.stats(0);
    REQUIRE(stats.droppedFrames > 0);          // the test provoked a real overrun
    // Written frames account for everything produced: the audio that made it
    // plus silence standing in for what did not.
    CHECK(stats.framesWritten == produced);
    CHECK(frameCountOf(path) == produced);
}

TEST_CASE("a dropped block becomes silence at its exact sample position",
          "[diskwriter][gap]") {
    TempDir dir;
    const std::string path = (dir.path / "positioned_gap.wav").string();
    std::vector<std::unique_ptr<DiskWriter::Track>> tracks;
    tracks.push_back(makeTrack(path, "track_1", 1));

    DiskWriter writer;
    REQUIRE(writer.start(std::move(tracks), 48000,
                         WavWriter::Format::Pcm24, 256));

    pushConstant(*writer.ring(0), 1, 128, 0.25f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // Larger than the entire ring: this drop is deterministic regardless of
    // writer-thread timing and belongs at ideal stream position 128.
    pushConstant(*writer.ring(0), 1, 300, -0.5f);
    pushConstant(*writer.ring(0), 1, 128, 0.75f);
    writer.stop();

    const auto samples = samplesOf(path);
    REQUIRE(samples.size() == 556);
    CHECK(samples[0] == Catch::Approx(0.25f).margin(0.001f));
    CHECK(samples[127] == Catch::Approx(0.25f).margin(0.001f));
    for (size_t i = 128; i < 428; ++i) {
        REQUIRE(std::fabs(samples[i]) < 0.001f);
    }
    CHECK(samples[428] == Catch::Approx(0.75f).margin(0.001f));
    CHECK(samples.back() == Catch::Approx(0.75f).margin(0.001f));
    CHECK_FALSE(writer.stats(0).failed);
}

TEST_CASE("a dropout on one track does not move its later audio out of phase",
          "[diskwriter][gap][multitrack]") {
    TempDir dir;
    const std::string droppedPath = (dir.path / "dropped.wav").string();
    const std::string intactPath = (dir.path / "intact.wav").string();
    std::vector<std::unique_ptr<DiskWriter::Track>> tracks;
    tracks.push_back(makeTrack(droppedPath, "dropped", 1));
    tracks.push_back(makeTrack(intactPath, "intact", 1));

    DiskWriter writer;
    REQUIRE(writer.start(std::move(tracks), 48000,
                         WavWriter::Format::Pcm24, 256));

    // Same ideal 300-frame interval. Track 0 receives it as one guaranteed
    // oversized drop; track 1 receives drainable chunks and retains it.
    pushConstant(*writer.ring(0), 1, 300, 0.25f);
    for (int i = 0; i < 3; ++i) {
        pushConstant(*writer.ring(1), 1, 100, 0.25f);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    pushConstant(*writer.ring(0), 1, 128, 0.75f);
    pushConstant(*writer.ring(1), 1, 128, 0.75f);
    writer.stop();

    const auto dropped = samplesOf(droppedPath);
    const auto intact = samplesOf(intactPath);
    REQUIRE(dropped.size() == 428);
    REQUIRE(intact.size() == 428);
    for (size_t i = 0; i < 300; ++i) {
        REQUIRE(std::fabs(dropped[i]) < 0.001f);
        REQUIRE(intact[i] == Catch::Approx(0.25f).margin(0.001f));
    }
    // The first retained post-gap sample lands at the same absolute position
    // in both takes — the phase-coherence property this phase exists to keep.
    CHECK(dropped[300] == Catch::Approx(0.75f).margin(0.001f));
    CHECK(intact[300] == Catch::Approx(0.75f).margin(0.001f));
    CHECK(dropped.back() == Catch::Approx(intact.back()).margin(0.000001f));
    CHECK_FALSE(writer.totalStats().failed);
}

TEST_CASE("every armed track gets its own file", "[diskwriter]") {
    TempDir dir;
    const std::string a = (dir.path / "a.wav").string();
    const std::string b = (dir.path / "b.wav").string();

    std::vector<std::unique_ptr<DiskWriter::Track>> tracks;
    tracks.push_back(makeTrack(a, "track_a", 1));
    tracks.push_back(makeTrack(b, "track_b", 2));

    DiskWriter writer;
    REQUIRE(writer.start(std::move(tracks), 48000, WavWriter::Format::Pcm16,
                         8192));
    CHECK(writer.trackCount() == 2);
    CHECK(writer.trackIdOf(0) == "track_a");
    CHECK(writer.trackIdOf(1) == "track_b");

    for (int i = 0; i < 10; ++i) {
        pushConstant(*writer.ring(0), 1, 256, 0.5f);
        pushConstant(*writer.ring(1), 2, 256, -0.5f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    writer.stop();

    CHECK(frameCountOf(a) == 2560);
    CHECK(frameCountOf(b) == 2560);
}

TEST_CASE("a take stopped immediately still leaves a valid file",
          "[diskwriter]") {
    // Arm, press record, press stop. The importer must not meet a truncated
    // header.
    TempDir dir;
    const std::string path = (dir.path / "empty.wav").string();
    std::vector<std::unique_ptr<DiskWriter::Track>> tracks;
    tracks.push_back(makeTrack(path, "track_1", 2));

    DiskWriter writer;
    REQUIRE(writer.start(std::move(tracks), 48000, WavWriter::Format::Pcm24,
                         4096));
    writer.stop();

    CHECK(std::filesystem::exists(path));
    CHECK(frameCountOf(path) == 0);
}

TEST_CASE("one unopenable file refuses the whole arm", "[diskwriter]") {
    // Arming three tracks and silently recording two is a failure the user
    // would only find after the performance.
    TempDir dir;
    std::vector<std::unique_ptr<DiskWriter::Track>> tracks;
    tracks.push_back(makeTrack((dir.path / "ok.wav").string(), "a", 1));
    tracks.push_back(makeTrack("/definitely/not/a/dir/bad.wav", "b", 1));

    DiskWriter writer;
    CHECK_FALSE(writer.start(std::move(tracks), 48000,
                             WavWriter::Format::Pcm24, 4096));
    CHECK_FALSE(writer.isRunning());
}

TEST_CASE("stopping twice is safe", "[diskwriter]") {
    TempDir dir;
    std::vector<std::unique_ptr<DiskWriter::Track>> tracks;
    tracks.push_back(makeTrack((dir.path / "twice.wav").string(), "a", 1));

    DiskWriter writer;
    REQUIRE(writer.start(std::move(tracks), 48000, WavWriter::Format::Pcm24,
                         4096));
    pushConstant(*writer.ring(0), 1, 512, 0.1f);
    writer.stop();
    writer.stop();
    CHECK_FALSE(writer.isRunning());
}

TEST_CASE("a finished take is hashed after close on the writer thread",
          "[diskwriter][hash]") {
    TempDir dir;
    const std::string path = (dir.path / "hashed.wav").string();
    std::vector<std::unique_ptr<DiskWriter::Track>> tracks;
    tracks.push_back(makeTrack(path, "hashed", 1));

    const std::thread::id callerThread = std::this_thread::get_id();
    std::thread::id hashThread;
    size_t hashCalls = 0;
    bool sawFinalizedHeader = false;
    DiskWriter writer([&](const std::string& file) {
        hashThread = std::this_thread::get_id();
        ++hashCalls;
        sawFinalizedHeader = frameCountOf(file) == 512;
        return dave::document::sha256HexOfFile(file);
    });

    REQUIRE(writer.start(std::move(tracks), 48000,
                         WavWriter::Format::Pcm24, 4096));
    pushConstant(*writer.ring(0), 1, 512, 0.25f);
    CHECK(writer.shaOf(0).empty());
    writer.stop();

    CHECK(hashCalls == 1);
    CHECK(hashThread != callerThread);
    CHECK(sawFinalizedHeader);
    CHECK_FALSE(writer.shaOf(0).empty());
    CHECK(writer.shaOf(0) == dave::document::sha256HexOfFile(path));
    CHECK_FALSE(writer.stats(0).failed);
    CHECK(writer.shaOf(1).empty());

    writer.stop();
    CHECK(hashCalls == 1);
}

TEST_CASE("hash failure fails the take and publishes no sha",
          "[diskwriter][hash]") {
    TempDir dir;
    const std::string path = (dir.path / "hash_failure.wav").string();
    std::vector<std::unique_ptr<DiskWriter::Track>> tracks;
    tracks.push_back(makeTrack(path, "failed_hash", 1));

    bool hasherCalled = false;
    DiskWriter writer([&](const std::string&) {
        hasherCalled = true;
        return std::string();
    });
    REQUIRE(writer.start(std::move(tracks), 48000,
                         WavWriter::Format::Pcm24, 4096));
    pushConstant(*writer.ring(0), 1, 128, 0.25f);
    writer.stop();

    CHECK(hasherCalled);
    CHECK(writer.shaOf(0).empty());
    CHECK(writer.stats(0).failed);
    // Hash failure does not undo the successful close or destroy the take.
    CHECK(frameCountOf(path) == 128);
}
