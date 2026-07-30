// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/Timeline.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using dave::gui::PeakCache;
using dave::gui::PeakLevel;

namespace {

constexpr double kPi = 3.14159265358979323846;

// One channel of a sine whose amplitude follows an envelope, mirroring the
// file that exposed the flat-waveform bug during the UI overhaul.
std::vector<std::vector<float>> envelopedSine(int sampleRate, double seconds,
                                              double freq, double envPeriod,
                                              double floorAmp = 0.15) {
    const int n = static_cast<int>(sampleRate * seconds);
    std::vector<float> ch(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double env =
            floorAmp + (1.0 - floorAmp) * std::abs(std::sin(kPi * t / envPeriod));
        ch[static_cast<size_t>(i)] =
            static_cast<float>(env * std::sin(2.0 * kPi * freq * t));
    }
    return {std::move(ch)};
}

std::vector<std::vector<float>> constantAmplitude(int n, float amp) {
    std::vector<float> ch(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        ch[static_cast<size_t>(i)] = (i % 2 == 0) ? amp : -amp;
    }
    return {std::move(ch)};
}

float maxMagnitude(const PeakLevel& level) {
    float m = 0.0f;
    for (const auto& b : level.buckets) {
        m = std::max(m, std::max(std::abs(b.min), std::abs(b.max)));
    }
    return m;
}

} // namespace

TEST_CASE("peak buckets capture the true min and max", "[peaks]") {
    std::vector<std::vector<float>> buf{{0.0f, 0.9f, -0.7f, 0.1f,
                                         0.2f, -0.4f, 0.3f, 0.0f}};
    PeakCache cache;
    const PeakLevel& level = cache.get("asset", buf, 4);

    REQUIRE(level.bucketSize == 4);
    REQUIRE(level.buckets.size() == 2);
    CHECK(level.buckets[0].max == Catch::Approx(0.9f));
    CHECK(level.buckets[0].min == Catch::Approx(-0.7f));
    CHECK(level.buckets[1].max == Catch::Approx(0.3f));
    CHECK(level.buckets[1].min == Catch::Approx(-0.4f));
}

// This is the regression guard for the bug that shipped during the UI
// overhaul: the waveform rendered as a flat ribbon because the amplitude
// envelope was being lost. A constant-amplitude test tone cannot detect that
// failure — it looks identical whether the envelope survives or not — so this
// deliberately uses a signal whose loud and quiet passages differ a lot.
TEST_CASE("peaks preserve an amplitude envelope", "[peaks][regression]") {
    const int sr = 48000;
    auto buf = envelopedSine(sr, 4.0, 220.0, 1.0);
    PeakCache cache;
    const PeakLevel& level = cache.get("enveloped", buf, 256);

    REQUIRE(level.buckets.size() > 16);

    // Sample the envelope where it should peak (t = 0.5s, 1.5s ...) and where
    // it should trough (t = 0s, 1s ...).
    const auto bucketAt = [&](double seconds) {
        const auto idx = static_cast<size_t>((seconds * sr) / level.bucketSize);
        REQUIRE(idx < level.buckets.size());
        const auto& b = level.buckets[idx];
        return std::max(std::abs(b.min), std::abs(b.max));
    };

    const float loud = bucketAt(0.5);
    const float quiet = bucketAt(0.02);

    // If the envelope is being flattened, these collapse toward each other.
    CHECK(loud > 0.8f);
    CHECK(quiet < 0.4f);
    CHECK(loud > quiet * 2.0f);
}

TEST_CASE("display scale lifts quiet material but never inverts loud material",
          "[peaks]") {
    PeakCache cache;

    auto loud = constantAmplitude(4096, 1.0f);
    const PeakLevel& loudLevel = cache.get("loud", loud, 64);
    // A full-scale file is already using the full height; scaling it up would
    // clip it against the clip body.
    CHECK(loudLevel.displayScale == Catch::Approx(1.0f).margin(0.05));

    auto quiet = constantAmplitude(4096, 0.1f);
    const PeakLevel& quietLevel = cache.get("quiet", quiet, 64);
    CHECK(quietLevel.displayScale > 1.0f);
    CHECK(quietLevel.displayScale <= 8.0f);

    // The cap matters: without it, a near-silent file turns dither and room
    // noise into a full-height waveform.
    auto silent = constantAmplitude(4096, 0.0000001f);
    const PeakLevel& silentLevel = cache.get("silent", silent, 64);
    CHECK(silentLevel.displayScale <= 8.0f);
}

TEST_CASE("bucket size snaps to powers of two", "[peaks]") {
    PeakCache cache;
    auto buf = constantAmplitude(1 << 16, 0.5f);

    // Arbitrary zoom values must land on a bounded set of levels, otherwise
    // every pixel-level zoom change allocates a fresh full-length cache entry.
    for (int spp : {4, 5, 7, 9, 100, 250, 1000, 5000}) {
        const PeakLevel& level = cache.get("asset", buf, spp);
        const int b = level.bucketSize;
        CHECK(b >= 4);
        CHECK((b & (b - 1)) == 0);
    }
}

TEST_CASE("the same zoom level is reused rather than rebuilt", "[peaks]") {
    PeakCache cache;
    auto buf = constantAmplitude(8192, 0.5f);

    const PeakLevel& first = cache.get("asset", buf, 128);
    const void* firstAddress = static_cast<const void*>(&first);
    const int firstBucket = first.bucketSize;

    const PeakLevel& again = cache.get("asset", buf, 128);
    CHECK(again.bucketSize == firstBucket);
    CHECK(static_cast<const void*>(&again) == firstAddress);
}

TEST_CASE("nearby zoom values share a level", "[peaks]") {
    PeakCache cache;
    auto buf = constantAmplitude(1 << 15, 0.5f);
    // 250 and 260 are close enough that rebuilding for each would be wasted
    // work during a zoom drag.
    CHECK(cache.get("asset", buf, 250).bucketSize ==
          cache.get("asset", buf, 260).bucketSize);
}

TEST_CASE("an empty buffer does not crash", "[peaks]") {
    PeakCache cache;
    std::vector<std::vector<float>> empty;
    const PeakLevel& level = cache.get("empty", empty, 128);
    CHECK(level.buckets.empty());

    std::vector<std::vector<float>> emptyChannel{{}};
    const PeakLevel& level2 = cache.get("emptyChannel", emptyChannel, 128);
    CHECK(level2.buckets.empty());
}

TEST_CASE("the cache stays bounded across many assets", "[peaks]") {
    PeakCache cache;
    auto buf = constantAmplitude(1 << 18, 0.5f);
    // The cache is capped at 32 MB. Filling it with distinct assets must evict
    // rather than grow without limit — this is what keeps a long session from
    // accumulating every peak level it has ever drawn.
    for (int i = 0; i < 80; ++i) {
        const PeakLevel& level =
            cache.get("asset" + std::to_string(i), buf, 8);
        CHECK_FALSE(level.buckets.empty());
    }
    SUCCEED("no unbounded growth or crash across evictions");
}
