// SPDX-License-Identifier: GPL-3.0-or-later
//
// The RT→disk hop. This is the one piece of the recorder that cannot be
// debugged after the fact: if it drops or reorders frames under load, the
// symptom is a take that sounds fine in the room and wrong on playback, hours
// later. So it gets a real two-thread stress test, not just index arithmetic.
#include "engine/util/SpscRing.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <random>
#include <thread>
#include <vector>

using dave::engine::SpscRing;

namespace {

// Planar helper: write `frames` of a known ramp starting at `first`.
bool writeRamp(SpscRing& ring, int channels, size_t frames, double first) {
    std::vector<std::vector<float>> planar(
        channels, std::vector<float>(frames, 0.0f));
    std::vector<const float*> ptrs(channels);
    for (int c = 0; c < channels; ++c) {
        for (size_t i = 0; i < frames; ++i) {
            // Channel offset makes an interleave slip visible: a shifted read
            // lands on a value that belongs to the other channel.
            planar[c][i] = static_cast<float>(first + static_cast<double>(i)) +
                           static_cast<float>(c) * 0.5f;
        }
        ptrs[c] = planar[c].data();
    }
    return ring.write(ptrs.data(), channels, frames);
}

} // namespace

TEST_CASE("a ring round-trips frames in order", "[ring]") {
    SpscRing ring;
    ring.reset(64, 2);
    CHECK(ring.capacityFrames() == 64);
    CHECK(ring.availableFrames() == 0);

    REQUIRE(writeRamp(ring, 2, 8, 0.0));
    CHECK(ring.availableFrames() == 8);

    std::vector<float> out(8 * 2, -1.0f);
    CHECK(ring.read(out.data(), 8) == 8);
    CHECK(ring.availableFrames() == 0);
    for (size_t i = 0; i < 8; ++i) {
        CHECK(out[i * 2 + 0] == static_cast<float>(i));
        CHECK(out[i * 2 + 1] == static_cast<float>(i) + 0.5f);
    }
}

TEST_CASE("capacity rounds up to a power of two", "[ring]") {
    // Wrapping is a mask, so a non-power-of-two capacity would corrupt indices
    // rather than merely waste space.
    SpscRing ring;
    ring.reset(100, 1);
    CHECK(ring.capacityFrames() == 128);
    ring.reset(128, 1);
    CHECK(ring.capacityFrames() == 128);
}

TEST_CASE("a ring wraps without disturbing frame order", "[ring]") {
    SpscRing ring;
    ring.reset(16, 2);

    double next = 0.0;
    std::vector<float> out(16 * 2, 0.0f);
    // Ten passes of 12 frames through a 16-frame ring: every pass wraps.
    for (int pass = 0; pass < 10; ++pass) {
        REQUIRE(writeRamp(ring, 2, 12, next));
        REQUIRE(ring.read(out.data(), 12) == 12);
        for (size_t i = 0; i < 12; ++i) {
            REQUIRE(out[i * 2 + 0] == static_cast<float>(next + i));
            REQUIRE(out[i * 2 + 1] == static_cast<float>(next + i) + 0.5f);
        }
        next += 12.0;
    }
}

TEST_CASE("a block that does not fit is dropped whole, not partially",
          "[ring]") {
    // A partial write leaves the interleave out of phase and every sample
    // after it lands on the wrong channel — silent, permanent corruption.
    // Dropping the block keeps the stream coherent and countable.
    SpscRing ring;
    ring.reset(16, 2);

    REQUIRE(writeRamp(ring, 2, 12, 0.0));
    CHECK(ring.availableFrames() == 12);

    // Only 4 frames of room left; an 8-frame block cannot fit.
    CHECK_FALSE(writeRamp(ring, 2, 8, 100.0));
    CHECK(ring.availableFrames() == 12);      // nothing was written
    CHECK(ring.overrunBlocks() == 1);

    // The dropped frames are still counted, which is what lets the consumer
    // pad the gap and keep the file aligned.
    CHECK(ring.framesProduced() == 20);
    CHECK(ring.droppedFrames() == 8);

    const auto view = ring.consumerView();
    REQUIRE(view.hasGap);
    CHECK(view.gap.streamPos == 12);
    CHECK(view.gap.frames == 8);

    // And the frames already in the ring are untouched by the failed write.
    std::vector<float> out(12 * 2, -1.0f);
    REQUIRE(ring.read(out.data(), 12) == 12);
    for (size_t i = 0; i < 12; ++i) {
        REQUIRE(out[i * 2 + 0] == static_cast<float>(i));
    }
}

TEST_CASE("a ring that was never reset refuses and counts audio", "[ring]") {
    SpscRing ring;
    const float sample = 0.5f;
    const float* channels[] = {&sample};

    CHECK_FALSE(ring.write(channels, 1, 1));
    CHECK(ring.droppedFrames() == 1);
    CHECK(ring.overrunBlocks() == 1);
    CHECK(ring.unlocatedDropBlocks() == 1);
}

TEST_CASE("a gap record pins a drop to its exact ideal stream position",
          "[ring]") {
    SpscRing ring;
    ring.reset(8, 1);
    REQUIRE(writeRamp(ring, 1, 8, 0.0));
    CHECK_FALSE(writeRamp(ring, 1, 3, 8.0));

    std::vector<float> first(8, -1.0f);
    auto view = ring.consumerView();
    REQUIRE(view.hasGap);
    CHECK(view.gap.streamPos == 8);
    CHECK(view.gap.frames == 3);
    REQUIRE(ring.read(first.data(), view.gap.streamPos, view) == 8);
    REQUIRE(ring.popGap());

    REQUIRE(writeRamp(ring, 1, 4, 11.0));
    std::vector<float> after(4, -1.0f);
    REQUIRE(ring.read(after.data(), after.size()) == after.size());
    for (size_t i = 0; i < after.size(); ++i) {
        CHECK(after[i] == static_cast<float>(11 + i));
    }
}

TEST_CASE("gap queue overflow is bounded and the next gap restores alignment",
          "[ring]") {
    SpscRing ring;
    ring.reset(2, 1, 1); // one record makes overflow deterministic

    REQUIRE(writeRamp(ring, 1, 2, 0.0));
    CHECK_FALSE(writeRamp(ring, 1, 2, 2.0)); // located at 2
    CHECK_FALSE(writeRamp(ring, 1, 3, 4.0)); // queue full, unlocated
    CHECK(ring.unlocatedDropBlocks() == 1);

    uint64_t outputPos = 0;
    std::vector<float> audio(2, -1.0f);
    auto view = ring.consumerView();
    REQUIRE(view.hasGap);
    REQUIRE(ring.read(audio.data(), 2, view) == 2);
    outputPos += 2;
    REQUIRE(outputPos == view.gap.streamPos);
    outputPos += view.gap.frames; // conceptual silence write
    REQUIRE(ring.popGap());
    CHECK(outputPos == 4);

    // Samples after the unlocated gap can be shifted only inside the degraded
    // region. The next publishable drop coalesces the old loss ahead of its
    // own exact tail and restores alignment after it.
    REQUIRE(writeRamp(ring, 1, 2, 7.0));
    CHECK_FALSE(writeRamp(ring, 1, 2, 9.0));
    view = ring.consumerView();
    REQUIRE(view.hasGap);
    CHECK(view.gap.streamPos == 6); // 9 minus the 3 unlocated frames
    CHECK(view.gap.frames == 5);    // 3 old + 2 current
    REQUIRE(ring.read(audio.data(), 2, view) == 2);
    outputPos += 2;
    REQUIRE(outputPos == view.gap.streamPos);
    outputPos += view.gap.frames;
    REQUIRE(ring.popGap());
    CHECK(outputPos == 11);

    REQUIRE(writeRamp(ring, 1, 2, 11.0));
    REQUIRE(ring.read(audio.data(), 2) == 2);
    CHECK(audio[0] == 11.0f);
    CHECK(audio[1] == 12.0f);
    outputPos += 2;
    CHECK(outputPos == ring.framesProduced());
}

TEST_CASE("exactly filling the ring is not an overrun", "[ring]") {
    // The classic off-by-one: monotonic counters make full and empty
    // distinguishable, so the last frame of capacity is usable.
    SpscRing ring;
    ring.reset(16, 1);
    CHECK(writeRamp(ring, 1, 16, 0.0));
    CHECK(ring.availableFrames() == 16);
    CHECK(ring.overrunBlocks() == 0);
    CHECK(ring.droppedFrames() == 0);

    // One more frame has nowhere to go.
    CHECK_FALSE(writeRamp(ring, 1, 1, 99.0));
    CHECK(ring.overrunBlocks() == 1);
}

TEST_CASE("reading takes only what is there", "[ring]") {
    SpscRing ring;
    ring.reset(32, 1);
    REQUIRE(writeRamp(ring, 1, 5, 0.0));
    std::vector<float> out(32, -1.0f);
    CHECK(ring.read(out.data(), 32) == 5);
    CHECK(out[5] == -1.0f);        // untouched past what was available
    CHECK(ring.read(out.data(), 32) == 0);
}

TEST_CASE("a producer and a consumer on real threads lose nothing",
          "[ring]") {
    // The load-bearing test. A producer writes a continuous ramp in randomised
    // block sizes; the consumer drains it with randomised stalls, which forces
    // genuine overruns. Every frame the consumer sees must be the next one in
    // the ramp — and the frames it never sees must be exactly the ones the
    // ring counted as dropped.
    constexpr int kChannels = 2;
    constexpr size_t kTotalFrames = 400000;
    SpscRing ring;
    ring.reset(2048, kChannels, 65536);

    std::atomic<bool> producerDone{false};
    std::atomic<uint64_t> reconstructedFrames{0};
    std::atomic<bool> positionViolation{false};
    std::atomic<bool> channelViolation{false};

    std::thread producer([&] {
        std::mt19937 rng(1234);
        std::uniform_int_distribution<size_t> blockDist(32, 512);
        size_t written = 0;
        while (written < kTotalFrames) {
            const size_t n = std::min(blockDist(rng), kTotalFrames - written);
            writeRamp(ring, kChannels, n, static_cast<double>(written));
            written += n;
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::mt19937 rng(5678);
        std::uniform_int_distribution<int> stallDist(0, 400);
        std::vector<float> out(4096 * kChannels);
        uint64_t outputPos = 0;
        while (true) {
            const auto view = ring.consumerView();
            if (view.hasGap && outputPos == view.gap.streamPos) {
                outputPos += view.gap.frames; // exact-position silence
                if (!ring.popGap()) positionViolation.store(true);
                continue;
            }

            size_t want = 4096;
            if (view.hasGap) {
                if (outputPos > view.gap.streamPos) {
                    positionViolation.store(true);
                    break;
                }
                want = std::min<uint64_t>(want,
                                          view.gap.streamPos - outputPos);
            }
            const size_t got = ring.read(out.data(), want, view);
            for (size_t i = 0; i < got; ++i) {
                const float value = out[i * kChannels + 0];
                // The producer's ramp value is its ideal stream position.
                if (value != static_cast<float>(outputPos + i)) {
                    positionViolation.store(true);
                }
                // Channel 1 is always its channel-0 partner plus 0.5. If the
                // interleave slipped, this is where it shows.
                if (out[i * kChannels + 1] != out[i * kChannels + 0] + 0.5f) {
                    channelViolation.store(true);
                }
            }
            outputPos += got;
            if (got == 0) {
                if (producerDone.load(std::memory_order_acquire) &&
                    ring.availableFrames() == 0) {
                    // Re-observe the queue after acquiring producerDone; the
                    // earlier view may legitimately predate the final drop.
                    if (!ring.consumerView().hasGap) break;
                }
                std::this_thread::yield();
            }
            // Randomised stalls: without these the consumer keeps up and the
            // overrun path is never exercised.
            const int stall = stallDist(rng);
            if (stall > 350) {
                std::this_thread::sleep_for(std::chrono::microseconds(stall));
            }
        }
        reconstructedFrames.store(outputPos, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    CHECK_FALSE(positionViolation.load());
    CHECK_FALSE(channelViolation.load());

    // Every frame is accounted for: what the consumer read, plus what the ring
    // says it dropped, equals what the producer was asked to write. That
    // identity is what lets the writer pad the gap and keep the take aligned.
    CHECK(ring.framesProduced() == kTotalFrames);
    CHECK(reconstructedFrames.load() == kTotalFrames);
    CHECK(ring.unlocatedDropBlocks() == 0);

    // And the run genuinely exercised the overrun path. Without this the test
    // silently degrades into a happy-path check the day the machine gets fast
    // enough for the consumer to keep up.
    INFO("dropped " << ring.droppedFrames() << " of " << kTotalFrames);
    CHECK(ring.overrunBlocks() > 0);
}
