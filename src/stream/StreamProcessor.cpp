#include "stream/StreamProcessor.hpp"
#include "common/Logger.hpp"

#include <spdlog/spdlog.h>

namespace cortex::stream {

void StreamProcessor::start(EventCallback cb) {
    if (running_.load(std::memory_order_acquire)) return;

    stop_flag_.store(false, std::memory_order_release);

    // jthread captures [this, cb] by value/reference — cb may be nullptr
    thread_ = std::jthread([this, cb = std::move(cb)]() mutable {
        run(std::move(cb));
    });
}

void StreamProcessor::stop() noexcept {
    stop_flag_.store(true, std::memory_order_release);
    // jthread joins automatically on destruction; we just signal here.
    // Callers can check running() == false to confirm shutdown.
    if (thread_.joinable()) thread_.join();
}

StreamProcessor::Metrics StreamProcessor::metrics() const noexcept {
    double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_time_).count();

    return {
        .events_processed = events_processed_,
        .events_per_sec   = secs > 0.0
                          ? static_cast<double>(events_processed_) / secs
                          : 0.0,
        .idle_spins       = idle_spins_,
    };
}

// ── Consumer loop (runs on dedicated jthread) ──────────────────────────────
void StreamProcessor::run(EventCallback cb) noexcept {
    auto log = cortex::get_logger("stream_proc");
    running_.store(true, std::memory_order_release);
    start_time_ = std::chrono::steady_clock::now();

    // Exponential backoff constants (avoids burning CPU when buffer is idle)
    static constexpr int kBackoffMax  = 64;   // max spin multiplier
    static constexpr int kYieldsBeforeSleep = 32;
    int backoff = 1;

    while (true) {
        auto ev = buffer_.try_pop();

        if (ev) {
            // ── Hot path ────────────────────────────────────────────────
            accumulator_.process(*ev);
            if (cb) cb(*ev);
            ++events_processed_;
            backoff = 1;  // reset backoff on successful pop
        } else {
            // ── Idle path ───────────────────────────────────────────────
            ++idle_spins_;

            // Check stop condition only when buffer is empty —
            // ensures all queued events are drained before exiting.
            if (stop_flag_.load(std::memory_order_acquire)) break;

            // Graduated backoff: yield first, then nanosleep
            if (backoff < kYieldsBeforeSleep) {
                for (int i = 0; i < backoff; ++i) {
#if defined(__aarch64__)
                    __asm__ volatile("yield" ::: "memory");
#else
                    __asm__ volatile("pause" ::: "memory");
#endif
                }
                backoff = std::min(backoff * 2, kBackoffMax);
            } else {
                // Sleep 50µs to avoid burning CPU when truly idle
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }

    running_.store(false, std::memory_order_release);

    auto m = metrics();
    log->info("StreamProcessor stopped. events={} throughput={:.0f} ev/s idle_spins={}",
              m.events_processed, m.events_per_sec, m.idle_spins);
}

} // namespace cortex::stream
