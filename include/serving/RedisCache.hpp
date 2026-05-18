#pragma once
// RedisCache — thin hiredis wrapper with TTL-based caching for stat aggregations.
//
// All methods are synchronous and safe to call from the server (poller) thread.
// Redis is used as a read-through cache: if a key is absent, caller computes
// the value and stores it with a TTL.

#include <string>
#include <optional>
#include <chrono>

struct redisContext;

namespace cortex::serving {

class RedisCache {
public:
    explicit RedisCache(const std::string& host = "127.0.0.1", int port = 6379);
    ~RedisCache();

    // Returns the cached value for key, or nullopt if absent / error.
    std::optional<std::string> get(const std::string& key) const;

    // Set key → value with a TTL. Returns false on error.
    bool set(const std::string& key, const std::string& value,
             std::chrono::seconds ttl = std::chrono::seconds{60});

    // Invalidate a key.
    void del(const std::string& key);

    bool connected() const noexcept { return ctx_ != nullptr; }

private:
    redisContext* ctx_ = nullptr;
};

} // namespace cortex::serving
