// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/DecodedAudioAsset.h"
#include "audio/TransientDetector.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace dave::audio {

class TransientAnalysisCache {
public:
    enum class Status { Missing, Pending, Ready, Failed };

    struct Snapshot {
        Status status = Status::Missing;
        std::shared_ptr<const std::vector<TransientCandidate>> candidates;
    };

    explicit TransientAnalysisCache(size_t maximumBytes = 32u * 1024u * 1024u);
    ~TransientAnalysisCache();

    TransientAnalysisCache(const TransientAnalysisCache&) = delete;
    TransientAnalysisCache& operator=(const TransientAnalysisCache&) = delete;

    // Idempotent for a pending or completed asset id. Empty ids and null
    // decodes are recorded as failures instead of reaching the worker.
    void request(std::string assetId, DecodedAudioAssetPtr asset);
    Snapshot snapshot(const std::string& assetId);

    // Invalidates queued/current work and all derived results. The worker
    // remains available for the next project.
    void cancelAll();

    size_t retainedBytes() const;
    size_t pendingCount() const;

private:
    struct Entry {
        Status status = Status::Missing;
        std::shared_ptr<const std::vector<TransientCandidate>> candidates;
        size_t bytes = 0;
        uint64_t lastUsed = 0;
        uint64_t generation = 0;
    };

    struct Job {
        std::string assetId;
        DecodedAudioAssetPtr asset;
        uint64_t generation = 0;
    };

    void workerMain();
    void trimLocked(size_t incomingBytes, const std::string& protectedId);

    const size_t maximumBytes_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<Job> jobs_;
    std::thread worker_;
    std::atomic<uint64_t> generation_{1};
    bool stopping_ = false;
    size_t retainedBytes_ = 0;
    uint64_t useClock_ = 0;
};

} // namespace dave::audio
