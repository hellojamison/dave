// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/record/DiskWriter.h"

#include "document/Sha256.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace dave::engine {

namespace {
// Audio and silence share this per-track budget. A long dropout therefore
// cannot monopolize the writer thread and starve other armed tracks.
constexpr size_t kDrainChunkFrames = 8192;
constexpr auto kWakeInterval = std::chrono::milliseconds(10);
} // namespace

DiskWriter::DiskWriter() : fileHasher_(document::sha256HexOfFile) {}

DiskWriter::DiskWriter(FileHasher fileHasher)
    : fileHasher_(fileHasher ? std::move(fileHasher)
                             : FileHasher(document::sha256HexOfFile)) {}

DiskWriter::~DiskWriter() { stop(); }

bool DiskWriter::start(std::vector<std::unique_ptr<Track>> tracks,
                       int sampleRate, WavWriter::Format format,
                       size_t ringFrames) {
    stop();
    if (tracks.empty()) return false;

    // Open every file before committing to any of them. A partial arm is
    // worse than a refused one.
    for (auto& track : tracks) {
        if (!track->writer.open(track->path, track->channels, sampleRate,
                                format)) {
            for (auto& opened : tracks) {
                if (!opened->writer.close()) {
                    opened->failed.store(true, std::memory_order_release);
                }
            }
            return false;
        }
        track->ring.reset(ringFrames, track->channels);
        track->outputPos = 0;
        track->paddedFrames = 0;
        track->padRemaining = 0;
        track->sha256.clear();
        track->failed.store(false, std::memory_order_relaxed);
    }

    tracks_ = std::move(tracks);
    stopping_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void DiskWriter::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        // Files may be open from a failed start. Consume close failures even
        // though close is safe to call twice.
        for (auto& track : tracks_) {
            if (!track->writer.close()) {
                track->failed.store(true, std::memory_order_release);
            }
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true, std::memory_order_release);
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

void DiskWriter::run() {
    while (!stopping_.load(std::memory_order_acquire)) {
        drainOnce(false);
        std::unique_lock<std::mutex> lock(mutex_);
        // Timed wait, not a producer notify: the producer is the audio thread
        // and must not make syscalls.
        cv_.wait_for(lock, kWakeInterval, [this] {
            return stopping_.load(std::memory_order_acquire);
        });
    }

    // The audio callback may be mid-block when stop() arrives. Keep taking
    // bounded round-robin passes until 20 consecutive idle observations make
    // the producer quiescence assumption explicit (~200 ms).
    constexpr int kMaxIdlePasses = 20;
    int idlePasses = 0;
    while (idlePasses < kMaxIdlePasses) {
        const size_t moved = drainOnce(true);
        if (moved == 0) {
            ++idlePasses;
            std::this_thread::sleep_for(kWakeInterval);
        } else {
            idlePasses = 0;
        }
    }

    // Only after quiescence may unresolved queue-overflow gaps be appended.
    // Doing this while the producer could resume would double-pad if it later
    // published a coalesced recovery record.
    finishUnlocatedDrops();

    for (auto& track : tracks_) closeAndHash(*track);
}

void DiskWriter::failTrack(Track& track) {
    track.failed.store(true, std::memory_order_release);
    if (!track.writer.close()) {
        track.failed.store(true, std::memory_order_release);
    }
}

void DiskWriter::closeAndHash(Track& track) {
    // A failed take is never promoted to a content-addressed asset, even when
    // close itself can still patch a readable partial file.
    const bool failedBeforeClose =
        track.failed.load(std::memory_order_acquire) || track.writer.hadError();
    if (!track.writer.close()) {
        track.failed.store(true, std::memory_order_release);
        return;
    }
    if (failedBeforeClose) return;

    // This method is called only by run(), after draining and closing. Keeping
    // the read here prevents file hashing from blocking the UI/RT threads (the
    // current hasher's large-file memory footprint is a separate concern).
    try {
        std::string sha = fileHasher_(track.path);
        if (sha.empty()) {
            track.failed.store(true, std::memory_order_release);
            return;
        }
        track.sha256 = std::move(sha);
    } catch (...) {
        track.failed.store(true, std::memory_order_release);
    }
}

size_t DiskWriter::drainOnce(bool final) {
    (void)final; // Final passes stay bounded for the same fairness guarantee.
    size_t movedTotal = 0;

    for (auto& trackPtr : tracks_) {
        Track& track = *trackPtr;
        if (!track.writer.isOpen()) continue;
        if (!track.writer.refreshHeaderIfDue()) {
            failTrack(track);
            continue;
        }

        size_t budget = kDrainChunkFrames;
        while (budget > 0 && track.writer.isOpen()) {
            if (track.padRemaining > 0) {
                const size_t frames = static_cast<size_t>(std::min<uint64_t>(
                    track.padRemaining, static_cast<uint64_t>(budget)));
                if (!track.writer.writeSilence(frames)) {
                    failTrack(track);
                    break;
                }
                track.padRemaining -= frames;
                track.paddedFrames += frames;
                track.outputPos += frames;
                budget -= frames;
                movedTotal += frames;
                continue;
            }

            const auto view = track.ring.consumerView();
            if (view.hasGap) {
                if (view.gap.streamPos < track.outputPos) {
                    // A gap behind the output cursor means the ordering
                    // invariant failed; continuing would silently corrupt the
                    // rest of this take.
                    failTrack(track);
                    break;
                }
                if (view.gap.streamPos == track.outputPos) {
                    track.padRemaining = view.gap.frames;
                    if (!track.ring.popGap()) {
                        failTrack(track);
                        break;
                    }
                    continue;
                }
            }

            size_t want = budget;
            if (view.hasGap) {
                want = static_cast<size_t>(std::min<uint64_t>(
                    want, view.gap.streamPos - track.outputPos));
            }
            if (want == 0) continue;

            scratch_.resize(want * static_cast<size_t>(track.channels));
            const size_t got = track.ring.read(scratch_.data(), want, view);
            if (got == 0) break;
            if (!track.writer.write(scratch_.data(), got)) {
                failTrack(track);
                break;
            }
            track.outputPos += got;
            budget -= got;
            movedTotal += got;
        }
    }
    return movedTotal;
}

void DiskWriter::finishUnlocatedDrops() {
    // Round-robin here too. This phase is normally empty; when metadata did
    // overflow, a large unresolved dropout must not make closure unfair.
    while (true) {
        bool wroteAny = false;
        for (auto& trackPtr : tracks_) {
            Track& track = *trackPtr;
            if (!track.writer.isOpen()) continue;

            const uint64_t dropped = track.ring.droppedFrames();
            if (track.paddedFrames > dropped) {
                failTrack(track);
                continue;
            }
            const uint64_t remaining = dropped - track.paddedFrames;
            if (remaining == 0) continue;

            const size_t frames = static_cast<size_t>(std::min<uint64_t>(
                remaining, static_cast<uint64_t>(kDrainChunkFrames)));
            if (!track.writer.writeSilence(frames)) {
                failTrack(track);
                continue;
            }
            track.paddedFrames += frames;
            track.outputPos += frames;
            wroteAny = true;
        }
        if (!wroteAny) break;
    }
}

DiskWriter::Stats DiskWriter::stats(size_t index) const {
    Stats stats;
    if (index >= tracks_.size()) return stats;
    const auto& track = *tracks_[index];
    stats.framesWritten = track.writer.framesWritten();
    stats.droppedFrames = track.ring.droppedFrames();
    stats.overrunBlocks = track.ring.overrunBlocks();
    stats.unlocatedDropBlocks = track.ring.unlocatedDropBlocks();
    stats.failed = track.failed.load(std::memory_order_acquire) ||
                   track.writer.hadError();
    return stats;
}

DiskWriter::Stats DiskWriter::totalStats() const {
    Stats total;
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const Stats current = stats(i);
        total.framesWritten =
            std::max(total.framesWritten, current.framesWritten);
        total.droppedFrames += current.droppedFrames;
        total.overrunBlocks += current.overrunBlocks;
        total.unlocatedDropBlocks += current.unlocatedDropBlocks;
        total.failed = total.failed || current.failed;
    }
    return total;
}

std::string DiskWriter::pathOf(size_t index) const {
    return index < tracks_.size() ? tracks_[index]->path : std::string();
}

std::string DiskWriter::trackIdOf(size_t index) const {
    return index < tracks_.size() ? tracks_[index]->trackId : std::string();
}

std::string DiskWriter::shaOf(size_t index) const {
    // Avoid touching the writer-owned string before stop() publishes the join
    // through running_'s release/acquire transition.
    if (running_.load(std::memory_order_acquire) || index >= tracks_.size()) {
        return {};
    }
    return tracks_[index]->sha256;
}

} // namespace dave::engine
