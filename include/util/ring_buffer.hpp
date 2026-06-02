#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace nuedc {

/// Lock-free SPSC (single-producer, single-consumer) ring buffer.
/// Capacity N must be a power of 2.
///
/// Producer (one thread): push()
/// Consumer (one thread): pop(), empty()
template <typename T, size_t N>
    requires ((N & (N - 1)) == 0)
struct RingBuffer {
    std::array<T, N> buf_{};
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_{0};

    [[nodiscard]] bool push(const T& item) {
        size_t w = write_.load(std::memory_order_relaxed);
        size_t r = read_.load(std::memory_order_acquire);
        if (w - r >= N) return false;
        buf_[w & (N - 1)] = item;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(T& item) {
        size_t r = read_.load(std::memory_order_relaxed);
        size_t w = write_.load(std::memory_order_acquire);
        if (r == w) return false;
        item = buf_[r & (N - 1)];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const {
        return read_.load(std::memory_order_relaxed)
            == write_.load(std::memory_order_relaxed);
    }
};

}  // namespace nuedc
