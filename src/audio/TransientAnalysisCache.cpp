// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/TransientAnalysisCache.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace dave::audio {

TransientAnalysisCache::TransientAnalysisCache(size_t maximumBytes)
    : maximumBytes_(maximumBytes),
      worker_([this] { workerMain(); }) {}

TransientAnalysisCache::~TransientAnalysisCache() {
    generation_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void TransientAnalysisCache::request(std::string assetId,
                                     DecodedAudioAssetPtr asset) {
    if (assetId.empty()) return;
    std::lock_guard lock(mutex_);
    auto found = entries_.find(assetId);
    if (found != entries_.end() &&
        (found->second.status == Status::Pending ||
         found->second.status == Status::Ready)) {
        found->second.lastUsed = ++useClock_;
        return;
    }
    Entry& entry = entries_[assetId];
    entry.lastUsed = ++useClock_;
    entry.generation = generation_.load(std::memory_order_relaxed);
    if (!asset || asset->channels.empty()) {
        entry.status = Status::Failed;
        return;
    }
    entry.status = Status::Pending;
    jobs_.push_back(Job{std::move(assetId), std::move(asset), entry.generation});
    wake_.notify_one();
}

TransientAnalysisCache::Snapshot TransientAnalysisCache::snapshot(
    const std::string& assetId) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(assetId);
    if (found == entries_.end()) return {};
    found->second.lastUsed = ++useClock_;
    return Snapshot{found->second.status, found->second.candidates};
}

void TransientAnalysisCache::cancelAll() {
    generation_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    jobs_.clear();
    entries_.clear();
    retainedBytes_ = 0;
}

size_t TransientAnalysisCache::retainedBytes() const {
    std::lock_guard lock(mutex_);
    return retainedBytes_;
}

size_t TransientAnalysisCache::pendingCount() const {
    std::lock_guard lock(mutex_);
    return static_cast<size_t>(std::count_if(
        entries_.begin(), entries_.end(), [](const auto& pair) {
            return pair.second.status == Status::Pending;
        }));
}

void TransientAnalysisCache::trimLocked(size_t incomingBytes,
                                        const std::string& protectedId) {
    while (retainedBytes_ + incomingBytes > maximumBytes_) {
        auto victim = entries_.end();
        uint64_t oldest = std::numeric_limits<uint64_t>::max();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->first == protectedId || it->second.status != Status::Ready) continue;
            if (it->second.lastUsed < oldest) {
                oldest = it->second.lastUsed;
                victim = it;
            }
        }
        if (victim == entries_.end()) break;
        retainedBytes_ -= victim->second.bytes;
        entries_.erase(victim);
    }
}

void TransientAnalysisCache::workerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        auto candidates = TransientDetector::analyze(
            *job.asset, &generation_, job.generation);
        const bool invalidated = generation_.load(std::memory_order_relaxed) !=
            job.generation;

        std::lock_guard lock(mutex_);
        if (stopping_ || invalidated) continue;
        auto found = entries_.find(job.assetId);
        if (found == entries_.end() ||
            found->second.generation != job.generation ||
            found->second.status != Status::Pending) {
            continue;
        }
        auto immutable = std::make_shared<const std::vector<TransientCandidate>>(
            std::move(candidates));
        const size_t bytes = immutable->size() * sizeof(TransientCandidate);
        if (bytes > maximumBytes_) {
            found->second.status = Status::Failed;
            found->second.candidates.reset();
            found->second.bytes = 0;
            found->second.lastUsed = ++useClock_;
            continue;
        }
        trimLocked(bytes, job.assetId);
        found = entries_.find(job.assetId);
        if (found == entries_.end()) continue;
        found->second.status = Status::Ready;
        found->second.candidates = std::move(immutable);
        found->second.bytes = bytes;
        found->second.lastUsed = ++useClock_;
        retainedBytes_ += bytes;
    }
}

} // namespace dave::audio
