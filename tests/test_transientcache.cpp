// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/TransientAnalysisCache.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace dave;

namespace {

audio::DecodedAudioAssetPtr assetWithImpulse(size_t frames = 4800) {
    auto asset = std::make_shared<audio::DecodedAudioAsset>();
    asset->sampleRate = 48000.0;
    asset->channels = {std::vector<float>(frames, 0.0f)};
    asset->channels[0][std::min<size_t>(100, frames - 1)] = 1.0f;
    return asset;
}

audio::TransientAnalysisCache::Snapshot waitForResult(
    audio::TransientAnalysisCache& cache, const char* id) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    for (;;) {
        auto snapshot = cache.snapshot(id);
        if (snapshot.status != audio::TransientAnalysisCache::Status::Pending) {
            return snapshot;
        }
        if (std::chrono::steady_clock::now() >= deadline) return snapshot;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

TEST_CASE("transient cache deduplicates work and publishes immutable results",
          "[transient][cache]") {
    audio::TransientAnalysisCache cache;
    auto asset = assetWithImpulse();
    cache.request("asset", asset);
    cache.request("asset", asset);
    const auto first = waitForResult(cache, "asset");
    REQUIRE(first.status == audio::TransientAnalysisCache::Status::Ready);
    REQUIRE(first.candidates);
    REQUIRE_FALSE(first.candidates->empty());

    cache.request("asset", asset);
    const auto second = cache.snapshot("asset");
    REQUIRE(second.status == audio::TransientAnalysisCache::Status::Ready);
    REQUIRE(second.candidates == first.candidates);
}

TEST_CASE("transient cache cancellation invalidates stale publication",
          "[transient][cache]") {
    audio::TransientAnalysisCache cache;
    cache.request("long", assetWithImpulse(2'000'000));
    cache.cancelAll();
    REQUIRE(cache.snapshot("long").status ==
            audio::TransientAnalysisCache::Status::Missing);
    REQUIRE(cache.retainedBytes() == 0);

    cache.request("fresh", assetWithImpulse());
    REQUIRE(waitForResult(cache, "fresh").status ==
            audio::TransientAnalysisCache::Status::Ready);
}

TEST_CASE("transient cache rejects invalid inputs and oversized results",
          "[transient][cache]") {
    audio::TransientAnalysisCache cache(0);
    cache.request("missing", nullptr);
    REQUIRE(cache.snapshot("missing").status ==
            audio::TransientAnalysisCache::Status::Failed);

    cache.request("asset", assetWithImpulse());
    REQUIRE(waitForResult(cache, "asset").status ==
            audio::TransientAnalysisCache::Status::Failed);
    REQUIRE(cache.retainedBytes() == 0);
}
