// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace dave::engine {

// A bounded single-producer / single-consumer queue.
//
// reset() is UI-thread setup. push() is producer-only; peek()/pop() are
// consumer-only. The hot path is allocation-free and lock-free.
template <typename T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscQueue records must be trivially copyable");

public:
    void reset(size_t requestedCapacity) {
        if (requestedCapacity == 0) {
            data_.clear();
            capacity_ = 0;
            mask_ = 0;
        } else {
            size_t capacity = 1;
            while (capacity < requestedCapacity) capacity <<= 1;
            data_.assign(capacity, T{});
            capacity_ = capacity;
            mask_ = capacity - 1;
        }
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const { return capacity_; }

    bool push(const T& value) {
        const uint64_t w = writePos_.load(std::memory_order_relaxed);
        const uint64_t r = readPos_.load(std::memory_order_acquire);
        if (capacity_ == 0 || w - r >= capacity_) return false;
        data_[static_cast<size_t>(w) & mask_] = value;
        writePos_.store(w + 1, std::memory_order_release);
        return true;
    }

    bool peek(T& value) const {
        const uint64_t r = readPos_.load(std::memory_order_relaxed);
        const uint64_t w = writePos_.load(std::memory_order_acquire);
        if (r == w) return false;
        value = data_[static_cast<size_t>(r) & mask_];
        return true;
    }

    bool pop(T& value) {
        const uint64_t r = readPos_.load(std::memory_order_relaxed);
        const uint64_t w = writePos_.load(std::memory_order_acquire);
        if (r == w) return false;
        value = data_[static_cast<size_t>(r) & mask_];
        readPos_.store(r + 1, std::memory_order_release);
        return true;
    }

    bool pop() {
        T ignored{};
        return pop(ignored);
    }

    size_t size() const {
        const uint64_t w = writePos_.load(std::memory_order_acquire);
        const uint64_t r = readPos_.load(std::memory_order_acquire);
        // Separate atomics can be observed at slightly different moments by
        // a diagnostic caller; never turn that transient into an underflow.
        return static_cast<size_t>(w >= r ? w - r : 0);
    }

private:
    std::vector<T> data_;
    size_t capacity_ = 0;
    size_t mask_ = 0;

    alignas(64) std::atomic<uint64_t> writePos_{0};
    alignas(64) std::atomic<uint64_t> readPos_{0};
};

} // namespace dave::engine
