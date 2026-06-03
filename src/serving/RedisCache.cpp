#include "serving/RedisCache.hpp"
#include "common/Logger.hpp"

#include <hiredis/hiredis.h>
#include <stdexcept>

namespace cortex::serving {

RedisCache::RedisCache(const std::string& host, int port) {
    auto log = cortex::get_logger("redis");
    struct timeval tv{1, 0};  // 1s connect timeout
    ctx_ = ::redisConnectWithTimeout(host.c_str(), port, tv);
    if (!ctx_ || ctx_->err) {
        std::string err = ctx_ ? ctx_->errstr : "null context";
        if (ctx_) { ::redisFree(ctx_); ctx_ = nullptr; }
        log->warn("Redis unavailable ({}:{}) — caching disabled: {}", host, port, err);
        return;
    }
    log->info("Redis connected {}:{}", host, port);
}

RedisCache::~RedisCache() {
    if (ctx_) { ::redisFree(ctx_); ctx_ = nullptr; }
}

std::optional<std::string> RedisCache::get(const std::string& key) {
    if (!ctx_) return std::nullopt;
    auto* reply = static_cast<redisReply*>(
        ::redisCommand(ctx_, "GET %b", key.data(), key.size()));
    if (!reply) { return std::nullopt; }
    std::optional<std::string> result;
    if (reply->type == REDIS_REPLY_STRING)
        result = std::string(reply->str, reply->len);
    ::freeReplyObject(reply);
    return result;
}

void RedisCache::set(const std::string& key, const std::string& value,
                     int ttl_sec) {
    if (!ctx_) return;
    auto* reply = static_cast<redisReply*>(
        ::redisCommand(ctx_, "SET %b %b EX %lld",
                       key.data(),   key.size(),
                       value.data(), value.size(),
                       static_cast<long long>(ttl_sec)));
    if (reply) ::freeReplyObject(reply);
}

bool RedisCache::set_with_status(const std::string& key, const std::string& value,
                                 std::chrono::seconds ttl) {
    if (!ctx_) return false;
    auto* reply = static_cast<redisReply*>(
        ::redisCommand(ctx_, "SET %b %b EX %lld",
                       key.data(),   key.size(),
                       value.data(), value.size(),
                       static_cast<long long>(ttl.count())));
    bool ok = reply && reply->type == REDIS_REPLY_STATUS;
    if (reply) ::freeReplyObject(reply);
    return ok;
}

void RedisCache::del(const std::string& key) {
    if (!ctx_) return;
    auto* reply = static_cast<redisReply*>(
        ::redisCommand(ctx_, "DEL %b", key.data(), key.size()));
    if (reply) ::freeReplyObject(reply);
}

} // namespace cortex::serving
