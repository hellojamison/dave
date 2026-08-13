// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/util/SpscQueue.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

using dave::engine::SpscQueue;

TEST_CASE("an SPSC queue wraps without reordering records", "[spscqueue]") {
    SpscQueue<uint64_t> queue;
    queue.reset(4);

    REQUIRE(queue.push(10));
    REQUIRE(queue.push(11));
    REQUIRE(queue.push(12));

    uint64_t value = 0;
    REQUIRE(queue.pop(value));
    CHECK(value == 10);
    REQUIRE(queue.pop(value));
    CHECK(value == 11);

    REQUIRE(queue.push(13));
    REQUIRE(queue.push(14));
    REQUIRE(queue.push(15));

    for (uint64_t expected : {12ull, 13ull, 14ull, 15ull}) {
        REQUIRE(queue.pop(value));
        CHECK(value == expected);
    }
    CHECK_FALSE(queue.pop(value));
}

TEST_CASE("an SPSC queue uses its full capacity and refuses overflow",
          "[spscqueue]") {
    SpscQueue<uint32_t> queue;
    queue.reset(3);
    CHECK(queue.capacity() == 4); // power-of-two storage

    REQUIRE(queue.push(1));
    REQUIRE(queue.push(2));
    REQUIRE(queue.push(3));
    REQUIRE(queue.push(4));
    CHECK(queue.size() == 4);
    CHECK_FALSE(queue.push(5));

    uint32_t value = 0;
    REQUIRE(queue.peek(value));
    CHECK(value == 1);
    CHECK(queue.size() == 4); // peek does not consume
    REQUIRE(queue.pop(value));
    CHECK(value == 1);
    REQUIRE(queue.push(5));
}

TEST_CASE("an SPSC queue transfers records between real threads",
          "[spscqueue]") {
    constexpr uint64_t kRecords = 500000;
    SpscQueue<uint64_t> queue;
    queue.reset(1024);

    std::atomic<bool> producerDone{false};
    std::atomic<bool> outOfOrder{false};

    std::thread producer([&] {
        for (uint64_t value = 0; value < kRecords; ++value) {
            while (!queue.push(value)) std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        uint64_t expected = 0;
        while (expected < kRecords) {
            uint64_t value = 0;
            if (!queue.pop(value)) {
                if (producerDone.load(std::memory_order_acquire) &&
                    queue.size() == 0) {
                    break;
                }
                std::this_thread::yield();
                continue;
            }
            if (value != expected) outOfOrder.store(true);
            ++expected;
        }
        if (expected != kRecords) outOfOrder.store(true);
    });

    producer.join();
    consumer.join();
    CHECK_FALSE(outOfOrder.load());
    CHECK(queue.size() == 0);
}
