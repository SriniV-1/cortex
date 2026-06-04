// Unit tests for CircuitBreaker — state transitions and thread safety.

#include "serving/CircuitBreaker.hpp"

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace cortex::serving;

// ── State transitions ────────────────────────────────────────────────────

TEST(CircuitBreaker, StartsInClosedState) {
    CircuitBreaker cb;
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Closed);
    EXPECT_EQ(cb.state_string(), "closed");
}

TEST(CircuitBreaker, ClosedAllowsRequests) {
    CircuitBreaker cb;
    EXPECT_TRUE(cb.allow_request());
    EXPECT_TRUE(cb.allow_request());
    EXPECT_TRUE(cb.allow_request());
}

TEST(CircuitBreaker, ClosedToOpenAfterThresholdFailures) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 3;
    CircuitBreaker cb(cfg);

    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Closed);
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Closed);
    cb.record_failure();  // 3rd failure — should trip
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Open);
    EXPECT_EQ(cb.state_string(), "open");
}

TEST(CircuitBreaker, OpenRejectsRequests) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout = std::chrono::seconds{60};  // won't elapse during test
    CircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Open);

    EXPECT_FALSE(cb.allow_request());
    EXPECT_FALSE(cb.allow_request());
}

TEST(CircuitBreaker, SuccessResetsFailureCount) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 3;
    CircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    cb.record_success();  // resets count
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Closed);
    EXPECT_EQ(cb.failure_count(), 0u);

    // Need 3 more failures to trip
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Closed);
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Open);
}

// ── Open → HalfOpen after recovery timeout ──────────────────────────────

TEST(CircuitBreaker, OpenToHalfOpenAfterTimeout) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout = std::chrono::milliseconds{50};
    cfg.half_open_max_attempts = 1;
    CircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Open);

    // Wait for recovery timeout to elapse
    std::this_thread::sleep_for(std::chrono::milliseconds{60});

    // allow_request triggers the transition
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.state(), CircuitBreaker::State::HalfOpen);
}

// ── HalfOpen → Closed on success ────────────────────────────────────────

TEST(CircuitBreaker, HalfOpenToClosedOnSuccess) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout = std::chrono::milliseconds{50};
    cfg.half_open_max_attempts = 1;
    CircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Open);

    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    EXPECT_TRUE(cb.allow_request());  // transitions to HalfOpen
    EXPECT_EQ(cb.state(), CircuitBreaker::State::HalfOpen);

    cb.record_success();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Closed);
    EXPECT_EQ(cb.state_string(), "closed");

    // Should allow requests normally again
    EXPECT_TRUE(cb.allow_request());
}

// ── HalfOpen → Open on failure ──────────────────────────────────────────

TEST(CircuitBreaker, HalfOpenToOpenOnFailure) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout = std::chrono::milliseconds{50};
    cfg.half_open_max_attempts = 1;
    CircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Open);

    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    EXPECT_TRUE(cb.allow_request());  // transitions to HalfOpen
    EXPECT_EQ(cb.state(), CircuitBreaker::State::HalfOpen);

    cb.record_failure();  // probe failed
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Open);
    EXPECT_EQ(cb.state_string(), "open");
}

// ── HalfOpen limits probe attempts ──────────────────────────────────────

TEST(CircuitBreaker, HalfOpenLimitsProbes) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout = std::chrono::milliseconds{50};
    cfg.half_open_max_attempts = 2;
    CircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds{60});

    EXPECT_TRUE(cb.allow_request());   // probe 1
    EXPECT_TRUE(cb.allow_request());   // probe 2
    EXPECT_FALSE(cb.allow_request());  // exceeds max attempts
}

// ── Thread safety ────────────────────────────────────────────────────────

TEST(CircuitBreaker, ThreadSafetyConcurrentAccess) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 100;
    cfg.recovery_timeout = std::chrono::milliseconds{10};
    CircuitBreaker cb(cfg);

    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 1000;
    std::atomic<int> allowed{0};
    std::atomic<int> denied{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                if (cb.allow_request()) {
                    allowed.fetch_add(1, std::memory_order_relaxed);
                    // Mix of successes and failures
                    if (i % 3 == 0)
                        cb.record_failure();
                    else
                        cb.record_success();
                } else {
                    denied.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& t : threads)
        t.join();

    // No crashes, no data races — that's the main assertion.
    // Verify total operations adds up.
    EXPECT_EQ(allowed.load() + denied.load(), kThreads * kOpsPerThread);

    // State should be one of the valid states.
    auto st = cb.state();
    EXPECT_TRUE(st == CircuitBreaker::State::Closed ||
                st == CircuitBreaker::State::Open ||
                st == CircuitBreaker::State::HalfOpen);
}

// ── Default configuration ────────────────────────────────────────────────

TEST(CircuitBreaker, DefaultConfigValues) {
    CircuitBreaker cb;
    // Should tolerate 4 failures without tripping
    for (int i = 0; i < 4; ++i) {
        cb.record_failure();
        EXPECT_EQ(cb.state(), CircuitBreaker::State::Closed);
    }
    // 5th failure trips the breaker
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitBreaker::State::Open);
}
