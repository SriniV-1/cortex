#pragma once
#include <optional>
#include <string>

namespace cortex::serving {
class ICache {
public:
    virtual ~ICache() = default;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual void set(const std::string& key, const std::string& value, int ttl_sec = 60) = 0;
    virtual void del(const std::string& key) = 0;
    virtual bool connected() const = 0;
};
} // namespace cortex::serving
