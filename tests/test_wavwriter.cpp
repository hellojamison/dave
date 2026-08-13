// SPDX-License-Identifier: GPL-3.0-or-later
//
// This is the proof that Edit::bitDepth() means something.
//
// The setting has been persisted in project.json and shown in the toolbar
// since the session-format work, but nothing consumed it — there was no code
// that wrote an audio file. WavWriter is that code, and these tests are what
// make the claim checkable: write a known signal at each depth, read it back
// with dr_wav, and assert both the container fields and the samples.
#include "engine/record/WavWriter.h"

#define DR_WAV_NO_IMPLEMENTATION
#include <dr_wav.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using dave::engine::WavWriter;

namespace {

// Each test writes into the system temp dir and removes the file after.
struct TempWav {
    explicit TempWav(const char* name) {
        path = (std::filesystem::temp_directory_path() /
                ("dave_test_" + std::string(name) + ".wav")).string();
        std::filesystem::remove(path);
    }
    ~TempWav() { std::filesystem::remove(path); }
    std::string path;
};

struct ReadBack {
    bool ok = false;
    drwav_uint32 channels = 0;
    drwav_uint32 sampleRate = 0;
    drwav_uint32 bitsPerSample = 0;
    drwav_uint16 formatTag = 0;
    drwav_container container = drwav_container_riff;
    drwav_uint64 frames = 0;
    std::vector<float> samples;   // interleaved f32
};

ReadBack readBack(const std::string& path) {
    ReadBack r;
    drwav wav;
    if (!drwav_init_file(&wav, path.c_str(), nullptr)) return r;
    r.ok = true;
    r.channels = wav.channels;
    r.sampleRate = wav.sampleRate;
    r.bitsPerSample = wav.bitsPerSample;
    r.formatTag = wav.translatedFormatTag;
    r.container = wav.container;
    r.frames = wav.totalPCMFrameCount;
    r.samples.resize(static_cast<size_t>(wav.totalPCMFrameCount) * wav.channels);
    drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, r.samples.data());
    drwav_uninit(&wav);
    return r;
}

uint32_t get32(const uint8_t* data, size_t offset) {
    return static_cast<uint32_t>(data[offset + 0]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint64_t get64(const uint8_t* data, size_t offset) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[offset + static_cast<size_t>(i)])
                 << (i * 8);
    }
    return value;
}

drwav_data_format drwavFormat(int channels, int sampleRate,
                              WavWriter::Format format) {
    drwav_data_format result{};
    result.container = drwav_container_rf64;
    result.channels = static_cast<drwav_uint32>(channels);
    result.sampleRate = static_cast<drwav_uint32>(sampleRate);
    result.format = format == WavWriter::Format::Float32
                        ? DR_WAVE_FORMAT_IEEE_FLOAT
                        : DR_WAVE_FORMAT_PCM;
    result.bitsPerSample = format == WavWriter::Format::Pcm16
                               ? 16
                               : (format == WavWriter::Format::Pcm24 ? 24 : 32);
    return result;
}

} // namespace

TEST_CASE("reserved RIFF header has the byte-exact 80-byte layout",
          "[wavwriter][header]") {
    uint8_t header[WavWriter::kHeaderBytes];
    constexpr uint64_t kDataBytes = 6000;
    WavWriter::buildRiffHeader(header, 2, 48000, WavWriter::Format::Pcm24,
                               kDataBytes);

    CHECK(std::memcmp(header + 0, "RIFF", 4) == 0);
    CHECK(get32(header, 4) == 72 + kDataBytes);
    CHECK(std::memcmp(header + 8, "WAVE", 4) == 0);
    CHECK(std::memcmp(header + 12, "JUNK", 4) == 0);
    CHECK(get32(header, 16) == 28);
    for (size_t i = 20; i < 48; ++i) CHECK(header[i] == 0);
    CHECK(std::memcmp(header + 48, "fmt ", 4) == 0);
    CHECK(get32(header, 52) == 16);
    CHECK(std::memcmp(header + 72, "data", 4) == 0);
    CHECK(get32(header, 76) == kDataBytes);
}

TEST_CASE("RF64 header agrees with dr_wav for a real payload",
          "[wavwriter][header][rf64]") {
    constexpr int kChannels = 2;
    constexpr int kFrames = 1000;
    constexpr uint64_t kDataBytes = kFrames * kChannels * 3ull;
    uint8_t ours[WavWriter::kHeaderBytes];
    WavWriter::buildRf64Header(ours, kChannels, 48000,
                               WavWriter::Format::Pcm24, kDataBytes);

    // dr_wav only refreshes a non-sequential writer's ds64 fields from its
    // write path. Init alone leaves an initialization-time size of 36 here.
    void* oracleData = nullptr;
    size_t oracleSize = 0;
    drwav oracle{};
    auto format = drwavFormat(kChannels, 48000, WavWriter::Format::Pcm24);
    REQUIRE(drwav_init_memory_write(&oracle, &oracleData, &oracleSize, &format,
                                    nullptr));
    std::vector<uint8_t> samples(static_cast<size_t>(kDataBytes), 0);
    REQUIRE(drwav_write_raw(&oracle, samples.size(), samples.data()) ==
            samples.size());
    REQUIRE(drwav_uninit(&oracle) == DRWAV_SUCCESS);
    REQUIRE(oracleSize >= WavWriter::kHeaderBytes);
    CHECK(std::memcmp(ours, oracleData, WavWriter::kHeaderBytes) == 0);
    drwav_free(oracleData, nullptr);
}

TEST_CASE("RIFF boundary and RF64 64-bit sizes need no large file",
          "[wavwriter][header][rf64]") {
    constexpr uint64_t kU32Max = std::numeric_limits<uint32_t>::max();
    // 72 bytes precede the payload in the RIFF chunk. An odd payload also
    // consumes a pad byte, so the superficially obvious upper edge does not fit.
    CHECK(WavWriter::riffCanHold(kU32Max - 73));
    CHECK_FALSE(WavWriter::riffCanHold(kU32Max - 72));
    CHECK_FALSE(WavWriter::riffCanHold(kU32Max - 71));

    uint8_t header[WavWriter::kHeaderBytes];
    constexpr uint64_t kFiveBillion = 5'000'000'000ull;
    WavWriter::buildRf64Header(header, 2, 48000, WavWriter::Format::Pcm24,
                               kFiveBillion);
    CHECK(std::memcmp(header, "RF64", 4) == 0);
    CHECK(get32(header, 4) == 0xFFFFFFFFu);
    CHECK(std::memcmp(header + 12, "ds64", 4) == 0);
    CHECK(get32(header, 16) == 28);
    CHECK(get64(header, 20) == 72 + kFiveBillion);
    CHECK(get64(header, 28) == kFiveBillion);
    CHECK(get64(header, 36) == 0);
    CHECK(get32(header, 44) == 0);
}

TEST_CASE("bit depth selects the container format", "[wavwriter][bitdepth]") {
    // The mapping the session setting relies on. 32 means IEEE float — that is
    // the convention Edit::bitDepth() already encodes, since there is no
    // 32-bit integer option to confuse it with.
    CHECK(WavWriter::formatForBitDepth(16) == WavWriter::Format::Pcm16);
    CHECK(WavWriter::formatForBitDepth(24) == WavWriter::Format::Pcm24);
    CHECK(WavWriter::formatForBitDepth(32) == WavWriter::Format::Float32);
    // Anything unexpected lands on 24, the session default.
    CHECK(WavWriter::formatForBitDepth(0) == WavWriter::Format::Pcm24);
    CHECK(WavWriter::formatForBitDepth(8) == WavWriter::Format::Pcm24);
}

TEST_CASE("each depth writes a readable file with the right header",
          "[wavwriter][bitdepth]") {
    struct Case {
        const char* name;
        WavWriter::Format format;
        drwav_uint32 bits;
        drwav_uint16 tag;
    };
    const Case cases[] = {
        {"pcm16", WavWriter::Format::Pcm16, 16, DR_WAVE_FORMAT_PCM},
        {"pcm24", WavWriter::Format::Pcm24, 24, DR_WAVE_FORMAT_PCM},
        {"f32", WavWriter::Format::Float32, 32, DR_WAVE_FORMAT_IEEE_FLOAT},
    };

    for (const auto& c : cases) {
        INFO("format " << c.name);
        TempWav tmp(c.name);
        constexpr int kFrames = 1000;

        WavWriter w;
        REQUIRE(w.open(tmp.path, 2, 48000, c.format));
        std::vector<float> block(kFrames * 2);
        for (int i = 0; i < kFrames; ++i) {
            const float s = std::sin(static_cast<float>(i) * 0.01f) * 0.5f;
            block[i * 2 + 0] = s;
            block[i * 2 + 1] = -s;
        }
        REQUIRE(w.write(block.data(), kFrames));
        CHECK(w.framesWritten() == kFrames);
        REQUIRE(w.close());

        const ReadBack r = readBack(tmp.path);
        REQUIRE(r.ok);
        CHECK(r.channels == 2);
        CHECK(r.sampleRate == 48000);
        CHECK(r.bitsPerSample == c.bits);
        CHECK(r.formatTag == c.tag);
        // The frame count comes from the patched data-chunk size. If close()
        // failed to patch it, this is where it shows.
        CHECK(r.frames == kFrames);

        // Samples survive within that format's quantisation step.
        const float tolerance = (c.bits == 16) ? 2.0f / 32768.0f
                              : (c.bits == 24) ? 2.0f / 8388608.0f
                                               : 1e-7f;
        for (int i = 0; i < kFrames; ++i) {
            REQUIRE(std::fabs(r.samples[i * 2 + 0] - block[i * 2 + 0]) <
                    tolerance);
            REQUIRE(std::fabs(r.samples[i * 2 + 1] - block[i * 2 + 1]) <
                    tolerance);
        }
    }
}

TEST_CASE("over-unity samples clip rather than wrapping sign",
          "[wavwriter][bitdepth]") {
    // The failure this prevents is not subtle: an integer conversion without a
    // clamp turns +1.5 into a large negative number, so a moment of overload
    // becomes a full-scale click — far more destructive than the flat top of
    // an honest clip.
    for (auto format : {WavWriter::Format::Pcm16, WavWriter::Format::Pcm24}) {
        TempWav tmp("clip");
        WavWriter w;
        REQUIRE(w.open(tmp.path, 1, 48000, format));
        const std::vector<float> block = {1.5f, -1.5f, 1.0f, -1.0f, 0.0f};
        REQUIRE(w.write(block.data(), block.size()));
        REQUIRE(w.close());

        const ReadBack r = readBack(tmp.path);
        REQUIRE(r.ok);
        REQUIRE(r.frames == 5);
        CHECK(r.samples[0] > 0.99f);     // clipped positive, still positive
        CHECK(r.samples[1] < -0.99f);    // clipped negative, still negative
        CHECK(r.samples[2] > 0.99f);
        CHECK(r.samples[3] < -0.99f);
        CHECK(std::fabs(r.samples[4]) < 0.01f);
    }
}

TEST_CASE("32-bit float is bit-exact and undithered", "[wavwriter][bitdepth]") {
    // Float is chosen precisely to avoid quantisation, so anything added here
    // — dither, a clamp, a round-trip through an integer — is a defect.
    TempWav tmp("exact");
    WavWriter w;
    REQUIRE(w.open(tmp.path, 1, 48000, WavWriter::Format::Float32));
    const std::vector<float> block = {0.0f,      1.0f,   -1.0f, 0.333333f,
                                      -0.12345f, 1.5f,   -2.5f, 1e-7f};
    REQUIRE(w.write(block.data(), block.size()));
    REQUIRE(w.close());

    const ReadBack r = readBack(tmp.path);
    REQUIRE(r.ok);
    REQUIRE(r.frames == block.size());
    for (size_t i = 0; i < block.size(); ++i) {
        INFO("sample " << i);
        // Including the over-unity values: float headroom is the whole point.
        REQUIRE(r.samples[i] == block[i]);
    }
}

TEST_CASE("16-bit dither is bounded and roughly zero-mean",
          "[wavwriter][bitdepth]") {
    // TPDF at 1 LSB. If it were unbounded or biased it would show up as a DC
    // offset or as noise well above the quantisation floor.
    TempWav tmp("dither");
    WavWriter w;
    REQUIRE(w.open(tmp.path, 1, 48000, WavWriter::Format::Pcm16));
    constexpr int kFrames = 20000;
    const std::vector<float> block(kFrames, 0.25f);   // constant, mid-scale
    REQUIRE(w.write(block.data(), kFrames));
    REQUIRE(w.close());

    const ReadBack r = readBack(tmp.path);
    REQUIRE(r.ok);
    REQUIRE(r.frames == kFrames);

    double sum = 0.0;
    float maxDeviation = 0.0f;
    for (int i = 0; i < kFrames; ++i) {
        const float d = r.samples[i] - 0.25f;
        sum += d;
        maxDeviation = std::max(maxDeviation, std::fabs(d));
    }
    const double mean = sum / kFrames;
    // A couple of LSBs at most — dither plus the quantisation itself.
    CHECK(maxDeviation < 4.0f / 32768.0f);
    // And no DC offset: well under one LSB averaged over 20k samples.
    CHECK(std::fabs(mean) < 0.5 / 32768.0);
}

TEST_CASE("silence padding keeps the file aligned", "[wavwriter]") {
    // This is how a ring overrun is made good: the gap is written as silence
    // so everything after it stays at its true timeline position. A shorter
    // file would pull all later audio earlier.
    TempWav tmp("silence");
    WavWriter w;
    REQUIRE(w.open(tmp.path, 2, 48000, WavWriter::Format::Pcm24));
    const std::vector<float> tone(100 * 2, 0.5f);
    REQUIRE(w.write(tone.data(), 100));
    REQUIRE(w.writeSilence(9000));      // more than one internal chunk
    REQUIRE(w.write(tone.data(), 100));
    CHECK(w.framesWritten() == 9200);
    REQUIRE(w.close());

    const ReadBack r = readBack(tmp.path);
    REQUIRE(r.ok);
    REQUIRE(r.frames == 9200);
    CHECK(r.samples[0] == Catch::Approx(0.5f).margin(0.001));
    CHECK(std::fabs(r.samples[500 * 2]) < 0.001f);        // inside the gap
    CHECK(r.samples[9100 * 2] == Catch::Approx(0.5f).margin(0.001));
}

TEST_CASE("a writer with no frames still produces a valid empty file",
          "[wavwriter]") {
    // A take armed and stopped immediately must not leave an unreadable file
    // for the asset importer to choke on.
    TempWav tmp("empty");
    WavWriter w;
    REQUIRE(w.open(tmp.path, 2, 48000, WavWriter::Format::Pcm24));
    CHECK(w.framesWritten() == 0);
    REQUIRE(w.close());

    const ReadBack r = readBack(tmp.path);
    REQUIRE(r.ok);
    CHECK(r.channels == 2);
    CHECK(r.frames == 0);
}

TEST_CASE("opening an unwritable path fails rather than half-succeeding",
          "[wavwriter]") {
    WavWriter w;
    CHECK_FALSE(w.open("/definitely/not/a/directory/take.wav", 2, 48000,
                       WavWriter::Format::Pcm24));
    CHECK_FALSE(w.isOpen());
}

TEST_CASE("a small take can be promoted to RF64 in place",
          "[wavwriter][rf64]") {
    TempWav tmp("promoted");
    WavWriter w;
    w.setRf64ThresholdBytes(3000);
    REQUIRE(w.open(tmp.path, 2, 48000, WavWriter::Format::Pcm24));
    constexpr size_t kFrames = 1000;
    const std::vector<float> samples(kFrames * 2, 0.25f);
    REQUIRE(w.write(samples.data(), kFrames));
    REQUIRE(w.close());

    const ReadBack r = readBack(tmp.path);
    REQUIRE(r.ok);
    CHECK(r.container == drwav_container_rf64);
    CHECK(r.frames == kFrames);
    CHECK(r.samples.front() == Catch::Approx(0.25f).margin(0.001f));
}

TEST_CASE("flushHeader makes an open take readable from a second handle",
          "[wavwriter][flush]") {
    TempWav tmp("live_header");
    WavWriter w;
    REQUIRE(w.open(tmp.path, 1, 48000, WavWriter::Format::Float32));
    const std::vector<float> first(1000, 0.125f);
    REQUIRE(w.write(first.data(), first.size()));
    REQUIRE(w.flushHeader());

    const ReadBack during = readBack(tmp.path);
    REQUIRE(during.ok);
    CHECK(during.frames == first.size());
    CHECK(during.samples.front() == first.front());

    const std::vector<float> second(250, -0.25f);
    REQUIRE(w.write(second.data(), second.size()));
    REQUIRE(w.close());
    CHECK_FALSE(w.hadError());

    const ReadBack final = readBack(tmp.path);
    REQUIRE(final.ok);
    REQUIRE(final.frames == first.size() + second.size());
    CHECK(final.samples[first.size()] == second.front());
}

TEST_CASE("odd PCM24 payload gets one pad byte without moving later audio",
          "[wavwriter][padding][flush]") {
    TempWav tmp("odd_pad");
    WavWriter w;
    REQUIRE(w.open(tmp.path, 1, 48000, WavWriter::Format::Pcm24));
    const float first = 0.25f;
    REQUIRE(w.write(&first, 1));
    REQUIRE(w.flushHeader());
    CHECK(std::filesystem::file_size(tmp.path) == WavWriter::kHeaderBytes + 4);

    // The live pad is overwritten by the next 3-byte sample; it must not turn
    // into an embedded zero or shift the second sample by one byte.
    const float second = -0.5f;
    REQUIRE(w.write(&second, 1));
    REQUIRE(w.close());
    CHECK(std::filesystem::file_size(tmp.path) == WavWriter::kHeaderBytes + 6);

    const ReadBack r = readBack(tmp.path);
    REQUIRE(r.ok);
    REQUIRE(r.frames == 2);
    CHECK(r.samples[0] == Catch::Approx(first).margin(0.000001f));
    CHECK(r.samples[1] == Catch::Approx(second).margin(0.000001f));
}
