// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dave::engine {

// Streams interleaved f32 to a WAV file at the session's bit depth.
//
// This is the thing that makes Edit::bitDepth() mean something. dr_wav's
// writer does NO conversion — drwav_write_pcm_frames requires samples already
// in the target format — so the quantisation here is the feature, not glue.
//
// Threading: one writer per file, used by one thread (the disk writer). Never
// touched from the audio thread: it opens files and allocates.
class WavWriter {
public:
    // 16 and 24 are integer PCM; 32 means IEEE float, matching the session
    // setting's own convention (Edit::bitDepth() stores a bare 32 for float —
    // there is no 32-bit integer option to confuse it with).
    enum class Format { Pcm16, Pcm24, Float32 };

    static Format formatForBitDepth(int bitDepth) {
        switch (bitDepth) {
            case 16: return Format::Pcm16;
            case 32: return Format::Float32;
            default: return Format::Pcm24;
        }
    }

    // Both forms reserve the same 36-byte slot before "fmt ", so audio always
    // starts at byte 80 and a long take can be promoted to RF64 in place.
    static constexpr size_t kHeaderBytes = 80;
    static void buildRiffHeader(uint8_t out[kHeaderBytes], int channels,
                                int sampleRate, Format format,
                                uint64_t dataBytes);
    static void buildRf64Header(uint8_t out[kHeaderBytes], int channels,
                                int sampleRate, Format format,
                                uint64_t dataBytes);
    static bool riffCanHold(uint64_t dataBytes);

    WavWriter() = default;
    ~WavWriter();

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // Creates (or truncates) `path`. Returns false and leaves the writer
    // closed on failure.
    bool open(const std::string& path, int channels, int sampleRate,
              Format format);

    // Appends interleaved f32 frames, converting to the target format.
    // Returns false on a short write (disk full, device removed).
    bool write(const float* interleaved, size_t frames);

    // Appends `frames` of silence. Used to make good the frames a ring
    // overrun dropped, so the file stays aligned to the timeline.
    bool writeSilence(size_t frames);

    // Rewrites the current RIFF/RF64 header and flushes stdio buffers. This
    // protects against a process crash; it is not a power-loss durability
    // guarantee (which would require fsync/F_FULLFSYNC).
    [[nodiscard]] bool flushHeader();

    // Cheap on every writer-thread wake; does real I/O only when five seconds
    // have elapsed since the last successful header refresh.
    [[nodiscard]] bool refreshHeaderIfDue();

    // Refreshes the header and closes. Safe to call twice. The return value is
    // false if any write, header refresh, flush, or close has failed since the
    // current file was opened.
    [[nodiscard]] bool close();

    bool isOpen() const { return file_ != nullptr; }
    bool hadError() const { return hadError_.load(std::memory_order_acquire); }
    uint64_t framesWritten() const {
        return framesWritten_.load(std::memory_order_acquire);
    }
    const std::string& path() const { return path_; }

    // Test seam for exercising promotion with a small file. Production keeps
    // the default, which is the largest nominal payload a reserved RIFF header
    // can represent; riffCanHold() remains authoritative for odd-byte padding.
    void setRf64ThresholdBytes(uint64_t bytes) { rf64ThresholdBytes_ = bytes; }

private:
    bool writeHeader();
    uint64_t dataBytesWritten() const;

    std::FILE* file_ = nullptr;
    std::string path_;
    int channels_ = 0;
    int sampleRate_ = 0;
    Format format_ = Format::Pcm24;
    // DiskWriter::stats() polls these from the UI thread while this writer is
    // active, so they must not be plain fields.
    std::atomic<uint64_t> framesWritten_{0};
    std::atomic<bool> hadError_{false};
    uint64_t rf64ThresholdBytes_ = 0xFFFFFFFFull - 72ull;
    std::chrono::steady_clock::time_point lastHeaderFlush_{};

    // Scratch for one write() call's converted bytes. Grown as needed; this
    // thread may allocate.
    std::vector<uint8_t> convertBuf_;

    // Dither state for Pcm16. A per-writer xorshift keeps takes independent
    // and avoids any shared RNG.
    uint32_t ditherState_ = 0x1234567u;
};

} // namespace dave::engine
