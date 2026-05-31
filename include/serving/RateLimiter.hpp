#pragma once
// RateLimiter — token bucket rate limiter for HTTP endpoints.
//
// Each client (identified by IP or fd) gets a bucket that refills at a
// configurable rate. Requests that exceed the burst capacity receive 429.
//
// Thread-safe: uses a mutex to protect the bucket map.
// Stale entries are evicted periodically to prevent memory growth.

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cortex::serving {

class RateLimiter {
public:
    struct Config {
        double tokens_per_sec;    // refill rate
        double max_burst;         // bucket capacity
        int    evict_after_sec;   // remove idle entries after N seconds

        Config() : tokens_per_sec(50.0), max_burst(100.0), evict_after_sec(300) {}
    };

    explicit RateLimiter(Config cfg = Config{}) : cfg_(cfg) {}

    // Returns true if the request is allowed, false if rate-limited.
    bool allow(const std::string& client_id) {
        std::lock_guard lock(mu_);
        auto now = std::chrono::steady_clock::now();
        auto& bucket = buckets_[client_id];

        if (bucket.tokens < 0.0) {
            // First access — initialize
            bucket.tokens    = cfg_.max_burst;
            bucket.last_time = now;
        }

        // Refill tokens based on elapsed time
        double elapsed = std::chrono::duration<double>(now - bucket.last_time).count();
        bucket.tokens    = std::min(cfg_.max_burst, bucket.tokens + elapsed * cfg_.tokens_per_sec);
        bucket.last_time = now;

        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }
        return false;
    }

    // Remove entries that haven't been seen in evict_after_sec seconds.
    void evict_stale() {
        std::lock_guard lock(mu_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = buckets_.begin(); it != buckets_.end(); ) {
            double age = std::chrono::duration<double>(now - it->second.last_time).count();
            if (age > cfg_.evict_after_sec)
                it = buckets_.erase(it);
            else
                ++it;
        }
    }

    size_t bucket_count() const {
        std::lock_guard lock(mu_);
        return buckets_.size();
    }

private:
    struct Bucket {
        double tokens = -1.0;  // -1 = uninitialized
        std::chrono::steady_clock::time_point last_time;
    };

    Config                                     cfg_;
    mutable std::mutex                         mu_;
    std::unordered_map<std::string, Bucket>    buckets_;
};

} // namespace cortex::serving
