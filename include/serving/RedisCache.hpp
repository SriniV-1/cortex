#pragma once
// RedisCache — thin hiredis wrapper with TTL-based caching for stat aggregations.
//
// All methods are synchronous and safe to call from the server (poller) thread.
// Redis is used as a read-through cache: if a key is absent, caller computes
// the value and stores it with a TTL.

#include "serving/ICache.hpp"
#include "serving/CircuitBreaker.hpp"

#include <string>
#include <optional>
#include <chrono>

struct redisContext;

namespace cortex::serving {

class RedisCache : public ICache {
public:
    explicit RedisCache(const std::string& host = "127.0.0.1", int port = 6379);
    ~RedisCache() override;

    // ICache interface
    std::optional<std::string> get(const std::string& key) override;

    void set(const std::string& key, const std::string& value, int ttl_sec = 60) override;

    void del(const std::string& key) override;

    bool connected() const noexcept override { return ctx_ != nullptr; }

    // Legacy overload accepting chrono::seconds (delegates to ICache::set).
    bool set_with_status(const std::string& key, const std::string& value,
                         std::chrono::seconds ttl = std::chrono::seconds{60});

    // Circuit breaker accessor for health monitoring.
    const CircuitBreaker& circuit_breaker() const { return breaker_; }
    CircuitBreaker&       circuit_breaker()       { return breaker_; }

private:
    redisContext*  ctx_     = nullptr;
    CircuitBreaker breaker_;
};

} // namespace cortex::serving
