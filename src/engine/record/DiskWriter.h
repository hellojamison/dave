// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/record/WavWriter.h"
#include "engine/util/SpscRing.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dave::engine {

// Drains capture rings to disk on a background thread.
//
// One thread serves every armed track, round-robin. One thread per track would
// multiply the seeking on a spinning disk and buy nothing — the work here is
// bulk sequential writes, which a single thread saturates.
//
// Threading:
//   - The audio thread only ever calls ring()->write(). It never touches this
//     class's mutex, and nothing here signals it.
//   - This thread wakes on a timeout rather than a condition-variable notify
//     from the audio thread: notifying a condvar is a syscall, which the RT
//     contract forbids (AGENTS.md, "no syscalls on the audio thread").
//   - Everything else is UI thread.
class DiskWriter {
public:
    // One recording destination: a ring the RT thread fills and the file it
    // drains to.
    struct Track {
        std::string path;
        std::string trackId;      // document track this take belongs to
        int channels = 1;
        SpscRing ring;
        WavWriter writer;
        // Writer-thread positioning. `outputPos` counts audio plus silence;
        // `paddedFrames` prevents EOF fallback from padding a gap twice.
        uint64_t outputPos = 0;
        uint64_t paddedFrames = 0;
        uint64_t padRemaining = 0;
        // Published only after the writer thread has successfully closed and
        // hashed the finished file. Empty while recording or after failure.
        std::string sha256;
        std::atomic<bool> failed{false};
    };

    // What the UI needs to know while a take is running, and after it ends.
    struct Stats {
        uint64_t framesWritten = 0;
        uint64_t droppedFrames = 0;
        uint64_t overrunBlocks = 0;
        uint64_t unlocatedDropBlocks = 0;
        bool failed = false;
    };

    using FileHasher = std::function<std::string(const std::string&)>;

    DiskWriter();
    // Test seam for deterministic hash failures and thread-affinity checks.
    explicit DiskWriter(FileHasher fileHasher);
    ~DiskWriter();

    DiskWriter(const DiskWriter&) = delete;
    DiskWriter& operator=(const DiskWriter&) = delete;

    // Opens every file and starts the thread. `ringFrames` sizes each track's
    // ring — big enough to ride out a filesystem stall, since the alternative
    // is a hole in the take. Returns false (and opens nothing) if any file
    // cannot be created: a partial arm is worse than a refused one.
    bool start(std::vector<std::unique_ptr<Track>> tracks, int sampleRate,
               WavWriter::Format format, size_t ringFrames);

    // Drains what is left, closes the files, joins. Safe to call twice.
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // Rings live for the whole session; the RT thread holds these pointers.
    size_t trackCount() const { return tracks_.size(); }
    SpscRing* ring(size_t index) {
        return index < tracks_.size() ? &tracks_[index]->ring : nullptr;
    }

    // Poll-able from the UI thread at any time, including after stop().
    Stats stats(size_t index) const;
    Stats totalStats() const;
    std::string pathOf(size_t index) const;
    std::string trackIdOf(size_t index) const;
    // Available only after stop() has joined the writer thread. Returns empty
    // while recording, for an invalid index, or when close/hash failed.
    std::string shaOf(size_t index) const;

private:
    void run();
    // Returns frames written this pass. `final` drains everything available
    // rather than a bounded chunk.
    size_t drainOnce(bool final);
    void failTrack(Track& track);
    void finishUnlocatedDrops();
    void closeAndHash(Track& track);

    std::vector<std::unique_ptr<Track>> tracks_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    FileHasher fileHasher_;

    // Scratch for one drain pass. Owned by the writer thread only.
    std::vector<float> scratch_;
};

} // namespace dave::engine
