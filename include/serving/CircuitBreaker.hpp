#pragma once
// CircuitBreaker — protects external dependencies with fast-fail behavior.
//
// Three states: Closed (normal), Open (fast-fail), HalfOpen (probe).
// When failures exceed a threshold, the breaker opens. After a recovery
// timeout, it transitions to half-open and allows a limited number of probe
// requests. If a probe succeeds, it closes; if it fails, it re-opens.
//
// Thread-safe: uses a mutex to protect internal state.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace cortex::serving {

class CircuitBreaker {
public:
    enum class State : uint8_t { Closed, Open, HalfOpen };

    struct Config {
        uint32_t                      failure_threshold      = 5;
        std::chrono::milliseconds     recovery_timeout       = std::chrono::seconds{30};
        uint32_t                      half_open_max_attempts = 1;
    };

    CircuitBreaker() : cfg_({}) {}
    explicit CircuitBreaker(Config cfg) : cfg_(cfg) {}

    // Returns true if the request should proceed, false to fast-fail.
    bool allow_request() {
        std::lock_guard lock(mu_);
        auto now = std::chrono::steady_clock::now();

        switch (state_) {
        case State::Closed:
            return true;

        case State::Open:
            // Check if recovery timeout has elapsed.
            if (now - opened_at_ >= cfg_.recovery_timeout) {
                state_ = State::HalfOpen;
                half_open_attempts_ = 0;
                // Fall through to HalfOpen logic.
            } else {
                return false;
            }
            [[fallthrough]];

        case State::HalfOpen:
            if (half_open_attempts_ < cfg_.half_open_max_attempts) {
                ++half_open_attempts_;
                return true;
            }
            return false;
        }
        return false;  // unreachable, silences compiler warning
    }

    // Record a successful operation.
    void record_success() {
        std::lock_guard lock(mu_);
        failure_count_ = 0;
        if (state_ == State::HalfOpen) {
            state_ = State::Closed;
        }
    }

    // Record a failed operation.
    void record_failure() {
        std::lock_guard lock(mu_);
        ++failure_count_;

        switch (state_) {
        case State::Closed:
            if (failure_count_ >= cfg_.failure_threshold) {
                transition_to_open();
            }
            break;

        case State::HalfOpen:
            // Probe failed — re-open.
            transition_to_open();
            break;

        case State::Open:
            // Already open, nothing to do.
            break;
        }
    }

    // Current state.
    State state() const {
        std::lock_guard lock(mu_);
        return state_;
    }

    // Human-readable state string.
    std::string state_string() const {
        std::lock_guard lock(mu_);
        return state_to_string(state_);
    }

    // Current failure count (useful for monitoring).
    uint32_t failure_count() const {
        std::lock_guard lock(mu_);
        return failure_count_;
    }

private:
    void transition_to_open() {
        state_ = State::Open;
        opened_at_ = std::chrono::steady_clock::now();
        failure_count_ = 0;
        half_open_attempts_ = 0;
    }

    static std::string state_to_string(State s) {
        switch (s) {
        case State::Closed:   return "closed";
        case State::Open:     return "open";
        case State::HalfOpen: return "half_open";
        }
        return "unknown";
    }

    Config                                        cfg_;
    mutable std::mutex                            mu_;
    State                                         state_          = State::Closed;
    uint32_t                                      failure_count_  = 0;
    std::chrono::steady_clock::time_point         opened_at_;
    uint32_t                                      half_open_attempts_ = 0;
};

} // namespace cortex::serving
