// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/record/WavWriter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace dave::engine {

namespace {

#ifdef _WIN32
using FileOffset = __int64;
bool seekTo(std::FILE* file, FileOffset offset, int origin) {
    return ::_fseeki64(file, offset, origin) == 0;
}
FileOffset tellOf(std::FILE* file) { return ::_ftelli64(file); }
#else
using FileOffset = off_t;
bool seekTo(std::FILE* file, FileOffset offset, int origin) {
    return ::fseeko(file, offset, origin) == 0;
}
FileOffset tellOf(std::FILE* file) { return ::ftello(file); }
#endif

constexpr auto kHeaderRefreshInterval = std::chrono::seconds(5);

void put16(uint8_t* out, size_t offset, uint16_t value) {
    out[offset + 0] = static_cast<uint8_t>(value & 0xFFu);
    out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void put32(uint8_t* out, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[offset + static_cast<size_t>(i)] =
            static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
    }
}

void put64(uint8_t* out, size_t offset, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[offset + static_cast<size_t>(i)] =
            static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
    }
}

void putFourCC(uint8_t* out, size_t offset, const char id[5]) {
    std::memcpy(out + offset, id, 4);
}

int bitsPerSample(WavWriter::Format format) {
    switch (format) {
        case WavWriter::Format::Pcm16: return 16;
        case WavWriter::Format::Pcm24: return 24;
        case WavWriter::Format::Float32: return 32;
    }
    return 24;
}

uint16_t formatTag(WavWriter::Format format) {
    return format == WavWriter::Format::Float32 ? 3u : 1u;
}

void buildCommonHeader(uint8_t* out, int channels, int sampleRate,
                       WavWriter::Format format) {
    const int bits = bitsPerSample(format);
    const uint16_t blockAlign = static_cast<uint16_t>(channels * bits / 8);
    putFourCC(out, 48, "fmt ");
    put32(out, 52, 16);
    put16(out, 56, formatTag(format));
    put16(out, 58, static_cast<uint16_t>(channels));
    put32(out, 60, static_cast<uint32_t>(sampleRate));
    put32(out, 64, static_cast<uint32_t>(sampleRate) * blockAlign);
    put16(out, 68, blockAlign);
    put16(out, 70, static_cast<uint16_t>(bits));
    putFourCC(out, 72, "data");
}

// Sample conversion. The clamp is not decoration: an over-unity sample that
// wraps sign becomes a full-scale click, which is far more destructive than
// the flat top of an honest clip.
inline float clamp1(float x) { return std::max(-1.0f, std::min(1.0f, x)); }

// TPDF dither at 1 LSB — two independent uniform draws summed. Applied at 16
// bits only: at 24 the dither sits below the noise floor of any real converter
// and buys nothing but lost headroom, and 32-bit float does not quantise at
// all.
inline float tpdf(uint32_t& state) {
    auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state) / static_cast<float>(UINT32_MAX);
    };
    return next() - next();
}

} // namespace

void WavWriter::buildRiffHeader(uint8_t out[kHeaderBytes], int channels,
                                int sampleRate, Format format,
                                uint64_t dataBytes) {
    std::memset(out, 0, kHeaderBytes);
    const uint64_t pad = dataBytes & 1u;
    putFourCC(out, 0, "RIFF");
    put32(out, 4, static_cast<uint32_t>(72ull + dataBytes + pad));
    putFourCC(out, 8, "WAVE");
    putFourCC(out, 12, "JUNK");
    put32(out, 16, 28);
    buildCommonHeader(out, channels, sampleRate, format);
    put32(out, 76, static_cast<uint32_t>(dataBytes));
}

void WavWriter::buildRf64Header(uint8_t out[kHeaderBytes], int channels,
                                int sampleRate, Format format,
                                uint64_t dataBytes) {
    std::memset(out, 0, kHeaderBytes);
    const uint64_t pad = dataBytes & 1u;
    putFourCC(out, 0, "RF64");
    put32(out, 4, 0xFFFFFFFFu);
    putFourCC(out, 8, "WAVE");
    putFourCC(out, 12, "ds64");
    put32(out, 16, 28);
    put64(out, 20, 72ull + dataBytes + pad);
    put64(out, 28, dataBytes);
    put64(out, 36, 0);  // Unknown sample count; readers derive it from data size.
    put32(out, 44, 0);  // No additional ds64 table entries.
    buildCommonHeader(out, channels, sampleRate, format);
    put32(out, 76, 0xFFFFFFFFu);
}

bool WavWriter::riffCanHold(uint64_t dataBytes) {
    constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();
    const uint64_t pad = dataBytes & 1u;
    return dataBytes <= kMax && dataBytes <= kMax - 72ull - pad;
}

WavWriter::~WavWriter() { (void)close(); }

bool WavWriter::open(const std::string& path, int channels, int sampleRate,
                     Format format) {
    if (file_ != nullptr && !close()) return false;
    hadError_.store(false, std::memory_order_release);
    if (channels <= 0 || sampleRate <= 0) return false;

    file_ = std::fopen(path.c_str(), "wb+");
    if (file_ == nullptr) return false;

    path_ = path;
    channels_ = channels;
    sampleRate_ = sampleRate;
    format_ = format;
    framesWritten_.store(0, std::memory_order_release);
    lastHeaderFlush_ = std::chrono::steady_clock::now();

    if (!writeHeader()) {
        hadError_.store(true, std::memory_order_release);
        (void)std::fclose(file_);
        file_ = nullptr;
        path_.clear();
        return false;
    }
    return true;
}

bool WavWriter::writeHeader() {
    uint8_t header[kHeaderBytes];
    buildRiffHeader(header, channels_, sampleRate_, format_, 0);
    return std::fwrite(header, 1, sizeof(header), file_) == sizeof(header);
}

bool WavWriter::write(const float* interleaved, size_t frames) {
    if (file_ == nullptr || frames == 0) return file_ != nullptr;

    const size_t samples = frames * static_cast<size_t>(channels_);
    const size_t bytesPerSample =
        (format_ == Format::Pcm16) ? 2 : (format_ == Format::Pcm24 ? 3 : 4);
    convertBuf_.resize(samples * bytesPerSample);
    uint8_t* out = convertBuf_.data();

    switch (format_) {
        case Format::Pcm16: {
            for (size_t i = 0; i < samples; ++i) {
                const float dithered =
                    interleaved[i] + tpdf(ditherState_) / 32768.0f;
                int32_t v = static_cast<int32_t>(
                    std::lrintf(clamp1(dithered) * 32767.0f));
                v = std::max(-32768, std::min(32767, v));
                const auto u = static_cast<uint16_t>(v);
                out[i * 2 + 0] = static_cast<uint8_t>(u & 0xFF);
                out[i * 2 + 1] = static_cast<uint8_t>((u >> 8) & 0xFF);
            }
            break;
        }
        case Format::Pcm24: {
            for (size_t i = 0; i < samples; ++i) {
                int32_t v = static_cast<int32_t>(
                    std::lrintf(clamp1(interleaved[i]) * 8388607.0f));
                v = std::max(-8388608, std::min(8388607, v));
                const auto u = static_cast<uint32_t>(v);
                out[i * 3 + 0] = static_cast<uint8_t>(u & 0xFF);
                out[i * 3 + 1] = static_cast<uint8_t>((u >> 8) & 0xFF);
                out[i * 3 + 2] = static_cast<uint8_t>((u >> 16) & 0xFF);
            }
            break;
        }
        case Format::Float32: {
            // No clamp: float headroom is the whole reason to pick this depth.
            std::memcpy(out, interleaved, samples * sizeof(float));
            break;
        }
    }

    const size_t bytes = samples * bytesPerSample;
    if (std::fwrite(convertBuf_.data(), 1, bytes, file_) != bytes) {
        hadError_.store(true, std::memory_order_release);
        return false;
    }
    framesWritten_.fetch_add(frames, std::memory_order_release);
    return refreshHeaderIfDue();
}

bool WavWriter::writeSilence(size_t frames) {
    if (file_ == nullptr || frames == 0) return file_ != nullptr;
    // Chunked so a long gap does not allocate a proportionally long buffer.
    constexpr size_t kChunk = 4096;
    std::vector<float> zeros(kChunk * static_cast<size_t>(channels_), 0.0f);
    while (frames > 0) {
        const size_t n = std::min(frames, kChunk);
        if (!write(zeros.data(), n)) return false;
        frames -= n;
    }
    return true;
}

uint64_t WavWriter::dataBytesWritten() const {
    const size_t bytesPerSample =
        (format_ == Format::Pcm16) ? 2 : (format_ == Format::Pcm24 ? 3 : 4);
    return framesWritten_.load(std::memory_order_acquire) *
           static_cast<uint64_t>(channels_) * bytesPerSample;
}

bool WavWriter::flushHeader() {
    if (file_ == nullptr) return !hadError();

    const FileOffset savedOffset = tellOf(file_);
    if (savedOffset < 0) {
        hadError_.store(true, std::memory_order_release);
        return false;
    }

    const uint64_t dataBytes = dataBytesWritten();
    uint8_t header[kHeaderBytes];
    if (dataBytes > rf64ThresholdBytes_ || !riffCanHold(dataBytes)) {
        buildRf64Header(header, channels_, sampleRate_, format_, dataBytes);
    } else {
        buildRiffHeader(header, channels_, sampleRate_, format_, dataBytes);
    }

    bool ok = true;
    // RIFF chunks are word-aligned, but the pad is not part of the data size.
    // Leave the cursor before it so the next audio write overwrites it.
    if ((dataBytes & 1u) != 0) {
        const uint8_t zero = 0;
        if (!seekTo(file_, static_cast<FileOffset>(kHeaderBytes + dataBytes),
                    SEEK_SET) ||
            std::fwrite(&zero, 1, 1, file_) != 1) {
            ok = false;
        }
    }
    if (!seekTo(file_, 0, SEEK_SET) ||
        std::fwrite(header, 1, sizeof(header), file_) != sizeof(header)) {
        ok = false;
    }
    if (!seekTo(file_, savedOffset, SEEK_SET)) ok = false;
    if (std::fflush(file_) != 0) ok = false;

    if (!ok) hadError_.store(true, std::memory_order_release);
    if (ok) lastHeaderFlush_ = std::chrono::steady_clock::now();
    return ok && !hadError();
}

bool WavWriter::refreshHeaderIfDue() {
    if (file_ == nullptr) return !hadError();
    if (std::chrono::steady_clock::now() - lastHeaderFlush_ <
        kHeaderRefreshInterval) {
        return !hadError();
    }
    return flushHeader();
}

bool WavWriter::close() {
    if (file_ == nullptr) return !hadError();
    bool ok = flushHeader();
    if (std::fclose(file_) != 0) ok = false;
    file_ = nullptr;
    if (!ok) hadError_.store(true, std::memory_order_release);
    return ok && !hadError();
}

} // namespace dave::engine
