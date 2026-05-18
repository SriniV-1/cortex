#pragma once
// StreamProcessor — drains the ring buffer and dispatches events to the
// StatAccumulator. Runs in a dedicated jthread; stops cleanly on shutdown.

#include "RingBuffer.hpp"
#include "StreamEvent.hpp"
#include "StatAccumulator.hpp"

#include <thread>
#include <atomic>
#include <functional>
#include <chrono>

namespace cortex::stream {

class StreamProcessor {
public:
    using EventCallback = std::function<void(const StreamEvent&)>;

    explicit StreamProcessor(RingBuffer<StreamEvent>& buffer,
                             StatAccumulator& accumulator)
        : buffer_(buffer), accumulator_(accumulator) {}

    ~StreamProcessor() { stop(); }

    // Start the consumer thread. Calls callback (if set) for every event
    // in addition to updating the stat accumulator.
    void start(EventCallback cb = nullptr);

    // Signal the consumer thread to drain remaining events and exit.
    void stop() noexcept;

    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // Throughput stats (approximate, non-atomic for display only)
    struct Metrics {
        int64_t events_processed;
        double  events_per_sec;
        int64_t idle_spins;       // times pop returned nullopt
    };
    Metrics metrics() const noexcept;

private:
    void run(EventCallback cb) noexcept;

    RingBuffer<StreamEvent>& buffer_;
    StatAccumulator&         accumulator_;
    std::atomic<bool>        running_{false};
    std::atomic<bool>        stop_flag_{false};
    std::jthread             thread_;

    // Metrics (written only by consumer thread)
    int64_t events_processed_ = 0;
    int64_t idle_spins_       = 0;
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace cortex::stream
