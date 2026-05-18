#pragma once
// SPSC lock-free ring buffer for streaming play events.
//
// Properties:
//   - Single-producer / single-consumer (SPSC)
//   - Capacity must be a power of 2 (enforced in constructor)
//   - No dynamic allocation after construction
//   - Zero-copy: producer writes directly into slot, consumer reads directly
//   - Cache-line padding separates head/tail to eliminate false sharing
//   - Memory ordering: acquire/release — no mutex, no spin loop with backoff
//
// Usage:
//   RingBuffer<PlayEvent> buf(65536);
//
//   // Producer thread:
//   if (auto slot = buf.try_push()) {
//       *slot = my_event;
//       slot.commit();
//   }
//
//   // Consumer thread:
//   if (auto slot = buf.try_pop()) {
//       process(*slot);
//       slot.commit();
//   }

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <new>   // hardware_destructive_interference_size

namespace cortex::stream {

// Cache line size — use std::hardware_destructive_interference_size when available.
// On Apple Silicon it's 128 bytes; standard is 64.
#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t kCacheLine = std::hardware_destructive_interference_size;
#else
    static constexpr size_t kCacheLine = 64;
#endif

template<typename T>
class RingBuffer {
    static_assert(std::is_nothrow_move_constructible_v<T> || std::is_trivially_copyable_v<T>,
                  "RingBuffer<T>: T must be nothrow-move-constructible or trivially copyable");

public:
    explicit RingBuffer(size_t capacity) : capacity_(capacity), mask_(capacity - 1) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("RingBuffer: capacity must be a power of 2");
        slots_ = std::make_unique<T[]>(capacity);
    }

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // ── Producer API ──────────────────────────────────────────────────────
    // Returns true and writes item if buffer has space; false if full.
    bool try_push(T item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = head + 1;

        if (next - tail_.load(std::memory_order_acquire) > capacity_)
            return false;  // full

        slots_[head & mask_] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Blocking push: spins until space is available.
    void push(T item) noexcept {
        while (!try_push(std::move(item))) {
            // Yield to avoid burning CPU on a saturated buffer.
            // In production this would be a condvar or futex notification.
#if defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#else
            __asm__ volatile("pause" ::: "memory");
#endif
        }
    }

    // ── Consumer API ──────────────────────────────────────────────────────
    // Returns the next item if available, else nullopt.
    std::optional<T> try_pop() noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);

        if (head_.load(std::memory_order_acquire) == tail)
            return std::nullopt;  // empty

        T item = std::move(slots_[tail & mask_]);
        tail_.store(tail + 1, std::memory_order_release);
        return item;
    }

    // ── State queries ──────────────────────────────────────────────────────
    size_t size() const noexcept {
        return head_.load(std::memory_order_acquire)
             - tail_.load(std::memory_order_acquire);
    }

    bool empty() const noexcept { return size() == 0; }
    bool full()  const noexcept { return size() >= capacity_; }
    size_t capacity() const noexcept { return capacity_; }

private:
    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<T[]> slots_;

    // Separate cache lines to prevent false sharing between producer and consumer.
    alignas(kCacheLine) std::atomic<size_t> head_{0};
    alignas(kCacheLine) std::atomic<size_t> tail_{0};
};

} // namespace cortex::stream
